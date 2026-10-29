// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <magic_enum/magic_enum.hpp>
#include <xxhash.h>

#include "bbport_copy.h"
#include "common/alignment.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/async_upload.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_adaptive.h"
#include "video_core/sotc_write_extent.h"
#include "video_core/buffer_cache/bpe_heap_guard.h"
#include <optional>

extern "C" uintptr_t runtime_sotc_heap(void);
extern "C" uintptr_t runtime_sotc_heap_table(void);
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

/// sotc: SOTC_RB_NARROW=<KiB> (power of two, 4-512; 1 or unset = 64 KiB): the readback window,
/// in bytes; 0 keeps the 512 KiB one (Shrine: 46.0 FPS at 512 KiB, 47.9-48.8 at 64, 47.5 at 128,
/// 42.9 at 32, where the readbacks double). A readback unprotects every GPU-written page of its
/// window and the next GPU writes of those pages protect them again one binding at a time (most of
/// the draw thread's VirtualProtect calls), every frame; a smaller window leaves the pages the
/// guest does not read protected and GPU-modified. Read once, at static initialisation (no
/// function-local guard in the fault paths); BufferCache::rb_narrow_window applies it.
static const u64 rb_narrow_env = [] {
    const char* v = std::getenv("SOTC_RB_NARROW");
    const u64 kib = v ? std::strtoull(v, nullptr, 10) : 1;
    if (kib == 0) {
        return u64{0};
    }
    return std::bit_floor(kib == 1 ? u64{64} : std::clamp<u64>(kib, 4, 512)) * 1_KB;
}();

// sotc: guest readback request pattern (SOTC_FPS) and SOTC_RB_GROW. Self-contained: the switches,
// the per-guest-thread state and the pattern table below, BufferCache::PrepareGuestReadback /
// NoteGuestReadbackDone (guest thread, around the async readback in ReadMemory) and
// GrowReadbackWindow (command processor, BeginAsyncReadback). Nothing here changes behaviour
// unless SOTC_RB_GROW=1; the counters run only with SOTC_FPS.
namespace {
/// SOTC_RB_GROW=1 (off by default): a guest thread that reads GPU-written memory sequentially
/// (each request adjacent to its previous window, soon after it) gets its window doubled per
/// request, up to SOTC_RB_GROW_MAX_KB, in the direction of travel; any other request goes back to
/// the base window. Needs the SOTC_RB_NARROW window (precise readback mode, the default).
const bool rb_grow_env = [] {
    const char* v = std::getenv("SOTC_RB_GROW");
    return v && v[0] == '1';
}();
/// SOTC_RB_GROW_US (default 1000): the longest time, in microseconds, from the end of the previous
/// request of the same thread to this one for the window to grow.
const u64 rb_grow_gap_ns = [] {
    const char* v = std::getenv("SOTC_RB_GROW_US");
    return (v ? std::strtoull(v, nullptr, 10) : u64{1000}) * 1000;
}();
/// SOTC_RB_GROW_MAX_KB (default 1024; power of two, 8-4096): the largest grown window.
const u64 rb_grow_max = [] {
    const char* v = std::getenv("SOTC_RB_GROW_MAX_KB");
    const u64 kib = v ? std::strtoull(v, nullptr, 10) : u64{1024};
    return std::bit_floor(std::clamp<u64>(kib, 8, 4096)) * 1_KB;
}();
/// SOTC_RB_PREFETCH=1 (off by default): see BufferCache::PrefetchIssue. Only with SOTC_RB_OWNER=1
/// and the asynchronous readbacks (SOTC_RB_ASYNC not 0): a prefetch is an in-flight readback
/// owning its pages, and only the RB_OWNER paths know how to wait for or finish such an owner.
const bool rb_prefetch_env = [] {
    const char* v = std::getenv("SOTC_RB_PREFETCH");
    const char* mat = std::getenv("SOTC_RB_MATERIALIZE"); // sotc: needs the prefetches
    const char* prev = std::getenv("SOTC_RB_PREV");       // sotc: needs the recurring windows
    const char* exact = std::getenv("SOTC_RB_EXACT");     // sotc: needs the recurring windows
    const char* group = std::getenv("SOTC_RB_GROUP");     // sotc: group downloads are prefetches
    if ((!v || v[0] != '1') && (!mat || mat[0] != '1') && (!prev || prev[0] != '1') &&
        (!exact || exact[0] != '1') && (!group || group[0] != '1')) {
        return false;
    }
    const char* owner = std::getenv("SOTC_RB_OWNER");
    const char* async = std::getenv("SOTC_RB_ASYNC");
    const bool active = owner && owner[0] == '1' && (!async || async[0] != '0');
    std::fprintf(stderr, active ? "SOTC RB prefetch: on\n"
                                : "SOTC RB prefetch: inactive (needs SOTC_RB_OWNER=1 and "
                                  "asynchronous readbacks)\n");
    return active;
}();
/// SOTC_RB_PREFETCH_TICKS (default 8): an unconsumed prefetch is freed this many ticks after its own.
const u64 rb_pf_ticks = [] {
    const char* v = std::getenv("SOTC_RB_PREFETCH_TICKS");
    return std::max<u64>(v ? std::strtoull(v, nullptr, 10) : u64{8}, 1);
}();
/// SOTC_RB_PREFETCH_MAX (default 32, 1-256): prefetches in flight at most.
const size_t rb_pf_max = [] {
    const char* v = std::getenv("SOTC_RB_PREFETCH_MAX");
    return static_cast<size_t>(std::clamp<u64>(v ? std::strtoull(v, nullptr, 10) : 32, 1, 256));
}();
/// SOTC_RB_PREFETCH_MB (default 4): staging memory the prefetches in flight hold at most.
const u64 rb_pf_budget = [] {
    const char* v = std::getenv("SOTC_RB_PREFETCH_MB");
    return std::max<u64>(v ? std::strtoull(v, nullptr, 10) : u64{4}, 1) << 20;
}();
/// SOTC_RB_MATERIALIZE=1 (off by default): see BufferCache::PrefetchMaterialize. Turns the
/// prefetches on (rb_prefetch_env, same requirements).
const bool rb_materialize_env = [] {
    const char* v = std::getenv("SOTC_RB_MATERIALIZE");
    if (!v || v[0] != '1') {
        return false;
    }
    std::fprintf(stderr, rb_prefetch_env ? "SOTC RB materialize: on\n"
                                         : "SOTC RB materialize: inactive (needs SOTC_RB_OWNER=1 "
                                           "and asynchronous readbacks)\n");
    return rb_prefetch_env;
}();
/// SOTC_RB_MATERIALIZE_US (default 100): PrefetchMaterialize looks at the prefetches at most this
/// often (each look reads the GPU timeline).
const u64 rb_mat_ns = [] {
    const char* v = std::getenv("SOTC_RB_MATERIALIZE_US");
    return (v ? std::strtoull(v, nullptr, 10) : u64{100}) * 1000;
}();
/// SOTC_RB_EXACT=1 (off by default): see BufferCache::TryServeExact. Turns the prefetch
/// bookkeeping on (rb_prefetch_env, same requirements: SOTC_RB_OWNER=1, asynchronous readbacks).
/// Replaces SOTC_RB_PREV (which is then off): the owed downloads of the two would dirty each other.
const bool rb_exact_env = [] {
    const char* v = std::getenv("SOTC_RB_EXACT");
    if (!v || v[0] != '1') {
        return false;
    }
    std::fprintf(stderr, rb_prefetch_env ? "SOTC RB exact: on\n"
                                         : "SOTC RB exact: inactive (needs SOTC_RB_OWNER=1 and "
                                           "asynchronous readbacks)\n");
    return rb_prefetch_env;
}();
/// SOTC_RB_GROUP=1 (off by default): see BufferCache::RbgAtFault. Turns the prefetch bookkeeping
/// on (rb_prefetch_env, same requirements: SOTC_RB_OWNER=1, asynchronous readbacks): a group
/// download is an in-flight readback owning its pages.
const bool rb_group_env = [] {
    const char* v = std::getenv("SOTC_RB_GROUP");
    if (!v || v[0] != '1') {
        return false;
    }
    std::fprintf(stderr, rb_prefetch_env ? "SOTC RB group: on\n"
                                         : "SOTC RB group: inactive (needs SOTC_RB_OWNER=1 and "
                                           "asynchronous readbacks)\n");
    return rb_prefetch_env;
}();
/// SOTC_RB_GROUP caps: ranges one bind contributes, page runs and bytes one group download
/// copies, group downloads in flight and their staging, closed groups and index entries kept, and
/// the age (in game frames) of a group still delivered.
constexpr size_t RbgRangesMax = 1024;
constexpr size_t RbgRunsMax = 1024;
constexpr u64 RbgBytesMax = 8ull << 20;
constexpr u32 RbgLiveMax = 16;
constexpr u64 RbgBudget = 32ull << 20;
constexpr size_t RbgGroupsMax = 4096;
constexpr size_t RbgIndexMax = 65536;
constexpr size_t RbgKeysMax = 4096;
constexpr u64 RbgMaxFrames = 2;
/// sotc: SOTC_RB_EXACT is active only with the label side (liverpool.cpp defers labels, applies
/// owed downloads before them and settles before immediate announcements): without it a label
/// could announce bytes that are still owed. Set once by the Liverpool constructor.
std::atomic<bool> rbx_label_side{false};
/// sotc: the draw thread (Liverpool BackLoop); the one identity every draw-thread check uses.
std::atomic<std::thread::id> rbx_draw_thread{};
/// SOTC_WRITE_FAULT_NARROW=1 (off by default): a guest write fault reads back only the faulting
/// page(s), not the readback window (see BufferCache::BeginAsyncReadbackImpl).
const bool rb_wf_narrow_env = [] {
    const char* v = std::getenv("SOTC_WRITE_FAULT_NARROW");
    return v && v[0] == '1';
}();
/// sotc: set by liverpool.cpp (BufferCache::SetWaitObserver); read on guest threads.
std::atomic<BufferCache::WaitObserver> rbx_wait_observer{nullptr};
inline bool RbxOn() noexcept {
    return rb_exact_env && rbx_label_side.load(std::memory_order_acquire);
}
inline bool RbxOnDrawThread() noexcept {
    return std::this_thread::get_id() == rbx_draw_thread.load(std::memory_order_acquire);
}
/// SOTC_RB_PREV=1 (off by default): see BufferCache::TryServePrevious. Turns the prefetch
/// bookkeeping on (rb_prefetch_env, same requirements).
const bool rb_prev_env = [] {
    const char* v = std::getenv("SOTC_RB_PREV");
    if (!v || v[0] != '1') {
        return false;
    }
    if (rb_exact_env) {
        std::fprintf(stderr, "SOTC RB previous: off (SOTC_RB_EXACT replaces it)\n");
        return false;
    }
    std::fprintf(stderr, rb_prefetch_env ? "SOTC RB previous: on\n"
                                         : "SOTC RB previous: inactive (needs SOTC_RB_OWNER=1 and "
                                           "asynchronous readbacks)\n");
    return rb_prefetch_env;
}();
/// Prefetches are issued only when asked for (SOTC_RB_PREFETCH, SOTC_RB_MATERIALIZE or
/// SOTC_RB_PREV); SOTC_RB_EXACT alone only needs the bookkeeping.
const bool rb_pf_issue_env = [] {
    const char* v = std::getenv("SOTC_RB_PREFETCH");
    const char* mat = std::getenv("SOTC_RB_MATERIALIZE");
    return rb_prefetch_env && ((v && v[0] == '1') || (mat && mat[0] == '1') || rb_prev_env);
}();
/// SOTC_RB_PREV_FRAMES (default 2, 1-4): the guest memory version served is at most this many
/// frames old; an older one makes the read wait for the GPU as before (and refreshes it).
const u64 rb_prev_frames = [] {
    const char* v = std::getenv("SOTC_RB_PREV_FRAMES");
    return std::clamp<u64>(v ? std::strtoull(v, nullptr, 10) : u64{2}, 1, 4);
}();
/// ... and at most this old in time (flips stop during loads and pauses).
constexpr u64 RbPrevMaxNs = 150'000'000;

/// A fresh range also stops counting this long after its first write: flips (the frame count)
/// stop during loads and pauses, and old data must not look fresh then (review of 2026-10-28).
constexpr u64 RbMatFreshMaxNs = 100'000'000;
/// Fresh ranges kept per window (SOTC_RB_MATERIALIZE); more replace the oldest.
constexpr size_t RbMatFreshMax = 8;

/// SOTC_RB_EXACT caps: owed downloads in flight, their staging, label-download staging, and the
/// writer map entries kept before it is pruned (and, past the cap after pruning, forgotten).
constexpr size_t RbxOwedMax = 256;
constexpr u64 RbxOwedBudget = 32ull << 20;
constexpr u64 RbxLabelBudget = 16ull << 20;
constexpr size_t RbxWritersMax = 32768;
constexpr size_t RbxLabelRangesMax = 256;

/// Granules remembered at most (PfHistory entries; older ones age out after 8 frames unread).
constexpr size_t RbPfHistoryMax = 4096;

/// A request is adjacent to the previous window when it starts at most this far past either end.
constexpr u64 RbAdjacentBytes = 64_KB;
/// ... and "within 1 ms" in the pattern line when the gap is at most this long.
constexpr u64 RbAdjacentNs = 1'000'000;

/// The previous async readback of this guest thread (ReadMemory runs on the faulting thread).
struct RbThreadState {
    VAddr prev_start = 0;
    VAddr prev_end = 0; ///< 0: no previous readback
    u64 prev_done_ns = 0;
    u64 window = 0; ///< SOTC_RB_GROW: its window size, 0 for the base one
};
thread_local RbThreadState tl_rb;

/// Open-addressing table of base-granule addresses (never 0) -> count, fixed size: no allocation
/// in the fault handler. Refuses new keys past 3/4 full.
template <u32 Bits>
struct RbKeyTable {
    static constexpr size_t Size = size_t{1} << Bits;
    std::array<VAddr, Size> keys{};
    std::array<u32, Size> counts{};
    size_t used = 0;
    u32* Get(VAddr key, bool insert) {
        size_t i = static_cast<size_t>(((key >> 12) * 0x9E3779B97F4A7C15ULL) >> (64 - Bits));
        for (;; i = (i + 1) & (Size - 1)) {
            if (keys[i] == key) {
                return &counts[i];
            }
            if (keys[i] == 0) {
                if (!insert || used >= Size / 4 * 3) {
                    return nullptr;
                }
                keys[i] = key;
                ++used;
                return &counts[i];
            }
        }
    }
    void Clear() {
        if (used != 0) {
            keys.fill(0);
            counts.fill(0);
            used = 0;
        }
    }
};
/// SOTC_FPS: granules asked this report period, and in this and the previous game frame.
struct RbPatternTable {
    std::mutex mutex;
    RbKeyTable<14> period;
    RbKeyTable<12> frames[2];
    u32 cur = 0;
    u64 frame = ~u64{0};
    u64 untracked = 0; ///< requests of this period whose granule did not fit in `period`
};
RbPatternTable rb_pattern;
} // namespace

} // namespace VideoCore

void SotcStats::RbPatternTake(char* out, std::size_t out_size) {
    using VideoCore::rb_pattern;
    std::array<std::pair<u32, VAddr>, 5> top{};
    size_t distinct = 0;
    u64 untracked = 0;
    {
        std::scoped_lock lock{rb_pattern.mutex};
        auto& table = rb_pattern.period;
        distinct = table.used;
        untracked = std::exchange(rb_pattern.untracked, 0);
        for (size_t i = 0; i < table.Size; ++i) {
            if (table.keys[i] == 0 || table.counts[i] <= top.back().first) {
                continue;
            }
            top.back() = {table.counts[i], table.keys[i]};
            std::ranges::sort(top, std::greater{});
        }
        table.Clear();
    }
    int used = std::snprintf(out, out_size, "distinct windows %zu%s, top windows", distinct,
                             untracked ? " (table full)" : "");
    for (const auto& [count, key] : top) {
        if (count == 0 || used < 0 || static_cast<size_t>(used) >= out_size) {
            break;
        }
        used += std::snprintf(out + used, out_size - used, " %#llx x%u",
                              static_cast<unsigned long long>(key), count);
    }
}

namespace VideoCore {

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      // sotc: SOTC_REBAR_UPLOAD puts the stream buffer in the large BAR heap when it fits.
      stream_buffer{instance, scheduler,
                    ResolveRebarType(instance, STREAM_BUFFER_SIZE, MemoryType::Stream),
                    STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    // sotc: SOTC_ASYNC_UPLOAD (see async_upload.h).
    AsyncUpload::Init();
    // sotc: SOTC_HRR dry run (see hrr.h).
    if (HostResidentRanges::Enabled()) {
        hrr = std::make_unique<HostResidentRanges>();
    }
    // sotc: SOTC_HOST_IMPORT (see host_import.h).
    if (instance.IsHostImportEnabled()) {
        host_import = std::make_unique<HostImport>(instance, scheduler);
        if (!host_import->Enabled()) {
            host_import.reset();
        }
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    // sotc: one u16 per block of the address space (32 MiB for 64 KiB blocks); calloc leaves
    // the pages nothing touches uncommitted.
    block_last_use =
        static_cast<u16*>(std::calloc(blocks_per_arena_page * NUM_ARENA_PAGES, sizeof(u16)));
    ASSERT_MSG(block_last_use, "No memory for the arena block use table");
    epoch_start = std::chrono::steady_clock::now();
    // sotc: SOTC_RB_NARROW only in the precise readback mode, where every GPU-modified page is
    // read-protected: the relaxed mode reads back only on write faults, and the wide window is
    // all that refreshes the data around them.
    if (rb_narrow_env && memory_tracker->ReadbacksMode() == GpuReadbacksMode::Precise) {
        rb_narrow_window = rb_narrow_env;
        SotcStats::rb_narrow_kib.store(rb_narrow_window / 1_KB, std::memory_order_relaxed);
    }
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    // System memory the sparse arenas can fall back to once video memory is exhausted.
    const auto& memory_properties = instance.GetMemoryProperties();
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const auto& heap = memory_properties.memoryHeaps[memory_properties.memoryTypes[i].heapIndex];
        if (((reqs.memoryTypeBits >> i) & 1) != 0 &&
            !(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal)) {
            arena_fallback_memory_type_index = i;
            break;
        }
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() {
    AsyncUpload::Shutdown(); // sotc: SOTC_ASYNC_UPLOAD
    std::free(block_last_use);
}

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks,
                                   bool guest_fault) {
    if (SotcStats::kCmdStats) SotcStats::inv_buffer.fetch_add(size, std::memory_order_relaxed);
    Core::MemoryManager::NoteHostWrite(device_addr, size, 9,
                                       reinterpret_cast<u64>(__builtin_return_address(0)));
    // sotc: SOTC_RB_EXACT: a guest write fault marks the whole 4 KiB page(s) CPU-modified, so
    // bytes owed anywhere on those pages are applied first (hot=false: the CPU mark has not
    // happened yet), waiting for their copy if needed. The check is made again after the tracker
    // looked at the pages: a release registers its owed bytes before it unmarks the pages, so a
    // fault that saw the pages released sees the owed bytes here.
    RbxSettleWritePages(device_addr, size, assume_locks);
    memory_tracker->InvalidateRegion(
        device_addr, size,
        [this, device_addr, size, assume_locks] {
            ReadMemory(device_addr, size, true, assume_locks);
        },
        guest_fault);
    // sotc: SOTC_RB_EXACT: see the top.
    RbxSettleWritePages(device_addr, size, assume_locks);
}

void BufferCache::RbxSettleWritePages(VAddr addr, u64 size, bool assume_locks) {
    if (rbx_owed_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const VAddr lo = Common::AlignDown(addr, VAddr{4096});
    const u64 len = Common::AlignUp(addr + size, VAddr{4096}) - lo;
    if (!RbxOwedIntersects(lo, len)) {
        return;
    }
    if (assume_locks) {
        RbxSettleSync(lo, len);
    } else {
        ReadMemory(lo, len, true, false);
    }
}

void BufferCache::DropGpuModified(VAddr device_addr, u64 size) {
    if (rb_prev_env) {
        PrevErase(device_addr, size); // sotc: SOTC_RB_PREV: the memory may hold other data now
    }
    // sotc: SOTC_ASYNC_UPLOAD write fence: the range may be mapped again (the same memory at
    // another address is not write-protected) once unmapped, so its pending copies finish first.
    if (auto* async_upload = AsyncUpload::Get()) {
        async_upload->WaitVa(device_addr, size);
    }
    // sotc: by default the guest thread waits for the command processor (SendCommand<true>).
    // SOTC_DROP_ASYNC=1 orders the drop with the command stream instead (SendOrderedCommand), so
    // the guest does not block — but then a readback from a nearby guest fault (command_queue,
    // run between operations) can overtake the drop, still see the unmapped range as GPU-written
    // and copy old arena bytes over what the guest wrote after mapping it again (broken
    // vertices while streaming; review of 2026-10-27). Experimental, off by default.
    static const bool async = [] {
        const char* v = std::getenv("SOTC_DROP_ASYNC");
        return v && v[0] == '1';
    }();
    // sotc: SOTC_DROP_FAST=1 (off by default). A range with no GPU-written page (the usual
    // unmap: streamed data the GPU only read) has no GPU state to forget and no readback in
    // flight (those keep their pages GPU-modified until applied): marking it CPU-modified is all
    // the drop does, and a guest write fault does the same from any thread. The guest thread
    // then does not wait for the draw thread to finish its current operation (up to a frame
    // while it waits for a free presentation frame; seconds per minute while loading, with the
    // region lock of the unmap held and the game's other threads sleeping on it).
    static const bool fast = [] {
        const char* v = std::getenv("SOTC_DROP_FAST");
        return v && v[0] == '1';
    }();
    if (fast) {
        bool gpu_ranges;
        {
            std::shared_lock lock{gpu_modified_mutex};
            gpu_ranges = gpu_modified_ranges.Intersects(device_addr, size);
        }
        // sotc: SOTC_RB_EXACT: owed bytes are trimmed by the drop on the draw thread.
        gpu_ranges = gpu_ranges || (rbx_owed_count.load(std::memory_order_relaxed) != 0 &&
                                    RbxOwedIntersects(device_addr, size));
        if (!gpu_ranges) {
            EraseGranuleSnapshots(device_addr, size);
            if (memory_tracker->MarkCpuModifiedUnlessGpu(device_addr, size)) {
                SotcStats::drop_fast.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        SotcStats::drop_slow.fetch_add(1, std::memory_order_relaxed);
    }
    auto drop = [this, device_addr, size] {
        EraseGranuleSnapshots(device_addr, size); // sotc: unmapped: the snapshots are void
        {
            std::unique_lock gpu_modified_lock{gpu_modified_mutex};
            gpu_modified_ranges.Subtract(device_addr, size);
        }
        if (rb_prev_env) {
            // sotc: SOTC_RB_PREV: again here, ordered with the draw thread: a readback applied
            // between the erase at the top and this drop recorded the range again.
            PrevErase(device_addr, size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
        MarkInflightReadbacksDirty(device_addr, size, StaleReason::Unmap);
        memory_tracker->MarkRegionAsCpuModified(device_addr, size);
    };
    if (async) {
        liverpool->SendOrderedCommand(std::move(drop));
        return;
    }
    const auto wait_start = SotcStats::kFpsLog ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    liverpool->SendCommand<true>(std::move(drop));
    if (SotcStats::kFpsLog) { // sotc: read only by the SOTC_FPS printer (sotc_stats.h)
        SotcStats::drop_gpu_wait_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wait_start)
                .count(),
            std::memory_order_relaxed);
    }
}

u64 BufferCache::WriteAheadPages() {
    static const u64 pages = [] {
        const char* v = std::getenv("SOTC_WRITE_AHEAD_PAGES");
        return v ? std::strtoull(v, nullptr, 10) : 15ULL;
    }();
    return pages;
}

void BufferCache::MarkWriteAhead(VAddr addr) {
    const u64 ahead = WriteAheadPages() * 4_KB;
    if (ahead == 0) {
        return;
    }
    const VAddr next_page = Common::AlignUp(addr + 1, 4_KB);
    u64 limit = ahead;
    // sotc: SOTC_RB_EXACT: never write-ahead over bytes still owed to guest memory (a page marked
    // CPU-modified would skip the owed apply, or upload the old guest bytes over the newer ones).
    if (rbx_owed_count.load(std::memory_order_relaxed) != 0) {
        std::scoped_lock lock{rbx_owed_mutex};
        rbx_owed_ranges.ForEachInRange(next_page, ahead, [&](VAddr lo, VAddr) {
            limit = std::min<u64>(limit, Common::AlignDown(lo, 4_KB) - next_page);
        });
    }
    if (limit == 0) {
        return;
    }
    memory_tracker->MarkAheadAsCpuModified(next_page, limit);
}

u64 BufferCache::MarkWriteAheadRange(VAddr begin, VAddr end, bool (*allow)(VAddr page)) {
    constexpr u64 Page = 4096;
    constexpr VAddr AddressLimit = VAddr{1} << MemoryTracker::MAX_CPU_PAGE_BITS;
    end = std::min<VAddr>(end, AddressLimit);
    if (end <= begin) {
        return 0;
    }
    const u64 pages = (end - begin) / Page;
    thread_local std::vector<u8> candidates;
    thread_local std::vector<u64> hashes;
    thread_local std::vector<u8> bytes;
    // Which pages look clean now (a hint: MarkAheadSnapshotted checks again under the lock).
    memory_tracker->PeekCleanPages(begin, pages * Page, candidates);
    if (rbx_owed_count.load(std::memory_order_relaxed) != 0) { // sotc: SOTC_RB_EXACT, see MarkWriteAhead
        std::scoped_lock lock{rbx_owed_mutex};
        for (u64 i = 0; i < pages; ++i) {
            if (candidates[i] && rbx_owed_ranges.Intersects(begin + i * Page, Page)) {
                candidates[i] = 0;
            }
        }
    }
    for (u64 i = 0; i < pages; ++i) {
        if (candidates[i] && !allow(begin + i * Page)) {
            candidates[i] = 0;
        }
    }
    // Snapshot the candidates' bytes. They are write-protected (clean), so nothing changes them
    // until MarkAheadSnapshotted unprotects them; a page that stops being a candidate by then
    // (another fault got to it first) is skipped there and its hash never stored.
    constexpr u64 ChunkPages = 64; // 256 KiB per copy: stays on the serial path of CopySparseMemory
    hashes.assign(pages, 0);
    bytes.resize(ChunkPages * Page);
    for (u64 i = 0; i < pages;) {
        if (!candidates[i]) {
            ++i;
            continue;
        }
        u64 j = i;
        while (j < pages && candidates[j] && j - i < ChunkPages) {
            ++j;
        }
        memory->CopySparseMemory(begin + i * Page, bytes.data(), (j - i) * Page);
        for (u64 k = 0; k < j - i; ++k) {
            hashes[i + k] = XXH3_64bits(bytes.data() + k * Page, Page);
        }
        i = j;
    }
    const u64 marked = memory_tracker->MarkAheadSnapshotted(
        begin, pages * Page, candidates, [&](VAddr addr, u64 count) {
            // Region lock held, run not unprotected yet: the snapshots exist before any write.
            std::scoped_lock lock{granule_mutex};
            if (granule_snapshots.size() + count > 65536) { // capped; a dropped page just uploads
                granule_snapshots.clear();
            }
            const u64 first = (addr - begin) / Page;
            for (u64 k = 0; k < count; ++k) {
                granule_snapshots.insert_or_assign(addr + k * Page,
                                                   (hashes[first + k] << 1) | 1);
            }
            granule_snapshot_count.store(granule_snapshots.size(), std::memory_order_relaxed);
        });
    if (SotcStats::kFpsLog) {
        SotcStats::FaultStat::ahead_pages.fetch_add(marked, std::memory_order_relaxed);
    }
    return marked;
}

std::pair<VAddr, VAddr> BufferCache::ReadbackWindow(const Buffer* arena, VAddr device_addr,
                                                    u64 size) {
    // GPU-modified ranges come as many small scattered islands,
    // so the download is widened to a window around the request
    constexpr u64 WindowSize = 512_KB;
    const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
    const auto window = [&](u64 window_size) {
        const VAddr start =
            std::max<VAddr>(Common::AlignDown(device_addr, window_size), arena->cpu_addr);
        const VAddr end =
            std::min<VAddr>(std::max<VAddr>(start + window_size, device_addr + size), arena_end);
        return std::pair<VAddr, VAddr>{start, end};
    };
    if (rb_narrow_window == 0) {
        return window(WindowSize);
    }
    // sotc: SOTC_RB_NARROW. The window is the aligned granule (or granules) of the request, so
    // the same reads ask for the same window every frame and the hot readbacks (GatherDownloads)
    // bring the other granules the guest reads along with the first readback, as the wide window
    // did. Everything the guest reads is still downloaded: a page outside the window keeps its
    // GPU bit and read protection, and its own fault reads it back.
    const auto narrow = window(rb_narrow_window);
    if (SotcStats::kFpsLog) {
        // The narrow window lies inside the wide one (both power-of-two aligned around the request,
        // clipped to the same arena): count what the wide one would also have taken.
        const auto wide = window(WindowSize);
        const VAddr narrow_end =
            std::min<VAddr>(Common::AlignUp(narrow.second, BYTES_PER_PAGE), wide.second);
        const u64 kept =
            memory_tracker->CountGpuModifiedPages(wide.first, narrow.first - wide.first) +
            memory_tracker->CountGpuModifiedPages(narrow_end, wide.second - narrow_end);
        SotcStats::rb_narrow_kept_bytes.fetch_add(kept * BYTES_PER_PAGE, std::memory_order_relaxed);
    }
    return narrow;
}

std::pair<VAddr, VAddr> BufferCache::GrowReadbackWindow(const Buffer* arena,
                                                        const AsyncReadback& readback) {
    // sotc: SOTC_RB_GROW (PrepareGuestReadback sets grow_window only with a narrow window). The
    // same clamping as ReadbackWindow; GatherDownloads still copies only the GPU-modified,
    // resident, unowned (SOTC_RB_OWNER) ranges in it. Not counted in rb_narrow_kept_bytes.
    const u64 base = rb_narrow_window;
    const u64 size = readback.grow_window;
    const VAddr addr = readback.device_addr;
    const VAddr ask_end = addr + readback.size;
    VAddr start;
    VAddr end;
    if (readback.grow_backward) {
        end = Common::AlignUp(ask_end, base);
        start = end > size ? end - size : 0;
    } else {
        start = Common::AlignDown(addr, base);
        end = start + size;
    }
    const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
    start = std::max<VAddr>(std::min<VAddr>(start, addr), arena->cpu_addr);
    end = std::min<VAddr>(std::max<VAddr>(end, ask_end), arena_end);
    return {start, end};
}

void BufferCache::PrepareGuestReadback(AsyncReadback& readback, u64 now_ns) {
    const RbThreadState& prev = tl_rb;
    const VAddr addr = readback.device_addr;
    const bool have_prev = prev.prev_end != 0;
    const bool forward =
        have_prev && addr >= prev.prev_end && addr - prev.prev_end < RbAdjacentBytes;
    const bool backward =
        have_prev && addr < prev.prev_start && prev.prev_start - addr <= RbAdjacentBytes;
    const u64 gap = have_prev && now_ns > prev.prev_done_ns ? now_ns - prev.prev_done_ns : 0;
    // SOTC_RB_GROW: double the previous window on a sequential request, else the base window
    // (grow_window 0). A request inside the previous window (pages it left protected) resets too.
    if (rb_grow_env && rb_narrow_window != 0 && (forward || backward) && gap <= rb_grow_gap_ns) {
        const u64 grown = std::min(std::max(prev.window, rb_narrow_window) * 2, rb_grow_max);
        if (grown > rb_narrow_window) {
            readback.grow_window = grown;
            readback.grow_backward = backward;
        }
    }
    if (!SotcStats::kFpsLog) {
        return;
    }
    SotcStats::rb_pat_count.fetch_add(1, std::memory_order_relaxed);
    if (forward || backward) {
        SotcStats::rb_pat_adjacent.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rb_pat_adjacent_all.fetch_add(1, std::memory_order_relaxed);
        if (gap <= RbAdjacentNs) {
            SotcStats::rb_pat_adjacent_1ms.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (have_prev && addr >= prev.prev_start && addr < prev.prev_end) {
        SotcStats::rb_pat_inside.fetch_add(1, std::memory_order_relaxed);
    }
    if (readback.grow_window != 0) {
        SotcStats::rb_grow_grown.fetch_add(1, std::memory_order_relaxed);
    }
    // Keyed by the base granule (not the window, which SOTC_RB_GROW changes), so the figures
    // compare between runs with and without it.
    const VAddr granule = Common::AlignDown(addr, rb_narrow_window ? rb_narrow_window : 512_KB);
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    bool repeat = false;
    {
        std::scoped_lock lock{rb_pattern.mutex};
        if (frame != rb_pattern.frame) {
            if (frame == rb_pattern.frame + 1) {
                rb_pattern.cur ^= 1; // the current frame becomes the previous one
            } else {
                rb_pattern.frames[rb_pattern.cur ^ 1].Clear();
            }
            rb_pattern.frames[rb_pattern.cur].Clear();
            rb_pattern.frame = frame;
        }
        repeat = rb_pattern.frames[rb_pattern.cur ^ 1].Get(granule, false) != nullptr;
        rb_pattern.frames[rb_pattern.cur].Get(granule, true);
        if (u32* count = rb_pattern.period.Get(granule, true)) {
            ++*count;
        } else {
            ++rb_pattern.untracked;
        }
    }
    if (repeat) {
        SotcStats::rb_pat_repeat.fetch_add(1, std::memory_order_relaxed);
    }
}

void BufferCache::NoteGuestReadbackDone(const AsyncReadback& readback) {
    RbThreadState& state = tl_rb;
    if (readback.window_size != 0) {
        state.prev_start = readback.window_start;
        state.prev_end = readback.window_start + readback.window_size;
    }
    state.prev_done_ns = SotcStats::NowNs();
    state.window = readback.grow_window;
    if (SotcStats::kFpsLog) {
        if (readback.waiting) {
            SotcStats::rb_pat_waited.fetch_add(1, std::memory_order_relaxed);
        }
        SotcStats::rb_pat_asked_bytes.fetch_add(readback.size, std::memory_order_relaxed);
        SotcStats::rb_pat_window_bytes.fetch_add(readback.window_size, std::memory_order_relaxed);
        SotcStats::rb_pat_download_bytes.fetch_add(readback.gathered_bytes,
                                                   std::memory_order_relaxed);
    }
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    if (hrr) {
        hrr->NoteReadback(device_addr, size); // sotc: SOTC_HRR dry run
    }
    if (SotcStats::kFpsLog) {
        SotcWriteExtent::NoteReadback(device_addr); // sotc: "SOTC GPU writes by shader" counter
    }
    const auto flush_request = [this, device_addr, size, is_write] {
        NoteHotRead(device_addr, size);
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        const auto [window_start, window_end] = ReadbackWindow(arena, device_addr, size);
        Core::MemoryManager::NoteHostWrite(device_addr, size, 6, window_start);
        fault_addr = device_addr;
        fault_size = size;
        DownloadMemory(arena, window_start, window_end - window_start);
        fault_size = 0;
        if (is_write) {
            MarkWrittenByGuest(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
        return;
    }
    // sotc: on by default (SOTC_RB_ASYNC=0 turns it off). Radeon RX 6650 XT, sanctuary, three
    // runs each: 25.3-25.8 fps instead of 23.2-23.7, frames over 40 ms 376-424 instead of
    // 566-594, readback waits 100 ms/s instead of 290 ms/s.
    static const bool async = [] {
        const char* v = std::getenv("SOTC_RB_ASYNC");
        return !v || v[0] != '0';
    }();
    if (!async) {
        liverpool->SendCommand<true>(std::move(flush_request));
        return;
    }
    // The guest thread waits for the GPU, not the command processor.
    AsyncReadback readback{.device_addr = device_addr, .size = size, .is_write = is_write};
    // sotc: SOTC_FPS: the guest's wall-clock time from the Begin request to the Finish return.
    // sotc: SOTC_FPS pattern counters and SOTC_RB_GROW (see rb_grow_env): this thread's previous
    // readback decides the window; NoteGuestReadbackDone remembers this one on every return.
    const bool track = SotcStats::kFpsLog || rb_grow_env;
    const u64 t_begin = track ? SotcStats::NowNs() : 0;
    if (track) {
        PrepareGuestReadback(readback, t_begin);
    }
    liverpool->SendCommand<true>([this, &readback] { BeginAsyncReadback(readback); });
    if (!readback.waiting) {
        if (track) {
            NoteGuestReadbackDone(readback);
        }
        return;
    }
    const WaitObserver wait_observer = rbx_wait_observer.load(std::memory_order_acquire);
    const u64 t_wait0 = wait_observer ? SotcStats::NowNs() : 0;
    const u64 t_submit = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
    // sotc: SOTC_RB_ASYNC_AHEAD: tick is 0 when only ahead copies are waited for (see
    // BeginAsyncReadback); without the switch wait_ahead is 0 and tick never is.
    if (readback.tick != 0) {
        scheduler.WaitSubmitted(readback.tick, false);
    }
    if (readback.wait_ahead != 0) {
        scheduler.WaitAheadSubmitted(readback.wait_ahead);
    }
    const u64 t_gpu = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
    if (readback.tick != 0) {
        scheduler.GetWorkSemaphore()->Wait(readback.tick);
    }
    if (readback.wait_ahead != 0) {
        scheduler.WaitAhead(readback.wait_ahead);
    }
    // sotc: SOTC_RB_TICK: the copy on the transfer queue (see BeginAsyncReadback). Polls the
    // timeline counter for SOTC_RB_SPIN_US (default 150) before blocking: the copy is short once
    // the writer tick is done, and waking from a blocking wait costs about as much.
    if (readback.wait_xfer != 0) {
        static const u32 rb_spin_us = [] {
            const char* v = std::getenv("SOTC_RB_SPIN_US");
            return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 150u;
        }();
        scheduler.XferWaitValue(readback.wait_xfer, rb_spin_us);
    }
    if (wait_observer && readback.wait_seq != 0) {
        wait_observer(readback.wait_seq, SotcStats::NowNs() - t_wait0);
    }
    const u64 t_finish = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
    liverpool->SendCommand<true>([this, &readback] {
        FinishAsyncReadback(readback);
        // sotc: SOTC_RB_GROUP: the group download this fault recorded went in the tick the guest
        // just waited for (or an earlier one): apply it in the same draw-thread step.
        if (readback.group_tick != 0) {
            RbgApplyReady(false);
        }
    });
    if (track) {
        NoteGuestReadbackDone(readback);
    }
    if (SotcStats::kFpsLog) {
        const u64 total = SotcStats::NowNs() - t_begin;
        if (hrr) {
            hrr->NoteGuestTime(device_addr, size, total); // sotc: SOTC_HRR dry run
        }
        SotcStats::rb_guest_count.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rb_guest_ns.fetch_add(total, std::memory_order_relaxed);
        SotcStats::rb_guest_submit_ns.fetch_add(t_gpu - t_submit, std::memory_order_relaxed);
        SotcStats::rb_guest_gpu_ns.fetch_add(t_finish - t_gpu, std::memory_order_relaxed);
        SotcStats::rb_guest_all_count.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rb_guest_all_ns.fetch_add(total, std::memory_order_relaxed);
        u64 max_ns = SotcStats::rb_guest_max_ns.load(std::memory_order_relaxed);
        while (max_ns < total && !SotcStats::rb_guest_max_ns.compare_exchange_weak(
                                     max_ns, total, std::memory_order_relaxed)) {
        }
        if (readback.ahead_value != 0) {
            SotcStats::rb_ahead_async.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rb_ahead_async_ns.fetch_add(total, std::memory_order_relaxed);
        }
        if (readback.xfer_value != 0) {
            SotcStats::rb_tick_waits.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rb_tick_wait_ns.fetch_add(t_finish - t_gpu, std::memory_order_relaxed);
        }
    }
}

bool BufferCache::IsReadbackComplete(const AsyncReadback& readback) {
    if (readback.xfer_value != 0) { // sotc: SOTC_RB_TICK, by the transfer timeline value
        return scheduler.XferDone(readback.xfer_value);
    }
    return readback.ahead_value != 0 ? scheduler.IsAheadDone(readback.ahead_value)
                                     : scheduler.IsFree(readback.tick);
}

void BufferCache::MarkWrittenByGuest(VAddr addr, u64 size) {
    // sotc: a page both CPU- and GPU-modified loses its write watcher but keeps its read
    // watcher: "write-only" protection, an assertion that ended the game (opt4, ~1 in 12 Shrine
    // runs). After a write fault's readback the requested pages can still be GPU-modified: GPU
    // data recorded since (left protected: the write faults again and the next readback brings
    // it) or a page bit no GPU-written byte backs any more (cleared, or the write would fault
    // forever).
    if (memory_tracker->IsRegionGpuModified(addr, size)) {
        const VAddr lo = Common::AlignDown(addr, VAddr{4096});
        const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
        if (gpu_modified_ranges.Intersects(lo, hi - lo)) {
            SotcStats::write_fault_gpu_pending.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        SotcStats::write_fault_stale_gpu.fetch_add(1, std::memory_order_relaxed);
        memory_tracker->UnmarkRegionAsGpuModified(lo, hi - lo, false);
    }
    memory_tracker->MarkRegionAsCpuModified(addr, size);
}

void BufferCache::SetWaitObserver(WaitObserver fn) noexcept {
    rbx_wait_observer.store(fn, std::memory_order_release);
}

u64 BufferCache::RbxWaitSeq(const AsyncReadback& readback) {
    // Newest hot seq among the writers (pending ticks) of the asked bytes and of the bytes the
    // readback copies: what the guest waits for.
    u64 free_max = 0;
    u64 pend_min = ~u64{0};
    u64 seq = 0;
    const auto scan = [&](VAddr lo, VAddr hi) {
        auto it = rbx_writers.upper_bound(lo);
        if (it != rbx_writers.begin() && std::prev(it)->second.hi > lo) {
            --it;
        }
        for (; it != rbx_writers.end() && it->first < hi; ++it) {
            const RbxWriter& w = it->second;
            if (w.seq <= seq) {
                continue;
            }
            bool done = w.tick <= free_max;
            if (!done && w.tick < pend_min) {
                done = scheduler.IsFree(w.tick);
                (done ? free_max : pend_min) = w.tick;
            }
            if (!done) {
                seq = w.seq;
            }
        }
    };
    scan(readback.device_addr, readback.device_addr + readback.size);
    for (const auto& item : readback.pending) {
        for (const auto& copy : item.copies) {
            const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
            scan(lo, lo + copy.size);
        }
    }
    return seq;
}

void BufferCache::BeginAsyncReadback(AsyncReadback& readback) {
    BeginAsyncReadbackImpl(readback);
    // sotc: SOTC_RB_GROUP: a group download recorded at this fault is sent now when the readback
    // itself did not submit the tick (served without a wait, transfer-queue or ahead copies,
    // waiting for owners), so it completes as soon as its writers do.
    if (readback.group_tick != 0 && readback.group_tick >= scheduler.CurrentTick() &&
        !ReadbackBatchScope::Defer(scheduler)) {
        scheduler.Flush();
    }
    // sotc: the wait observer (liverpool.cpp's adaptive label policy) learns which hot data the
    // guest waits for; computed here on the draw thread, from the writer map.
    if (readback.waiting && RbxOn() && rbx_wait_observer.load(std::memory_order_acquire)) {
        readback.wait_seq = RbxWaitSeq(readback);
    }
}

void BufferCache::BeginAsyncReadbackImpl(AsyncReadback& readback) {
    RbxDepth rbx_depth_guard{rbx_depth}; // sotc: SOTC_RB_EXACT: no label op applies in here
    SotcStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    NoteHotRead(readback.device_addr, readback.size);
    // sotc: SOTC_RB_PREFETCH: learn the recurring windows, and let the prefetches of the asked
    // granules land first (the gather below then finds their pages no longer GPU-modified).
    if (RbxOn()) {
        // sotc: SOTC_RB_EXACT: owed downloads of the asked range that are done land right here.
        if (!rbx_live.empty()) {
            RbxFinishOverlapping(readback.device_addr, readback.size, !readback.is_write);
        }
    }
    if (rb_prefetch_env) {
        PrefetchNoteRead(readback.device_addr);
        if (!pf_live.empty()) {
            PrefetchAtFault(readback.device_addr, readback.size);
        }
        // sotc: SOTC_RB_PREV: the last applied version instead of a wait (see TryServePrevious).
        if (rb_prev_env && TryServePrevious(readback)) {
            readback.waiting = false;
            return;
        }
    }
    const u32 first_block = readback.device_addr >> block_shift;
    const u32 last_block = (readback.device_addr + readback.size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    // sotc: SOTC_RB_GROW: the window the guest thread chose (PrepareGuestReadback), if grown.
    auto [window_start, window_end] =
        readback.grow_window != 0 ? GrowReadbackWindow(arena, readback)
                                  : ReadbackWindow(arena, readback.device_addr, readback.size);
    // sotc: SOTC_WRITE_FAULT_NARROW: a write fault needs the bytes of the pages the guest is about
    // to write and nothing else: the window shrinks to them (every GPU-modified byte of those
    // pages is still copied, so the page holds the GPU's final bytes before the write lands; the
    // rest of the window keeps its protection and is read back by its own faults). Fewer owners,
    // smaller gather and copy, and with a writer tick known (the write ring) the transfer-queue
    // copy of the RB_TICK path waits only for that writer.
    if (rb_wf_narrow_env && readback.is_write) {
        const VAddr narrow_lo = std::max<VAddr>(Common::AlignDown(readback.device_addr, 4096ULL),
                                                arena->cpu_addr);
        const VAddr narrow_hi = std::min<VAddr>(
            Common::AlignUp(readback.device_addr + readback.size, 4096ULL),
            arena->cpu_addr + arena->size_bytes);
        if (narrow_lo < narrow_hi && narrow_lo >= window_start && narrow_hi <= window_end) {
            SotcStats::rb_wf_narrow.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rb_wf_saved.fetch_add((window_end - window_start) - (narrow_hi - narrow_lo),
                                             std::memory_order_relaxed);
            window_start = narrow_lo;
            window_end = narrow_hi;
        }
    }
    readback.arena = arena;
    readback.window_start = window_start;
    readback.window_size = window_end - window_start;
    // sotc: SOTC_RB_GROUP: the other pages the bind that wrote the asked bytes marked, copied in
    // this tick before the readback's own copies (see RbgAtFault). Outside the window, so the
    // readback below never meets them.
    if (rb_group_env && !readback.is_write) {
        readback.group_tick = RbgAtFault(readback, window_start, window_end);
    }
    // sotc: SOTC_RB_OWNER=1 (off by default). Several guest threads read the same GPU-written
    // memory at once, and each readback copied it: the first one applied made the others stale,
    // and those were read back again synchronously (the draw thread waiting for the GPU). Now
    // the pages a readback in flight is downloading belong to it: a new one leaves them out and,
    // for the part of its asked range they cover, waits for them (their ticks are older) and
    // finishes them with itself.
    static const bool rb_owner = [] {
        const char* v = std::getenv("SOTC_RB_OWNER");
        return v && v[0] == '1';
    }();
    // sotc: SOTC_RB_EXACT: rules 1-4 (see TryServeExact). Served: nothing to wait for. Download:
    // readback.pending holds only done ranges. Fallback: the usual readback below.
    u64 exact_total = 0;
    u64 exact_writer = 0;
    bool exact_used = false;
    if (RbxOn()) {
        switch (TryServeExact(readback, arena, window_start, window_end, exact_total,
                              exact_writer)) {
        case ExactResult::Served:
            readback.waiting = false;
            return;
        case ExactResult::Download:
            exact_used = true;
            break;
        case ExactResult::Fallback:
            break;
        }
    }
    thread_local std::vector<std::pair<VAddr, VAddr>> owned;
    u64 owner_tick = 0;
    u64 owner_ahead = 0; // sotc: SOTC_RB_ASYNC_AHEAD: owners whose copies went ahead
    u64 owner_xfer = 0;  // sotc: SOTC_RB_TICK: owners whose copies went on the transfer queue
    if (rb_owner && !inflight_readbacks.empty() && !exact_used) {
        owned.clear();
        const VAddr ask_end = readback.device_addr + readback.size;
        for (const AsyncReadback* other : inflight_readbacks) {
            for (const auto& item : other->pending) {
                for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                    owned.emplace_back(range_addr, range_addr + range_size);
                    if (range_addr < ask_end && readback.device_addr < range_addr + range_size) {
                        owner_tick = std::max(owner_tick, other->tick);
                        owner_ahead = std::max(owner_ahead, other->ahead_value);
                        owner_xfer = std::max(owner_xfer, other->xfer_value);
                    }
                }
            }
        }
        std::ranges::sort(owned);
        size_t merged = 0;
        for (size_t i = 0; i < owned.size(); ++i) {
            if (merged && owned[i].first <= owned[merged - 1].second) {
                owned[merged - 1].second = std::max(owned[merged - 1].second, owned[i].second);
            } else {
                owned[merged++] = owned[i];
            }
        }
        owned.resize(merged);
        rb_owned = &owned;
    }
    const u64 total =
        exact_used ? exact_total
                   : GatherDownloads(arena, window_start, readback.window_size, readback.pending,
                                     false);
    rb_owned = nullptr;
    // sotc: SOTC_RB_EXACT: asked bytes owed to guest memory by an earlier release: the guest waits
    // for that download and applies it (usual path; the exact path never gets here for them).
    if (RbxOn() && !exact_used && rbx_owed_count.load(std::memory_order_relaxed) != 0) {
        bool unsent = false;
        const u64 owed_tick = RbxOwedTick(readback.device_addr, readback.size, unsent);
        if (owed_tick != 0) {
            owner_tick = std::max(owner_tick, owed_tick);
            if (unsent && total == 0 && !ReadbackBatchScope::Defer(scheduler)) {
                scheduler.Flush();
            }
        }
    }
    if (SotcStats::kFpsLog) {
        readback.gathered_bytes = total;
        if (readback.grow_window != 0 && total != 0) {
            // sotc: SOTC_RB_GROW: the other base granules of the grown window with GPU-modified
            // data copied (each a fault of its own with the base window, if the guest reads it).
            // The copies come in address order, so each granule is counted once.
            const u64 base = rb_narrow_window;
            const VAddr own = Common::AlignDown(readback.device_addr, base);
            VAddr last = own;
            u64 granules = 0;
            u64 bytes = 0;
            for (const auto& item : readback.pending) {
                for (const auto& copy : item.copies) {
                    const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
                    const VAddr hi = lo + copy.size;
                    for (VAddr g = Common::AlignDown(lo, base); g < hi; g += base) {
                        if (g == own) {
                            continue;
                        }
                        bytes += std::min<VAddr>(hi, g + base) - std::max<VAddr>(lo, g);
                        if (g != last) {
                            ++granules;
                            last = g;
                        }
                    }
                }
            }
            SotcStats::rb_grow_extra_granules.fetch_add(granules, std::memory_order_relaxed);
            SotcStats::rb_grow_extra_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    if (owner_tick != 0 || owner_ahead != 0 || owner_xfer != 0) {
        SotcStats::rb_owner_shared.fetch_add(1, std::memory_order_relaxed);
        if (total == 0) {
            // Everything it asked for is being downloaded already.
            readback.tick = owner_tick;
            readback.wait_ahead = owner_ahead;
            readback.wait_xfer = owner_xfer;
            readback.waiting = true;
            readback.dependent = true;
            return;
        }
        readback.has_owners = true;
    }
    if (total == 0) {
        // Its bytes may belong to a readback still in flight (taken out of the GPU-modified set,
        // pages still protected): wait for that one and finish it if its thread has not yet.
        if (const AsyncReadback* other = FindInflightReadback(readback.device_addr, readback.size)) {
            readback.tick = other->tick;
            readback.wait_ahead = other->ahead_value; // sotc: SOTC_RB_ASYNC_AHEAD
            readback.wait_xfer = other->xfer_value;   // sotc: SOTC_RB_TICK
            readback.waiting = true;
            readback.dependent = true;
            return;
        }
        if (readback.is_write) {
            MarkWrittenByGuest(readback.device_addr, readback.size);
        }
        return;
    }
    // Held until FinishAsyncReadback: the pool must not hand it out again before.
    const auto download =
        staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
    // sotc: SOTC_RB_ASYNC_AHEAD=1 (off by default). Could the copies go ahead of the command
    // buffer being recorded: do they read only what submitted ticks wrote (nothing in
    // current_writes, the same test as DownloadMemory's readback ahead), with no sparse binds
    // pending? Counted with SOTC_FPS even when the switch is off.
    static const bool async_ahead = [] {
        const char* v = std::getenv("SOTC_RB_ASYNC_AHEAD");
        return v && v[0] == '1';
    }();
    // sotc: SOTC_RB_TICK=1 (off by default). Who wrote the bytes, and when? write_ring knows the
    // newest tick that wrote each range (RecordCurrentWrite). Counted with SOTC_FPS even when the
    // switch is off ("SOTC RB writer" line; no change of behaviour).
    // With the switch, when that writer tick is already flushed (not the tick being recorded),
    // the data is complete in the arena once the writer tick ran on the GPU, and nothing newer
    // wrote it: the copy goes to the transfer queue, waiting on the graphics timeline for the
    // writer tick only, so the guest does not wait behind the graphics work submitted since.
    // Anything else (no transfer queue, writer unknown or still being recorded, sparse binds
    // pending, no free copy slot) takes today's path. The ordering argument is below, where the
    // readback is filed.
    static const bool rb_tick_env = [] {
        const char* v = std::getenv("SOTC_RB_TICK");
        return v && v[0] == '1';
    }();
    u64 xfer_writer = 0; // graphics tick the transfer copy waits for (0: not on the transfer queue)
    const bool wf_tick = rb_wf_narrow_env && readback.is_write;
    if (rb_tick_env || exact_used || wf_tick || SotcStats::kFpsLog || hrr) {
        bool unknown = false;
        const u64 writer =
            exact_used ? exact_writer : ReadbackWriterTick(readback.pending, unknown);
        const u64 cur = scheduler.CurrentTick();
        if (hrr) { // sotc: SOTC_HRR dry run: would the fault have waited for its writer?
            hrr->NoteFault(readback.device_addr, readback.size,
                           !unknown && (writer == 0 || (writer < cur && scheduler.IsFree(writer))));
        }
        if (SotcStats::kFpsLog) {
            if (writer >= cur) {
                SotcStats::rb_writer_unsub.fetch_add(1, std::memory_order_relaxed);
            } else if (scheduler.IsFree(writer)) {
                SotcStats::rb_writer_done.fetch_add(1, std::memory_order_relaxed);
            } else {
                SotcStats::rb_writer_sub.fetch_add(1, std::memory_order_relaxed);
            }
            SotcStats::rb_writer_age_sum.fetch_add(cur - std::min(writer, cur),
                                                   std::memory_order_relaxed);
            SotcStats::rb_writer_count.fetch_add(1, std::memory_order_relaxed);
        }
        if (rb_tick_env || exact_used || wf_tick) {
            if (!instance.IsRbTickEnabled()) {
                SotcStats::rb_tick_fb_noxfer.fetch_add(1, std::memory_order_relaxed);
            } else if (unknown) {
                SotcStats::rb_tick_fb_unknown.fetch_add(1, std::memory_order_relaxed);
            } else if (!pending_binds.empty()) {
                SotcStats::rb_tick_fb_binds.fetch_add(1, std::memory_order_relaxed);
            } else if (writer < arena_migrate_tick) {
                // sotc: the arena was replaced after the writer tick: the copy would read through
                // sparse binds the writer's submit did not wait for.
                SotcStats::rb_tick_fb_binds.fetch_add(1, std::memory_order_relaxed);
            } else if (writer < cur && writer != 0) {
                xfer_writer = writer;
            }
            if (xfer_writer == 0) {
                SotcStats::rb_tick_fb.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    vk::CommandBuffer ahead_cmdbuf{};
    if (async_ahead || SotcStats::kFpsLog) {
        RollCurrentWrites();
        bool written_by_current = false;
        for (const auto& item : readback.pending) {
            for (const auto& copy : item.copies) {
                written_by_current |=
                    current_writes.Intersects(item.arena->cpu_addr + copy.srcOffset, copy.size);
            }
        }
        const bool eligible = !written_by_current && pending_binds.empty();
        // sotc: SOTC_RB_GROW: does a grown window hit the tick being recorded more often?
        if (SotcStats::kFpsLog && written_by_current) {
            (readback.grow_window != 0 ? SotcStats::rb_grow_cur_writes
                                       : SotcStats::rb_base_cur_writes)
                .fetch_add(1, std::memory_order_relaxed);
        }
        if (eligible) {
            SotcStats::rb_ahead_eligible.fetch_add(1, std::memory_order_relaxed);
        }
        // sotc: not with transfer-queue copies (SOTC_XFER_QUEUE): one of them later in this tick
        // only waits for graphics tick CurrentTick()-1 and could overwrite the bytes before the
        // ahead copy reads them (review of 2026-10-28).
        if (eligible && async_ahead && !instance.IsXferRoutingEnabled() && xfer_writer == 0) {
            ahead_cmdbuf = scheduler.BeginAheadAsync();
            if (!ahead_cmdbuf) {
                SotcStats::rb_ahead_async_busy.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    for (auto& item : readback.pending) {
        for (auto& copy : item.copies) {
            copy.dstOffset += download.offset;
        }
        if (!ahead_cmdbuf && xfer_writer == 0) {
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        }
    }
    u64 xfer_value = 0;
    if (xfer_writer != 0) {
        boost::container::small_vector<Vulkan::XferQueue::ReadbackRegion, 16> regions;
        for (const auto& item : readback.pending) {
            regions.push_back({item.arena->Handle(), download.buffer->Handle(), item.copies});
        }
        xfer_value = scheduler.XferReadbackCopy(xfer_writer, regions);
        if (xfer_value != 0) {
            NoteXferRead(readback.pending, xfer_value);
            SotcStats::rb_tick_xfer.fetch_add(1, std::memory_order_relaxed);
        } else {
            // No free slot: the usual path.
            for (const auto& item : readback.pending) {
                runtime.CopyBuffer(item.arena, download.buffer, item.copies);
            }
        }
    }
    readback.download_buffer = download.buffer;
    readback.download_size = download.size;
    readback.download_offset = download.offset;
    readback.download_mapped = download.mapped;
    if (xfer_value != 0) {
        // sotc: SOTC_RB_TICK, copies on the transfer queue. Why the data is complete and stays so:
        // - Write->read: the writer tick (newest tick that wrote the bytes, from write_ring) was
        //   flushed before this point, and no tick after it wrote them (a write of the tick being
        //   recorded would have made that tick the writer). The copy waits on the graphics
        //   timeline for exactly that value; the semaphore dependency makes the writes of that
        //   submit and every earlier one available and visible to it. Uploads on the transfer
        //   queue (SOTC_XFER_QUEUE) at or before that tick finished before it (it waited for them).
        // - Read->later write: every graphics write recorded over these bytes after this point
        //   (RecordCurrentWrite -> NoteWriteAgainstXferReads) makes the submit of its tick wait
        //   on the copy's transfer value (TakePendingTick), so it runs after the copy read them.
        //   Without such a write nothing waits for the copy: graphics never needed it. An upload
        //   routed to the transfer queue waits for the newest readback copy itself, and sparse
        //   unbinds (SubmitPendingArenaBinds) and Scheduler::Finish wait for it too.
        // - Host read: the copy ends with transfer write -> host read; the guest waits on the
        //   transfer timeline (spin, then block) and FinishAsyncReadback invalidates.
        // - Lifetime: the staging stays held until FinishAsyncReadback. The arena memory is not
        //   freed under the copy: an unbind waits for it, and arena memory is reclaimed only after
        //   an unbind (ReclaimArenaRuns). Sparse binds pending now send this readback to the usual
        //   path, so no bind of this arena sits between the writer tick and this copy.
        // - Recording thread: the writer tick's submit was put on the queue first
        //   (XferQueue::ReadbackCopy -> WaitSubmitted), so the semaphore wait cannot dangle.
        readback.xfer_value = xfer_value;
        readback.wait_xfer = std::max(owner_xfer, xfer_value);
        readback.tick = owner_tick;
        readback.wait_ahead = owner_ahead;
    } else if (ahead_cmdbuf) {
        // sotc: SOTC_RB_ASYNC_AHEAD. The copies go in an ahead command buffer of their own,
        // submitted without flushing the tick being recorded and without waiting for its async
        // uploads; the guest waits for that command buffer only, not for everything up to this
        // tick. Why the data is complete and stays so:
        // - Write->read: the bytes were written by submitted ticks only (not in current_writes).
        //   The ahead command buffer lands on the graphics queue after the submits of those ticks
        //   (SubmitAheadAsync), and the first barrier below (all commands, memory writes ->
        //   transfer reads) covers every command earlier in submission order on the queue, other
        //   command buffers included. Transfer-queue uploads are waited for by SubmitAheadAsync.
        // - Read->later write: the tick being recorded may write these bytes after this point
        //   (MarkInflightReadbacksDirty / later_gpu, unchanged, decide what that means). Its
        //   submit comes after this one, and the last barrier (transfer -> all commands) makes
        //   every later command wait for the copies, so they read what the memory held when the
        //   guest asked, as the copies recorded in the tick itself would.
        // - Host read: transfer writes -> host reads, then the timeline signal; the guest waits on
        //   the host for that value (Scheduler::WaitAhead), FinishAsyncReadback invalidates.
        // - Lifetime: the staging stays held until FinishAsyncReadback; FreeDeferred and the
        //   deferred destruction of arenas key on the tick being recorded or a later one, whose
        //   signal comes after this submit on the same queue, so neither is reused before the
        //   copies ran. The command buffer slot is reused only once its timeline value passed.
        // - Recording thread: the command buffer is real (not a fake handle), recorded here on
        //   the producer; only the submit goes through the recorder's stream, in order.
        const vk::MemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        ahead_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &pre_barrier,
        });
        for (const auto& item : readback.pending) {
            ahead_cmdbuf.copyBuffer(item.arena->Handle(), download.buffer->Handle(), item.copies);
        }
        const std::array<vk::MemoryBarrier2, 2> post_barriers = {{
            {
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            },
            {
                // Execution only (write after read): later submits wait for the copies.
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
        }};
        ahead_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = static_cast<u32>(post_barriers.size()),
            .pMemoryBarriers = post_barriers.data(),
        });
        readback.ahead_value = scheduler.SubmitAheadAsync();
        // Its owners (SOTC_RB_OWNER) are waited for too: an ahead owner by its value, one on the
        // usual path by its tick (the usual readback's own tick covered them, being newer).
        readback.wait_ahead = std::max(owner_ahead, readback.ahead_value);
        readback.tick = owner_tick;
        readback.wait_xfer = owner_xfer;
    } else {
        readback.tick = scheduler.CurrentTick();
        readback.wait_ahead = owner_ahead; // sotc: ahead owners (0 without SOTC_RB_ASYNC_AHEAD)
        readback.wait_xfer = owner_xfer;   // sotc: SOTC_RB_TICK
        if (!ReadbackBatchScope::Defer(scheduler)) {
            scheduler.Flush();
        }
        SotcStats::readback_finish.fetch_add(1, std::memory_order_relaxed);
    }
    readback.waiting = true;
    inflight_readbacks.push_back(&readback);
    ++inflight_version;
}

// sotc: SOTC_RB_KEEP_LATER=1 (off by default). A GPU write recorded after a readback's copies
// does not make the read data wrong: it is what the memory held when the guest asked, and the write
// belongs to commands parsed after that. Such a readback is applied as usual, and the parts the
// write covers stay GPU-modified (protected), so the next read of them fetches the new data. Before,
// it was read back again synchronously, the draw thread waiting for the GPU (~140 a second while
// the camera turns in a dense area). The other reasons (an unmap, another readback applied, a
// write by the command processor itself) still redo it.
static bool RbKeepLater() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_RB_KEEP_LATER");
        return v && v[0] == '1';
    }();
    return enabled;
}

void BufferCache::MarkInflightReadbacksDirty(VAddr addr, u64 size, StaleReason reason,
                                             bool keep_owed) {
    // sotc: SOTC_RB_EXACT: a write, unmap or guest write takes its bytes out of the owed
    // downloads (an applied readback or a page release does not touch owed bytes).
    if (!keep_owed && reason != StaleReason::Applied &&
        rbx_owed_count.load(std::memory_order_relaxed) != 0) {
        RbxTrim(addr, addr + size);
    }
    // sotc: the command processor (a GPU write, its own write or an applied readback after
    // which the guest may write) changed these bytes: asynchronous readbacks of them in flight
    // hold stale data (FinishAsyncReadback reads them back again).
    const VAddr end = addr + size;
    // sotc: SOTC_RB_OWNER: a readback holds only the pages it downloads (gpu_ranges); its window
    // may cover pages another readback in flight owns, whose changes are not its concern.
    static const bool rb_owner = [] {
        const char* v = std::getenv("SOTC_RB_OWNER");
        return v && v[0] == '1';
    }();
    for (AsyncReadback* readback : inflight_readbacks) {
        // sotc: SOTC_RB_PREFETCH: whatever the reason (a later GPU write too, SOTC_RB_KEEP_LATER
        // or not: no guest asked before it), the prefetch no longer holds what a readback would
        // copy now. Hull test first: there can be many of them.
        if (readback->prefetch) {
            if (readback->pf_group) {
                RbgTrim(*readback, addr, end); // sotc: SOTC_RB_GROUP: only the pages hit
                continue;
            }
            if (!readback->dirty && addr < readback->pf_hi && readback->pf_lo < end) {
                for (const auto& item : readback->pending) {
                    for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                        readback->dirty |= range_addr < end && addr < range_addr + range_size;
                    }
                }
            }
            continue;
        }
        bool hit = false;
        const auto mark = [&](VAddr lo, VAddr hi) {
            if (!(addr < hi && lo < end)) {
                return;
            }
            hit = true;
            if (reason == StaleReason::GpuWrite && RbKeepLater()) {
                readback->later_gpu.emplace_back(std::max(addr, lo), std::min(end, hi));
            } else {
                readback->dirty = true;
            }
        };
        for (const auto& item : readback->pending) {
            if (rb_owner) {
                for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                    mark(range_addr, range_addr + range_size);
                }
            } else {
                mark(item.addr, item.addr + item.size);
            }
        }
        if (hit) {
            SotcStats::readback_stale[static_cast<u32>(reason)].fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

void BufferCache::FinishReadyOwners(VAddr addr, u64 size) {
    // Only owners whose GPU work is done: one started later (higher tick) may not be, and the
    // asking guest waited only for the ticks it saw. Their pages stay GPU-modified meanwhile, so
    // a read of them faults again and finds them finished.
    for (bool again = true; again;) {
        again = false;
        for (AsyncReadback* other : inflight_readbacks) {
            bool overlaps = false;
            for (const auto& item : other->pending) {
                for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                    overlaps |= range_addr < addr + size && addr < range_addr + range_size;
                }
            }
            if (overlaps && IsReadbackComplete(*other)) { // sotc: ahead copies by their value
                FinishAsyncReadback(*other); // erases it from inflight_readbacks
                again = true;
                break;
            }
        }
    }
}

BufferCache::AsyncReadback* BufferCache::FindInflightReadback(VAddr addr, u64 size) {
    for (AsyncReadback* readback : inflight_readbacks) {
        for (const auto& item : readback->pending) {
            for (const auto& copy : item.copies) {
                const VAddr start = item.arena->cpu_addr + copy.srcOffset;
                if (addr < start + copy.size && start < addr + size) {
                    return readback;
                }
            }
        }
    }
    return nullptr;
}

// sotc: SOTC_RB_PREFETCH=1 (off by default; needs SOTC_RB_OWNER=1 and the asynchronous readbacks,
// see rb_prefetch_env). Stall frames were dominated by guest threads waiting for readbacks (up to
// ~126 a frame, 0.4-1 ms of round trip each), mostly of the same granules every frame. A granule
// the guest read back in at least 2 of the 3 previous frames is "recurring" (PrefetchNoteRead,
// PrefetchRebuild); a GPU write into one arms it (PrefetchNoteWrite, from AddGpuModified); the
// next command boundary outside a render pass (PrefetchIssue, from Rasterizer::FlushPeriodic)
// records the copy of its GPU-modified ranges into staging in the tick being recorded and inserts
// it into inflight_readbacks as an owner of those pages (SOTC_RB_OWNER). Nothing else changes:
// no page protection, no tracker bit, no GPU-modified range, no guest byte. When a guest read
// fault later asks for that granule (PrefetchAtFault) and the tick is done, it is applied right
// there with no GPU wait and the guest readback finds nothing left to copy.
//
// Why applying it is exact. A guest readback at that moment would copy, after every command
// recorded so far, the GPU-modified ranges of the faulting window (the granule, or a grown window
// that contains it) and apply them through ApplyDownloads. The prefetch copied the same ranges
// of the granule after every command recorded up to its own point, a command boundary: every
// write a command makes is marked (ObtainBuffer is_written -> AddGpuModified, RecordCurrentWrite)
// while that command is prepared, before any later command, so all writes marked before the copy
// are recorded before it and run before it on the GPU (Runtime::CopyBuffer barriers against the
// recorded accesses; the tick's submit waits for that tick's transfer-queue copies). After the
// copy, any change to those bytes is one of:
// - an arena write recorded later (GPU-written binding, upload, image sync, write-through,
//   transfer-queue copy): AddGpuModified / RecordCurrentWrite -> dirty;
// - a GPU-modified range dropped without a readback: unmap (DropGpuModified, StaleReason::Unmap),
//   CPU replacement (TryReplaceFromCpu / TryWriteThroughFromCpu, CpuWrite), released memory
//   (ReleaseMemory, PrefetchMarkDirty) -> dirty;
// - another readback applying them (ApplyDownloads, StaleReason::Applied: a synchronous
//   DownloadMemory does not skip owned pages) -> dirty.
// A dirty prefetch is freed, never applied (SOTC_RB_KEEP_LATER does not apply to it: its "the
// guest asked before the write" argument needs a guest request, which a prefetch does not have).
// sotc: one a guest readback waits for (pf_claimed) is kept until that readback finishes it, and
// its window is then read back the usual way (FinishAsyncReadback), so the guest makes progress.
// So an applied prefetch holds exactly the bytes the guest readback would copy for those ranges.
// The pages it applies are the ones the asking readback's window would download anyway (its
// granule); the rest of that window (pages the GPU wrote after the prefetch, without touching its
// ranges) stays GPU-modified, and the guest readback gathers it right after. Guest memory is
// written only on a guest fault in that granule, through the same ApplyDownloads (BPE heap guard,
// stack ranges, tracker unmark of only its own gpu_ranges) and SubtractDownloaded as a guest
// readback, at fault time: the heap guard looks at the heap as it is then, not when the copy was
// recorded. Unconsumed ones are freed after SOTC_RB_PREFETCH_TICKS ticks; their pages never left
// the GPU-modified set, so the next fault reads them the usual way. Nothing speculative ever
// reaches guest memory (unlike SOTC_RB_HOT, which applied windows nobody asked for).
//
// Other switches: SOTC_RB_GROW windows skip the prefetches' pages (rb_owned) as for any owner,
// and a prefetch is applied only by a fault in its own granule. SOTC_RB_BATCH: a prefetch does
// not submit; a guest readback that must wait for an unsent prefetch tick submits it unless the
// batch scope does. SOTC_RB_ASYNC_AHEAD: prefetches always go in the tick being recorded (tick,
// never ahead_value); IsReadbackComplete covers both kinds of owner. A grown or ahead guest
// readback waiting for a prefetch owner (pf_claimed) waits for its tick, which keeps it alive.

void BufferCache::PrefetchNoteRead(VAddr addr) {
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    const VAddr key = Common::AlignDown(addr, PfGranule());
    auto it = pf_history.find(key);
    if (it == pf_history.end()) {
        if (pf_history.size() >= RbPfHistoryMax) {
            return; // ages out in PrefetchRebuild
        }
        it = pf_history.emplace(key, PfHistory{frame, 0}).first;
    }
    PfHistory& history = it->second;
    if (frame > history.frame) {
        const u64 shift = frame - history.frame;
        history.mask = shift >= 32 ? 0 : history.mask << shift;
        history.frame = frame;
    }
    history.mask |= 1;
}

void BufferCache::PrefetchRebuild(u64 frame) {
    pf_frame = frame;
    thread_local std::vector<VAddr> armed_keys;
    armed_keys.clear();
    for (const u32 index : pf_armed) {
        armed_keys.push_back(pf_recurring[index].key);
    }
    pf_armed.clear();
    pf_recurring.clear();
    for (auto it = pf_history.begin(); it != pf_history.end();) {
        const u64 shift = frame > it->second.frame ? frame - it->second.frame : 0;
        const u32 mask = shift >= 32 ? 0 : it->second.mask << shift;
        if ((mask & 0xff) == 0) {
            it = pf_history.erase(it); // not read in the last 8 frames
            continue;
        }
        // Frames frame-1 .. frame-3 (bits 1-3): at least two.
        if (std::popcount((mask >> 1) & 7u) >= 2) {
            pf_recurring.push_back({.key = it->first});
        }
        ++it;
    }
    std::ranges::sort(pf_recurring, {}, &PfWindow::key);
    // sotc: SOTC_RB_PREV: forget windows not written in the last 8 frames, and applied ranges
    // older than SOTC_RB_PREV_FRAMES.
    if (rb_prev_env) {
        const u64 cpf = SotcStats::cp_frame.load(std::memory_order_relaxed);
        for (auto it = pf_wmask.begin(); it != pf_wmask.end();) {
            const u64 shift = cpf > it->second.frame ? cpf - it->second.frame : 0;
            it = shift >= 8 ? pf_wmask.erase(it) : std::next(it);
        }
        std::scoped_lock lock{prev_mutex};
        for (auto it = pf_applied.begin(); it != pf_applied.end();) {
            auto& spans = it->second;
            spans.erase(std::remove_if(spans.begin(), spans.end(),
                                       [cpf](const PfFresh& f) {
                                           return f.frame + rb_prev_frames < cpf;
                                       }),
                        spans.end());
            it = spans.empty() ? pf_applied.erase(it) : std::next(it);
        }
    }
    // sotc: SOTC_RB_MATERIALIZE: only writes of this frame and the previous one count.
    for (auto it = pf_fresh.begin(); it != pf_fresh.end();) {
        auto& fresh = it->second;
        fresh.erase(std::remove_if(fresh.begin(), fresh.end(),
                                   [frame](const PfFresh& f) { return f.frame + 1 < frame; }),
                    fresh.end());
        it = it->second.empty() ? pf_fresh.erase(it) : std::next(it);
    }
    pf_rec_lo = pf_recurring.empty() ? 0 : pf_recurring.front().key;
    pf_rec_hi = pf_recurring.empty() ? 0 : pf_recurring.back().key + PfGranule();
    // Windows armed and not yet prefetched stay armed when they are still recurring.
    for (const VAddr key : armed_keys) {
        const auto it = std::ranges::lower_bound(pf_recurring, key, {}, &PfWindow::key);
        if (it != pf_recurring.end() && it->key == key && !it->armed) {
            it->armed = true;
            pf_armed.push_back(static_cast<u32>(it - pf_recurring.begin()));
        }
    }
}

void BufferCache::PrefetchNoteWrite(VAddr addr, u64 size) {
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (frame != pf_frame) {
        PrefetchRebuild(frame);
    }
    const VAddr end = addr + size;
    if (pf_recurring.empty() || end <= pf_rec_lo || addr >= pf_rec_hi) {
        return;
    }
    // Windows [key, key + granule) with key + granule > addr and key < end.
    const u64 granule = PfGranule();
    const VAddr from = addr >= granule ? addr - granule + 1 : 0;
    for (auto it = std::ranges::lower_bound(pf_recurring, from, {}, &PfWindow::key);
         it != pf_recurring.end() && it->key < end; ++it) {
        if (!it->armed) {
            it->armed = true;
            pf_armed.push_back(static_cast<u32>(it - pf_recurring.begin()));
        }
        if (rb_prev_env) {
            // sotc: SOTC_RB_PREV: the frames this window was written in.
            // Counted in command processor frames (cp_frame), the timeline the GPU writes follow:
            // game flips run ahead or behind it, and a window written once per frame then showed
            // gaps (first run: almost every serve refused as "not rewritten every frame").
            const u64 cpf = SotcStats::cp_frame.load(std::memory_order_relaxed);
            PfWriteMask& wm = pf_wmask[it->key];
            if (cpf > wm.frame) {
                const u64 shift = cpf - wm.frame;
                wm.mask = shift >= 32 ? 0 : wm.mask << shift;
                wm.frame = cpf;
            }
            wm.mask |= 1;
        }
        if (rb_materialize_env) {
            // sotc: SOTC_RB_MATERIALIZE: remember what the GPU wrote into the window and when.
            // Merged only with a range of the same frame, so an entry never looks newer than
            // its oldest byte; a dropped entry only makes a prefetch ineligible.
            const VAddr lo = std::max<VAddr>(addr, it->key);
            const VAddr hi = std::min<VAddr>(end, it->key + granule);
            auto& fresh = pf_fresh[it->key];
            bool merged = false;
            for (PfFresh& f : fresh) {
                if (f.frame == frame && lo <= f.hi && f.lo <= hi) {
                    f.lo = std::min(f.lo, lo);
                    f.hi = std::max(f.hi, hi);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (fresh.size() < RbMatFreshMax) {
                    fresh.push_back({lo, hi, frame, SotcStats::NowNs()});
                } else {
                    *std::ranges::min_element(fresh, {}, &PfFresh::frame) = {lo, hi, frame,
                                                                             SotcStats::NowNs()};
                }
            }
        }
    }
}

bool BufferCache::PrefetchFreshCovers(VAddr key, const AsyncReadback& p) const {
    const auto found = pf_fresh.find(key);
    if (found == pf_fresh.end()) {
        return false;
    }
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    const u64 now = SotcStats::NowNs();
    boost::container::small_vector<std::pair<VAddr, VAddr>, RbMatFreshMax> spans;
    for (const PfFresh& f : found->second) {
        if (f.frame + 1 >= frame && now - f.ns <= RbMatFreshMaxNs) {
            spans.emplace_back(f.lo, f.hi);
        }
    }
    std::ranges::sort(spans);
    size_t merged = 0;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (merged && spans[i].first <= spans[merged - 1].second) {
            spans[merged - 1].second = std::max(spans[merged - 1].second, spans[i].second);
        } else {
            spans[merged++] = spans[i];
        }
    }
    spans.resize(merged);
    if (spans.empty()) {
        return false;
    }
    // The bytes ApplyDownloads writes into guest memory are the copies (byte ranges of the
    // GPU-modified set). gpu_ranges are the tracker's page runs around them: the rest of those
    // pages is only unmarked, never written, so it is not checked (it was never fresh, and
    // checking it refused every prefetch on the first run).
    for (const auto& item : p.pending) {
        const VAddr arena_base = item.arena->cpu_addr;
        for (const auto& copy : item.copies) {
            const VAddr copy_addr = arena_base + copy.srcOffset;
            const auto it = std::ranges::upper_bound(spans, copy_addr, {},
                                                     &std::pair<VAddr, VAddr>::first);
            if (it == spans.begin() || std::prev(it)->second < copy_addr + copy.size) {
                return false;
            }
        }
    }
    return true;
}

void BufferCache::PrefetchMarkDirty(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    for (const auto& prefetch : pf_live) {
        AsyncReadback& p = *prefetch;
        if (p.finished || p.dirty || !(addr < p.pf_hi && p.pf_lo < end)) {
            continue;
        }
        if (p.pf_group) {
            RbgTrim(p, addr, end); // sotc: SOTC_RB_GROUP: only the pages hit
            continue;
        }
        for (const auto& item : p.pending) {
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                p.dirty |= range_addr < end && addr < range_addr + range_size;
            }
        }
    }
}

void BufferCache::PrefetchDrop(size_t index) {
    AsyncReadback& p = *pf_live[index];
    if (!p.finished) {
        // Not applied: its ranges stay in the GPU-modified set, its pages protected. The
        // staging is reused once the tick being recorded is done, after its copies.
        p.finished = true;
        if (p.pf_label) { // sotc: SOTC_RB_EXACT: dropped dirty, expired or unused
            SotcStats::rb_ex_ld_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        if (p.pf_group) { // sotc: SOTC_RB_GROUP: every page trimmed away, or expired
            (p.dirty ? SotcStats::rbg_fb_dirty : SotcStats::rbg_expired)
                .fetch_add(1, std::memory_order_relaxed);
        }
        std::erase(inflight_readbacks, &p);
        ++inflight_version;
        staging_pool.FreeDeferred(Vulkan::StagingBufferRef{
            .buffer = p.download_buffer,
            .offset = p.download_offset,
            .size = p.download_size,
            .mapped = const_cast<u8*>(p.download_mapped),
            .type = VideoCore::MemoryType::HostCached,
        });
    }
    pf_bytes_held -= p.pf_bytes;
    if (p.pf_label) {
        rbx_label_bytes -= p.rbx_bytes; // sotc: SOTC_RB_EXACT
    }
    if (p.pf_group) { // sotc: SOTC_RB_GROUP
        rbg_bytes_held -= p.group_bytes;
        --rbg_live;
    }
    pf_live.erase(pf_live.begin() + static_cast<std::ptrdiff_t>(index));
}

void BufferCache::PrefetchUpdateHull() {
    pf_lo = ~VAddr{0};
    pf_hi = 0;
    for (const auto& p : pf_live) {
        pf_lo = std::min(pf_lo, p->pf_lo);
        pf_hi = std::max(pf_hi, p->pf_hi);
    }
    if (pf_live.empty()) {
        pf_lo = 0;
    }
}

void BufferCache::PrefetchAtFault(VAddr addr, u64 size) {
    RbxDepth rbx_depth_guard{rbx_depth}; // sotc: SOTC_RB_EXACT: pf_live is walked here
    const VAddr end = addr + size;
    bool unsent = false;
    for (size_t i = 0; i < pf_live.size();) {
        AsyncReadback& p = *pf_live[i];
        if (p.finished) {
            PrefetchDrop(i); // applied or dropped by a guest readback that waited for it
            continue;
        }
        // sotc: a claimed one stays even when stale: the guest readback waiting for it finds it
        // in FinishReadyOwners and reads its window back (FinishAsyncReadback). Dropped here,
        // that guest got nothing and faulted again, possibly onto a newer prefetch that went
        // stale the same way (review of 2026-10-28).
        // sotc: SOTC_RB_GROUP: a dirty group download (every page trimmed) owns nothing any more:
        // dropped even when claimed (the guest waiting for it then finds its pages GPU-modified
        // and reads them back the usual way; no new group download is made for that group).
        if (p.dirty && (!p.pf_claimed || p.pf_group)) {
            SotcStats::rb_pf_stale.fetch_add(1, std::memory_order_relaxed);
            PrefetchDrop(i);
            continue;
        }
        if (!(p.window_start < end && addr < p.window_start + p.window_size)) {
            ++i;
            continue;
        }
        if (!p.dirty && IsReadbackComplete(p)) {
            // The guest faulted in its granule and its copies are done: apply it now (not dirty,
            // see above), no GPU wait.
            FinishAsyncReadback(p); // erases it from inflight_readbacks, frees its staging
            if (p.pf_label) {
                SotcStats::rb_ex_ld_applied.fetch_add(1, std::memory_order_relaxed);
                SotcStats::rb_ex_ld_bytes.fetch_add(p.gathered_bytes, std::memory_order_relaxed);
            }
            SotcStats::rb_pf_consumed.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rb_pf_consumed_all.fetch_add(1, std::memory_order_relaxed);
            PrefetchDrop(i);
            continue;
        }
        // Still on the GPU. If it owns part of the asked range, the guest readback waits for its
        // tick (RB_OWNER) and finishes it: keep it alive until then, and send its tick if the
        // copies are still in the command buffer being recorded.
        bool owner = false;
        for (const auto& item : p.pending) {
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                owner |= range_addr < end && addr < range_addr + range_size;
            }
        }
        if (owner && !p.pf_claimed) {
            p.pf_claimed = true;
            ++inflight_version;
            SotcStats::rb_pf_waited.fetch_add(1, std::memory_order_relaxed);
        }
        unsent |= owner && p.tick >= scheduler.CurrentTick();
        ++i;
    }
    PrefetchUpdateHull();
    if (unsent && !ReadbackBatchScope::Defer(scheduler)) {
        scheduler.Flush();
    }
}

bool BufferCache::PrefetchIssue() {
    if (!rb_prefetch_env) {
        return false;
    }
    RbxDepth rbx_depth_guard{rbx_depth}; // sotc: SOTC_RB_EXACT: pf_live is walked here
    if (RbxOn() && !rbx_live.empty()) {
        RbxApplyOwed(true); // sotc: SOTC_RB_EXACT: owed downloads that completed
    }
    if (rb_group_env && rbg_live != 0) {
        RbgApplyReady(true); // sotc: SOTC_RB_GROUP: group downloads that completed
    }
    if ((rb_materialize_env || rb_prev_env) && !pf_live.empty()) {
        PrefetchMaterialize(); // sotc: also applies SOTC_RB_PREV's owed downloads
    }
    const u64 tick = scheduler.CurrentTick();
    // Once per tick: free the dirty ones and the ones nobody asked for in time.
    if (!pf_live.empty() && tick != pf_reap_tick) {
        pf_reap_tick = tick;
        for (size_t i = 0; i < pf_live.size();) {
            AsyncReadback& p = *pf_live[i];
            if (p.finished) {
                PrefetchDrop(i);
            } else if (p.dirty && (!p.pf_claimed || p.pf_group)) { // sotc: see PrefetchAtFault
                SotcStats::rb_pf_stale.fetch_add(1, std::memory_order_relaxed);
                PrefetchDrop(i);
            } else if (p.pf_label && tick - p.tick < 4096) {
                ++i; // sotc: SOTC_RB_EXACT: a label download lives until its tick completes
                     // (sanity bound: 4096 ticks, in case its label never pops)
            } else if (tick - p.tick >= ((p.pf_claimed || p.pf_owed || p.pf_group)
                                             ? rb_pf_ticks * 16
                                             : rb_pf_ticks)) {
                // sotc: SOTC_RB_GROUP: a group download gets the long bound (applied when done).
                // sotc: SOTC_RB_PREV: an owed download gets the long bound too (applied when done;
                // the bound only frees one claimed or stuck, so the budget never leaks).
                // A claimed one is finished by the guest readback waiting for it; the longer
                // bound only frees one stranded otherwise (that guest would just fault again).
                SotcStats::rb_pf_expired.fetch_add(1, std::memory_order_relaxed);
                PrefetchDrop(i);
            } else {
                ++i;
            }
        }
        PrefetchUpdateHull();
    }
    if (pf_armed.empty() || !rb_pf_issue_env) { // sotc: SOTC_RB_EXACT alone issues no prefetches
        return false;
    }
    if (const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
        frame != pf_frame) {
        PrefetchRebuild(frame);
    }
    // The pages readbacks in flight (guest ones and prefetches) download are theirs.
    thread_local std::vector<std::pair<VAddr, VAddr>> owned;
    owned.clear();
    for (const AsyncReadback* other : inflight_readbacks) {
        for (const auto& item : other->pending) {
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                owned.emplace_back(range_addr, range_addr + range_size);
            }
        }
    }
    std::ranges::sort(owned);
    size_t merged = 0;
    for (size_t i = 0; i < owned.size(); ++i) {
        if (merged && owned[i].first <= owned[merged - 1].second) {
            owned[merged - 1].second = std::max(owned[merged - 1].second, owned[i].second);
        } else {
            owned[merged++] = owned[i];
        }
    }
    owned.resize(merged);
    const u64 granule = PfGranule();
    bool issued = false;
    size_t kept = 0;
    for (size_t a = 0; a < pf_armed.size(); ++a) {
        PfWindow& window = pf_recurring[pf_armed[a]];
        if (window.issued_tick == tick) {
            // One prefetch per window and tick: one written again in this tick waits for the next
            // one (its prefetch of this tick went dirty), instead of a copy after every command.
            pf_armed[kept++] = pf_armed[a];
            continue;
        }
        window.armed = false;
        if (pf_live.size() >= rb_pf_max) {
            SotcStats::rb_pf_capped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Never creates an arena: a window outside every arena holds no GPU data.
        const Buffer* arena = address_space[window.key >> ARENA_PAGE_BITS];
        if (!arena) {
            continue;
        }
        const VAddr start = std::max<VAddr>(window.key, arena->cpu_addr);
        const VAddr stop =
            std::min<VAddr>(window.key + granule, arena->cpu_addr + arena->size_bytes);
        if (start >= stop) {
            continue;
        }
        auto prefetch = std::make_unique<AsyncReadback>();
        AsyncReadback& p = *prefetch;
        rb_owned = &owned;
        rb_gather_no_hot = true;
        const u64 total = GatherDownloads(arena, start, stop - start, p.pending, false);
        rb_owned = nullptr;
        rb_gather_no_hot = false;
        if (total == 0) {
            continue; // read back since, or owned by a readback in flight
        }
        // The pool hands deferred requests out in power-of-two size classes of at least 64 KiB.
        const u64 bytes = std::bit_ceil(std::max<u64>(total, 64_KB));
        if (pf_bytes_held + bytes > rb_pf_budget) {
            SotcStats::rb_pf_capped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Held until it is applied or dropped.
        const auto download =
            staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
        VAddr lo = ~VAddr{0};
        VAddr hi = 0;
        for (auto& item : p.pending) {
            for (auto& copy : item.copies) {
                copy.dstOffset += download.offset;
            }
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                lo = std::min(lo, range_addr);
                hi = std::max(hi, range_addr + range_size);
            }
        }
        p.device_addr = start;
        p.size = stop - start;
        p.arena = arena;
        p.window_start = start;
        p.window_size = stop - start;
        p.tick = tick;
        p.waiting = true;
        p.prefetch = true;
        p.pf_lo = lo;
        p.pf_hi = hi;
        p.pf_bytes = bytes;
        p.pf_fresh_ok = rb_materialize_env && PrefetchFreshCovers(window.key, p);
        if (rb_materialize_env && !p.pf_fresh_ok) {
            SotcStats::rb_mat_not_fresh.fetch_add(1, std::memory_order_relaxed);
        }
        p.gathered_bytes = total;
        p.download_buffer = download.buffer;
        p.download_size = download.size;
        p.download_offset = download.offset;
        p.download_mapped = download.mapped;
        window.issued_tick = tick;
        pf_lo = pf_live.empty() ? lo : std::min(pf_lo, lo);
        pf_hi = pf_live.empty() ? hi : std::max(pf_hi, hi);
        pf_bytes_held += bytes;
        inflight_readbacks.push_back(&p);
        ++inflight_version;
        pf_live.push_back(std::move(prefetch));
        SotcStats::rb_pf_issued.fetch_add(1, std::memory_order_relaxed);
        issued = true;
    }
    pf_armed.resize(kept);
    return issued;
}

void BufferCache::PrevNoteApplied(VAddr addr, u64 size) {
    // Any thread (ApplyDownloads also runs on the command processor): only prev_mutex state.
    if (size == 0) {
        return;
    }
    const u64 granule = PfGranule();
    const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    const u64 now = SotcStats::NowNs();
    const VAddr end = addr + size;
    std::scoped_lock lock{prev_mutex};
    for (VAddr key = Common::AlignDown(addr, granule); key < end; key += granule) {
        if (pf_applied.size() >= RbPfHistoryMax && !pf_applied.contains(key)) {
            continue; // bounded; entries age out in PrefetchRebuild
        }
        const VAddr lo = std::max<VAddr>(addr, key);
        const VAddr hi = std::min<VAddr>(end, key + granule);
        auto& spans = pf_applied[key];
        bool merged = false;
        for (PfFresh& f : spans) {
            if (f.frame == frame && lo <= f.hi && f.lo <= hi) {
                f.lo = std::min(f.lo, lo);
                f.hi = std::max(f.hi, hi);
                merged = true;
                break;
            }
        }
        if (!merged) {
            if (spans.size() < RbMatFreshMax) {
                spans.push_back({lo, hi, frame, now});
            } else {
                *std::ranges::min_element(spans, {}, &PfFresh::frame) = {lo, hi, frame, now};
            }
        }
    }
}

void BufferCache::PrevErase(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    std::scoped_lock lock{prev_mutex};
    if (pf_applied.empty()) {
        return;
    }
    const u64 granule = PfGranule();
    std::erase_if(pf_applied, [&](const auto& entry) {
        return entry.first < end && addr < entry.first + granule;
    });
}

// sotc: SOTC_RB_PREV=1 (off by default; turns the prefetch bookkeeping on, needs SOTC_RB_OWNER=1).
// What is left of the stalls after SOTC_RB_MATERIALIZE are guest reads of the XPR occlusion masks
// of the current frame while the GPU is still producing them (the writer's tick not done, often
// not even submitted). No copy or memory trick removes that wait: the data does not exist yet.
// This switch accepts a frame of latency for those masks, as many PC engines do with occlusion
// queries: the guest reads the version already in its memory (the last one applied by a readback
// or by SOTC_RB_MATERIALIZE) instead of waiting.
//
// How: nothing is written to guest memory. The faulting pages lose their GPU-modified state
// without a download (gpu_modified_ranges and the tracker, like an unmap but without marking them
// CPU-modified, so the arena keeps the GPU's newer bytes and nothing uploads the old ones over
// them); the guest then reads what is there. The GPU's frame-N bytes are not shown to the CPU;
// its write of the next frame marks the pages again and the next read gets frame N+1 (exactly, if
// done by then; served again from the last version otherwise).
//
// When (all of them; otherwise the usual exact readback):
// - a read (a write fault needs the exact bytes);
// - the window is read back every frame (pf_recurring) and the GPU wrote it in at least two of
//   the last three command processor frames (pf_wmask, cp_frame): a per-frame producer, and a
//   one-off GPU result (written once, read later) is still delivered by the owed download below;
// - every GPU-modified byte of every page dropped lies inside ranges ApplyDownloads wrote into
//   guest memory at most SOTC_RB_PREV_FRAMES (2) frames and 150 ms ago (pf_applied): the guest
//   memory holds a recent version of exactly those bytes, not memory the game may have reused;
//   unmapped or released memory erases its applied ranges (PrevErase);
// - each page of the asked range qualifies. Pages of its window that do not are left as they are.
// Once a window's last applied version is older than SOTC_RB_PREV_FRAMES, its read waits as before
// and applies the current data, which bounds the age of what the guest sees.
// Readbacks in flight that own the dropped pages go dirty (StaleReason::Unmap) and read again the
// usual way when finished. Risk accepted with the switch: an object that just became visible may
// be culled for a frame or two.
bool BufferCache::TryServePrevious(AsyncReadback& readback) {
    if (readback.is_write) {
        return false;
    }
    const VAddr addr = readback.device_addr;
    const u64 size = readback.size;
    const u64 granule = PfGranule();
    const VAddr key = Common::AlignDown(addr, granule);
    if (addr + size > key + granule) {
        return false; // spans two windows: the usual path
    }
    const u64 frame = SotcStats::cp_frame.load(std::memory_order_relaxed);
    const auto wm = pf_wmask.find(key);
    if (wm == pf_wmask.end()) {
        return false;
    }
    {
        const u64 shift = frame > wm->second.frame ? frame - wm->second.frame : 0;
        const u32 mask = shift >= 32 ? 0 : wm->second.mask << shift;
        // Written in at least two of the frames frame-1 .. frame-3 (bits 1-3), whatever this frame
        // did: frames the command processor splits differently still count.
        if (std::popcount(mask & 0xEu) < 2) {
            SotcStats::rb_prev_not_producer.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    const auto rec = std::ranges::lower_bound(pf_recurring, key, {}, &PfWindow::key);
    if (rec == pf_recurring.end() || rec->key != key) {
        return false;
    }
    const Buffer* arena = address_space[addr >> ARENA_PAGE_BITS];
    if (!arena) {
        return false;
    }
    boost::container::small_vector<std::pair<VAddr, VAddr>, RbMatFreshMax> spans;
    {
        const u64 now = SotcStats::NowNs();
        std::scoped_lock lock{prev_mutex};
        const auto found = pf_applied.find(key);
        if (found == pf_applied.end()) {
            SotcStats::rb_prev_too_old.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        for (const PfFresh& f : found->second) {
            if (f.frame + rb_prev_frames >= frame && now - f.ns <= RbPrevMaxNs) {
                spans.emplace_back(f.lo, f.hi);
            }
        }
    }
    std::ranges::sort(spans);
    size_t merged = 0;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (merged && spans[i].first <= spans[merged - 1].second) {
            spans[merged - 1].second = std::max(spans[merged - 1].second, spans[i].second);
        } else {
            spans[merged++] = spans[i];
        }
    }
    spans.resize(merged);
    if (spans.empty()) {
        SotcStats::rb_prev_too_old.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto covered = [&](VAddr lo, VAddr hi) {
        const auto it =
            std::ranges::upper_bound(spans, lo, {}, &std::pair<VAddr, VAddr>::first);
        return it != spans.begin() && std::prev(it)->second >= hi;
    };
    auto [window_start, window_end] = ReadbackWindow(arena, addr, size);
    window_start = std::max<VAddr>(window_start, key);
    window_end = std::min<VAddr>(window_end, key + granule);
    constexpr u64 Page = 4096;
    const VAddr ask_lo = Common::AlignDown(addr, Page);
    const VAddr ask_hi = Common::AlignUp(addr + size, Page);
    thread_local std::vector<std::pair<VAddr, VAddr>> drop;
    drop.clear();
    {
        std::shared_lock lock{gpu_modified_mutex};
        for (VAddr page = Common::AlignDown(window_start, Page); page < window_end; page += Page) {
            bool any = false;
            bool ok = true;
            gpu_modified_ranges.ForEachInRange(page, Page, [&](VAddr lo, VAddr hi) {
                any = true;
                ok = ok && covered(lo, hi);
            });
            const bool asked = page >= ask_lo && page < ask_hi;
            // A page the guest also wrote (CPU and GPU bits) would upload the old guest bytes over
            // the arena's newer GPU bytes once released: the exact path (review of 2026-10-28).
            if (any && ok && memory_tracker->IsRegionCpuModified(page, Page)) {
                ok = false;
            }
            if (any && ok) {
                if (!drop.empty() && drop.back().second == page) {
                    drop.back().second = page + Page;
                } else {
                    drop.emplace_back(page, page + Page);
                }
            } else if (asked) {
                // An asked page with GPU data older than the limit (or none: some other reason
                // for the fault): the usual readback.
                SotcStats::rb_prev_too_old.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }
    if (drop.empty()) {
        return false;
    }
    // A guest readback in flight already downloads some of these pages: leave them to it (its
    // data would be thrown away as stale and the guest would get the old bytes).
    for (const AsyncReadback* other : inflight_readbacks) {
        if (other->prefetch && !other->pf_claimed) {
            continue; // ours: dirtied below, like any prefetch of released pages
        }
        // A claimed prefetch has a guest readback waiting for its exact bytes: also refused.
        for (const auto& item : other->pending) {
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                for (const auto& [lo, hi] : drop) {
                    if (range_addr < hi && lo < range_addr + range_size) {
                        SotcStats::rb_prev_busy.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                }
            }
        }
    }
    // The GPU's newer bytes are still owed to guest memory: if the producer stops writing the
    // window, nothing would ever fault there again and the guest would keep the old version for
    // good (review of 2026-10-28). Their download is recorded now, after every write recorded so
    // far, while they are still GPU-modified; PrefetchMaterialize applies it once done unless a
    // later write, an unmap or a guest write made it dirty (the usual prefetch rules), so the
    // guest gets the newer version a little later instead of never.
    if (pf_live.size() >= rb_pf_max) {
        SotcStats::rb_pf_capped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto owed = std::make_unique<AsyncReadback>();
    u64 owed_total = 0;
    rb_gather_no_hot = true;
    for (const auto& [lo, hi] : drop) {
        const size_t before = owed->pending.size();
        const u64 got = GatherDownloads(arena, lo, hi - lo, owed->pending, false);
        // sotc: every call counts its staging offsets from 0: rebase the items it added, or the
        // copies of the runs overwrite each other.
        for (size_t k = before; k < owed->pending.size(); ++k) {
            for (auto& copy : owed->pending[k].copies) {
                copy.dstOffset += owed_total;
            }
        }
        owed_total += got;
    }
    rb_gather_no_hot = false;
    const u64 owed_bytes = owed_total ? std::bit_ceil(std::max<u64>(owed_total, 64_KB)) : 0;
    if (owed_total != 0 && pf_bytes_held + owed_bytes > rb_pf_budget) {
        SotcStats::rb_pf_capped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (owed_total != 0) {
        AsyncReadback& o = *owed;
        const auto download =
            staging_pool.Request(owed_total, VideoCore::MemoryType::HostCached, 0, true);
        VAddr lo_hull = ~VAddr{0};
        VAddr hi_hull = 0;
        for (auto& item : o.pending) {
            for (auto& copy : item.copies) {
                copy.dstOffset += download.offset;
            }
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                lo_hull = std::min(lo_hull, range_addr);
                hi_hull = std::max(hi_hull, range_addr + range_size);
            }
        }
        o.device_addr = drop.front().first;
        o.size = drop.back().second - drop.front().first;
        o.arena = arena;
        o.window_start = o.device_addr;
        o.window_size = o.size;
        o.tick = scheduler.CurrentTick();
        o.waiting = true;
        o.prefetch = true;
        o.pf_owed = true;
        o.pf_fresh_ok = true;
        o.pf_lo = lo_hull;
        o.pf_hi = hi_hull;
        o.pf_bytes = owed_bytes;
        o.gathered_bytes = owed_total;
        o.download_buffer = download.buffer;
        o.download_size = download.size;
        o.download_offset = download.offset;
        o.download_mapped = download.mapped;
    }
    u64 bytes = 0;
    for (const auto& [lo, hi] : drop) {
        {
            std::unique_lock gpu_modified_lock{gpu_modified_mutex};
            gpu_modified_ranges.Subtract(lo, hi - lo);
        }
        EraseGranuleSnapshots(lo, hi - lo);
        memory_tracker->UnmarkRegionAsGpuModified(lo, hi - lo, false);
        MarkInflightReadbacksDirty(lo, hi - lo, StaleReason::Unmap);
        if (!pf_live.empty()) {
            PrefetchMarkDirty(lo, hi - lo);
        }
        bytes += hi - lo;
    }
    if (owed_total != 0) {
        // Inserted after the drop, so its own release did not dirty it.
        AsyncReadback& o = *owed;
        pf_lo = pf_live.empty() ? o.pf_lo : std::min(pf_lo, o.pf_lo);
        pf_hi = pf_live.empty() ? o.pf_hi : std::max(pf_hi, o.pf_hi);
        pf_bytes_held += o.pf_bytes;
        inflight_readbacks.push_back(&o);
        ++inflight_version;
        pf_live.push_back(std::move(owed));
        SotcStats::rb_prev_owed.fetch_add(1, std::memory_order_relaxed);
    }
    SotcStats::rb_prev_served.fetch_add(1, std::memory_order_relaxed);
    SotcStats::rb_prev_served_all.fetch_add(1, std::memory_order_relaxed);
    SotcStats::rb_prev_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return true;
}

// sotc: SOTC_RB_MATERIALIZE=1 (off by default; turns SOTC_RB_PREFETCH on, same requirements).
// The XPR occlusion masks are ~80% of the guest readbacks (r_EnableXPR=0: ~900 -> ~200 a second,
// but twice the draws and 8-17 FPS, so the masks must stay), and in 78-89% of them the GPU work
// that wrote the bytes was already done when the guest read: the wait was the fault, the round
// trip through the draw thread and the copy. A prefetch (above) already holds those bytes; here a
// finished, clean one is applied as soon as its copies are done, at the next command boundary,
// instead of at the guest's fault: the pages leave the GPU-modified set and the guest reads them
// without a fault.
//
// Why it is exact: it is the same apply PrefetchAtFault does (same staging, same ApplyDownloads
// and SubtractDownloaded, same dirty rules: any write recorded after the copies, unmap, CPU
// replacement, released memory or another readback makes it dirty and it is never applied), only
// earlier; the bytes are the ones a guest readback at that moment would copy.
//
// Why it is not SOTC_RB_HOT again (speculative applies wrote old GPU data over memory the game
// had reused for textures and heaps: rainbow garbage, 0x49500). Two guards:
// 1. Only fresh data: a prefetch is eligible (pf_fresh_ok, set when it is issued) only when every
//    GPU-modified range it copied lies inside ranges the GPU wrote into that window in this frame
//    or the previous one (pf_fresh, from AddGpuModified). A range left GPU-modified from long ago
//    (the reused-memory case) is never written by it: it waits for a guest fault as before.
// 2. Never over the guest's writes: its items are applied as hot items, so ApplyDownloads writes
//    only pages without the CPU bit, page by page under the region lock (WriteUnlessCpuModified):
//    a guest write in between wins.
// A claimed one (a guest readback waits for it) is left to that readback. Checked at most every
// SOTC_RB_MATERIALIZE_US (default 100 us), from PrefetchIssue (draw thread, like every readback).
void BufferCache::PrefetchMaterialize() {
    const u64 now = SotcStats::NowNs();
    if (now - pf_mat_last_ns < rb_mat_ns) {
        return;
    }
    pf_mat_last_ns = now;
    bool changed = false;
    for (size_t i = 0; i < pf_live.size();) {
        AsyncReadback& p = *pf_live[i];
        if (p.finished || p.dirty || p.pf_claimed || !p.pf_fresh_ok || !IsReadbackComplete(p)) {
            ++i;
            continue;
        }
        for (auto& item : p.pending) {
            item.hot = true; // guard 2: pages the guest wrote keep its bytes
        }
        FinishAsyncReadback(p); // erases it from inflight_readbacks, frees its staging
        SotcStats::rb_mat_applied.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rb_mat_bytes.fetch_add(p.gathered_bytes, std::memory_order_relaxed);
        PrefetchDrop(i);
        changed = true;
    }
    if (changed) {
        PrefetchUpdateHull();
    }
}

// sotc: SOTC_RB_EXACT=1 (off by default; needs SOTC_RB_OWNER=1 and the asynchronous readbacks;
// replaces SOTC_RB_PREV). Guest threads wait 40-145 ms in the slow frames for readbacks, yet in
// 78-89% of them the GPU work that wrote the asked bytes was already done. They waited because the
// port protects whole pages and a readback gathers every GPU-modified range of its window: a read
// of an old, finished slot waited for the GPU work of the slot being written right now, which
// shares its 4 KiB pages. On the PS4 a GPU write means something to the CPU only once the GPU is
// done with it, and the game learns that from labels (and other CPU-visible GPU writes). Bytes
// whose work the game was not told about are a race on the PS4 as well: their old contents are a
// valid result. That is the whole idea; everything below is bookkeeping to make it exact.
//
// Definitions. Every GPU write to the arena is recorded with the tick of the command buffer that
// holds it (RbxNoteWrite from RecordCurrentWrite; EndWriteScope re-records it in the tick the
// command landed in) and, when it hits a hot window, a hot sequence number (AddGpuModified: the
// window is a recurring readback granule, the one the guest read in at least 2 of the last 3
// frames; HotWriteSeq() counts these writes). rbx_writers holds, per byte range, the newest
// writer, so the writer of a byte is known precisely (byte ranges, not pages). A GPU-modified
// range is "done" when its writer's tick has completed, otherwise "pending". A pending range is
// "announced" when its sequence number is <= the largest snapshot HotWriteSeq() handed out or
// NoteLabelWritten() received (liverpool.cpp snapshots it when it processes a label or another
// CPU-visible GPU write, so "announced" includes labels deferred but not yet written: the game
// may see them any moment). A pending range with sequence number 0 (written outside a hot window
// or when the window was not recurring) or with an unknown writer (the map overflowed) is never
// released: unknown is always the exact fallback.
//
// Rules at a guest read fault (TryServeExact, draw thread), per 4 KiB page of the readback window:
// 1. Only the writers of the page's GPU-modified bytes count. A page whose bytes are all done
//    needs no GPU wait; the asked pages are what the guest waits for.
// 2. The download takes only the done byte ranges of the window (copies of byte spans, not of the
//    page runs, so no pending byte is waited for): the same transfer-queue copy as SOTC_RB_TICK,
//    waiting only for the newest done writer tick, which already finished.
// 3. A page is released (made readable by the usual ApplyDownloads at the end of the readback, or
//    right away when it holds nothing to download) when each of its ranges is done or pending
//    and not announced. The pending, unannounced ranges become "owed": an AsyncReadback with
//    prefetch, pf_owed, pf_fresh_ok and hot items (ReadbackItem::owed) records their copy in the
//    tick being recorded, i.e. after their writer in the stream and after every write recorded so
//    far, and PrefetchMaterialize-style application writes the bytes into guest memory once the
//    copy is done (RbxApplyOwed, at command boundaries, and ApplyOwedBeforeLabel).
// 4. A page that holds a pending range that is announced, or whose writer is unknown, or that
//    carries the CPU bit, or that belongs to a guest readback in flight, is not released. If the
//    page is one of the asked ones the fault takes the usual readback (wait, exact); if it only
//    lies in the window it is left as it is (protected, GPU-modified), as before.
// Write faults and everything else not listed take the usual path.
//
// Why the result is exact. (a) The guest sees new bytes only if their work is done (rules 1-2) or
// the bytes are owed and applied by their copy, which follows their writer, so what is applied is
// final for the bytes at that point of the stream. (b) An owed byte never reaches guest memory
// after something newer: owed downloads are trimmed byte by byte (RbxTrim, from
// MarkInflightReadbacksDirty and RecordCurrentWrite) by any later GPU write, unmap, release,
// CPU replacement or upload over them, and a guest write onto owed bytes first applies them
// (InvalidateMemory); the write-ahead and fault granules skip them (IsRegionGpuModified and
// MarkWriteAhead* see owed bytes as GPU-modified). Trimmed bytes are GPU-modified again (a later
// write) or gone, so their next read is exact by the usual means; an owed download with nothing
// left is dropped, never applied, and an "Applied" event does not touch owed bytes (they are not
// in the GPU-modified set, so no readback applies them). (c) The game must never see a label
// before the data it announces: a pending range released as owed has a sequence number above
// every snapshot taken so far, so the next label that announces it is deferred (liverpool.cpp
// defers a label iff HotWriteSeq() changed) until the GPU has finished the tick that holds the
// label, which also holds every owed copy recorded before it; ApplyOwedBeforeLabel writes all
// complete owed downloads before the label lands. (d) Released pages carry no CPU bit at release,
// and nothing can set it while bytes are owed without first applying them (guest write fault:
// InvalidateMemory; write-ahead: MarkWriteAhead*; fault granules: IsRegionGpuModified; unmap:
// trims). Owed items are still applied as hot items, so a page that got the CPU bit anyway keeps
// the guest's bytes.
//
// Label downloads (RecordLabelDownloads). Site 1 of the game (63% of the readbacks) allocates
// fresh bitsets every frame, dispatches a cull compute that writes them, emits a label and
// spin-waits on it in guest memory, then reads the bitsets: the bytes are announced by that very
// label, so rules 1-4 alone cannot help. When liverpool.cpp defers a label it calls this: it
// records copies of every GPU-modified range in hot windows written since the previous call (the
// GPU-modified state is NOT touched) as a prefetch-like AsyncReadback (pf_label) in the tick being
// recorded, i.e. after every write the label announces. ApplyOwedBeforeLabel applies it as hot
// items through the usual FinishAsyncReadback (ApplyDownloads + SubtractDownloaded) before the
// label lands, so the pages are unprotected and hold the final bytes when the game sees the label
// and its read does not fault. Dirty marking is the usual one: a later write, unmap or applied
// readback makes it dirty and it is dropped (the pages stay protected: exact fallback). It does
// not expire before its tick completes. A guest fault in between finds it as an owner of its
// pages (the usual prefetch rules: applied if complete, else waited for).
//
// Residual: the command processor's SRT walkers read guest memory directly; owed bytes in a
// released page are old there until applied.

u64 BufferCache::RbxHotSeq(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    if (pf_recurring.empty() || end <= pf_rec_lo || addr >= pf_rec_hi) {
        return 0;
    }
    const u64 granule = PfGranule();
    const VAddr from = addr >= granule ? addr - granule + 1 : 0;
    const auto it = std::ranges::lower_bound(pf_recurring, from, {}, &PfWindow::key);
    if (it == pf_recurring.end() || it->key >= end) {
        return 0;
    }
    return rbx_hot_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool BufferCache::RbExactEnabled() noexcept {
    return rb_exact_env;
}

void BufferCache::SetLabelSideActive(bool on) noexcept {
    rbx_label_side.store(on, std::memory_order_release);
}

void BufferCache::SetDrawThread(std::thread::id id) noexcept {
    rbx_draw_thread.store(id, std::memory_order_release);
}

u64 BufferCache::HotWriteSeq() const noexcept {
    if (!RbxOn()) {
        return 0;
    }
    const u64 seq = rbx_hot_seq.load(std::memory_order_acquire);
    u64 seen = rbx_seq_queried.load(std::memory_order_relaxed);
    while (seen < seq &&
           !rbx_seq_queried.compare_exchange_weak(seen, seq, std::memory_order_acq_rel)) {
    }
    return seq;
}

void BufferCache::NoteLabelWritten(u64 hot_seq) noexcept {
    if (!RbxOn()) {
        return;
    }
    u64 seen = rbx_announced.load(std::memory_order_relaxed);
    while (seen < hot_seq &&
           !rbx_announced.compare_exchange_weak(seen, hot_seq, std::memory_order_acq_rel)) {
    }
    // Diagnostic (draw thread only): an owed download that announced bytes should be applied.
    // No timeline query: an owed download that announced bytes and is not applied yet is unsafe
    // whether or not its copy is done (the scan is a few compares over at most RbxOwedMax).
    if (RbxOnDrawThread()) {
        for (const auto& o : rbx_live) {
            if (!o->finished && !o->dirty && o->rbx_seq_min <= hot_seq) {
                SotcStats::rb_ex_unsafe_announce.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

bool BufferCache::ApplyOwedBeforeLabel() {
    if (!RbxOn()) {
        return true; // nothing is ever owed
    }
    if (!RbxOnDrawThread()) {
        SotcStats::rb_ex_apply_wrongthread.fetch_add(1, std::memory_order_relaxed);
        return false; // requeue the label op for the draw thread
    }
    if (rbx_depth > 0 || rbx_in_apply) {
        // Re-entered from inside a buffer-cache operation (a flush, a staging wait): its lists
        // are in the middle of a change. The label is requeued.
        SotcStats::rb_ex_apply_reentered.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // The op runs once the tick it was deferred at is done. Every owed and label download
    // recorded before the label has a tick at or before that one, so it is complete here; one
    // that is not complete was recorded after the label and announces nothing of it.
    RbxApplyOwed(false);
    for (size_t i = 0; i < pf_live.size(); ++i) {
        AsyncReadback& p = *pf_live[i];
        if (!p.pf_label || p.finished || p.dirty || !IsReadbackComplete(p)) {
            continue;
        }
        for (auto& item : p.pending) {
            item.hot = true;
        }
        FinishAsyncReadback(p);
        SotcStats::rb_ex_ld_applied.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rb_ex_ld_bytes.fetch_add(p.gathered_bytes, std::memory_order_relaxed);
    }
    return true;
}

void BufferCache::SettleOwedUpTo(u64 seq) {
    if (!RbxOn() || !RbxOnDrawThread()) {
        return;
    }
    RbxDepth depth{rbx_depth};
    // Owed downloads (bytes already released to the guest) holding a range at or below seq:
    // applied now, waiting for their copy if needed.
    bool waited = false;
    u64 t0 = 0;
    const auto settle = [&](AsyncReadback& p) {
        if (!IsReadbackComplete(p)) {
            if (!waited) {
                waited = true;
                t0 = SotcStats::NowNs();
            }
            if (p.tick >= scheduler.CurrentTick()) {
                scheduler.Flush();
            }
            scheduler.GetWorkSemaphore()->Wait(p.tick);
        }
        for (auto& item : p.pending) {
            item.hot = true;
        }
        FinishAsyncReadback(p);
    };
    for (size_t i = 0; i < rbx_live.size(); ++i) {
        AsyncReadback& o = *rbx_live[i];
        if (o.rbx_seq_min <= seq && !o.finished && !o.dirty) {
            settle(o);
            SotcStats::rb_ex_owed_applied.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Label downloads are not waited for: their bytes are still GPU-modified (protected), so a
    // fault on them takes the exact path whatever was announced.
    for (size_t i = 0; i < rbx_live.size();) { // applied ones leave
        if (rbx_live[i]->finished || rbx_live[i]->dirty) {
            RbxDrop(i);
        } else {
            ++i;
        }
    }
    if (waited) {
        SotcStats::rb_ex_settle_waits.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rb_ex_settle_ns.fetch_add(SotcStats::NowNs() - t0, std::memory_order_relaxed);
    }
}

const std::vector<std::pair<VAddr, VAddr>>& BufferCache::RbxOwnedAll() {
    // Sorted, merged page runs of every readback in flight; rebuilt only after
    // inflight_readbacks changed (inflight_version).
    if (rbx_owned_version != inflight_version) {
        rbx_owned_version = inflight_version;
        rbx_owned_cache.clear();
        for (const AsyncReadback* other : inflight_readbacks) {
            for (const auto& item : other->pending) {
                for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                    rbx_owned_cache.emplace_back(range_addr, range_addr + range_size);
                }
            }
        }
        std::ranges::sort(rbx_owned_cache);
        size_t merged = 0;
        for (size_t i = 0; i < rbx_owned_cache.size(); ++i) {
            if (merged && rbx_owned_cache[i].first <= rbx_owned_cache[merged - 1].second) {
                rbx_owned_cache[merged - 1].second =
                    std::max(rbx_owned_cache[merged - 1].second, rbx_owned_cache[i].second);
            } else {
                rbx_owned_cache[merged++] = rbx_owned_cache[i];
            }
        }
        rbx_owned_cache.resize(merged);
    }
    return rbx_owned_cache;
}

bool BufferCache::RecordLabelDownloads() {
    if (!RbxOn() || rbx_since_label.Empty()) {
        return false;
    }
    RbxDepth depth{rbx_depth};
    thread_local std::vector<std::pair<VAddr, VAddr>> ranges;
    ranges.clear();
    constexpr u64 Page = 4096;
    rbx_since_label.ForEach([&](VAddr lo, VAddr hi) {
        lo = Common::AlignDown(lo, Page);
        hi = Common::AlignUp(hi, Page);
        if (!ranges.empty() && lo <= ranges.back().second) {
            ranges.back().second = std::max(ranges.back().second, hi);
        } else {
            ranges.emplace_back(lo, hi);
        }
    });
    rbx_since_label.Clear();
    if (ranges.size() > RbxLabelRangesMax) {
        ranges.resize(RbxLabelRangesMax); // the rest stays protected: the usual fallback
    }
    const std::vector<std::pair<VAddr, VAddr>>& owned = RbxOwnedAll();
    auto label = std::make_unique<AsyncReadback>();
    AsyncReadback& p = *label;
    const Buffer* arena = nullptr;
    u64 total = 0;
    for (const auto& [lo_in, hi_in] : ranges) {
        const Buffer* a = address_space[lo_in >> ARENA_PAGE_BITS];
        if (!a || (arena && a != arena)) {
            continue; // one arena per label download (the window of a dirty one is one arena)
        }
        const VAddr lo = std::max<VAddr>(lo_in, a->cpu_addr);
        const VAddr hi = std::min<VAddr>(hi_in, a->cpu_addr + a->size_bytes);
        if (lo >= hi) {
            continue;
        }
        arena = a;
        const size_t before = p.pending.size();
        rb_owned = &owned;
        rb_gather_no_hot = true;
        const u64 got = GatherDownloads(a, lo, hi - lo, p.pending, false);
        rb_owned = nullptr;
        rb_gather_no_hot = false;
        for (size_t k = before; k < p.pending.size(); ++k) {
            for (auto& copy : p.pending[k].copies) {
                copy.dstOffset += total; // GatherDownloads counts from 0 on every call
            }
        }
        total += got;
    }
    if (total == 0) {
        return false;
    }
    const u64 bytes = std::bit_ceil(std::max<u64>(total, 64_KB));
    if (rbx_label_bytes + bytes > RbxLabelBudget) {
        SotcStats::rb_pf_capped.fetch_add(1, std::memory_order_relaxed);
        return false; // the pages stay protected: the usual fallback
    }
    const auto download = staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
    VAddr lo_hull = ~VAddr{0};
    VAddr hi_hull = 0;
    for (auto& item : p.pending) {
        for (auto& copy : item.copies) {
            copy.dstOffset += download.offset;
        }
        runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        for (const auto& [range_addr, range_size] : item.gpu_ranges) {
            lo_hull = std::min(lo_hull, range_addr);
            hi_hull = std::max(hi_hull, range_addr + range_size);
        }
    }
    p.device_addr = lo_hull;
    p.size = hi_hull - lo_hull;
    p.arena = arena;
    p.window_start = lo_hull;
    p.window_size = hi_hull - lo_hull;
    p.tick = scheduler.CurrentTick(); // after the copies: they are in this tick or an earlier one
    p.waiting = true;
    p.prefetch = true;
    p.pf_label = true;
    p.rbx_seq_min = 0;
    p.pf_fresh_ok = true;
    p.pf_lo = lo_hull;
    p.pf_hi = hi_hull;
    p.pf_bytes = 0; // not counted against the prefetch budget
    p.rbx_bytes = bytes;
    p.gathered_bytes = total;
    p.download_buffer = download.buffer;
    p.download_size = download.size;
    p.download_offset = download.offset;
    p.download_mapped = download.mapped;
    pf_lo = pf_live.empty() ? lo_hull : std::min(pf_lo, lo_hull);
    pf_hi = pf_live.empty() ? hi_hull : std::max(pf_hi, hi_hull);
    rbx_label_bytes += bytes;
    inflight_readbacks.push_back(&p);
    ++inflight_version;
    pf_live.push_back(std::move(label));
    SotcStats::rb_ex_ld_recorded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BufferCache::RbxNoteWrite(VAddr addr, u64 size, u64 tick, u64 seq, bool gpu_write) {
    if (size == 0) {
        return;
    }
    const VAddr lo = addr;
    const VAddr hi = addr + size;
    const u64 own = gpu_write ? seq : 0; // a write that is not a hot GPU write has no floor
    // Bytes whose older version is not in guest memory yet: GPU-modified (this includes the
    // bytes under a label download) or owed. Their older version must still be delivered, so the
    // new write inherits its floor; an unknown history (no entry) is floor 0.
    const auto undelivered = [&](VAddr a, VAddr b) {
        return gpu_modified_ranges.Intersects(a, b - a) || RbxOwedIntersects(a, b - a);
    };
    struct Piece {
        VAddr a, b;
        u64 floor;
    };
    thread_local std::vector<Piece> pieces;
    pieces.clear();
    auto it = rbx_writers.upper_bound(lo);
    if (it != rbx_writers.begin() && std::prev(it)->second.hi > lo) {
        --it;
    }
    std::optional<std::pair<VAddr, RbxWriter>> tail;
    VAddr cursor = lo;
    while (it != rbx_writers.end() && it->first < hi) {
        const VAddr entry_lo = it->first;
        const RbxWriter entry = it->second;
        it = rbx_writers.erase(it);
        if (entry_lo < lo) {
            rbx_writers.emplace(entry_lo, RbxWriter{lo, entry.tick, entry.seq, entry.floor});
        }
        if (entry.hi > hi) {
            tail = std::pair<VAddr, RbxWriter>{hi, entry};
        }
        const VAddr a = std::max(entry_lo, lo);
        const VAddr b = std::min(entry.hi, hi);
        if (a > cursor) {
            pieces.push_back({cursor, a, gpu_write && undelivered(cursor, a) ? 0 : own});
        }
        pieces.push_back({a, b,
                          !gpu_write ? 0
                                     : (undelivered(a, b) ? std::min(entry.floor, own) : own)});
        cursor = std::max(cursor, b);
    }
    if (cursor < hi) {
        pieces.push_back({cursor, hi, gpu_write && undelivered(cursor, hi) ? 0 : own});
    }
    if (tail) {
        rbx_writers.emplace(tail->first, tail->second);
    }
    for (const Piece& piece : pieces) {
        rbx_writers.emplace(piece.a, RbxWriter{piece.b, tick, seq, piece.floor});
    }
    if ((++rbx_notes & 1023) == 0 || rbx_writers.size() > RbxWritersMax) {
        RbxPrune();
    }
}

void BufferCache::RbxPrune() {
    u64 free_max = 0;
    u64 pend_min = ~u64{0};
    const auto done = [&](u64 t) {
        if (t <= free_max) {
            return true;
        }
        if (t >= pend_min) {
            return false;
        }
        const bool d = scheduler.IsFree(t);
        (d ? free_max : pend_min) = t;
        return d;
    };
    // An entry leaves only when its tick is done AND its bytes are delivered (neither
    // GPU-modified nor owed): the floor of a later write over them needs it until then.
    std::scoped_lock owed_lock{rbx_owed_mutex};
    for (auto it = rbx_writers.begin(); it != rbx_writers.end();) {
        const VAddr span_lo = it->first;
        const u64 span = it->second.hi - span_lo;
        if (done(it->second.tick) && !gpu_modified_ranges.Intersects(span_lo, span) &&
            !rbx_owed_ranges.Intersects(span_lo, span)) {
            rbx_pruned_tick = std::max(rbx_pruned_tick, it->second.tick);
            it = rbx_writers.erase(it);
        } else {
            ++it;
        }
    }
    if (rbx_writers.size() > RbxWritersMax) {
        // Too many pending writers: forget them. Bytes they wrote are unknown until their ticks
        // are done (TryServeExact: absent bytes are done only if rbx_lost_tick is).
        for (const auto& [lo, entry] : rbx_writers) {
            rbx_lost_tick = std::max(rbx_lost_tick, entry.tick);
        }
        rbx_writers.clear();
    }
}

bool BufferCache::RbxOwedIntersects(VAddr addr, u64 size) const {
    if (rbx_owed_count.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    std::scoped_lock lock{rbx_owed_mutex};
    return rbx_owed_ranges.Intersects(addr, size);
}

void BufferCache::RbxUnregister(AsyncReadback& o) {
    std::scoped_lock lock{rbx_owed_mutex};
    for (auto& item : o.pending) {
        for (const auto& copy : item.copies) {
            rbx_owed_ranges.Subtract(item.arena->cpu_addr + copy.srcOffset, copy.size);
        }
        item.copies.clear();
        item.gpu_ranges.clear();
    }
}

void BufferCache::RbxTrim(VAddr lo, VAddr hi) {
    if (rbx_live.empty() || lo >= hi) {
        return;
    }
    {
        std::scoped_lock lock{rbx_owed_mutex};
        if (!rbx_owed_ranges.Intersects(lo, hi - lo)) {
            return;
        }
    }
    for (const auto& up : rbx_live) {
        AsyncReadback& o = *up;
        if (o.finished || o.dirty || !(lo < o.pf_hi && o.pf_lo < hi)) {
            continue;
        }
        bool any_left = false;
        for (auto& item : o.pending) {
            boost::container::small_vector<vk::BufferCopy, 4> copies;
            boost::container::small_vector<std::pair<VAddr, u64>, 4> ranges;
            for (const auto& copy : item.copies) {
                const VAddr a = item.arena->cpu_addr + copy.srcOffset;
                const VAddr b = a + copy.size;
                if (b <= lo || a >= hi) {
                    copies.push_back(copy);
                    ranges.emplace_back(a, copy.size);
                    continue;
                }
                if (a < lo) {
                    copies.push_back(vk::BufferCopy{copy.srcOffset, copy.dstOffset, lo - a});
                    ranges.emplace_back(a, lo - a);
                }
                if (b > hi) {
                    copies.push_back(vk::BufferCopy{copy.srcOffset + (hi - a),
                                                    copy.dstOffset + (hi - a), b - hi});
                    ranges.emplace_back(hi, b - hi);
                }
                const u64 removed = std::min(b, hi) - std::max(a, lo);
                {
                    std::scoped_lock lock{rbx_owed_mutex};
                    rbx_owed_ranges.Subtract(std::max(a, lo), removed);
                }
                SotcStats::rb_ex_owed_trimmed.fetch_add(removed, std::memory_order_relaxed);
            }
            item.copies = std::move(copies);
            item.gpu_ranges = std::move(ranges);
            any_left |= !item.copies.empty();
        }
        if (!any_left) {
            o.dirty = true; // nothing left to apply: dropped by the next RbxApplyOwed
        }
    }
}

void BufferCache::RbxDrop(size_t index) {
    AsyncReadback& o = *rbx_live[index];
    if (!o.finished) {
        o.finished = true;
        staging_pool.FreeDeferred(Vulkan::StagingBufferRef{
            .buffer = o.download_buffer,
            .offset = o.download_offset,
            .size = o.download_size,
            .mapped = const_cast<u8*>(o.download_mapped),
            .type = VideoCore::MemoryType::HostCached,
        });
        SotcStats::rb_ex_owed_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    RbxUnregister(o);
    rbx_owed_bytes -= o.rbx_bytes;
    rbx_live.erase(rbx_live.begin() + static_cast<std::ptrdiff_t>(index));
    rbx_owed_count.store(static_cast<u32>(rbx_live.size()), std::memory_order_release);
}

void BufferCache::RbxApplyOwed(bool throttled) {
    if (rbx_live.empty() || rbx_in_apply) {
        return;
    }
    if (throttled) {
        const u64 now = SotcStats::NowNs();
        if (now - rbx_apply_ns < rb_mat_ns) {
            return;
        }
        rbx_apply_ns = now;
    }
    rbx_in_apply = true;
    for (size_t i = 0; i < rbx_live.size();) {
        AsyncReadback& o = *rbx_live[i];
        if (o.finished || o.dirty) {
            RbxDrop(i);
        } else if (IsReadbackComplete(o)) {
            u64 bytes = 0;
            for (auto& item : o.pending) {
                item.hot = true; // a page that got the CPU bit keeps the guest's bytes
                for (const auto& copy : item.copies) {
                    bytes += copy.size;
                }
            }
            FinishAsyncReadback(o);
            SotcStats::rb_ex_owed_applied.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rb_ex_owed_bytes.fetch_add(bytes, std::memory_order_relaxed);
            RbxDrop(i);
        } else {
            ++i;
        }
    }
    rbx_in_apply = false;
}

void BufferCache::RbxFinishOverlapping(VAddr addr, u64 size, bool hot) {
    if (rbx_in_apply || !RbxOwedIntersects(addr, size)) {
        return;
    }
    rbx_in_apply = true;
    const VAddr end = addr + size;
    for (size_t i = 0; i < rbx_live.size();) {
        AsyncReadback& o = *rbx_live[i];
        bool overlaps = false;
        if (!o.finished && !o.dirty && addr < o.pf_hi && o.pf_lo < end) {
            for (const auto& item : o.pending) {
                for (const auto& copy : item.copies) {
                    const VAddr a = item.arena->cpu_addr + copy.srcOffset;
                    overlaps |= a < end && addr < a + copy.size;
                }
            }
        }
        if (overlaps && IsReadbackComplete(o)) {
            u64 bytes = 0;
            for (auto& item : o.pending) {
                item.hot = hot;
                for (const auto& copy : item.copies) {
                    bytes += copy.size;
                }
            }
            FinishAsyncReadback(o);
            SotcStats::rb_ex_owed_applied.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rb_ex_owed_bytes.fetch_add(bytes, std::memory_order_relaxed);
            RbxDrop(i);
        } else {
            ++i;
        }
    }
    rbx_in_apply = false;
}

u64 BufferCache::RbxOwedTick(VAddr addr, u64 size, bool& unsent) {
    u64 tick = 0;
    const VAddr end = addr + size;
    for (const auto& up : rbx_live) {
        const AsyncReadback& o = *up;
        if (o.finished || o.dirty || !(addr < o.pf_hi && o.pf_lo < end)) {
            continue;
        }
        bool overlaps = false;
        for (const auto& item : o.pending) {
            for (const auto& copy : item.copies) {
                const VAddr a = item.arena->cpu_addr + copy.srcOffset;
                overlaps |= a < end && addr < a + copy.size;
            }
        }
        if (overlaps) {
            tick = std::max(tick, o.tick);
            unsent |= o.tick >= scheduler.CurrentTick();
        }
    }
    return tick;
}

void BufferCache::RbxSettleSync(VAddr addr, u64 size) {
    // Called with the locks held (the draw thread itself): wait for the owed download, apply it.
    RbxDepth depth{rbx_depth};
    bool unsent = false;
    const u64 tick = RbxOwedTick(addr, size, unsent);
    if (tick == 0) {
        return;
    }
    if (unsent) {
        scheduler.Flush();
    }
    scheduler.GetWorkSemaphore()->Wait(tick);
    RbxFinishOverlapping(addr, size, false);
}

void BufferCache::FinishOwners(VAddr addr, u64 size, bool hot) {
    FinishReadyOwners(addr, size);
    if (RbxOn() && !rbx_live.empty()) {
        RbxFinishOverlapping(addr, size, hot);
    }
}

void BufferCache::AddResidentCopies(ReadbackItem& item, VAddr start, VAddr end, u64& total) {
    // Only copy what is still bound; a released range must never be read (as GatherDownloads).
    resident_ranges.ForEachInRange(
        start >> block_shift, ((end - 1) >> block_shift) + 1, [&](const Backing& backing) {
            const VAddr a = std::max<VAddr>(start, backing.start << block_shift);
            const VAddr b = std::min<VAddr>(end, backing.end << block_shift);
            if (a >= b) {
                return;
            }
            item.copies.push_back(vk::BufferCopy{
                .srcOffset = a - item.arena->cpu_addr,
                .dstOffset = total,
                .size = b - a,
            });
            total += Common::AlignUp(b - a, 64ULL);
        });
}

BufferCache::ExactResult BufferCache::TryServeExact(AsyncReadback& readback, const Buffer* arena,
                                                    VAddr ws, VAddr we, u64& total_out,
                                                    u64& writer_out) {
    constexpr u64 Page = 4096;
    enum class Why { Ok, Announced, Unknown, Cpu, Owned, Floor };
    const auto fallback = [](Why why) {
        switch (why) {
        case Why::Announced:
            SotcStats::rb_ex_fb_announced.fetch_add(1, std::memory_order_relaxed);
            break;
        case Why::Unknown:
            SotcStats::rb_ex_fb_unknown.fetch_add(1, std::memory_order_relaxed);
            break;
        case Why::Floor:
            SotcStats::rb_ex_fb_floor.fetch_add(1, std::memory_order_relaxed);
            break;
        case Why::Cpu:
            SotcStats::rb_ex_fb_cpu.fetch_add(1, std::memory_order_relaxed);
            break;
        case Why::Owned:
            SotcStats::rb_ex_fb_inflight.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            SotcStats::rb_ex_fb_other.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        return ExactResult::Fallback;
    };
    if (readback.is_write || !arena) {
        return fallback(Why::Ok); // a write fault needs the exact bytes
    }
    const VAddr ask_lo = Common::AlignDown(readback.device_addr, Page);
    const VAddr ask_hi = Common::AlignUp(readback.device_addr + readback.size, Page);
    const VAddr win_lo = Common::AlignDown(ws, Page);
    const VAddr win_hi = Common::AlignUp(we, Page);
    const u64 limit = std::max(rbx_announced.load(std::memory_order_acquire),
                               rbx_seq_queried.load(std::memory_order_acquire));
    u64 free_max = 0;
    u64 pend_min = ~u64{0};
    const auto tick_done = [&](u64 t) {
        if (t <= free_max) {
            return true;
        }
        if (t >= pend_min) {
            return false;
        }
        const bool d = scheduler.IsFree(t);
        (d ? free_max : pend_min) = t;
        return d;
    };
    // Pages of guest readbacks in flight (and of claimed prefetches) belong to them.
    thread_local std::vector<std::pair<VAddr, VAddr>> owned;
    owned.clear();
    bool any_inflight = false;
    for (const auto& [lo, hi] : RbxOwnedAll()) {
        any_inflight |= lo < win_hi && win_lo < hi;
    }
    for (const AsyncReadback* other : inflight_readbacks) {
        if (!any_inflight) {
            break; // no readback in flight over the window
        }
        if (other->finished || (other->prefetch && !other->pf_claimed)) {
            continue; // an unclaimed prefetch is dirtied by the release, as for any owner of it
        }
        for (const auto& item : other->pending) {
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                if (range_addr < win_hi && win_lo < range_addr + range_size) {
                    owned.emplace_back(range_addr, range_addr + range_size);
                }
            }
        }
    }
    struct Piece {
        VAddr lo, hi;
    };
    thread_local std::vector<Piece> done_all, owed_all, pg_done, pg_owed;
    thread_local std::vector<std::pair<VAddr, u8>> pages; // page, 1: has done bytes, 2: only owed
    done_all.clear();
    owed_all.clear();
    pages.clear();
    u64 done_writer = 0;
    u64 owed_seq = 0;
    u64 owed_seq_min = ~u64{0};
    u64 owed_pages = 0;
    for (VAddr page = win_lo; page < win_hi; page += Page) {
        const bool asked = page >= ask_lo && page < ask_hi;
        if (!gpu_modified_ranges.Intersects(page, Page)) {
            if (asked) {
                return fallback(Why::Ok); // some other reason for the fault: the usual path
            }
            continue;
        }
        Why bad = Why::Ok;
        if (memory_tracker->IsRegionCpuModified(page, Page)) {
            bad = Why::Cpu;
        }
        if (bad == Why::Ok) {
            for (const auto& [lo, hi] : owned) {
                if (lo < page + Page && page < hi) {
                    bad = Why::Owned;
                    break;
                }
            }
        }
        pg_done.clear();
        pg_owed.clear();
        u64 page_seq = 0;
        u64 page_seq_min = ~u64{0};
        u64 page_writer = 0;
        if (bad == Why::Ok) {
            const auto absent = [&](VAddr a, VAddr b) {
                if (rbx_lost_tick == 0 || tick_done(rbx_lost_tick)) {
                    pg_done.push_back({a, b});
                    page_writer = std::max(page_writer, std::max(rbx_pruned_tick, rbx_lost_tick));
                } else {
                    bad = Why::Unknown;
                }
            };
            gpu_modified_ranges.ForEachInRange(page, Page, [&](VAddr lo, VAddr hi) {
                if (bad != Why::Ok) {
                    return;
                }
                VAddr cursor = lo;
                auto it = rbx_writers.upper_bound(lo);
                if (it != rbx_writers.begin() && std::prev(it)->second.hi > lo) {
                    --it;
                }
                for (; it != rbx_writers.end() && it->first < hi && bad == Why::Ok; ++it) {
                    const VAddr a = std::max(it->first, lo);
                    const VAddr b = std::min(it->second.hi, hi);
                    if (a > cursor) {
                        absent(cursor, a);
                    }
                    if (bad != Why::Ok) {
                        return;
                    }
                    const RbxWriter& w = it->second;
                    if (tick_done(w.tick)) {
                        pg_done.push_back({a, b});
                        page_writer = std::max(page_writer, w.tick);
                    } else if (w.seq == 0) {
                        bad = Why::Unknown; // pending, not from a hot window: no announce info
                    } else if (w.seq <= limit) {
                        bad = Why::Announced;
                    } else if (w.floor == 0 || w.floor <= limit) {
                        // An older version of these bytes, announced by a label, is not in guest
                        // memory yet: releasing would show a version older than the announced one.
                        bad = Why::Floor;
                    } else {
                        pg_owed.push_back({a, b});
                        page_seq = std::max(page_seq, w.seq);
                        page_seq_min = std::min(page_seq_min, w.seq);
                    }
                    cursor = std::max(cursor, b);
                }
                if (bad == Why::Ok && cursor < hi) {
                    absent(cursor, hi);
                }
            });
        }
        if (bad != Why::Ok) {
            if (asked) {
                return fallback(bad);
            }
            continue; // left as it is: protected and GPU-modified
        }
        for (const Piece& d : pg_done) {
            if (!done_all.empty() && done_all.back().hi == d.lo) {
                done_all.back().hi = d.hi;
            } else {
                done_all.push_back(d);
            }
        }
        for (const Piece& d : pg_owed) {
            if (!owed_all.empty() && owed_all.back().hi == d.lo) {
                owed_all.back().hi = d.hi;
            } else {
                owed_all.push_back(d);
            }
        }
        done_writer = std::max(done_writer, page_writer);
        owed_seq = std::max(owed_seq, page_seq);
        owed_seq_min = std::min(owed_seq_min, page_seq_min);
        owed_pages += !pg_owed.empty();
        pages.emplace_back(page, pg_done.empty() ? u8{2} : u8{1});
    }
    if (pages.empty()) {
        return fallback(Why::Ok);
    }
    // Everything above only looked. Caps first, then the state changes.
    if (!owed_all.empty() && rbx_live.size() >= RbxOwedMax) {
        return fallback(Why::Ok);
    }
    u64 owed_total = 0;
    ReadbackItem owed_item{arena, 0, 0, {}};
    owed_item.hot = true;
    owed_item.owed = true;
    for (const Piece& o : owed_all) {
        AddResidentCopies(owed_item, o.lo, o.hi, owed_total);
    }
    u64 owed_bytes = 0;
    if (!owed_item.copies.empty()) {
        owed_bytes = std::bit_ceil(std::max<u64>(owed_total, 64_KB));
        if (rbx_owed_bytes + owed_bytes > RbxOwedBudget) {
            return fallback(Why::Ok);
        }
    }
    u64 done_total = 0;
    ReadbackItem done_item{arena, ws, we - ws, {}};
    for (const Piece& d : done_all) {
        AddResidentCopies(done_item, d.lo, d.hi, done_total);
    }
    const bool download = !done_item.copies.empty();
    thread_local std::vector<std::pair<VAddr, VAddr>> release_now, download_runs;
    release_now.clear();
    download_runs.clear();
    const auto add_run = [](std::vector<std::pair<VAddr, VAddr>>& runs, VAddr lo, VAddr hi) {
        if (!runs.empty() && runs.back().second == lo) {
            runs.back().second = hi;
        } else {
            runs.emplace_back(lo, hi);
        }
    };
    for (const auto& [page, kind] : pages) {
        // Pages holding only owed bytes (or done bytes with nothing bound to copy) are released
        // now; the others by ApplyDownloads when the done copy has been applied.
        add_run(kind == 2 || !download ? release_now : download_runs, page, page + Page);
    }
    // The owed copy: recorded now, after every write recorded so far (their writers included).
    std::unique_ptr<AsyncReadback> owed;
    if (!owed_item.copies.empty()) {
        owed = std::make_unique<AsyncReadback>();
        AsyncReadback& o = *owed;
        const auto staging =
            staging_pool.Request(owed_total, VideoCore::MemoryType::HostCached, 0, true);
        VAddr lo_hull = ~VAddr{0};
        VAddr hi_hull = 0;
        for (auto& copy : owed_item.copies) {
            copy.dstOffset += staging.offset;
            const VAddr a = arena->cpu_addr + copy.srcOffset;
            owed_item.gpu_ranges.emplace_back(a, copy.size);
            lo_hull = std::min(lo_hull, a);
            hi_hull = std::max(hi_hull, a + copy.size);
        }
        owed_item.addr = lo_hull;
        owed_item.size = hi_hull - lo_hull;
        runtime.CopyBuffer(arena, staging.buffer, owed_item.copies);
        o.pending.push_back(std::move(owed_item));
        o.device_addr = lo_hull;
        o.size = hi_hull - lo_hull;
        o.arena = arena;
        o.window_start = lo_hull;
        o.window_size = hi_hull - lo_hull;
        o.tick = scheduler.CurrentTick();
        o.waiting = true;
        o.prefetch = true;
        o.pf_owed = true;
        o.pf_fresh_ok = true;
        o.pf_rbx = true;
        o.rbx_seq = owed_seq;
        o.rbx_seq_min = owed_seq_min;
        o.rbx_bytes = owed_bytes;
        o.pf_lo = lo_hull;
        o.pf_hi = hi_hull;
        o.gathered_bytes = owed_total;
        o.download_buffer = staging.buffer;
        o.download_size = staging.size;
        o.download_offset = staging.offset;
        o.download_mapped = staging.mapped;
    }
    if (owed) {
        // Registered before the pages become readable (a guest write fault onto them, checked
        // after the tracker looked at them, sees the owed bytes). A release never trims it.
        {
            std::scoped_lock lock{rbx_owed_mutex};
            for (const auto& item : owed->pending) {
                for (const auto& [a, size] : item.gpu_ranges) {
                    rbx_owed_ranges.Add(a, size);
                }
            }
        }
        rbx_owed_bytes += owed_bytes;
        rbx_live.push_back(std::move(owed));
        rbx_owed_count.store(static_cast<u32>(rbx_live.size()), std::memory_order_release);
        SotcStats::rb_ex_owed_objs.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rb_ex_pages_owed.fetch_add(owed_pages, std::memory_order_relaxed);
    }
    // Pages with nothing to download leave the GPU-modified set now (no readback is in flight
    // over them: checked above). Plain prefetches over them go dirty; owed downloads are not
    // touched by a release (keep_owed): they are about bytes, not pages.
    for (const auto& [lo, hi] : release_now) {
        {
            std::unique_lock gpu_modified_lock{gpu_modified_mutex};
            gpu_modified_ranges.Subtract(lo, hi - lo);
        }
        EraseGranuleSnapshots(lo, hi - lo);
        memory_tracker->UnmarkRegionAsGpuModified(lo, hi - lo, false);
        MarkInflightReadbacksDirty(lo, hi - lo, StaleReason::Unmap, true);
        if (!pf_live.empty()) {
            PrefetchMarkDirty(lo, hi - lo);
        }
    }
    if (!download) {
        SotcStats::rb_ex_served.fetch_add(1, std::memory_order_relaxed);
        return ExactResult::Served;
    }
    for (const auto& [lo, hi] : download_runs) {
        done_item.gpu_ranges.emplace_back(lo, hi - lo);
    }
    readback.pending.push_back(std::move(done_item));
    total_out = done_total;
    writer_out = done_writer;
    SotcStats::rb_ex_served_dl.fetch_add(1, std::memory_order_relaxed);
    return ExactResult::Download;
}

// sotc: SOTC_RB_GROUP=1 (off by default; needs SOTC_RB_OWNER=1 and the asynchronous readbacks,
// see rb_group_env; works with or without SOTC_RB_EXACT). A heavy frame made ~400 separate guest
// readbacks of the XPR visibility bitsets: small separate allocations, written by ~9 cull
// dispatches, each read through its own fault -> draw-thread command -> copy -> wait round trip
// (one frame: 442 readbacks, ~200 ms of waiting). The bytes one dispatch wrote are read together,
// so the first faults on them can bring all of them in one download.
//
// Groups. Every GPU-modified range a bind marks (AddGpuModified between BeginWriteScope and
// EndWriteScope, i.e. one draw or dispatch: Rasterizer::BindResources) joins that bind's group
// (RbgNoteWrite); EndWriteScope closes it (RbgCloseScope): it gets an id (bind order), the game
// frame and time, and a key (program hash, range count and sizes: the same bind in the next frame
// has the same key), and rbg_index maps each of its bytes to it (newest group wins, byte-exact,
// like rbx_writers). Groups older than SOTC's freshness bound (RbgMaxFrames game frames, or
// RbMatFreshMaxNs) are forgotten.
//
// Delivery (RbgAtFault, draw thread, from BeginAsyncReadbackImpl once the readback window is
// known, before anything of the readback is gathered or copied). A guest read fault whose asked
// bytes belong to a group counts a hit on it. A group is delivered on its second hit (the guest
// reads it in more than one window), or on its first one when its key was delivered before (the
// same bind of an earlier frame was read in pieces: the key is "hot"). Delivering records ONE
// group download: an AsyncReadback like a label download (prefetch, pf_group, hot items) whose
// copies take every GPU-modified byte of the group's 4 KiB pages, recorded in the tick being
// recorded, i.e. after every command recorded so far, the group's writer included. The fault's
// own readback follows in the same tick, so the guest's single wait (its tick, flushed by it, or
// by BeginAsyncReadback when the readback did not flush) covers the group download too, and the
// draw-thread step that finishes the readback applies it (ReadMemory -> RbgApplyReady): one
// download, one wait, one batch apply that releases every page of the group. A group download
// not complete then is applied at the next command boundary that sees it complete (PrefetchIssue
// -> RbgApplyReady), or by the next fault on its pages (PrefetchAtFault: applied when complete,
// else claimed and waited for as an owner, the usual SOTC_RB_OWNER rules). One delivery per group;
// later faults on it are counted as late (they raced the apply or hit a page left out).
//
// Pages left out of a group download (they stay GPU-modified and protected, untouched: their own
// faults read them back as before):
// - the readback window of the fault (the readback itself takes them; no page has two readers);
// - pages owned by a readback in flight (guest readbacks, prefetches, label downloads, owed or
//   other group downloads: RbxOwnedAll), the SOTC_RB_OWNER rule;
// - pages with the CPU bit (the guest's bytes win there; the items are hot as well, so a page
//   that gets the CPU bit before the apply keeps the guest's bytes: WriteUnlessCpuModified);
// - pages holding SOTC_RB_EXACT owed bytes (their owed download delivers them);
// - pages holding any GPU-modified byte not written by a fresh group (bytes left GPU-modified
//   from long ago, the reused-memory case that made SOTC_RB_HOT write garbage: never applied
//   without a fault on them);
// - past the caps (RbgRunsMax page runs, RbgBytesMax bytes, RbgLiveMax downloads, RbgBudget).
//
// Why the result is exact. A guest read fault on any page P of the group download, at any moment
// after it was recorded, would copy (after every command recorded up to then) every GPU-modified
// byte of P and apply it through ApplyDownloads. The group download copied every GPU-modified
// byte of P after every command recorded up to its own point, a point between commands (the
// group's bind was closed and its command recorded; a guest readback never runs inside a bind).
// So it holds what that fault would copy, unless P changed after the copy. Any such change is one
// of: a later arena write (AddGpuModified -> MarkInflightReadbacksDirty, RecordCurrentWrite ->
// PrefetchMarkDirty), an unmap, a CPU replacement, a release (DropGpuModified, TryReplaceFromCpu,
// ReleaseMemory, an SOTC_RB_EXACT release), or another readback applying P (StaleReason::
// Applied: DownloadMemory does not skip owned pages). Each one trims P (whole pages, RbgTrim) out
// of the group download before it can be applied: P never left the GPU-modified set, so its next
// fault reads it back as usual. A download with no page left is dirty and dropped, never applied,
// even if a guest waits for it (that guest faults again on its still protected page and is served
// the usual way; no second group download is made, so this terminates). What is applied is applied
// through the usual FinishAsyncReadback (BPE heap guard, stack ranges, ApplyDownloads, then
// SubtractDownloaded and the tracker unmark of only its own page runs), exactly like a guest
// readback of those pages at apply time. Bytes the guest never reads are harmless: they hold the
// bytes a readback of them would apply, and the arena keeps them (the pages are neither CPU- nor
// GPU-modified afterwards, so nothing is uploaded over them).
// SOTC_RB_EXACT: a group download never releases a page without its bytes and never exposes a
// version older than the newest one recorded, so the announced and floor rules (which are about
// releasing pending bytes WITHOUT their data) are not involved; labels may be written before or
// after it is applied: until it is applied its pages are protected and a read of them takes the
// exact path (owned pages: the usual readback, waiting for the group download as an owner). The
// label downloads and owed downloads own their pages, so the two never copy the same page.
// Residual: the command processor's SRT walkers read guest memory directly, as they always did.

void BufferCache::RbgNoteWrite(VAddr addr, u64 size) {
    if (size == 0) {
        return;
    }
    auto& ranges = rbg_open.ranges;
    if (!ranges.empty()) {
        auto& [last_addr, last_size] = ranges.back();
        if (last_addr == addr && last_size == size) {
            return; // the same binding twice
        }
        if (last_addr + last_size == addr) {
            last_size += size; // contiguous with the previous one
            rbg_open.bytes += size;
            return;
        }
    }
    if (ranges.size() >= RbgRangesMax) {
        rbg_open.oversize = true;
        return;
    }
    ranges.emplace_back(addr, size);
    rbg_open.bytes += size;
}

void BufferCache::RbgIndexInsert(VAddr lo, VAddr hi, u64 id) {
    // Newest group wins: entries overlapping [lo, hi) are cut back to their parts outside it.
    auto it = rbg_index.upper_bound(lo);
    if (it != rbg_index.begin() && std::prev(it)->second.hi > lo) {
        --it;
    }
    std::optional<std::pair<VAddr, RbgRef>> tail;
    while (it != rbg_index.end() && it->first < hi) {
        const VAddr entry_lo = it->first;
        const RbgRef entry = it->second;
        it = rbg_index.erase(it);
        if (entry_lo < lo) {
            rbg_index.emplace(entry_lo, RbgRef{lo, entry.id});
        }
        if (entry.hi > hi) {
            tail = std::pair<VAddr, RbgRef>{hi, entry};
        }
    }
    if (tail) {
        rbg_index.emplace(tail->first, tail->second);
    }
    rbg_index.emplace(lo, RbgRef{hi, id});
}

void BufferCache::RbgPruneIndex() {
    const u64 min_id = rbg_groups.empty() ? rbg_next_id : rbg_groups.front().id;
    std::erase_if(rbg_index, [min_id](const auto& entry) { return entry.second.id < min_id; });
    if (rbg_index.size() > RbgIndexMax) {
        rbg_index.clear(); // forgotten bytes belong to no group: their faults read back as usual
    }
}

void BufferCache::RbgCloseScope() {
    if (rbg_open.ranges.empty()) {
        return;
    }
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    const u64 now = SotcStats::NowNs();
    RbgGroup& group = rbg_open;
    group.id = rbg_next_id++;
    group.frame = frame;
    group.ns = now;
    group.hits = 0;
    group.delivered = false;
    u64 key = (SotcStats::current_program.load(std::memory_order_relaxed) *
               0x9E3779B97F4A7C15ULL) ^
              group.ranges.size();
    for (const auto& [addr, size] : group.ranges) {
        RbgIndexInsert(addr, addr + size, group.id);
        key = (key ^ size) * 0x100000001B3ULL;
    }
    group.key = key;
    // Groups too old to be delivered leave (ids, frames and times all grow along the deque).
    while (!rbg_groups.empty() &&
           (rbg_groups.size() >= RbgGroupsMax || rbg_groups.front().frame + RbgMaxFrames < frame ||
            now - rbg_groups.front().ns > RbMatFreshMaxNs)) {
        rbg_groups.pop_front();
    }
    rbg_groups.push_back(std::move(rbg_open));
    rbg_open = RbgGroup{};
    if ((++rbg_closed & 1023) == 0 || rbg_index.size() > RbgIndexMax) {
        RbgPruneIndex();
    }
}

u64 BufferCache::RbgAtFault(AsyncReadback& readback, VAddr ws, VAddr we) {
    if (rbg_index.empty() || rbg_groups.empty()) {
        return 0;
    }
    constexpr u64 Page = 4096;
    const VAddr ask_lo = readback.device_addr;
    const VAddr ask_hi = ask_lo + readback.size;
    // The newest group that wrote any asked byte.
    u64 id = 0;
    {
        auto it = rbg_index.upper_bound(ask_lo);
        if (it != rbg_index.begin() && std::prev(it)->second.hi > ask_lo) {
            --it;
        }
        for (; it != rbg_index.end() && it->first < ask_hi; ++it) {
            id = std::max(id, it->second.id);
        }
    }
    if (id == 0) {
        return 0;
    }
    const auto git = std::ranges::lower_bound(rbg_groups, id, {}, &RbgGroup::id);
    if (git == rbg_groups.end() || git->id != id) {
        return 0; // forgotten (too old)
    }
    RbgGroup& group = *git;
    ++group.hits;
    SotcStats::rbg_faults.fetch_add(1, std::memory_order_relaxed);
    if (group.delivered) {
        SotcStats::rbg_late.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const bool learned = rbg_hot_keys.contains(group.key);
    if (!learned && group.hits < 2) {
        SotcStats::rbg_fb_cold.fetch_add(1, std::memory_order_relaxed);
        return 0; // read in one window so far: nothing to bring along yet
    }
    const u64 frame = SotcStats::flip_count.load(std::memory_order_relaxed);
    const u64 now = SotcStats::NowNs();
    const auto too_old = [&](const RbgGroup& g) {
        return g.frame + RbgMaxFrames < frame || now - g.ns > RbMatFreshMaxNs;
    };
    if (too_old(group)) {
        group.delivered = true;
        SotcStats::rbg_fb_old.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (!learned) {
        if (rbg_hot_keys.size() >= RbgKeysMax) {
            rbg_hot_keys.clear();
        }
        rbg_hot_keys.emplace(group.key, 1);
        SotcStats::rbg_learned.fetch_add(1, std::memory_order_relaxed);
    }
    if (group.oversize) {
        group.delivered = true;
        SotcStats::rbg_fb_size.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (rbg_live >= RbgLiveMax) {
        SotcStats::rbg_fb_cap.fetch_add(1, std::memory_order_relaxed);
        return 0; // a later fault on the group tries again
    }
    // Page runs of the group's ranges, sorted and merged.
    thread_local std::vector<std::pair<VAddr, VAddr>> runs, keep;
    runs.clear();
    keep.clear();
    for (const auto& [addr, size] : group.ranges) {
        runs.emplace_back(Common::AlignDown(addr, Page), Common::AlignUp(addr + size, Page));
    }
    std::ranges::sort(runs);
    size_t merged = 0;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (merged && runs[i].first <= runs[merged - 1].second) {
            runs[merged - 1].second = std::max(runs[merged - 1].second, runs[i].second);
        } else {
            runs[merged++] = runs[i];
        }
    }
    runs.resize(merged);
    // Groups still fresh: every GPU-modified byte of a delivered page must belong to one of them.
    const auto first_fresh = std::ranges::partition_point(rbg_groups, too_old);
    const u64 fresh_id = first_fresh == rbg_groups.end() ? rbg_next_id : first_fresh->id;
    const auto fresh = [&](VAddr lo, VAddr hi) {
        auto it = rbg_index.upper_bound(lo);
        if (it != rbg_index.begin() && std::prev(it)->second.hi > lo) {
            --it;
        }
        VAddr cursor = lo;
        for (; it != rbg_index.end() && it->first < hi; ++it) {
            if (it->first > cursor || it->second.id < fresh_id) {
                return false;
            }
            cursor = std::max(cursor, it->second.hi);
            if (cursor >= hi) {
                return true;
            }
        }
        return cursor >= hi;
    };
    const VAddr win_lo = Common::AlignDown(ws, Page);
    const VAddr win_hi = Common::AlignUp(we, Page);
    const std::vector<std::pair<VAddr, VAddr>>& owned = RbxOwnedAll();
    size_t own_i = 0;
    u64 skip_window = 0, skip_owned = 0, skip_cpu = 0, skip_owed = 0, skip_stale = 0;
    for (const auto& [run_lo, run_hi] : runs) {
        const bool any_cpu = memory_tracker->IsRegionCpuModified(run_lo, run_hi - run_lo);
        for (VAddr page = run_lo; page < run_hi; page += Page) {
            if (page >= win_lo && page < win_hi) {
                ++skip_window;
                continue;
            }
            while (own_i < owned.size() && owned[own_i].second <= page) {
                ++own_i;
            }
            if (own_i < owned.size() && owned[own_i].first < page + Page) {
                ++skip_owned;
                continue;
            }
            if (!gpu_modified_ranges.Intersects(page, Page)) {
                continue; // nothing of it left to deliver
            }
            if (any_cpu && memory_tracker->IsRegionCpuModified(page, Page)) {
                ++skip_cpu;
                continue;
            }
            if (RbxOwedIntersects(page, Page)) {
                ++skip_owed;
                continue;
            }
            bool ok = true;
            gpu_modified_ranges.ForEachInRange(page, Page,
                                               [&](VAddr lo, VAddr hi) { ok = ok && fresh(lo, hi); });
            if (!ok) {
                ++skip_stale;
                continue;
            }
            if (!keep.empty() && keep.back().second == page) {
                keep.back().second = page + Page;
            } else {
                keep.emplace_back(page, page + Page);
            }
        }
    }
    SotcStats::rbg_skip_window.fetch_add(skip_window, std::memory_order_relaxed);
    SotcStats::rbg_skip_owned.fetch_add(skip_owned, std::memory_order_relaxed);
    SotcStats::rbg_skip_cpu.fetch_add(skip_cpu, std::memory_order_relaxed);
    SotcStats::rbg_skip_owed.fetch_add(skip_owed, std::memory_order_relaxed);
    SotcStats::rbg_skip_stale.fetch_add(skip_stale, std::memory_order_relaxed);
    // Everything above only looked. The gather is read-only too (subtract = false).
    auto group_download = std::make_unique<AsyncReadback>();
    AsyncReadback& p = *group_download;
    u64 total = 0;
    size_t runs_kept = 0;
    bool capped = false;
    const Buffer* first_arena = nullptr;
    rb_owned = &owned;
    rb_gather_no_hot = true;
    for (const auto& [lo_in, hi_in] : keep) {
        if (runs_kept >= RbgRunsMax) {
            capped = true;
            break;
        }
        const Buffer* a = address_space[lo_in >> ARENA_PAGE_BITS];
        if (!a) {
            continue; // outside every arena: no GPU data
        }
        const VAddr lo = std::max<VAddr>(lo_in, a->cpu_addr);
        const VAddr hi = std::min<VAddr>(hi_in, a->cpu_addr + a->size_bytes);
        if (lo >= hi) {
            continue;
        }
        const size_t before = p.pending.size();
        const u64 got = GatherDownloads(a, lo, hi - lo, p.pending, false);
        if (total + got > RbgBytesMax) {
            p.pending.erase(p.pending.begin() + static_cast<std::ptrdiff_t>(before),
                            p.pending.end());
            capped = true;
            break;
        }
        for (size_t k = before; k < p.pending.size(); ++k) {
            for (auto& copy : p.pending[k].copies) {
                copy.dstOffset += total; // GatherDownloads counts from 0 on every call
            }
        }
        if (got != 0) {
            total += got;
            ++runs_kept;
            first_arena = first_arena ? first_arena : a;
        }
    }
    rb_owned = nullptr;
    rb_gather_no_hot = false;
    if (capped) {
        SotcStats::rbg_fb_size.fetch_add(1, std::memory_order_relaxed); // partial delivery
    }
    if (total == 0) {
        group.delivered = true;
        SotcStats::rbg_fb_empty.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const u64 bytes = std::bit_ceil(std::max<u64>(total, 64_KB));
    if (rbg_bytes_held + bytes > RbgBudget) {
        SotcStats::rbg_fb_cap.fetch_add(1, std::memory_order_relaxed);
        return 0; // a later fault on the group tries again
    }
    // Held until it is applied or dropped (FinishAsyncReadback / PrefetchDrop).
    const auto download = staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
    VAddr lo_hull = ~VAddr{0};
    VAddr hi_hull = 0;
    for (auto& item : p.pending) {
        item.hot = true; // a page that gets the CPU bit before the apply keeps the guest's bytes
        for (auto& copy : item.copies) {
            copy.dstOffset += download.offset;
        }
        runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        for (const auto& [range_addr, range_size] : item.gpu_ranges) {
            lo_hull = std::min(lo_hull, range_addr);
            hi_hull = std::max(hi_hull, range_addr + range_size);
        }
    }
    // Whole pages: the apply unmarks whole pages, so every bound a later write is compared with
    // (pf_lo/pf_hi, the global prefetch hull, the window) covers the whole pages, or a write to
    // another byte of a kept page would skip the trim and lose its GPU-modified state.
    lo_hull = Common::AlignDown(lo_hull, Page);
    hi_hull = Common::AlignUp(hi_hull, Page);
    p.device_addr = lo_hull;
    p.size = hi_hull - lo_hull;
    p.arena = first_arena;
    p.window_start = lo_hull;
    p.window_size = hi_hull - lo_hull;
    p.tick = scheduler.CurrentTick(); // after the copies: they are in this tick or an earlier one
    p.waiting = true;
    p.prefetch = true;
    p.pf_group = true;
    p.pf_fresh_ok = true; // fresh by construction (see above)
    p.pf_lo = lo_hull;
    p.pf_hi = hi_hull;
    p.pf_bytes = 0; // not counted against the prefetch budget (RbgBudget instead)
    p.group_bytes = bytes;
    p.gathered_bytes = total;
    p.download_buffer = download.buffer;
    p.download_size = download.size;
    p.download_offset = download.offset;
    p.download_mapped = download.mapped;
    pf_lo = pf_live.empty() ? lo_hull : std::min(pf_lo, lo_hull);
    pf_hi = pf_live.empty() ? hi_hull : std::max(pf_hi, hi_hull);
    rbg_bytes_held += bytes;
    ++rbg_live;
    inflight_readbacks.push_back(&p);
    ++inflight_version;
    pf_live.push_back(std::move(group_download));
    group.delivered = true;
    SotcStats::rbg_delivered.fetch_add(1, std::memory_order_relaxed);
    (learned && group.hits == 1 ? SotcStats::rbg_first_hit : SotcStats::rbg_second_hit)
        .fetch_add(1, std::memory_order_relaxed);
    // The group's ranges with a page in the download (each would have been a fault of its own if
    // the guest reads it: the "faults avoided" estimate is these minus the late faults).
    u64 ranges_in = 0;
    for (const auto& [addr, size] : group.ranges) {
        const VAddr a = Common::AlignDown(addr, Page);
        const VAddr b = Common::AlignUp(addr + size, Page);
        const auto it = std::ranges::upper_bound(keep, a, {}, &std::pair<VAddr, VAddr>::second);
        ranges_in += it != keep.end() && it->first < b && p.pf_lo < b && a < p.pf_hi;
    }
    SotcStats::rbg_ranges.fetch_add(ranges_in, std::memory_order_relaxed);
    SotcStats::rbg_runs.fetch_add(runs_kept, std::memory_order_relaxed);
    SotcStats::rbg_bytes.fetch_add(total, std::memory_order_relaxed);
    u64 seen = SotcStats::rbg_bytes_max.load(std::memory_order_relaxed);
    while (seen < total && !SotcStats::rbg_bytes_max.compare_exchange_weak(
                               seen, total, std::memory_order_relaxed)) {
    }
    return p.tick;
}

void BufferCache::RbgTrim(AsyncReadback& g, VAddr lo, VAddr hi) {
    // Whole pages: a page applied is unmarked whole, so a page any byte of which changed after
    // the copy must not be applied at all. Rounded before any test (review of 2026-10-28: a write
    // to another byte of a kept page skipped the trim against the byte bounds).
    constexpr u64 Page = 4096;
    lo = Common::AlignDown(lo, Page);
    hi = Common::AlignUp(hi, Page);
    if (g.finished || g.dirty || lo >= hi || !(lo < g.pf_hi && g.pf_lo < hi)) {
        return;
    }
    bool any_left = false;
    bool touched = false;
    u64 removed = 0;
    for (auto& item : g.pending) {
        bool hit = false;
        for (const auto& [range_addr, range_size] : item.gpu_ranges) {
            hit |= range_addr < hi && lo < range_addr + range_size;
        }
        if (!hit) {
            any_left |= !item.copies.empty();
            continue;
        }
        touched = true;
        boost::container::small_vector<vk::BufferCopy, 4> copies;
        for (const auto& copy : item.copies) {
            const VAddr a = item.arena->cpu_addr + copy.srcOffset;
            const VAddr b = a + copy.size;
            if (b <= lo || a >= hi) {
                copies.push_back(copy);
                continue;
            }
            if (a < lo) {
                copies.push_back(vk::BufferCopy{copy.srcOffset, copy.dstOffset, lo - a});
            }
            if (b > hi) {
                copies.push_back(
                    vk::BufferCopy{copy.srcOffset + (hi - a), copy.dstOffset + (hi - a), b - hi});
            }
            removed += std::min(b, hi) - std::max(a, lo);
        }
        boost::container::small_vector<std::pair<VAddr, u64>, 4> ranges;
        for (const auto& [range_addr, range_size] : item.gpu_ranges) {
            const VAddr a = range_addr;
            const VAddr b = a + range_size;
            if (b <= lo || a >= hi) {
                ranges.emplace_back(a, range_size);
                continue;
            }
            if (a < lo) {
                ranges.emplace_back(a, lo - a);
            }
            if (b > hi) {
                ranges.emplace_back(hi, b - hi);
            }
        }
        item.copies = std::move(copies);
        item.gpu_ranges = std::move(ranges);
        any_left |= !item.copies.empty();
    }
    SotcStats::rbg_trimmed.fetch_add(removed, std::memory_order_relaxed);
    if (touched) {
        ++inflight_version; // its gpu_ranges changed: RbxOwnedAll rebuilds its page runs
    }
    if (!any_left) {
        g.dirty = true; // nothing left to apply: dropped, never applied
    }
}

void BufferCache::RbgApplyReady(bool throttled) {
    if (rbg_live == 0) {
        return;
    }
    if (throttled) {
        const u64 now = SotcStats::NowNs();
        if (now - rbg_apply_ns < rb_mat_ns) {
            return;
        }
        rbg_apply_ns = now;
    }
    bool changed = false;
    for (size_t i = 0; i < pf_live.size();) {
        AsyncReadback& p = *pf_live[i];
        if (!p.pf_group) {
            ++i;
            continue;
        }
        if (p.finished || p.dirty) {
            PrefetchDrop(i); // applied by a fault (PrefetchAtFault, an owner wait), or dirty
            changed = true;
            continue;
        }
        if (!IsReadbackComplete(p)) {
            ++i;
            continue;
        }
        FinishAsyncReadback(p); // erases it from inflight_readbacks, frees its staging
        PrefetchDrop(i);
        changed = true;
    }
    if (changed) {
        PrefetchUpdateHull();
    }
}

void BufferCache::FinishAsyncReadback(AsyncReadback& readback) {
    RbxDepth rbx_depth_guard{rbx_depth}; // sotc: SOTC_RB_EXACT: see BeginAsyncReadback
    if (readback.dependent) {
        // Finish the readbacks this one waited for (owners still blocked keep them alive).
        static const bool rb_owner = [] {
            const char* v = std::getenv("SOTC_RB_OWNER");
            return v && v[0] == '1';
        }();
        if (rb_owner) {
            FinishOwners(readback.device_addr, readback.size, !readback.is_write);
        } else {
            while (AsyncReadback* other =
                       FindInflightReadback(readback.device_addr, readback.size)) {
                // sotc: SOTC_RB_ASYNC_AHEAD: an ahead readback is not covered by the ticks this
                // one waited for; one still copying is left to its own guest thread (its pages
                // stay GPU-modified, a read of them faults again).
                if ((other->ahead_value != 0 || other->xfer_value != 0) &&
                    !IsReadbackComplete(*other)) {
                    break;
                }
                FinishAsyncReadback(*other);
            }
        }
        if (readback.is_write) {
            MarkWrittenByGuest(readback.device_addr, readback.size);
        }
        return;
    }
    if (readback.finished) {
        if (readback.is_write) {
            MarkWrittenByGuest(readback.device_addr, readback.size);
        }
        return;
    }
    readback.finished = true;
    std::erase(inflight_readbacks, &readback);
    ++inflight_version;
    const Vulkan::StagingBufferRef download{
        .buffer = readback.download_buffer,
        .offset = readback.download_offset,
        .size = readback.download_size,
        .mapped = const_cast<u8*>(readback.download_mapped),
        .type = VideoCore::MemoryType::HostCached,
    };
    if (readback.pf_group && readback.dirty) {
        // sotc: SOTC_RB_GROUP: dirty means every page was trimmed away: nothing to apply, and the
        // pages never left the GPU-modified set (their own faults read them back as usual).
        staging_pool.FreeDeferred(download);
        SotcStats::rbg_fb_dirty.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (readback.prefetch && readback.dirty) {
        // sotc: SOTC_RB_PREFETCH: a stale prefetch is reached here only as an owner finished by
        // a guest readback that waited for it (FinishReadyOwners; PrefetchAtFault applies clean
        // ones only). Its data is not applied. sotc: it is read back again, as a stale guest
        // owner is: leaving its pages to "the next fault" did not guarantee progress, since a
        // window the GPU rewrites every tick has a newer prefetch in flight at that fault too,
        // which goes stale the same way before the guest finishes it (review of 2026-10-28).
        staging_pool.FreeDeferred(download);
        SotcStats::rb_pf_stale.fetch_add(1, std::memory_order_relaxed);
        DownloadMemory(readback.arena, readback.window_start, readback.window_size);
        return;
    }
    if (readback.dirty) {
        // A GPU write recorded after the copies (or another readback applied first and the
        // guest may have written since): the data may be stale. The ranges never left the
        // GPU-modified set; read the window back the usual way.
        staging_pool.FreeDeferred(download);
        SotcStats::readback_dirty.fetch_add(1, std::memory_order_relaxed);
        // sotc: SOTC_RB_OWNER: the owners of part of the range apply theirs first (the download
        // below takes the whole window, their pages again included, newest data last).
        if (readback.has_owners) {
            FinishOwners(readback.device_addr, readback.size, !readback.is_write);
        }
        DownloadMemory(readback.arena, readback.window_start, readback.window_size);
    } else {
        download.buffer->Invalidate(download.offset, download.size);
        // sotc: SOTC_RB_KEEP_LATER: the parts a later GPU write covers stay GPU-modified.
        auto& keep = readback.later_gpu;
        if (!keep.empty()) {
            std::ranges::sort(keep);
            SotcStats::readback_kept_later.fetch_add(1, std::memory_order_relaxed);
        }
        // sotc: the ranges leave the GPU-modified set only once the backing holds their data:
        // the command processor's clean reads (ReadClean) would read the stale backing before.
        ApplyDownloads(readback.pending, download.mapped, download.offset, keep);
        SubtractDownloaded(readback.pending);
        if (readback.pf_group) { // sotc: SOTC_RB_GROUP
            u64 bytes = 0;
            for (const auto& item : readback.pending) {
                for (const auto& copy : item.copies) {
                    bytes += copy.size;
                }
            }
            SotcStats::rbg_applied.fetch_add(1, std::memory_order_relaxed);
            SotcStats::rbg_applied_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        if (!keep.empty()) {
            std::unique_lock gpu_modified_lock{gpu_modified_mutex};
            for (const auto& [keep_start, keep_end] : keep) {
                gpu_modified_ranges.Add(keep_start, keep_end - keep_start);
            }
        }
        staging_pool.FreeDeferred(download);
        // sotc: SOTC_RB_OWNER: the part of the asked range other readbacks were downloading.
        if (readback.has_owners) {
            FinishOwners(readback.device_addr, readback.size, !readback.is_write);
        }
    }
    if (readback.pf_rbx) {
        RbxUnregister(readback); // sotc: SOTC_RB_EXACT
    }
    if (readback.is_write) {
        MarkWrittenByGuest(readback.device_addr, readback.size);
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    RbxDepth rbx_depth_guard{rbx_depth}; // sotc: SOTC_RB_EXACT: see BeginAsyncReadback
    SotcStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    const SotcStats::ScopedTimer readback_timer{SotcStats::readback_ns};
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameReadback};
    boost::container::small_vector<ReadbackItem, 16> pending;
    const u64 total_size_bytes = GatherDownloads(arena, device_addr, size, pending);
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    // Was the data written by work already submitted, or by the command buffer being built?
    RollCurrentWrites();
    bool written_by_current = false;
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            written_by_current |=
                current_writes.Intersects(item.arena->cpu_addr + copy.srcOffset, copy.size);
        }
    }
    for (auto& item : pending) {
        for (auto& copy : item.copies) {
            copy.dstOffset += download.offset;
        }
    }
    // Readback ahead (idea from Pink-shadPS4, luizgustavs): when the data was written by work
    // already submitted, copy it in a command buffer of its own instead of submitting and waiting
    // for everything recorded so far. SOTC_RB_AHEAD=0/1 forces it; by default on NVIDIA and AMD
    // (Pink-shadPS4 advises turning it off on AMD; measured right on a Radeon RX 6650 XT).
    static const bool ahead_enabled = [this] {
        const char* v = std::getenv("SOTC_RB_AHEAD");
        constexpr u32 VendorNvidia = 0x10DE;
        constexpr u32 VendorAmd = 0x1002;
        return v ? std::atoi(v) != 0
                 : instance.GetVendorID() == VendorNvidia || instance.GetVendorID() == VendorAmd;
    }();
    if (ahead_enabled && !written_by_current && pending_binds.empty()) {
        const auto cmdbuf = scheduler.BeginAhead();
        const vk::MemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &pre_barrier,
        });
        for (const auto& item : pending) {
            cmdbuf.copyBuffer(item.arena->Handle(), download.buffer->Handle(), item.copies);
        }
        const vk::MemoryBarrier2 host_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &host_barrier,
        });
        scheduler.SubmitAheadAndWait();
        SotcStats::readback_ahead.fetch_add(1, std::memory_order_relaxed);
    } else {
        SotcStats::readback_finish.fetch_add(1, std::memory_order_relaxed);
        // Did the guest ask for bytes the command buffer being built writes, or only a neighbour
        // in the window (or a hot region) did?
        if (fault_size != 0 && written_by_current &&
            current_writes.Intersects(fault_addr, fault_size)) {
            SotcStats::readback_finish_asked.fetch_add(1, std::memory_order_relaxed);
        }
        for (const auto& item : pending) {
            runtime.CopyBuffer(item.arena, download.buffer, item.copies);
        }
        scheduler.FinishHostRead();
    }

    download.buffer->Invalidate(download.offset, download.size);
    // sotc: GPU output of dispatches on stale descriptors must not overwrite the allocator
    // metadata of the game's BPE heap (bpe_heap_guard.cpp, from Pink-shadPS4; the loader finds
    // the heap, SOTC_HEAP_GUARD=0 turns it off).
    // The free tree is walked again only when it changed: its root (heap+0x80), node count
    // (heap+0xa8, kept by the game's tree code) and pending block (heap+0xc0) sign it. Block
    // headers are followed through the physical chain on every write anyway. Only the command
    // processor thread downloads memory.
    ApplyDownloads(pending, download.mapped, download.offset);
    SubtractDownloaded(pending); // sotc: after the backing holds the data (see FinishAsyncReadback)
}

void BufferCache::SubtractDownloaded(std::span<const ReadbackItem> items) {
    std::unique_lock gpu_modified_lock{gpu_modified_mutex};
    for (const auto& item : items) {
        if (item.owed) {
            continue; // sotc: SOTC_RB_EXACT: already out of the set
        }
        for (const auto& [address, range_size] : item.gpu_ranges) {
            gpu_modified_ranges.Subtract(address, range_size);
        }
    }
}

u64 BufferCache::GatherDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                                 boost::container::small_vector<ReadbackItem, 16>& pending,
                                 bool subtract) {
    // A readback costs a full CPU/GPU sync. Games tend to read back the same few regions every
    // frame, one after another, so while the GPU is idle anyway also bring along the other
    // regions read back recently ("hot" readbacks) that the GPU has modified since. The reads
    // that follow then find their data already on the CPU instead of each needing a sync.
    // SOTC_RB_HOT=0 (default): off, 1: same arena and small regions only, 2: any existing arena,
    // any size. sotc: off since 22 Oct. Those speculative copies land on memory the game may have
    // reused since: rainbow, striped garbage on the Shrine's stones after loading a save again, and
    // heap free lists corrupted (0x49500, the game closing "out of nowhere", once after 23 min idle
    // in the title demo); with them off the user's recipe (a new area, then reload the save, 3
    // times) stayed clean. Attract demo, full settings: 54.8 / 36.4 FPS off against ~57 / 40 with
    // mode 2. Keeping the hot data aside until the guest reads it (no speculative writes) cost more
    // than it saved (42.7 / 23.5): copying it on the command processor thread.
    static const int hot_mode = [] {
        const char* v = std::getenv("SOTC_RB_HOT");
        return v ? std::atoi(v) : 0;
    }();
    // sotc: SOTC_RB_PREFETCH: a prefetch copies its own window only (rb_gather_no_hot).
    const bool hot_enabled = hot_mode != 0 && !rb_gather_no_hot;
    const auto now = std::chrono::steady_clock::now();
    struct Region {
        const Buffer* arena;
        VAddr addr;
        u64 size;
    };
    boost::container::small_vector<Region, 16> regions;
    regions.push_back({arena, device_addr, size});
    if (hot_enabled) {
        bool known = false;
        for (auto& hot : hot_readbacks) {
            if (hot.addr == device_addr && hot.size == size) {
                hot.last_request = now;
                known = true;
                continue;
            }
            if (now - hot.last_request > HotReadbackLifetime) {
                continue;
            }
            // Stay inside the arena of this readback: never create or migrate arenas here.
            const u64 first_page = hot.addr >> ARENA_PAGE_BITS;
            const u64 last_page = (hot.addr + hot.size - 1) >> ARENA_PAGE_BITS;
            const Buffer* hot_arena = address_space[first_page];
            if (!hot_arena || address_space[last_page] != hot_arena ||
                (hot_mode == 1 && hot_arena != arena)) {
                continue;
            }
            regions.push_back({hot_arena, hot.addr, hot.size});
        }
        // sotc: SOTC_RB_NARROW: the reads one wide window covered ask for several narrow ones;
        // room for up to 4 times as many.
        const size_t max_hot =
            rb_narrow_window ? MaxHotReadbacks * std::min<u64>(4, 512_KB / rb_narrow_window)
                             : MaxHotReadbacks;
        if (!known) {
            if (hot_readbacks.size() < max_hot) {
                hot_readbacks.push_back({device_addr, size, now});
            } else {
                auto oldest = std::ranges::min_element(hot_readbacks, {}, &HotReadback::last_request);
                *oldest = {device_addr, size, now};
            }
        }
        if (rb_narrow_window && SotcStats::kFpsLog) {
            SotcStats::rb_hot_entries.store(hot_readbacks.size(), std::memory_order_relaxed);
        }
    }

    u64 total_size_bytes = 0;
    for (size_t region_index = 0; region_index < regions.size(); ++region_index) {
        const Region& region = regions[region_index];
        // sotc: a hot region (brought along, not asked for) skips the pages whose CPU bit is set:
        // the guest may have written them since (repeat-fault grace leaves hot pages writable),
        // and its bytes are the right ones. Applying the GPU copy there wrote what the GPU drew
        // before the game reused the memory over new textures (rainbow garbage on the Shrine's
        // stones after a save reload, 22 Oct; SOTC_RB_HOT=0 made it go away).
        const bool hot_region = region_index > 0;
        ReadbackItem item{region.arena, region.addr, region.size, {}};
        item.hot = hot_region;
        const u64 total_before = total_size_bytes;
        const VAddr arena_base = region.arena->cpu_addr;
        memory_tracker->ForEachDownloadRange<false>(
            region.addr, region.size, [&](u64 address, u64 range_size) {
                const auto add_download = [&](VAddr start, VAddr end) {
                    // Only copy what is still bound; a released range must never be read.
                    resident_ranges.ForEachInRange(
                        start >> block_shift, ((end - 1) >> block_shift) + 1,
                        [&](const Backing& backing) {
                            const VAddr a = std::max<VAddr>(start, backing.start << block_shift);
                            const VAddr b = std::min<VAddr>(end, backing.end << block_shift);
                            if (a >= b) {
                                return;
                            }
                            item.copies.push_back(vk::BufferCopy{
                                .srcOffset = a - arena_base,
                                .dstOffset = total_size_bytes,
                                .size = b - a,
                            });
                            // Align up to avoid cache conflicts
                            constexpr u64 align = 64ULL;
                            total_size_bytes += Common::AlignUp(b - a, align);
                        });
                };
                // sotc: SOTC_RB_OWNER: pages a readback in flight downloads are left to it.
                const auto unowned = [&](VAddr lo, VAddr hi, auto&& fn) {
                    if (!rb_owned) {
                        fn(lo, hi);
                        return;
                    }
                    VAddr cursor = lo;
                    for (const auto& [own_start, own_end] : *rb_owned) {
                        if (own_end <= cursor || own_start >= hi) {
                            continue;
                        }
                        if (own_start > cursor) {
                            fn(cursor, own_start);
                        }
                        SotcStats::rb_owner_skipped_bytes.fetch_add(
                            std::min(own_end, hi) - std::max(own_start, cursor),
                            std::memory_order_relaxed);
                        cursor = std::max(cursor, own_end);
                    }
                    if (cursor < hi) {
                        fn(cursor, hi);
                    }
                };
                if (!hot_region) {
                    unowned(address, address + range_size, [&](VAddr lo, VAddr hi) {
                        gpu_modified_ranges.ForEachInRange(lo, hi - lo, add_download);
                        item.gpu_ranges.emplace_back(lo, hi - lo);
                    });
                    return;
                }
                // Page by page: runs of pages the CPU has not touched.
                VAddr run_start = address;
                const VAddr range_end = address + range_size;
                const auto flush_run = [&](VAddr end) {
                    if (end > run_start) {
                        unowned(run_start, end, [&](VAddr lo, VAddr hi) {
                            gpu_modified_ranges.ForEachInRange(lo, hi - lo, add_download);
                            item.gpu_ranges.emplace_back(lo, hi - lo);
                        });
                    }
                };
                for (VAddr page = address; page < range_end;) {
                    const VAddr next = std::min(Common::AlignDown(page, 4_KB) + 4_KB, range_end);
                    if (memory_tracker->IsRegionCpuModified(page, next - page)) {
                        flush_run(page);
                        run_start = next;
                        SotcStats::rb_hot_cpu_skipped.fetch_add(next - page, std::memory_order_relaxed);
                    }
                    page = next;
                }
                flush_run(range_end);
            });
        if (item.copies.empty()) {
            if (subtract) {
                SubtractDownloaded({&item, 1}); // nothing bound to read back
            }
            continue;
        }
        // Large modified regions (render targets and the like) are left to their own readback.
        constexpr u64 MaxHotBytes = 64_KB;
        u64 item_bytes = 0;
        for (const auto& copy : item.copies) {
            item_bytes += copy.size;
        }
        if (hot_mode == 1 && !pending.empty() && item_bytes > MaxHotBytes) {
            // Not downloaded now: its ranges stay in the modified set.
            total_size_bytes = total_before;
            continue;
        }
        pending.push_back(std::move(item));
    }
    return total_size_bytes;
}

void BufferCache::ApplyDownloads(std::span<const ReadbackItem> pending, const u8* mapped,
                                 u64 mapped_offset,
                                 std::span<const std::pair<VAddr, VAddr>> keep_gpu) {
    // sotc (#146): every heap of the guarded heap's class, not only the one picked at boot: the
    // heaps a world creates when it loads (the last colossus' area) crashed the same way
    // (eboot+0x49500, freed blocks overwritten). They come from the engine's heap table.
    // sotc: SOTC_HEAP_FAST (default on): a write into an in-use block a guard found before, or
    // above the in-use, size-0 block that ends the heap's block chain (BpeUsedBlocks), skips the
    // guard, so a guard (its build walks the whole free tree) is built only for the other writes.
    // SOTC_HEAP_FAST=0 builds every touched heap's guard for each readback batch.
    static const bool heap_fast = [] {
        const char* v = std::getenv("SOTC_HEAP_FAST");
        return !v || std::atoi(v) != 0;
    }();
    struct GuardSlot {
        VAddr heap = 0;
        std::array<u64, 3> signature{};
        std::chrono::steady_clock::time_point built{};
        std::optional<BpeHeapGuard> guard;
        std::optional<BpeUsedBlocks> used;
    };
    static std::array<GuardSlot, 8> slots;
    struct Active {
        GuardSlot* slot;
        VAddr base;
        u64 size;
        bool checked; ///< the guard was checked against the heap's signature in this batch
    };
    std::array<Active, 8> active{};
    size_t active_count = 0;
    std::array<VAddr, 8> heaps{};
    size_t heap_count = 0;
    if (const VAddr chosen = runtime_sotc_heap(); chosen != 0) {
        heaps[heap_count++] = chosen;
        const u64 heap_class = *std::bit_cast<const u64*>(chosen);
        if (const VAddr table = runtime_sotc_heap_table(); table != 0) {
            for (u32 id = 1; id < 48 && heap_count < heaps.size(); ++id) {
                const VAddr other = std::bit_cast<const VAddr*>(table)[id];
                if (other == 0 || other == chosen || !(other >= 0x10000 && other < 0x10000000000ULL)) {
                    continue;
                }
                const bool in_image = other - table < 0x20000000;
                if (!in_image && !memory->GetBackingPointer(other)) {
                    continue;
                }
                if (*std::bit_cast<const u64*>(other) == heap_class) {
                    heaps[heap_count++] = other;
                }
            }
        }
    }
    const auto find_slot = [&](VAddr heap) -> GuardSlot* {
        for (auto& candidate : slots) {
            if (candidate.heap == heap) {
                return &candidate;
            }
        }
        for (auto& candidate : slots) { // a free slot, else one of a heap not seen now
            if (candidate.heap == 0 ||
                std::find(heaps.begin(), heaps.begin() + heap_count, candidate.heap) ==
                    heaps.begin() + heap_count) {
                candidate.heap = heap;
                candidate.guard.reset();
                candidate.used.reset();
                return &candidate;
            }
        }
        return nullptr;
    };
    const auto signature_of = [](VAddr heap) -> std::array<u64, 3> {
        return {*std::bit_cast<const u64*>(heap + 0x80), *std::bit_cast<const u64*>(heap + 0xa8),
                *std::bit_cast<const u64*>(heap + 0xc0)};
    };
    for (size_t h = 0; h < heap_count; ++h) {
        const VAddr heap = heaps[h];
        // Only readbacks into the heap's own memory need it: building the guard walks the free
        // tree, too costly for every readback. The heap picked at boot lives in [heap+0x28],
        // [heap+0x30] bytes (flexible memory). sotc (#146): the heaps a world creates (the last
        // colossus' area, after the ending) grow in 1 MiB pieces over direct memory and share that
        // +0x28 pointer, so readbacks into them were never guarded and the ending still crashed
        // in their free lists (eboot+0x49500): their range comes from their own guard's free
        // blocks, refreshed every 2 s while their free tree changes, and after 250 ms already when
        // a readback lands outside the range (a piece the heap may have added since).
        VAddr heap_base = *std::bit_cast<const VAddr*>(heap + 0x28);
        u64 heap_size = *std::bit_cast<const u64*>(heap + 0x30);
        GuardSlot* known = nullptr;
        if (h != 0) {
            known = find_slot(heap);
            if (!known) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto signature = signature_of(heap);
            const auto outside_extent = [&] {
                if (!known->guard || !known->guard->IsValid()) {
                    return true;
                }
                const auto [lo, hi] = known->guard->FreeExtent();
                constexpr u64 Margin = 1ULL << 20;
                return std::any_of(pending.begin(), pending.end(), [&](const ReadbackItem& item) {
                    return item.addr + Margin < lo || item.addr + item.size > hi + Margin;
                });
            };
            const auto age = now - known->built;
            if (!known->guard ||
                (signature != known->signature &&
                 (age > std::chrono::seconds(2) ||
                  (age > std::chrono::milliseconds(250) && outside_extent())))) {
                SotcStats::heap_guard_builds.fetch_add(1, std::memory_order_relaxed);
                known->guard.emplace(memory, heap);
                known->signature = signature;
                known->built = now;
            }
            const auto [lo, hi] = known->guard->IsValid() ? known->guard->FreeExtent()
                                                          : std::pair<VAddr, VAddr>{0, 0};
            if (hi <= lo) {
                continue;
            }
            constexpr u64 Margin = 1ULL << 20; // headers of the used blocks next to the free ones
            heap_base = lo > Margin ? lo - Margin : 0;
            heap_size = hi - heap_base + Margin;
            static std::array<VAddr, 16> reported{};
            if (std::find(reported.begin(), reported.end(), heap) == reported.end()) {
                if (auto free = std::find(reported.begin(), reported.end(), VAddr{0});
                    free != reported.end()) {
                    *free = heap;
                    std::fprintf(stderr, "SOTC heap guard: heap %#llx guarded in %#llx..%#llx\n",
                                 static_cast<unsigned long long>(heap),
                                 static_cast<unsigned long long>(lo),
                                 static_cast<unsigned long long>(hi));
                }
            }
        }
        bool touches_heap = false;
        for (const auto& item : pending) {
            touches_heap |= item.addr < heap_base + heap_size && heap_base < item.addr + item.size;
        }
        if (!touches_heap) {
            continue;
        }
        GuardSlot* slot = known;
        if (slot) {
            if (!slot->used) {
                slot->used.emplace(memory);
            }
            active[active_count++] = {slot, heap_base, heap_size, false};
            continue;
        }
        for (auto& candidate : slots) {
            if (candidate.heap == heap) {
                slot = &candidate;
                break;
            }
        }
        if (!slot) {
            for (auto& candidate : slots) {  // a free slot, else one of a heap not seen now
                if (candidate.heap == 0 ||
                    std::find(heaps.begin(), heaps.begin() + heap_count, candidate.heap) ==
                        heaps.begin() + heap_count) {
                    slot = &candidate;
                    slot->heap = heap;
                    slot->guard.reset();
                    slot->used.reset();
                    break;
                }
            }
        }
        if (!slot) {
            continue;
        }
        if (!slot->used) {
            slot->used.emplace(memory);
        }
        active[active_count++] = {slot, heap_base, heap_size, false};
    }
    // The heap's guard, built again when its free tree changed (signature) since the last build
    const auto guard_of = [&](Active& a) -> BpeHeapGuard* {
        GuardSlot& slot = *a.slot;
        if (!a.checked) {
            a.checked = true;
            const VAddr heap = slot.heap;
            const std::array<u64, 3> signature = {*std::bit_cast<const u64*>(heap + 0x80),
                                                   *std::bit_cast<const u64*>(heap + 0xa8),
                                                   *std::bit_cast<const u64*>(heap + 0xc0)};
            if (!slot.guard || signature != slot.signature) {
                SotcStats::heap_guard_builds.fetch_add(1, std::memory_order_relaxed);
                slot.guard.emplace(memory, heap);
                slot.signature = signature;
            }
        }
        return slot.guard->IsValid() ? &*slot.guard : nullptr;
    };
    if (!heap_fast) {
        for (size_t a = 0; a < active_count; ++a) {
            guard_of(active[a]);
        }
    }
    const auto active_for = [&](VAddr addr, u64 size) -> Active* {
        for (size_t a = 0; a < active_count; ++a) {
            if (addr < active[a].base + active[a].size && active[a].base < addr + size) {
                return &active[a];
            }
        }
        return nullptr;
    };
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            const VAddr dst_addr = item.arena->cpu_addr + copy.srcOffset;
            const u8* src = mapped + (copy.dstOffset - mapped_offset);
            static const bool change_stats = std::getenv("SOTC_RB_CHANGES") != nullptr;
            if (const u8* now = change_stats ? memory->GetBackingPointer(dst_addr) : nullptr) {
                // sotc: did the GPU actually change these bytes? (SOTC_RB_CHANGES=1, SOTC_FPS line)
                u64 changed = 0;
                for (u64 i = 0; i < copy.size; ++i) {
                    changed += now[i] != src[i];
                }
                SotcStats::readback_copies.fetch_add(1, std::memory_order_relaxed);
                SotcStats::readback_bytes.fetch_add(copy.size, std::memory_order_relaxed);
                if (changed) {
                    SotcStats::readback_copies_changed.fetch_add(1, std::memory_order_relaxed);
                    SotcStats::readback_bytes_changed.fetch_add(changed, std::memory_order_relaxed);
                }
            }
            // sotc: hot data is applied only to pages the guest has not written since the
            // gather: a write fault during the GPU wait (the range was still GPU-modified, or
            // its page left writable by the fault grace) set the CPU bit, and the guest's bytes
            // are the right ones. Checked again here, page by page, and written under the page's
            // region lock, so a write fault in between waits instead of being overwritten (the
            // game's resource loader crashed asking for 59 GiB once, 24 Oct: a size it had just
            // read from a file, replaced by stale GPU bytes).
            const auto write_pages = [&](VAddr start, u64 size, auto&& write) {
                if (!item.hot) {
                    write(start, size);
                    return;
                }
                const u64 skipped = memory_tracker->WriteUnlessCpuModified(start, size, write);
                if (skipped) {
                    SotcStats::rb_hot_late_skipped.fetch_add(skipped, std::memory_order_relaxed);
                }
            };
            write_pages(dst_addr, copy.size, [&](VAddr part_addr, u64 part_size) {
            if (rb_prev_env) {
                // sotc: SOTC_RB_PREV: only the parts written (hot items skip guest-written pages)
                PrevNoteApplied(part_addr, part_size);
            }
            ForEachNonStackRange(part_addr, part_size, [&](VAddr addr, u64 size) {
                BpeHeapGuard* heap_guard = nullptr;
                if (Active* a = active_for(addr, size)) {
                    BpeUsedBlocks& used = *a->slot->used;
                    if (heap_fast && used.Covers(a->slot->heap, addr, addr + size)) {
                        memory->TryWriteBacking(std::bit_cast<u8*>(addr), src + (addr - dst_addr),
                                                size, 1);
                        return;
                    }
                    heap_guard = guard_of(*a);
                    int find = 0;
                    if (VAddr block = 0, next = 0;
                        heap_fast && heap_guard &&
                        heap_guard->FindUsedBlock(addr, block, next, &find)) {
                        used.Add(block, next);
                    } else if (find == 6) {
                        used.SetFence(block);
                    }
                } else if (active_count == 1 && active[0].checked) {
                    // Outside the heap: its guard when this batch built it, as before
                    heap_guard = guard_of(active[0]);
                }
                if (heap_guard) {
                    if (const u64 kept = heap_guard->Write(addr, src + (addr - dst_addr), size)) {
                        static u64 reported = 0;
                        if (reported++ < 8) {
                            LOG_WARNING(Render, "BPE heap guard kept {:#x} bytes of heap metadata at {:#x}",
                                        kept, addr);
                        }
                    }
                    return;
                }
                memory->TryWriteBacking(std::bit_cast<u8*>(addr), src + (addr - dst_addr), size,
                                        1);
            });
            });
        }
    }
    // sotc: one tracker update (and host protection call) per contiguous run instead of one per
    // item: a readback batch is often many neighbouring ranges, and each update ends in a
    // VirtualProtect. Runs only merge when they touch, so no byte outside the items changes state.
    thread_local std::vector<std::pair<VAddr, u64>> runs;
    runs.clear();
    // sotc: SOTC_RB_OWNER: only the pages this readback downloaded (its window may hold pages
    // another readback in flight owns, and their state is that one's to change).
    static const bool rb_owner = [] {
        const char* v = std::getenv("SOTC_RB_OWNER");
        return v && v[0] == '1';
    }();
    for (const auto& item : pending) {
        if (item.owed) {
            continue; // sotc: SOTC_RB_EXACT: the pages were released before; only bytes written
        }
        if (rb_owner) {
            for (const auto& [range_addr, range_size] : item.gpu_ranges) {
                runs.emplace_back(range_addr, range_addr + range_size);
            }
        } else {
            runs.emplace_back(item.addr, item.addr + item.size);
        }
    }
    std::ranges::sort(runs);
    std::size_t merged = 0;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (merged != 0 && runs[i].first <= runs[merged - 1].second) {
            runs[merged - 1].second = std::max(runs[merged - 1].second, runs[i].second);
        } else {
            runs[merged++] = runs[i];
        }
    }
    for (std::size_t i = 0; i < merged; ++i) {
        const auto [start, stop] = runs[i];
        // sotc: SOTC_RB_KEEP_LATER: pages a later GPU write covers keep their GPU state.
        VAddr cursor = start;
        const auto unmark = [&](VAddr lo, VAddr hi) {
            if (hi > lo) {
                memory_tracker->UnmarkRegionAsGpuModified(lo, hi - lo, false);
                MarkInflightReadbacksDirty(lo, hi - lo, StaleReason::Applied);
            }
        };
        for (const auto& [keep_start, keep_end] : keep_gpu) {
            const VAddr lo = std::max(Common::AlignDown(keep_start, VAddr{4096}), cursor);
            const VAddr hi = std::min(Common::AlignUp(keep_end, VAddr{4096}), stop);
            if (lo >= hi) {
                continue;
            }
            unmark(cursor, lo);
            cursor = hi;
        }
        unmark(cursor, stop);
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer,
                                                        u32 mark_offset, u32 mark_size) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    // sotc: SOTC_STREAM_THRESHOLD=<bytes> (default 16 KiB) for A/B: small read-only buffers are
    // copied into the stream buffer on every bind instead of using the synchronized arena.
    static const u64 stream_threshold = [] {
        const char* v = std::getenv("SOTC_STREAM_THRESHOLD");
        // sotc: 0 by default: with the unlocked no-change check the arena is cheaper than copying
        // (+7% on the heavy benchmark, attract demo min 20 -> 24 FPS).
        return v ? std::strtoull(v, nullptr, 0) : u64{0};
    }();
    if (!is_written && size <= stream_threshold && !IsRegionGpuModified(device_addr, size)) {
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    if (hrr && hrr->NoteBinding(device_addr, size, is_written)) { // sotc: SOTC_HRR dry run
        hrr->NoteImageOverlap(device_addr, size, texture_cache.HasImageInRange(device_addr, size));
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    // SynchronizeMemory also syncs read-only texel buffers from an aliasing image (sotc: a second
    // SynchronizeMemoryFromImage call here repeated that work, re-tiling the image twice).
    // sotc: SOTC_WRITE_EXTENT: only [mark_offset, +mark_size) is a written range (page-level GPU
    // state, GPU-modified bytes, current writes); the rest of the binding is synchronized as a
    // read. The written part goes first: it never keeps CPU-dirty hot pages (see
    // ForEachModifiedRange), the read part afterwards finds those pages already clean. Never for
    // texel buffers (the caller does not narrow them).
    const bool narrowed = is_written && !is_texel_buffer && mark_size != ~0u && mark_size != 0 &&
                          u64{mark_offset} + mark_size <= size &&
                          (mark_offset != 0 || mark_size < size);
    if (narrowed) {
        SynchronizeMemory(arena, device_addr + mark_offset, mark_size, true, false);
        SynchronizeMemory(arena, device_addr, size, false, false);
    } else {
        SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    }
    if (is_texel_buffer && !is_written && SotcStats::kOptOff) {
        SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    if (is_written) {
        Core::MemoryManager::NoteHostWrite(
            device_addr, size, 8, reinterpret_cast<u64>(__builtin_return_address(0)));
        const VAddr mark_addr = narrowed ? device_addr + mark_offset : device_addr;
        const u64 mark_len = narrowed ? mark_size : size;
        const u64 hot_seq = AddGpuModified(mark_addr, mark_len);
        RecordCurrentWrite(mark_addr, mark_len, hot_seq, true);
    }
    return {arena, arena->Offset(device_addr)};
}

u64 BufferCache::AddGpuModified(VAddr addr, u64 size) {
    // sotc: SOTC_RB_EXACT: a write into a hot window (a recurring readback granule) gets the next
    // hot sequence number; labels snapshot it (HotWriteSeq) to say which writes they announce.
    // The write is noted (with its seq_floor, which needs the owed and GPU-modified state as it
    // was before this write) before MarkInflightReadbacksDirty trims the owed bytes it covers and
    // before the range is added to the GPU-modified set.
    u64 hot_seq = 0;
    const bool rbx = RbxOn();
    if (rbx) {
        if (rb_prefetch_env) {
            PrefetchNoteWrite(addr, size); // arms the recurring windows hit (and rebuilds them)
        }
        hot_seq = RbxHotSeq(addr, size);
        if (hot_seq != 0 && rbx_since_label.m_ranges_set.iterative_size() < 4096) {
            rbx_since_label.Add(addr, size);
        }
        RbxNoteWrite(addr, size, scheduler.CurrentTick(), hot_seq, true);
    }
    MarkInflightReadbacksDirty(addr, size, StaleReason::GpuWrite);
    if (rb_group_env && write_scope_on) {
        RbgNoteWrite(addr, size); // sotc: SOTC_RB_GROUP: a range of the bind's group
    }
    if (rb_prefetch_env && !rbx) {
        PrefetchNoteWrite(addr, size); // sotc: SOTC_RB_PREFETCH: arms the recurring windows hit
    }
    // sotc: SOTC_GPUMOD_WATCH=<address>: who marks it as GPU-written (first 40 times).
    static const VAddr env_watch = [] {
        const char* v = std::getenv("SOTC_GPUMOD_WATCH");
        return v ? static_cast<VAddr>(std::strtoull(v, nullptr, 0)) : VAddr{0};
    }();
    // SOTC_CS_WATCH (vk_rasterizer.cpp) arms it at run time.
    const VAddr watch = env_watch ? env_watch : SotcStats::watch_addr.load(std::memory_order_relaxed);
    if (watch && addr < watch + 0x10000 && watch < addr + size) {
        // The first 40 writes, then the first write of every other program (up to 64 more).
        static int shown = 0;
        static u64 programs[64]{};
        static int num_programs = 0;
        const u64 program = SotcStats::current_program.load();
        bool new_program = std::find(programs, programs + num_programs, program) ==
                           programs + num_programs;
        if (new_program && num_programs < 64) {
            programs[num_programs++] = program;
        } else {
            new_program = false;
        }
        if (shown++ < 40 || new_program) {
            std::fprintf(stderr, "SOTC gpu-modified %#llx+%#llx by %p <- %p, program %#llx\n",
                         static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size),
                         __builtin_return_address(0), __builtin_return_address(1),
                         static_cast<unsigned long long>(SotcStats::current_program.load()));
        }
    }
    // sotc: gpu_modified_ranges only grows here (every other use subtracts), and the clean page
    // cache below only has to drop pages whose GPU-written hull grew: when the range was already
    // contained, no page can have become dirty. SOTC_GPUMOD_NOGROW_SKIP=0 always scans.
    static const bool nogrow_skip = [] {
        const char* v = std::getenv("SOTC_GPUMOD_NOGROW_SKIP");
        return !v || v[0] != '0';
    }();
    bool grew;
    {
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        grew = gpu_modified_ranges.Add(addr, size);
    }
    if (!clean_pages_used || size == 0 || (!grew && nogrow_skip)) {
        return hot_seq;
    }
    // The GPU-written hull of these pages grows: drop them from the clean page cache.
    const u64 first = addr >> CleanPageBits;
    const u64 last = (addr + size - 1) >> CleanPageBits;
    if (last - first >= NumCleanPages) {
        for (auto& page : clean_pages) {
            const u64 index = page.page_addr >> CleanPageBits;
            if (index >= first && index <= last) {
                page.page_addr = ~0ULL;
            }
        }
        return hot_seq;
    }
    for (u64 index = first; index <= last; ++index) {
        auto& page = clean_pages[index % NumCleanPages];
        if (page.page_addr == index << CleanPageBits) {
            page.page_addr = ~0ULL;
        }
    }
    return hot_seq;
}

bool BufferCache::ReadClean(VAddr addr, void* out, u32 size) {
    if (!ReadCleanBacking(addr, out, size)) {
        return false;
    }
    // sotc: SOTC_RB_EXACT: bytes owed to guest memory are old in the backing. Checked after the
    // GPU-modified set, the order a release reverses (it registers the owed bytes first, then
    // leaves the set): a reader that finds the set clear then finds the owed bytes.
    return !(rbx_owed_count.load(std::memory_order_relaxed) != 0 && RbxOwedIntersects(addr, size));
}

bool BufferCache::ReadCleanBacking(VAddr addr, void* out, u32 size) {
    const VAddr page_addr = addr & ~(CleanPageSize - 1);
    if (addr + size > page_addr + CleanPageSize) {
        return false;
    }
    // sotc: the command processor while the draw thread owns the GPU state: no page cache (the
    // draw thread maintains it), the ranges under the lock.
    if (liverpool->RunsGpuWorkElsewhere()) {
        const u8* backing = memory->GetBackingPointer(page_addr);
        if (!backing) {
            return false;
        }
        std::shared_lock gpu_modified_lock{gpu_modified_mutex};
        if (gpu_modified_ranges.Intersects(addr, size)) {
            return false;
        }
        std::memcpy(out, backing + (addr - page_addr), size);
        return true;
    }
    const u64 generation = clean_map_generation.load(std::memory_order_acquire);
    auto& page = clean_pages[(addr >> CleanPageBits) % NumCleanPages];
    if (page.page_addr != page_addr || page.map_generation != generation) {
        const u8* backing = memory->GetBackingPointer(page_addr);
        if (!backing) {
            return false;
        }
        u32 dirty_lo = CleanPageSize;
        u32 dirty_hi = 0;
        gpu_modified_ranges.ForEachInRange(page_addr, CleanPageSize, [&](VAddr start, VAddr end) {
            dirty_lo = std::min<u32>(dirty_lo, static_cast<u32>(start - page_addr));
            dirty_hi = std::max<u32>(dirty_hi, static_cast<u32>(end - page_addr));
        });
        clean_pages_used = true;
        page = {
            .page_addr = page_addr,
            .map_generation = generation,
            .backing = backing,
            .dirty_lo = dirty_lo,
            .dirty_hi = dirty_hi,
        };
    }
    const u32 offset = static_cast<u32>(addr - page_addr);
    if (offset < page.dirty_hi && offset + size > page.dirty_lo &&
        gpu_modified_ranges.Intersects(addr, size)) {
        return false;
    }
    std::memcpy(out, page.backing + offset, size);
    return true;
}

bool BufferCache::TakeHotWrite() noexcept {
    // Only while that write is still in the command buffer being built.
    return std::exchange(hot_write_pending, false) && current_writes_tick == scheduler.CurrentTick();
}

void BufferCache::NoteHotRead(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    for (const auto& [start, stop] : hot_reads) {
        if (start == addr && stop == end) {
            return;
        }
    }
    hot_reads[hot_reads_next] = {addr, end};
    hot_reads_next = (hot_reads_next + 1) % NumHotReads;
}

void BufferCache::NoteGranulePage(VAddr page) {
    std::array<u8, 4096> bytes;
    memory->CopySparseMemory(page, bytes.data(), bytes.size());
    const u64 hash = XXH3_64bits(bytes.data(), bytes.size());
    std::scoped_lock lock{granule_mutex};
    if (granule_snapshots.size() >= 65536) { // sotc: capped; a dropped page just uploads as before
        granule_snapshots.clear();
    }
    granule_snapshots.insert_or_assign(page, hash << 1); // sotc: bit 0 = write-ahead snapshot
    granule_snapshot_count.store(granule_snapshots.size(), std::memory_order_relaxed);
}

void BufferCache::EraseGranuleSnapshots(VAddr addr, u64 size) {
    if (granule_snapshot_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::scoped_lock lock{granule_mutex};
    for (VAddr page = addr & ~VAddr{4095}; page < addr + size; page += 4096) {
        granule_snapshots.erase(page);
    }
    granule_snapshot_count.store(granule_snapshots.size(), std::memory_order_relaxed);
}

void BufferCache::DropUnchangedGranulePages(boost::container::small_vector<vk::BufferCopy, 4>& copies) {
    constexpr u64 Page = 4096;
    boost::container::small_vector<vk::BufferCopy, 4> kept;
    std::array<u8, Page> bytes;
    // sotc: the snapshots of the copies' pages are taken out of the map under the lock; the bytes
    // are read and hashed outside it (the fault handlers of the guest threads insert snapshots
    // under the same lock). The decision per page is the one the old code made under the lock:
    // the entry is consumed either way, and compared with the bytes as they are now.
    struct Taken {
        VAddr page;
        u64 stored;
    };
    boost::container::small_vector<Taken, 32> taken;
    {
        std::scoped_lock lock{granule_mutex};
        for (const auto& copy : copies) {
            const VAddr end = copy.dstOffset + copy.size;
            for (VAddr page = copy.dstOffset & ~(Page - 1); page < end; page += Page) {
                const auto it = granule_snapshots.find(page);
                if (it != granule_snapshots.end()) {
                    taken.push_back({page, it->second});
                    granule_snapshots.erase(it);
                }
            }
        }
        granule_snapshot_count.store(granule_snapshots.size(), std::memory_order_relaxed);
    }
    size_t next = 0;
    for (const auto& copy : copies) {
        const VAddr end = copy.dstOffset + copy.size;
        VAddr run_start = copy.dstOffset;
        for (VAddr page = copy.dstOffset & ~(Page - 1); page < end; page += Page) {
            // `taken` holds the pages in the order of this walk; a page may repeat across
            // overlapping copies (the second finds its entry already consumed, as before).
            if (next >= taken.size() || taken[next].page != page) {
                continue;
            }
            const u64 stored = taken[next++].stored;
            memory->CopySparseMemory(page, bytes.data(), bytes.size());
            // Compared now, at upload time (the page was unprotected since the snapshot: a write
            // after it changes the bytes, hence the hash). Value = hash << 1 | write-ahead tag.
            const bool unchanged = (XXH3_64bits(bytes.data(), bytes.size()) << 1) == (stored & ~u64{1});
            if (!unchanged) {
                continue;
            }
            if ((stored & 1) != 0 && SotcStats::kFpsLog) {
                SotcStats::FaultStat::ahead_unchanged.fetch_add(1, std::memory_order_relaxed);
            }
            // Cut [page, page + 4 KiB) out of the run.
            const VAddr cut_begin = std::max<VAddr>(page, copy.dstOffset);
            const VAddr cut_end = std::min<VAddr>(page + Page, end);
            if (cut_begin > run_start) {
                kept.push_back({0, run_start, cut_begin - run_start});
            }
            run_start = cut_end;
            SotcStats::fault_granule_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        if (run_start < end) {
            kept.push_back({0, run_start, end - run_start});
        }
    }
    copies.assign(kept.begin(), kept.end());
}

void BufferCache::EraseHashedUploads(VAddr addr, u64 size) {
    if (hashed_upload_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::scoped_lock lock{hashed_uploads_mutex};
    // Entries are at most kHashSmall long: an overlapping one starts within that much before addr.
    const VAddr end = addr + size;
    const VAddr window = SotcStats::kHashSmall;
    auto it = hashed_uploads.lower_bound(addr > window ? addr - window : 0);
    while (it != hashed_uploads.end() && it->first < end) {
        if (it->first + it->second.size > addr) {
            it = hashed_uploads.erase(it);
        } else {
            ++it;
        }
    }
    hashed_upload_count.store(hashed_uploads.size(), std::memory_order_relaxed);
}

void BufferCache::RecordCurrentWrite(VAddr addr, u64 size, u64 seq, bool gpu_write) {
    EraseHashedUploads(addr, size);
    // sotc: SOTC_RB_EXACT: any arena write over owed bytes takes them out of the owed downloads.
    if (RbxOn() && rbx_owed_count.load(std::memory_order_relaxed) != 0) {
        RbxTrim(addr, addr + size);
    }
    // sotc: SOTC_RB_PREFETCH: every arena write recorded after a prefetch's copies (GPU-written
    // bindings, uploads, image syncs, write-throughs, transfer-queue copies) makes it stale.
    if (rb_prefetch_env && !pf_live.empty() && addr < pf_hi && pf_lo < addr + size) {
        PrefetchMarkDirty(addr, size);
    }
    if (!hot_write_pending) {
        const VAddr end = addr + size;
        for (const auto& [start, stop] : hot_reads) {
            if (addr < stop && start < end) {
                hot_write_pending = true;
                break;
            }
        }
    }
    RollCurrentWrites();
    current_writes.Add(addr, size);
    // sotc: SOTC_RB_EXACT: who wrote these bytes and when. GPU writes always; other arena writes
    // (uploads, image syncs) only over bytes that are GPU-modified (the only bytes a readback
    // decides about).
    if (RbxOn() && !gpu_write && gpu_modified_ranges.Intersects(addr, size)) {
        RbxNoteWrite(addr, size, scheduler.CurrentTick(), 0, false);
    }
    // sotc: SOTC_RB_TICK: a write over bytes a transfer-queue readback copy still reads.
    if (!xfer_reads.empty()) {
        NoteWriteAgainstXferReads(addr, size);
    }
    if (write_scope_on) {
        write_scope.emplace_back(addr, size);
        if (RbxOn()) {
            rbx_scope_seq.push_back(gpu_write ? seq : ~u64{0}); // ~0: not recorded as a writer
        }
    }
}

void BufferCache::BeginWriteScope() {
    rbx_scope_seq.clear();
    write_scope_on = true;
    write_scope_tick = scheduler.CurrentTick();
    write_scope.clear();
    if (rb_group_env) { // sotc: SOTC_RB_GROUP: a new group (a bind left open never happens)
        rbg_open.ranges.clear();
        rbg_open.bytes = 0;
        rbg_open.oversize = false;
    }
}

void BufferCache::EndWriteScope() {
    write_scope_on = false;
    if (rb_group_env) {
        RbgCloseScope(); // sotc: SOTC_RB_GROUP: the bind's group is complete
    }
    if (write_scope.empty() || scheduler.CurrentTick() == write_scope_tick) {
        write_scope.clear();
        rbx_scope_seq.clear();
        return;
    }
    // A submit happened during the bind: the command is in the current tick.
    RollCurrentWrites();
    size_t scope_index = 0;
    for (const auto& [addr, size] : write_scope) {
        current_writes.Add(addr, size);
        if (RbxOn() && scope_index < rbx_scope_seq.size() &&
            rbx_scope_seq[scope_index] != ~u64{0}) {
            // sotc: SOTC_RB_EXACT: the writer's tick is the one the command landed in
            RbxNoteWrite(addr, size, scheduler.CurrentTick(), rbx_scope_seq[scope_index], true);
        }
        ++scope_index;
        if (!xfer_reads.empty()) {
            NoteWriteAgainstXferReads(addr, size);
        }
    }
    write_scope.clear();
    rbx_scope_seq.clear();
}

void BufferCache::RollCurrentWrites() {
    const u64 tick = scheduler.CurrentTick();
    if (current_writes_tick == tick) {
        return;
    }
    // sotc: the write sets of the last ticks are kept for SOTC_RB_TICK and the SOTC_FPS writer
    // counters (ReadbackWriterTick); the cost without either is the one test.
    static const bool ring_on = [] {
        const char* v = std::getenv("SOTC_RB_TICK");
        const char* narrow = std::getenv("SOTC_WRITE_FAULT_NARROW"); // sotc: its writer ticks
        return SotcStats::kFpsLog || (v && v[0] == '1') || (narrow && narrow[0] == '1');
    }();
    // sotc: SOTC_HRR's dry run asks ReadbackWriterTick too (BeginAsyncReadback): without the ring
    // it would always answer "unknown".
    if (ring_on || hrr) {
        if (!current_writes.Empty()) {
            WriteRingEntry& entry = write_ring[write_ring_head];
            if (entry.tick != 0) {
                write_ring_floor = std::max(write_ring_floor, entry.tick); // no longer tracked
            }
            entry.set.Swap(current_writes); // current_writes now holds the evicted set
            entry.tick = current_writes_tick;
            write_ring_head = (write_ring_head + 1) % NumWriteRing;
        }
        if (!xfer_reads.empty()) {
            PruneXferReads();
        }
    }
    current_writes.Clear();
    current_writes_tick = tick;
}

u64 BufferCache::ReadbackWriterTick(std::span<const ReadbackItem> pending, bool& unknown) {
    RollCurrentWrites();
    const u64 cur = scheduler.CurrentTick();
    u64 best = 0;
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            const VAddr addr = item.arena->cpu_addr + copy.srcOffset;
            if (current_writes.Intersects(addr, copy.size)) {
                return cur; // being recorded: not submitted
            }
            for (const auto& entry : write_ring) {
                if (entry.tick > best && entry.set.Intersects(addr, copy.size)) {
                    best = entry.tick;
                }
            }
        }
    }
    if (best != 0) {
        return best;
    }
    if (write_ring_floor != 0) {
        return write_ring_floor; // older than the ring: written at that tick or before
    }
    unknown = true;
    return cur - 1; // today's assumption
}

void BufferCache::PruneXferReads() {
    xfer_reads.erase(std::remove_if(xfer_reads.begin(), xfer_reads.end(),
                                    [this](const XferRead& read) {
                                        return scheduler.XferDone(read.value);
                                    }),
                     xfer_reads.end());
}

void BufferCache::NoteXferRead(std::span<const ReadbackItem> pending, u64 value) {
    // sotc: SOTC_RB_TICK, write after read. The copy reads these ranges until the transfer
    // timeline reaches value. Graphics submits do not wait for it, except one whose tick records a
    // write over them: RecordCurrentWrite -> NoteWriteAgainstXferReads asks the scheduler for that
    // wait (the next submit collects it, as it does for uploads). Every write is checked while the
    // copy runs (not just the first), so no later tick depends on queue order alone.
    PruneXferReads();
    if (xfer_reads.size() >= 32) {
        // Too many copies still running: the next submit waits for the newest of them (the
        // timeline is monotonic, that covers all) and the list starts again.
        u64 newest = 0;
        for (const auto& read : xfer_reads) {
            newest = std::max(newest, read.value);
        }
        scheduler.XferRequireWait(newest);
        xfer_reads.clear();
    }
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            const VAddr lo = item.arena->cpu_addr + copy.srcOffset;
            xfer_reads.push_back({lo, lo + copy.size, value, 0});
        }
    }
    if (xfer_reads.size() > 64) {
        // A readback with very many copies: one wait for this copy covers them all.
        scheduler.XferRequireWait(value);
        xfer_reads.clear();
    }
}

void BufferCache::NoteWriteAgainstXferReads(VAddr addr, u64 size) {
    const u64 cur = scheduler.CurrentTick();
    const VAddr end = addr + size;
    for (auto& read : xfer_reads) {
        if (addr < read.hi && read.lo < end && read.required_tick != cur) {
            scheduler.XferRequireWait(read.value); // the submit of this tick waits for the copy
            read.required_tick = cur;
        }
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainGuestCopyForImage(VAddr device_addr, u32 size) {
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size,
                                                                bool pages_protected) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    // sotc: SOTC_ASYNC_TEXTURE_UPLOAD. The image's pages are write-protected by its tracking
    // (pages_protected): its guest copy goes to the copy threads like a buffer upload (the write
    // fence and the submit wait of async_upload.h apply; the detiling reads the staging on the
    // GPU, after that wait). Guest stacks are never protected: those stay here.
    if (auto* async_upload = AsyncUpload::Get();
        pages_protected && async_upload && AsyncUpload::TexturesEnabled() &&
        size >= async_upload->MinBytes() && !IntersectsGuestStack(device_addr, size)) {
        const std::array<vk::BufferCopy, 1> copy{vk::BufferCopy{0, device_addr, size}};
        if (QueueAsyncGuestCopy(*async_upload, copy, staging, size)) {
            SotcStats::async_texture_uploads.fetch_add(1, std::memory_order_relaxed);
            return {staging.buffer, staging.offset};
        }
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::TryReplaceFromCpu(VAddr addr, const void* data, u64 size) {
    if (size == 0) {
        return true;
    }
    if (!memory->GetBackingPointer(addr) || !memory->GetBackingPointer(addr + size - 1)) {
        return false;
    }
    // Page tracking is page granular: other GPU-written bytes on these pages would be lost.
    const VAddr lo = Common::AlignDown(addr, VAddr{4096});
    const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
    bool foreign = false;
    gpu_modified_ranges.ForEachInRange(lo, hi - lo, [&](VAddr start, VAddr end) {
        foreign |= start < addr || end > addr + size;
    });
    if (foreign) {
        return false;
    }
    {
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        gpu_modified_ranges.Subtract(addr, size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(addr, size, false);
    MarkInflightReadbacksDirty(addr, size, StaleReason::CpuWrite);
    if (!memory->TryWriteBacking(std::bit_cast<void*>(addr), data, size, 10)) {
        return false;
    }
    memory_tracker->MarkRegionAsCpuModified(addr, size);
    return true;
}

bool BufferCache::TryWriteThroughFromCpu(VAddr addr, const void* data, u64 size) {
    if (size == 0) {
        return true;
    }
    const u64 first_block = addr >> block_shift;
    const u64 last_block = (addr + size - 1) >> block_shift;
    bool resident = true;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64, u64) { resident = false; });
    const Buffer* arena =
        resident ? address_space[first_block >> blocks_per_arena_page_shift] : nullptr;
    if (!arena || arena != address_space[last_block >> blocks_per_arena_page_shift]) {
        return TryReplaceFromCpu(addr, data, size);
    }
    if (!memory->GetBackingPointer(addr) || !memory->GetBackingPointer(addr + size - 1)) {
        return false;
    }
    // Page tracking is page granular: other GPU-written bytes on these pages would be lost.
    const VAddr lo = Common::AlignDown(addr, VAddr{4096});
    const VAddr hi = Common::AlignUp(addr + size, VAddr{4096});
    bool foreign = false;
    gpu_modified_ranges.ForEachInRange(lo, hi - lo, [&](VAddr start, VAddr end) {
        foreign |= start < addr || end > addr + size;
    });
    if (foreign) {
        return false;
    }
    {
        // sotc: the command processor reads the set under the shared lock (ReadClean).
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        gpu_modified_ranges.Subtract(addr, size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(addr, size, false);
    MarkInflightReadbacksDirty(addr, size, StaleReason::CpuWrite);
    if (!memory->TryWriteBacking(std::bit_cast<void*>(addr), data, size, 10)) {
        return false;
    }
    // The arena gets the same bytes now, in stream order, so the CPU bits stay as they are: a
    // clean page stays clean and write-protected, a dirty one uploads the same bytes later.
    const auto staging = staging_pool.Request(size, MemoryType::HostUncached);
    std::memcpy(staging.mapped, data, size);
    staging.Flush();
    const vk::BufferCopy copy{staging.offset, arena->Offset(addr), size};
    runtime.CopyBuffer(staging.buffer, arena, std::span{&copy, 1});
    RecordCurrentWrite(addr, size);
    return true;
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    // sotc: SOTC_RB_EXACT: bytes owed to guest memory still count as GPU-modified for the callers
    // outside the readback path (page manager granules, indirect arguments, textures).
    return memory_tracker->IsRegionGpuModified(addr, size) ||
           (rbx_owed_count.load(std::memory_order_relaxed) != 0 && RbxOwedIntersects(addr, size));
}

void BufferCache::SynchronizeDmaBuffers() {
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameDma};
    fault_process_pending = true;
    const u64 uploads_before = SotcStats::uploads.load(std::memory_order_relaxed);
    const auto sweep_start = std::chrono::steady_clock::now();
    u64 ranges = 0;
    // sotc (SOTC_DMA_MERGE=0 disables): the sweep visited ~1200 resident ranges ~100 times a
    // second, one tracker walk + staging request each. Touching ranges whose start maps to the
    // same arena are merged into runs (one SynchronizeMemory per run), rebuilt only when
    // resident_ranges or the arena set changed. Same pages are uploaded into the same arena,
    // just batched. A run is capped (SOTC_DMA_MERGE_MAX MiB, default 16 = staging ring block,
    // max 64) so merging never pushes an upload to RequestLarge; a single range larger than the
    // cap stays one run as before. Runs hold addresses only; the arena is looked up per run at
    // sync time (a migration merges arenas, so entries sharing one keep sharing one).
    static const bool dma_merge = [] {
        const char* v = std::getenv("SOTC_DMA_MERGE");
        return !(v && v[0] == '0');
    }();
    static const u64 dma_merge_max = [] {
        const char* v = std::getenv("SOTC_DMA_MERGE_MAX");
        const u64 mib = v ? std::strtoull(v, nullptr, 10) : 0;
        return std::clamp<u64>(mib ? mib : 16, 1, 64) << 20;
    }();
    if (dma_merge) {
        if (dma_runs_version != resident_version || dma_runs_arenas != arenas.size()) {
            dma_runs_version = resident_version;
            dma_runs_arenas = arenas.size();
            dma_runs_ranges = resident_ranges.Size();
            dma_runs.clear();
            for (const auto& range : resident_ranges) {
                const VAddr device_addr = range.start << block_shift;
                const u64 size = (range.end - range.start) << block_shift;
                if (!dma_runs.empty()) {
                    auto& run = dma_runs.back();
                    if (run.first + run.second == device_addr &&
                        run.second + size <= dma_merge_max &&
                        address_space[run.first >> ARENA_PAGE_BITS] ==
                            address_space[device_addr >> ARENA_PAGE_BITS]) {
                        run.second += size;
                        continue;
                    }
                }
                dma_runs.emplace_back(device_addr, size);
            }
            dma_full_pending = true;
        }
        // sotc: incremental sweep (SOTC_DMA_INCR=0: every run, every time). Only the 256 KiB
        // words where a CPU bit rose since the last sweep can need an upload
        // (RegionManager::TakeSweepWords; hot pages kept by FaultGrace are marked again on each
        // upload). After the runs change (pages that became resident may hold old CPU bits) one
        // full sweep restores that. Each run is still synced once, over the hull of its dirty
        // words, so the uploads per run stay as before.
        static const bool dma_incr = [] {
            const char* v = std::getenv("SOTC_DMA_INCR");
            return !(v && v[0] == '0');
        }();
        if (!dma_incr || std::exchange(dma_full_pending, false)) {
            if (dma_incr) {
                memory_tracker->ForEachSweepSpan([](VAddr, u64) {});
            }
            // sotc: by index with a copied entry, re-reading size(): nothing nested in
            // SynchronizeMemory may invalidate the iteration even if it touched dma_runs.
            for (size_t i = 0; i < dma_runs.size(); ++i) {
                const auto [device_addr, size] = dma_runs[i];
                SynchronizeMemory(address_space[device_addr >> ARENA_PAGE_BITS], device_addr,
                                  static_cast<u32>(size), false, false);
            }
            ranges = dma_runs_ranges;
        } else {
            dma_spans.clear();
            memory_tracker->ForEachSweepSpan(
                [this](VAddr address, u64 size) { dma_spans.emplace_back(address, size); });
            std::ranges::sort(dma_spans);
            size_t s = 0;
            for (size_t i = 0; i < dma_runs.size() && s < dma_spans.size(); ++i) {
                const auto [run_start, run_size] = dma_runs[i];
                const VAddr run_end = run_start + run_size;
                while (s < dma_spans.size() &&
                       dma_spans[s].first + dma_spans[s].second <= run_start) {
                    ++s;
                }
                VAddr lo = ~0ULL;
                VAddr hi = 0;
                for (size_t k = s; k < dma_spans.size() && dma_spans[k].first < run_end; ++k) {
                    lo = std::min(lo, std::max(run_start, dma_spans[k].first));
                    hi = std::max(hi, std::min(run_end, dma_spans[k].first + dma_spans[k].second));
                }
                if (lo < hi) {
                    SynchronizeMemory(address_space[run_start >> ARENA_PAGE_BITS], lo,
                                      static_cast<u32>(hi - lo), false, false);
                    ++ranges;
                }
            }
        }
        SotcStats::dma_sweep_runs.fetch_add(dma_runs.size(), std::memory_order_relaxed);
    } else {
        for (const auto& range : resident_ranges) {
            const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
            const VAddr device_addr = range.start << block_shift;
            const u64 size = (range.end - range.start) << block_shift;
            SynchronizeMemory(address_space[page], device_addr, size, false, false);
            ++ranges;
        }
    }
    SotcStats::dma_sweeps.fetch_add(1, std::memory_order_relaxed);
    SotcStats::dma_sweep_ranges.fetch_add(ranges, std::memory_order_relaxed);
    const u64 sweep_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - sweep_start)
                             .count();
    SotcStats::dma_sweep_ns.fetch_add(sweep_ns, std::memory_order_relaxed);
    if (SotcStats::uploads.load(std::memory_order_relaxed) != uploads_before) {
        SotcStats::dma_sweep_uploading.fetch_add(1, std::memory_order_relaxed);
    } else {
        SotcStats::dma_sweep_idle_ns.fetch_add(sweep_ns, std::memory_order_relaxed);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            // sotc: an arena covers a group of SOTC_ARENA_GROUP pages (default 4, 16 GB) when
            // they are all free. With one page per arena, a buffer across two pages migrated
            // both arenas into a new one and rebound all their memory: a stall of 0.8-0.9 s,
            // about once a session. SotC's GPU memory lies in 0x10'0000'0000-0x13'ffff'ffff.
            static const u64 group = [] {
                const char* v = std::getenv("SOTC_ARENA_GROUP");
                const u64 n = v ? std::strtoull(v, nullptr, 10) : 4;
                return n != 0 && std::has_single_bit(n) ? n : u64{1};
            }();
            u64 group_first = first_page & ~(group - 1);
            u64 group_last = group_first + group - 1;
            for (u64 page = group_first; page <= group_last; ++page) {
                if (page >= NUM_ARENA_PAGES || address_space[page]) {
                    group_first = first_page;
                    group_last = last_page;
                    break;
                }
            }
            if (last_page > group_last) {
                group_first = first_page;
                group_last = last_page;
            }
            const u64 num_pages = group_last - group_first + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, group_first << ARENA_PAGE_BITS,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            for (u64 page = group_first; page <= group_last; ++page) {
                address_space[page] = new_arena;
            }
            std::fprintf(stderr, "SOTC arena at %#llx, %llu GB\n",
                         static_cast<unsigned long long>(group_first << ARENA_PAGE_BITS),
                         static_cast<unsigned long long>(num_pages << (ARENA_PAGE_BITS - 30)));
        }
        return address_space[first_page];
    }

    const auto migrate_start = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        LOG_WARNING(Render, "Migrating arena: {} + {} MB of address space, {} ms",
                    (first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE) >> 20,
                    (last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE) >> 20,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - migrate_start)
                        .count());
    };

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    // sotc: SOTC_BIND_CACHE_XTICK. The old arenas stay bound and valid, so earlier bindings keep
    // working, but later unbinds only reach the arena address_space names now; rare, so a cached
    // bind is dropped rather than reasoned about.
    ++bind_version;
    arena_migrate_tick = scheduler.CurrentTick(); // sotc: SOTC_RB_TICK, see the member
    return new_arena;
}

void BufferCache::NoteBindUse(VAddr device_addr, u64 size) noexcept {
    // sotc: SOTC_BIND_CACHE_XTICK. The stamping of EnsureResident for a range found resident.
    const u64 first = device_addr >> block_shift;
    const u64 last = (device_addr + size - 1) >> block_shift;
    for (u64 block = first; block <= last; ++block) {
        block_last_use[block] = use_epoch;
    }
}

void BufferCache::SynchronizeBinding(const Buffer* arena, VAddr device_addr, u32 size,
                                     bool is_texel_buffer) {
    // sotc: SOTC_BIND_CACHE_XTICK. ObtainBuffer's SynchronizeMemory for a read-only binding.
    SynchronizeMemory(arena, device_addr, size, false, is_texel_buffer);
    if (is_texel_buffer && SotcStats::kOptOff) {
        SynchronizeMemoryFromImage(arena, device_addr, size);
    }
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    // sotc: binds check the same ranges thousands of times a frame, each a search of ~2,600
    // resident ranges (~2% of the command processor): ranges found resident are remembered
    // (64 slots by first block) until the resident set changes (resident_version).
    // sotc: the bind keeps its blocks out of MaybeEvictCold for SOTC_ARENA_EVICT_IDLE_S. A
    // cached range is stamped as a whole once per epoch (a 64 MiB binding is 1,024 blocks).
    const auto stamp = [this](u64 first, u64 end) {
        for (u64 block = first; block < end; ++block) {
            block_last_use[block] = use_epoch;
        }
    };
    auto& cached = resident_cache[(first_block * 0x9E3779B97F4A7C15ull) >> 58];
    if (cached.version == resident_version && first_block >= cached.first &&
        last_block < cached.end) {
        if (cached.epoch != use_epoch) {
            stamp(cached.first, cached.end);
            cached.epoch = use_epoch;
        }
        return;
    }
    stamp(first_block, last_block + 1);
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        cached = {resident_version, first_block, last_block + 1, use_epoch};
        return;
    }
    if (!evicted_ranges.Empty()) {
        for (const auto& range : bind_ranges) {
            evicted_ranges.ForEachInRange(range.start, range.end, [&](const Interval& evicted) {
                const u64 start = std::max(range.start, evicted.start);
                const u64 end = std::min(range.end, evicted.end);
                SotcStats::arena_rebound.fetch_add((end - start) << block_shift,
                                                   std::memory_order_relaxed);
            });
            evicted_ranges.Subtract(range.start, range.end);
        }
    }
    const SotcStats::FrameTimer frame_timer{SotcStats::FrameAlloc};
    const SotcStats::AllocTrace alloc_trace{SotcStats::AllocArena, u64(resident_blocks) << block_shift};

    // Memory first: running out of it may submit work (see AllocateArenaBlocks), which must not
    // happen with half of the binds queued.
    boost::container::small_vector<std::pair<vk::DeviceMemory, u64>, 8> placements;
    for (const auto& range : bind_ranges) {
        placements.push_back(AllocateArenaBlocks(range.end - range.start));
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    size_t placement_index = 0;
    for (const auto& range : bind_ranges) {
        const auto [device_memory, memory_block] = placements[placement_index++];
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_block;
        resident_ranges.Add(backing);
        ++resident_version; // sotc: invalidates the merged DMA runs

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_block << block_shift,
        });

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

// sotc: SOTC_SYNC_FAST (sotc_stats.h). The ranges last found clean after a sync, per thread
// (SynchronizeMemory may run on more than one thread), direct-mapped by (address, size, written).
// A slot holds the generation sum (MemoryTracker::SyncGeneration) loaded BEFORE that sync.
namespace {
struct SyncFastSlot {
    VAddr addr{};
    u64 gen{};
    u32 size{}; // 0: empty (a real sync has size > 0)
    bool is_written{};
};
constexpr u64 SyncFastSlotBits = 10;
thread_local std::array<SyncFastSlot, 1ULL << SyncFastSlotBits> sync_fast_slots{};

SyncFastSlot& SyncFastSlotFor(VAddr addr, u32 size, bool is_written) {
    const u64 key = addr ^ (u64(size) << 40) ^ (u64(is_written) << 63);
    return sync_fast_slots[(key * 0x9E3779B97F4A7C15ull) >> (64 - SyncFastSlotBits)];
}
} // namespace

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    if (!SotcStats::kSyncFast) {
        return SynchronizeMemoryFull(arena, device_addr, size, is_written, is_texel_buffer);
    }
    // sotc: SOTC_SYNC_FAST. Skip the page scan (and its locks) when the range was found clean
    // after an earlier sync and no granule over it changed since. Why that is the same as the
    // full path: the full path's upload step depends only on the tracker bits (no CPU bit and,
    // for a written binding, every GPU bit set = nothing to do, ForEachModifiedRange's own early
    // return). Bits only become "dirty" (CPU rises, GPU falls) in
    // RegionManager::UpdateStateAndProtection, which bumps the granule generation after the
    // store (release); clearing CPU bits / setting GPU bits (what a sync does) does not bump.
    // Ordering: gen0 is loaded (acquire) BEFORE the full sync and the clean check after it.
    //  - a change whose bump gen0 saw: its bit store happens-before the clean check, which then
    //    sees the bit (or a later clear by a sync that uploaded it): either not recorded, or
    //    correctly clean;
    //  - a change whose bump gen0 missed: the bump is later in the generation's order, so the
    //    sum differs at the check that reads it. A check that does not see it yet linearizes
    //    before the bump, hence before the page is unprotected (the bump precedes the unprotect),
    //    i.e. before the guest could write new bytes; anything that orders the draw after the
    //    guest's write (a submit) also orders it after the bump.
    // Hot pages (fault grace, SOTC_HOT_PAGES) keep their CPU bit set without a new bump: such a
    // range is never clean, so it always takes the full path. What follows the upload step
    // (texel buffers synced from an aliasing image) runs on both paths.
    if (SotcStats::kFpsLog) {
        SotcStats::sync_fast_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    u64 gen0 = 0;
    if (!memory_tracker->SyncGeneration(device_addr, size, gen0)) {
        return SynchronizeMemoryFull(arena, device_addr, size, is_written, is_texel_buffer);
    }
    SyncFastSlot& slot = SyncFastSlotFor(device_addr, size, is_written);
    if (slot.size == size && slot.addr == device_addr && slot.is_written == is_written &&
        slot.gen == gen0) {
        if (SotcStats::kFpsLog) {
            SotcStats::sync_fast_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        return is_texel_buffer && !is_written
                   ? SynchronizeMemoryFromImage(arena, device_addr, size)
                   : false;
    }
    const bool result =
        SynchronizeMemoryFull(arena, device_addr, size, is_written, is_texel_buffer);
    // A slot left stale (range not clean now) cannot match again: its sum only grows.
    if (memory_tracker->IsSyncClean(device_addr, size, is_written)) {
        slot = {device_addr, gen0, size, is_written};
    }
    return result;
}

bool BufferCache::SynchronizeMemoryFull(const Buffer* arena, VAddr device_addr, u32 size,
                                        bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    {
        // sotc: SOTC_PROTECT_BATCH (sotc_stats.h). The re-protects of the ranges collected here
        // are applied together when this scope closes, so before anything below reads guest
        // memory for them (the order of today's path: protect, then copy). A guest write landing
        // before the flush is in the bytes copied below; one after it faults.
        const PageManager::ProtectBatch protect_batch{SotcStats::kProtectBatch};
        memory_tracker->ForEachUploadRange(device_addr, size, is_written,
                                           [&](u64 addr, u64 size) {
                                               copies.emplace_back(total_size_bytes, addr, size);
                                               total_size_bytes += size;
                                           });
    }
    // sotc: SOTC_FAULT_GRANULE: pages a granule marked dirty that the guest never wrote stay as
    // the arena holds them. Only runs when such pages are pending (otherwise one atomic load).
    if (!copies.empty() && granule_snapshot_count.load(std::memory_order_relaxed) != 0) {
        DropUnchangedGranulePages(copies);
        total_size_bytes = 0;
        for (auto& copy : copies) {
            copy.srcOffset = total_size_bytes;
            total_size_bytes += copy.size;
        }
    }
    if (!copies.empty()) {
        const SotcStats::FrameTimer frame_timer{SotcStats::FrameBuffer};
        if (SotcStats::kFpsLog) {
            SotcStats::frame_copy_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
        }
        SotcStats::uploads.fetch_add(1, std::memory_order_relaxed);
        if (SotcStats::kCmdStats) { // sotc: read only by the SOTC_CMD_STATS printer
            SotcStats::upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
        }
        // sotc: SOTC_HOST_IMPORT. A large upload is copied by the GPU from imported guest memory
        // into device-local staging (the CPU waits for that copy, so the bytes are taken now, as
        // below), then from there into the arena on the graphics queue, as below. SOTC_HASH_SMALL
        // only skips runs of up to 16 KiB, which the CPU path below keeps handling for small
        // uploads; RecordCurrentWrite drops the stored hashes of the runs written here.
        if (host_import && total_size_bytes >= host_import->MinBytes()) {
            const auto device_staging =
                staging_pool.Request(total_size_bytes, MemoryType::DeviceLocal);
            if (host_import->Upload(copies, device_staging.buffer->Handle(),
                                    device_staging.offset)) {
                for (auto& copy : copies) {
                    copy.srcOffset += device_staging.offset;
                    copy.dstOffset -= arena->cpu_addr;
                }
                runtime.CopyBuffer(device_staging.buffer, arena, copies);
                for (const auto& copy : copies) {
                    RecordCurrentWrite(arena->cpu_addr + copy.dstOffset, copy.size);
                }
                return is_texel_buffer && !is_written
                           ? SynchronizeMemoryFromImage(arena, device_addr, size)
                           : false;
            }
        }
        // sotc: SOTC_ASYNC_UPLOAD. A large upload is copied into staging by the copy threads
        // (async_upload.h); the staging-to-arena copy is recorded now, and its submit waits for
        // them. The pages of the copies must be write-protected now, so that the write fence
        // sees every guest write to them; hot pages the tracker leaves writable stay here.
        if (auto* async_upload = AsyncUpload::Get(); async_upload &&
                                                     AsyncUpload::BuffersEnabled() &&
                                                     total_size_bytes >= async_upload->MinBytes()) {
            if (TryUploadAsync(*async_upload, arena, copies, total_size_bytes)) { // sotc: may keep a rest in copies
                return is_texel_buffer && !is_written
                           ? SynchronizeMemoryFromImage(arena, device_addr, size)
                           : false;
            }
            SotcStats::async_upload_kept.fetch_add(1, std::memory_order_relaxed);
        }
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        // sotc: SOTC_HASH_SMALL=<bytes> (off by default). A dirty run no larger than that whose bytes
        // equal the ones last uploaded for exactly that run is not copied again: the arena already
        // holds them (any other arena write drops the stored hash, RecordCurrentWrite and
        // UnbindResident). Only the dirty runs the tracker reported are considered, so bytes the
        // tracker calls clean are never overwritten from guest memory: the first version uploaded
        // the whole range and wrote guest bytes over arena data that differs on clean pages
        // (device lost, 2026-10-27).
        // sotc: the bytes hashed are exactly the bytes copied to staging: a run is read from guest
        // memory once into a snapshot, which is hashed and then copied into staging. Before, the
        // run was copied to staging and read again for the hash: a guest write in between (the
        // page is write-protected again, so it faults and sets the CPU bit for the next sync)
        // stored the hash of the NEW bytes while the arena received the OLD ones, and the next
        // sync saw equal hashes and skipped the upload for good. A run that hits is not copied
        // into staging at all.
        boost::container::small_vector<std::pair<vk::BufferCopy, u64>, 4> new_hashes;
        boost::container::small_vector<vk::BufferCopy, 4> bulk; // copied by the passes below
        if (SotcStats::kHashSmall != 0 && !is_written && !is_texel_buffer) {
            boost::container::small_vector<vk::BufferCopy, 4> kept;
            for (const auto& copy : copies) { // here srcOffset = staging offset, dstOffset = address
                if (copy.size > SotcStats::kHashSmall ||
                    IsRegionGpuModified(copy.dstOffset, copy.size)) {
                    kept.push_back(copy);
                    bulk.push_back(copy);
                    continue;
                }
                // Hashed from guest memory: staging can be write-combined (SOTC_REBAR_UPLOAD),
                // which is very slow to read back.
                thread_local std::vector<u8> hash_bytes;
                hash_bytes.resize(copy.size);
                memory->CopySparseMemory(copy.dstOffset, hash_bytes.data(), copy.size);
                const u64 hash = XXH3_64bits(hash_bytes.data(), copy.size);
                bool hit = false;
                {
                    std::scoped_lock lock{hashed_uploads_mutex};
                    const auto [first, last] = hashed_uploads.equal_range(copy.dstOffset);
                    for (auto it = first; it != last && !hit; ++it) {
                        hit = it->second.size == copy.size && it->second.hash == hash;
                    }
                }
                if (hit) {
                    SotcStats::hash_small_hits.fetch_add(1, std::memory_order_relaxed);
                    SotcStats::hash_small_bytes.fetch_add(copy.size, std::memory_order_relaxed);
                    continue;
                }
                SotcStats::hash_small_uploads.fetch_add(1, std::memory_order_relaxed);
                std::memcpy(staging.mapped + copy.srcOffset, hash_bytes.data(), copy.size);
                kept.push_back(copy);
                new_hashes.emplace_back(copy, hash);
            }
            copies.assign(kept.begin(), kept.end());
            if (copies.empty()) {
                return is_texel_buffer && !is_written
                           ? SynchronizeMemoryFromImage(arena, device_addr, size)
                           : false;
            }
        } else {
            bulk.assign(copies.begin(), copies.end());
        }
        // sotc: a large upload made of several ranges (a streaming burst: 200+ MB in one frame)
        // is copied in one parallel pass over 512 KiB pieces of all its ranges; per range, only
        // ranges of 2 MiB and more were split. SOTC_COPY_BATCH=0: per range.
        static const bool copy_batch = [] {
            const char* v = std::getenv("SOTC_COPY_BATCH");
            return !v || v[0] != '0';
        }();
        if (copy_batch && bulk.size() > 1 && total_size_bytes >= 2_MB && BbCopy::Enabled()) {
            struct Piece {
                VAddr source;
                u8* dest;
                u64 size;
            };
            constexpr u64 PieceSize = 512_KB;
            boost::container::small_vector<Piece, 64> pieces;
            for (const auto& copy : bulk) {
                for (u64 done = 0; done < copy.size; done += PieceSize) {
                    pieces.push_back({copy.dstOffset + done, staging.mapped + copy.srcOffset + done,
                                      std::min(PieceSize, copy.size - done)});
                }
            }
            BbCopy::ParallelFor(pieces.size(), [&](std::size_t i) {
                memory->CopySparseMemory(pieces[i].source, pieces[i].dest, pieces[i].size);
            });
        } else {
            for (const auto& copy : bulk) {
                memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset,
                                         copy.size);
            }
        }
        for (auto& copy : copies) {
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        // sotc: SOTC_XFER_QUEUE. Sparse binds of this arena still pending are submitted with the
        // next graphics submit, after any transfer copy routed now: such copies stay put.
        const bool binds_pending = std::ranges::any_of(pending_binds, [arena](const auto& b) {
            return b.arena == arena && !b.binds.empty();
        });
        const bool routed =
            !binds_pending && runtime.TryCopyBufferXfer(staging.buffer, arena, copies);
        if (!routed) {
            runtime.CopyBuffer(staging.buffer, arena, copies);
        }
        for (const auto& copy : copies) {
            RecordCurrentWrite(arena->cpu_addr + copy.dstOffset, copy.size);
        }
        // The hashes go in after RecordCurrentWrite, which drops the older ones of these runs.
        if (!new_hashes.empty()) {
            std::scoped_lock lock{hashed_uploads_mutex};
            if (hashed_uploads.size() >= 4096) { // sotc: capped; refilled by later uploads
                hashed_uploads.clear();
            }
            for (const auto& [copy, hash] : new_hashes) {
                hashed_uploads.emplace(copy.dstOffset, HashedUpload{copy.size, hash});
            }
            hashed_upload_count.store(hashed_uploads.size(), std::memory_order_relaxed);
        }
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

bool BufferCache::QueueAsyncGuestCopy(AsyncUpload& async_upload,
                                      std::span<const vk::BufferCopy> copies,
                                      const Vulkan::StagingBufferRef& staging, u64 total_size) {
    if (!staging.mapped) {
        return false;
    }
    const u8* view = runtime_memory_backing_view(nullptr);
    std::vector<AsyncUpload::Piece> pieces;
    std::vector<AsyncUpload::Range> va;
    std::vector<AsyncUpload::Range> phys;
    for (const auto& copy : copies) { // srcOffset: staging offset, dstOffset: guest address
        va.push_back({copy.dstOffset, copy.dstOffset + copy.size});
        VAddr address = copy.dstOffset;
        u8* dst = staging.mapped + copy.srcOffset;
        u64 left = copy.size;
        while (left > 0) {
            uintptr_t start = 0, end = 0;
            int mapped = 0;
            if (!runtime_memory_region(address, &start, &end, &mapped)) {
                end = address + left;
            }
            const u64 n = std::min<u64>(left, end - address);
            const u8* backing = mapped ? runtime_memory_backing_pointer(address) : nullptr;
            if (mapped && (!backing || !view)) {
                return false; // read through the guest address: on the draw thread only
            }
            pieces.push_back({backing, dst, n});
            if (backing) {
                const u64 offset = static_cast<u64>(backing - view);
                phys.push_back({offset, offset + n});
            }
            address += n;
            dst += n;
            left -= n;
        }
    }
    async_upload.Queue(&scheduler, scheduler.CurrentTick(), std::move(pieces), std::move(va),
                       std::move(phys), staging.buffer, staging.offset, total_size);
    return true;
}

bool BufferCache::TryUploadAsync(AsyncUpload& async_upload, const Buffer* arena,
                                 boost::container::small_vector<vk::BufferCopy, 4>& copies,
                                 u64& total_size_bytes) {
    // sotc: the copies are split at the pages that cannot go to the copy threads: hot pages keep
    // their CPU bit (the tracker leaves them writable, so the write fence would never see a write
    // to them) and guest stacks are never protected. Those parts stay with the caller (copies and
    // total_size_bytes are replaced by them, false is returned); the clean parts are queued for
    // the copy threads. Before, one such page sent the whole upload to the draw thread.
    constexpr u64 Page = 4096;
    boost::container::small_vector<vk::BufferCopy, 4> fast;
    boost::container::small_vector<vk::BufferCopy, 4> slow;
    u64 fast_bytes = 0;
    u64 slow_bytes = 0;
    const auto add = [](auto& list, u64& bytes, VAddr lo, u64 len) {
        if (!list.empty() && list.back().dstOffset + list.back().size == lo) {
            list.back().size += len; // adjacent in address and in staging
        } else {
            list.push_back(vk::BufferCopy{bytes, lo, len});
        }
        bytes += len;
    };
    const auto blocked = [&](VAddr lo, u64 len) {
        return memory_tracker->IsRegionCpuModified(lo, len) || IntersectsGuestStack(lo, len);
    };
    for (const auto& copy : copies) { // srcOffset: staging offset, dstOffset: guest address
        if (!blocked(copy.dstOffset, copy.size)) {
            add(fast, fast_bytes, copy.dstOffset, copy.size);
            continue;
        }
        const VAddr end = copy.dstOffset + copy.size;
        for (VAddr at = copy.dstOffset; at < end;) {
            const VAddr seg_end = std::min<VAddr>(Common::AlignDown(at, Page) + Page, end);
            if (blocked(at, seg_end - at)) {
                add(slow, slow_bytes, at, seg_end - at);
            } else {
                add(fast, fast_bytes, at, seg_end - at);
            }
            at = seg_end;
        }
    }
    if (fast.empty() || (!slow.empty() && fast_bytes < async_upload.MinBytes())) {
        return false; // nothing for the copy threads, or not worth splitting: all stays here
    }
    const auto staging = staging_pool.Request(fast_bytes, MemoryType::HostUncached);
    if (!QueueAsyncGuestCopy(async_upload, fast, staging, fast_bytes)) {
        return false;
    }
    for (auto& copy : fast) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    runtime.CopyBuffer(staging.buffer, arena, fast);
    for (const auto& copy : fast) {
        RecordCurrentWrite(arena->cpu_addr + copy.dstOffset, copy.size);
    }
    if (slow.empty()) {
        return true;
    }
    copies.assign(slow.begin(), slow.end());
    total_size_bytes = slow_bytes;
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    // sotc: SOTC_HASH_SMALL: the image may rewrite this arena range, which voids the stored hash.
    EraseHashedUploads(device_addr, size);
    // sotc: a current write is recorded only on the two paths that write the arena range. Recording
    // it unconditionally marked unwritten ranges as written by the command buffer being recorded,
    // which sent their readbacks to the slower flushing path instead of the ahead path.
    if (SotcStats::OptOff()) {
        RecordCurrentWrite(device_addr, size);
    }
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            RecordCurrentWrite(device_addr, size);
            return true;
        } else if (static u32 shown = 0; shown < 4) {
            // sotc: first few only. In PS4 Pro performance mode the game reads CMask surfaces as
            // texel buffers hundreds of times a second; each stderr line (piped to the launcher)
            // took milliseconds on the draw thread: 28 vs 16 ms per frame in the same scene.
            ++shown;
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {} (logged {} of 4)",
                        magic_enum::enum_name(*type), shown);
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    RecordCurrentWrite(device_addr, size);
    return true;
}

// sotc: SOTC_ARENA_TICK_FREE=1 (default off): freed arena runs are reused only once the GPU
// has passed the tick of their unbind, and an exhausted card waits for the oldest freed run of
// video memory instead of for all GPU work (Finish).
static bool ArenaTickFree() {
    // sotc: also on with SOTC_UNBIND_GPU_WAIT=1: the unmap no longer waits for the GPU, so the
    // blocks it frees must not be handed out again before the GPU has passed the unbind.
    static const bool on = [] {
        const char* v = std::getenv("SOTC_ARENA_TICK_FREE");
        const char* unbind = std::getenv("SOTC_UNBIND_GPU_WAIT");
        return (v && v[0] == '1') || (unbind && unbind[0] == '1');
    }();
    return on;
}

std::pair<vk::DeviceMemory, u64> BufferCache::AllocateArenaBlocks(u64 blocks) {
    if (ArenaTickFree()) {
        ReclaimArenaRuns();
    }
    // Room in a chunk of video memory, then in a chunk of system memory.
    const auto take = [&](u32 memory_type) -> std::optional<std::pair<vk::DeviceMemory, u64>> {
        for (auto& chunk : arena_chunks) {
            if (chunk->memory_type != memory_type) {
                continue;
            }
            for (auto it = chunk->free_runs.begin(); it != chunk->free_runs.end(); ++it) {
                if (it->second < blocks) {
                    continue;
                }
                const auto [start, length] = *it;
                chunk->free_runs.erase(it);
                if (length > blocks) {
                    chunk->free_runs.emplace(start + blocks, length - blocks);
                }
                chunk->used_blocks += blocks;
                return std::make_pair(chunk->memory, start);
            }
        }
        return std::nullopt;
    };
    const auto grow = [&](u32 memory_type, bool exact = false)
        -> std::optional<std::pair<vk::DeviceMemory, u64>> {
        // exact: only the asked size, for a nearly full card that has no 64 MiB left (#137).
        const u64 chunk_blocks = exact ? blocks : std::max<u64>(ARENA_CHUNK_SIZE >> block_shift, blocks);
        const vk::MemoryAllocateInfo alloc_info = {
            .allocationSize = chunk_blocks << block_shift,
            .memoryTypeIndex = memory_type,
        };
        const auto [result, memory] = instance.GetDevice().allocateMemory(alloc_info);
        if (result != vk::Result::eSuccess) {
            return std::nullopt;
        }
        SotcStats::arena_allocs.fetch_add(1, std::memory_order_relaxed);
        SotcStats::arena_bytes.fetch_add(alloc_info.allocationSize, std::memory_order_relaxed);
        auto& chunk = arena_chunks.emplace_back(std::make_unique<ArenaChunk>());
        chunk->memory = memory;
        chunk->memory_type = memory_type;
        chunk->free_runs.emplace(0, chunk_blocks);
        chunk_by_memory.emplace(static_cast<VkDeviceMemory>(memory), chunk.get());
        return take(memory_type);
    };
    if (auto placed = take(arena_memory_type_index)) {
        return *placed;
    }
    if (auto placed = grow(arena_memory_type_index)) {
        return *placed;
    }
    if (arena_fallback_memory_type_index) {
        // Keep running from system memory (slower GPU access) instead of aborting.
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render, "Video memory exhausted, placing new buffer memory in system RAM");
        }
        if (auto placed = take(*arena_fallback_memory_type_index)) {
            return *placed;
        }
        if (auto placed = grow(*arena_fallback_memory_type_index)) {
            return *placed;
        }
    }
    // Drop old cached images, wait for the GPU so their memory is really released, try again.
    LOG_WARNING(Render,
                "Video memory exhausted, collecting cached images (asked {} KB; in use {} MB of {} "
                "MB; arenas {} chunks, {} MB)",
                (blocks << block_shift) >> 10, instance.GetDeviceMemoryUsage() >> 20,
                SotcStats::vram_budget.load() >> 20, SotcStats::arena_allocs.load(),
                SotcStats::arena_bytes.load() >> 20);
    texture_cache.EmergencyCollect();
    if (ArenaTickFree()) {
        // sotc: no Finish. Reclaim the freed runs the GPU has passed; if none fits, wait for the
        // oldest freed video memory run only, and try again, until none is left pending.
        for (;;) {
            ReclaimArenaRuns();
            scheduler.PopPendingOperations();
            if (auto placed = take(arena_memory_type_index)) {
                return *placed;
            }
            if (auto placed = grow(arena_memory_type_index)) {
                return *placed;
            }
            if (arena_fallback_memory_type_index) {
                if (auto placed = take(*arena_fallback_memory_type_index)) {
                    return *placed;
                }
            }
            const auto oldest =
                std::find_if(pending_arena_runs.begin(), pending_arena_runs.end(),
                             [&](const PendingArenaRun& run) {
                                 return run.memory_type == arena_memory_type_index;
                             });
            if (oldest == pending_arena_runs.end()) {
                break;
            }
            const u64 wait_tick = oldest->tick;
            SotcStats::arena_tick_waits.fetch_add(1, std::memory_order_relaxed);
            scheduler.Wait(wait_tick);
        }
    } else {
        scheduler.Finish();
        scheduler.PopPendingOperations();
    }
    if (auto placed = grow(arena_memory_type_index)) {
        return *placed;
    }
    // sotc: still no room for a whole chunk (#137: 6 GB cards): the asked size alone, then
    // system memory, before giving up.
    if (auto placed = grow(arena_memory_type_index, true)) {
        return *placed;
    }
    if (arena_fallback_memory_type_index) {
        if (auto placed = grow(*arena_fallback_memory_type_index, true)) {
            return *placed;
        }
    }
    LOG_CRITICAL(Render, "Out of video and system memory for buffers; lower the video memory use "
                         "setting in the launcher (Economy)");
    ASSERT_MSG(false, "Failed to allocate buffer arena memory");
    return {};
}

u64 BufferCache::ArenaUsedBytes() const noexcept {
    u64 blocks = 0;
    for (const auto& chunk : arena_chunks) {
        blocks += chunk->used_blocks;
    }
    return blocks << block_shift;
}

void BufferCache::FreeArenaBlocks(vk::DeviceMemory memory, u64 start, u64 blocks) {
    if (ArenaTickFree()) {
        // sotc: the run stays used (its chunk too) until the GPU passes this tick. The unbind is
        // applied by the next submit, which signals a tick no later than this one.
        if (const auto found = chunk_by_memory.find(static_cast<VkDeviceMemory>(memory));
            found != chunk_by_memory.end()) {
            pending_arena_runs.push_back({memory, found->second->memory_type, start, blocks,
                                          scheduler.CurrentTick()});
        }
        return;
    }
    ReturnArenaRun(memory, start, blocks);
}

void BufferCache::ReturnArenaRun(vk::DeviceMemory memory, u64 start, u64 blocks) {
    const auto found = chunk_by_memory.find(static_cast<VkDeviceMemory>(memory));
    if (found == chunk_by_memory.end()) {
        return;
    }
    ArenaChunk& chunk = *found->second;
    chunk.used_blocks -= std::min(chunk.used_blocks, blocks);
    // Give the run back, merged with free neighbours.
    auto next = chunk.free_runs.lower_bound(start);
    if (next != chunk.free_runs.end() && next->first == start + blocks) {
        blocks += next->second;
        next = chunk.free_runs.erase(next);
    }
    if (next != chunk.free_runs.begin()) {
        const auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            blocks += prev->second;
            chunk.free_runs.erase(prev);
        }
    }
    chunk.free_runs.emplace(start, blocks);
    if (chunk.used_blocks != 0) {
        return;
    }
    // Empty: give the memory back. The unbind is submitted before this tick's work, which
    // waits for it.
    SotcStats::arena_allocs.fetch_sub(1, std::memory_order_relaxed);
    chunk_by_memory.erase(found);
    std::erase_if(arena_chunks, [&](const auto& c) {
        if (c->memory != memory) {
            return false;
        }
        u64 size = 0;
        for (const auto& [run_start, run_blocks] : c->free_runs) {
            size += run_blocks;
        }
        SotcStats::arena_bytes.fetch_sub(size << block_shift, std::memory_order_relaxed);
        return true;
    });
    scheduler.DeferOperation([device = instance.GetDevice(), memory] { device.freeMemory(memory); });
}

void BufferCache::ReclaimArenaRuns() {
    // sotc: the GPU tick is the one of the submit that unbound the run (see FreeArenaBlocks).
    while (!pending_arena_runs.empty() && scheduler.IsFree(pending_arena_runs.front().tick)) {
        const PendingArenaRun run = pending_arena_runs.front();
        pending_arena_runs.pop_front();
        ReturnArenaRun(run.memory, run.start, run.blocks);
    }
}

void BufferCache::ReleaseMemory(VAddr device_addr, u64 size) {
    // Only whole blocks inside the unmapped range can go; partial blocks may back neighbours.
    const u64 first_block = Common::AlignUp(device_addr, u64{block_size}) >> block_shift;
    const u64 end_block = (device_addr + size) >> block_shift;
    if (first_block >= end_block) {
        return;
    }
    evicted_ranges.Subtract(first_block, end_block);
    pinned_ranges.Subtract(first_block, end_block);
    boost::container::small_vector<Backing, 16> released;
    const auto collect = [&] {
        released.clear();
        resident_ranges.ForEachInRange(first_block, end_block, [&](const Backing& backing) {
            const u64 start = std::max(first_block, backing.start);
            const u64 end = std::min(end_block, backing.end);
            if (start < end) {
                released.push_back(backing.SubRange(start, end));
            }
        });
    };
    collect();
    if (released.empty()) {
        return;
    }
    ++bind_version; // sotc: SOTC_BIND_CACHE_XTICK: cached binds of the range are void from now on
    // sotc: SOTC_UNBIND_GPU_WAIT=1 (default off): no CPU wait for the GPU here. The unbind's
    // sparse bind waits GPU-side for the work submitted before it instead (see
    // SubmitPendingArenaBinds).
    static const bool unbind_gpu_wait = [] {
        const char* v = std::getenv("SOTC_UNBIND_GPU_WAIT");
        return v && v[0] == '1';
    }();
    // Work recorded before the unmap may still write this memory, and sparse binds are not
    // ordered against earlier submissions: finish that work before unbinding, or the GPU
    // writes to unbound memory (WriteInvalid, device lost). Unmaps are rare, the sync is cheap.
    const auto sync_start = SotcStats::kFpsLog ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    if (unbind_gpu_wait) {
        scheduler.Flush();
    } else {
        scheduler.Finish();
    }
    if (SotcStats::kFpsLog) { // sotc: read only by the SOTC_FPS printer (sotc_stats.h)
        SotcStats::release_finish_count.fetch_add(1, std::memory_order_relaxed);
        SotcStats::release_finish_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - sync_start)
                .count(),
            std::memory_order_relaxed);
    }
    // sotc: the deferred operations Finish ran (fault buffer rebinds) may have bound more of
    // the range, and their binds must not share the unbind's sparse batch.
    collect();
    if (!pending_binds.empty()) {
        scheduler.Flush();
    }
    // sotc: SOTC_RB_PREFETCH: the range loses its GPU-modified state below without a readback;
    // a prefetch of it must never land afterwards (the guest may map other data there).
    if (rb_prev_env) {
        PrevErase(first_block << block_shift, (end_block - first_block) << block_shift);
    }
    if (rb_prefetch_env && !pf_live.empty()) {
        PrefetchMarkDirty(first_block << block_shift, (end_block - first_block) << block_shift);
    }
    if (RbxOn() && rbx_owed_count.load(std::memory_order_relaxed) != 0) {
        RbxTrim(first_block << block_shift, end_block << block_shift); // sotc: SOTC_RB_EXACT
    }
    UnbindResident(released);
    std::erase_if(hot_readbacks, [&](const HotReadback& hot) {
        return hot.addr < (end_block << block_shift) &&
               (first_block << block_shift) < hot.addr + hot.size;
    });
    resident_ranges.Subtract(first_block, end_block);
    ++resident_version; // sotc: invalidates the merged DMA runs
    {
        std::unique_lock gpu_modified_lock{gpu_modified_mutex};
        gpu_modified_ranges.Subtract(first_block << block_shift,
                                     (end_block - first_block) << block_shift);
    }
    if (unbind_gpu_wait) {
        // sotc: the unbind's bind waits for the last submitted work tick. The submit of that
        // tick may still sit with the recording thread: a wait queued before its signal blocks
        // the queue for good, so wait until it is queued (not until the GPU ran it).
        const u64 last_submitted = scheduler.CurrentTick() - 1;
        pending_bind_wait_tick = std::max(pending_bind_wait_tick, last_submitted);
        scheduler.WaitSubmitted(pending_bind_wait_tick);
        unbind_bind_next = true;
    }
    // sotc: a bind of the same blocks (the guest maps them again) must not share the unbind's
    // sparse bind batch: binds of one range within a batch are not ordered (the evicted blocks
    // of MaybeEvictCold came back unbound: black scenes).
    scheduler.Flush();
}

void BufferCache::UnbindResident(std::span<const Backing> released) {
    ++bind_version; // sotc: SOTC_BIND_CACHE_XTICK: bindings of these ranges are void
    for (const Backing& range : released) {
        // sotc: the memory is unbound (zeroed when it comes back): its hashed uploads and granule
        // snapshots are void.
        EraseHashedUploads(range.start << block_shift, (range.end - range.start) << block_shift);
        EraseGranuleSnapshots(range.start << block_shift, (range.end - range.start) << block_shift);
        // Split at arena page boundaries, each page may belong to a different arena.
        for (u64 start = range.start; start < range.end;) {
            const u64 page = start >> blocks_per_arena_page_shift;
            const u64 end = std::min(range.end, (page + 1) << blocks_per_arena_page_shift);
            if (const Buffer* arena = address_space[page]) {
                BindsForArena(arena)->binds.push_back(vk::SparseMemoryBind{
                    .resourceOffset = (start << block_shift) - arena->cpu_addr,
                    .size = (end - start) << block_shift,
                    .memory = {},
                    .memoryOffset = 0,
                });
            }
            start = end;
        }
        // Shaders reach arena memory through the BDA page table; a null entry faults the block
        // back in (fault buffer) instead of touching unbound memory.
        runtime.FillBuffer(bda_pagetable_buffer.get(), range.start * sizeof(vk::DeviceAddress),
                           (range.end - range.start) * sizeof(vk::DeviceAddress), 0u);
        FreeArenaBlocks(range.memory, range.offset, range.end - range.start);
    }
}

bool BufferCache::MaybeEvictCold(std::chrono::steady_clock::time_point now) {
    // sotc: the game keeps its 4 GiB direct memory pool mapped and hands its addresses out
    // again and again, so arena blocks became resident once and stayed: ~45 MB/min in the
    // attract demo, over the 6 GB card's budget after ~40 minutes. Once resident arena memory
    // passes SOTC_ARENA_EVICT_MB (default 1536, 0 = never), allocations (4 MiB slabs or exact
    // ones) whose blocks were not bound for SOTC_ARENA_EVICT_IDLE_S (default 30) seconds and
    // hold no GPU-modified data go back, coldest first, up to SOTC_ARENA_EVICT_STEP_MB (default
    // 64) per check (every 2 s). Their guest pages are marked CPU-modified, so the next bind
    // uploads them again.
    // The arena does not always match guest memory on pages the tracker calls clean (attract
    // demo: 0-13,000 of 2,000-65,000 pages per run; mostly not image memory). Uploading guest
    // memory over those blanked whole scenes, so each allocation is read back first and goes
    // only when every page that is not CPU-modified is equal (a mismatch is tried again after
    // 60 s). Shaders reading arena memory through pointers fault an evicted block back in (one
    // frame of zeros); blocks that came back that way stay resident from then on.
    static const u64 limit = [] {
        const char* v = std::getenv("SOTC_ARENA_EVICT_MB");
        return (v ? std::strtoull(v, nullptr, 10) : 1536ull) << 20;
    }();
    static const u64 step = [] {
        const char* v = std::getenv("SOTC_ARENA_EVICT_STEP_MB");
        return std::max<u64>(v ? std::strtoull(v, nullptr, 10) : 64ull, 1) << 20;
    }();
    static const u16 idle_epochs = [] {
        const char* v = std::getenv("SOTC_ARENA_EVICT_IDLE_S");
        return static_cast<u16>(std::clamp<unsigned long>(v ? std::strtoul(v, nullptr, 10) : 30ul,
                                                          2ul, 30000ul));
    }();
    if (pending_eviction) {
        AdvanceUseEpoch(now);
        return FinishEviction(now, idle_epochs, false);
    }
    if (now < next_evict_check) {
        return false;
    }
    // sotc: SOTC_ADAPTIVE_LOAD (sotc_adaptive.h): while the governor is relieved, a new check
    // (scan, readback of the candidates) waits, at most 8 s past its time; an eviction already
    // under way finishes above. Draw thread only (FlushPeriodic), hence the plain static.
    if (SotcAdaptive::DeferBackground() && now < next_evict_check + std::chrono::seconds(8)) {
        static std::chrono::steady_clock::time_point counted{};
        if (SotcStats::kFpsLog && counted != next_evict_check) {
            counted = next_evict_check;
            SotcStats::adaptive_evict_deferred.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    next_evict_check = now + std::chrono::seconds(2);
    AdvanceUseEpoch(now);
    const u64 resident = ArenaUsedBytes();
    if (limit == 0 || resident <= limit) {
        return false;
    }
    const SotcStats::ScopedTimer evict_timer{SotcStats::arena_evict_ns};
    // sotc: with the 64 MiB chunks the unit is a resident range (its blocks go back to their
    // chunk, and a chunk left empty to the driver), not a whole allocation.
    struct Candidate {
        u16 age{0xFFFF}; // youngest block
        bool keep{};
        u64 bytes{};
        boost::container::small_vector<Backing, 4> ranges;
    };
    std::unordered_map<u64, Candidate> candidates;
    std::erase_if(evict_retry, [&](const auto& entry) { return entry.second <= now; });
    std::vector<std::pair<u16, u64>> order;
    for (const Backing& range : resident_ranges) {
        auto& candidate = candidates[range.start];
        for (u64 block = range.start; block < range.end && !candidate.keep; ++block) {
            const u16 age = static_cast<u16>(use_epoch - block_last_use[block]);
            candidate.age = std::min(candidate.age, age);
            candidate.keep = age < idle_epochs;
        }
        const VAddr addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        candidate.keep = candidate.keep || pinned_ranges.Overlaps(range.start, range.end) ||
                         memory_tracker->IsRegionGpuModified(addr, size) ||
                         [&] {
                             std::shared_lock gpu_modified_lock{gpu_modified_mutex};
                             return gpu_modified_ranges.Intersects(addr, size) ||
                                    RbxOwedIntersects(addr, size); // owed last (see ReadClean)
                         }() ||
                         evict_retry.contains(range.start) || size > step;
        candidate.bytes = size;
        candidate.ranges.push_back(range);
        if (!candidate.keep) {
            order.emplace_back(candidate.age, range.start);
        }
    }
    if (order.empty()) {
        next_evict_check = now + std::chrono::seconds(10); // all of it in use: look again later
        return false;
    }
    std::ranges::sort(order, std::greater{});

    // Read the coldest allocations back, up to `step` bytes of blocks.
    std::vector<EvictChoice> chosen;
    u64 total = 0;
    for (const auto& [age, key] : order) {
        if (total >= step || resident - total <= limit / 8 * 7) {
            break;
        }
        const Candidate& candidate = candidates[key];
        EvictChoice& pick = chosen.emplace_back(EvictChoice{key, candidate.bytes, {}, candidate.ranges});
        for (const Backing& range : candidate.ranges) {
            // Split at arena page boundaries, each page may belong to a different arena.
            for (u64 start = range.start; start < range.end;) {
                const u64 page = start >> blocks_per_arena_page_shift;
                const u64 end = std::min(range.end, (page + 1) << blocks_per_arena_page_shift);
                pick.pieces.push_back({start << block_shift, (end - start) << block_shift, total});
                total += (end - start) << block_shift;
                start = end;
            }
        }
    }
    const auto download = staging_pool.Request(total, MemoryType::HostCached, 0, true);
    for (const EvictChoice& pick : chosen) {
        for (const EvictPiece& piece : pick.pieces) {
            const Buffer* arena = address_space[piece.addr >> ARENA_PAGE_BITS];
            if (!arena) {
                continue; // never resident: the comparison fails and the allocation stays
            }
            const vk::BufferCopy copy{arena->Offset(piece.addr), download.offset + piece.offset,
                                      piece.size};
            runtime.CopyBuffer(arena, download.buffer, std::span{&copy, 1});
        }
    }
    pending_eviction = PendingEviction{std::move(chosen), download, scheduler.CurrentTick(),
                                       now + std::chrono::seconds(5)};
    // sotc: SOTC_EVICT_ASYNC=0 waits for the read-back here, as before: scheduler.Finish() on the
    // command processor, ~50 ms per 64 MiB run, one late frame each.
    static const bool async = [] {
        const char* v = std::getenv("SOTC_EVICT_ASYNC");
        return !v || v[0] != '0';
    }();
    return async ? false : FinishEviction(now, idle_epochs, true);
}

bool BufferCache::FinishEviction(std::chrono::steady_clock::time_point now, u16 idle_epochs,
                                 bool wait) {
    // sotc: the unbind's sparse batch does not wait for earlier submits, so it goes only once
    // every command buffer submitted so far (the read-back's included) is done. The GPU catches
    // up within a frame or two at the benchmark scenes; after 5 s, or with `wait`, Finish() it.
    PendingEviction& pending = *pending_eviction;
    if (!scheduler.IsFree(pending.tick) || !scheduler.IsFree(scheduler.CurrentTick() - 1)) {
        if (!wait && now < pending.deadline) {
            return false;
        }
        scheduler.Finish();
    }
    const SotcStats::ScopedTimer evict_timer{SotcStats::arena_evict_ns};
    const auto& download = pending.download;
    download.buffer->Invalidate(download.offset, download.size);

    std::vector<Backing> released;
    std::array<u8, 4096> guest;
    for (const EvictChoice& pick : pending.chosen) {
        // Bound again, or written by the GPU, after the read-back was recorded: stays.
        bool keep = false;
        for (const Backing& range : pick.ranges) {
            for (u64 block = range.start; block < range.end && !keep; ++block) {
                keep = static_cast<u16>(use_epoch - block_last_use[block]) < idle_epochs;
            }
            const VAddr addr = range.start << block_shift;
            const u64 size = (range.end - range.start) << block_shift;
            keep = keep || pinned_ranges.Overlaps(range.start, range.end) ||
                   memory_tracker->IsRegionGpuModified(addr, size) || [&] {
                       std::shared_lock gpu_modified_lock{gpu_modified_mutex};
                       return gpu_modified_ranges.Intersects(addr, size) ||
                              RbxOwedIntersects(addr, size); // owed last (see ReadClean)
                   }();
        }
        if (keep) {
            continue;
        }
        bool equal = true;
        for (const EvictPiece& piece : pick.pieces) {
            for (u64 at = 0; at < piece.size && equal; at += guest.size()) {
                const VAddr addr = piece.addr + at;
                if (memory_tracker->IsRegionCpuModified(addr, guest.size())) {
                    continue; // uploaded at the next bind anyway
                }
                if (!memory->TryCopySparseMemory(addr, guest.data(), guest.size())) {
                    continue; // unmapped: nothing can read it
                }
                equal = std::memcmp(download.mapped + piece.offset + at, guest.data(),
                                    guest.size()) == 0;
            }
        }
        if (!equal) {
            evict_retry.emplace(pick.key, now + std::chrono::seconds(60));
            SotcStats::arena_evict_mismatch.fetch_add(pick.bytes, std::memory_order_relaxed);
            continue;
        }
        released.insert(released.end(), pick.ranges.begin(), pick.ranges.end());
    }
    staging_pool.FreeDeferred(download);
    pending_eviction.reset();
    // Binds queued by Finish's deferred operations (fault rebinds, arena migration) must not
    // share the unbind's sparse batch; the chosen ranges must still be resident as chosen.
    if (!pending_binds.empty()) {
        scheduler.Flush();
    }
    std::erase_if(released, [&](const Backing& range) {
        bool same = false;
        resident_ranges.ForEachInRange(range.start, range.end, [&](const Backing& now_range) {
            same |= now_range.start <= range.start && range.end <= now_range.end &&
                    now_range.memory == range.memory;
        });
        return !same;
    });
    if (!released.empty()) {
        ++bind_version; // sotc: SOTC_BIND_CACHE_XTICK: before the ranges turn CPU-modified
    }
    for (const Backing& range : released) {
        const VAddr addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        memory_tracker->MarkRegionAsCpuModified(addr, size);
        evicted_ranges.Add({range.start, range.end});
        SotcStats::arena_evicted.fetch_add(size, std::memory_order_relaxed);
        std::erase_if(hot_readbacks, [&](const HotReadback& hot) {
            return hot.addr < addr + size && addr < hot.addr + hot.size;
        });
        resident_ranges.Subtract(range.start, range.end);
    }
    if (!released.empty()) {
        UnbindResident(released);
        ++resident_version; // sotc: invalidates the merged DMA runs and the resident cache
        // A bind of the same blocks recorded before the next submit would share one sparse
        // bind batch with the unbind; submit the unbind on its own.
        scheduler.Flush();
    }
    SotcStats::arena_evict_runs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BufferCache::NoteFaultRebind(VAddr addr, u64 size) {
    const u64 first_block = addr >> block_shift;
    const u64 end_block = (addr + size + block_size - 1) >> block_shift;
    evicted_ranges.ForEachInRange(first_block, end_block, [&](const Interval& evicted) {
        const u64 start = std::max(first_block, evicted.start);
        const u64 end = std::min(end_block, evicted.end);
        pinned_ranges.Add({start, end});
        SotcStats::arena_pinned.fetch_add((end - start) << block_shift, std::memory_order_relaxed);
    });
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    // sotc: binds stamp blocks with use_epoch; keep it current even when no eviction check runs.
    AdvanceUseEpoch(std::chrono::steady_clock::now());
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    // sotc: SOTC_UNBIND_GPU_WAIT: GPU-side waits instead of the CPU wait of ReleaseMemory. Both
    // are timeline waits on values already queued (no wait on a signal that is not queued yet).
    vk::Semaphore wait_semas[3];
    u64 wait_ticks[3];
    u32 num_waits = 0;
    // sotc: SOTC_RB_TICK: transfer-queue readback copies read the arenas without being ordered
    // by the graphics timeline; a bind (an unbind above all) must not run under one. The value
    // was submitted already, so the wait cannot dangle.
    if (const u64 rb_xfer = scheduler.XferLastReadbackTick(); rb_xfer != 0) {
        wait_semas[num_waits] = scheduler.XferSemaphore();
        wait_ticks[num_waits++] = rb_xfer;
    }
    if (pending_bind_wait_tick) {
        wait_semas[num_waits] = scheduler.GetWorkSemaphore()->Handle();
        wait_ticks[num_waits++] = std::exchange(pending_bind_wait_tick, 0);
    }
    if (unbind_bind_tick) {
        wait_semas[num_waits] = memory_semaphore.Handle();
        wait_ticks[num_waits++] = unbind_bind_tick;
    }
    // Set after the waits above: this bind must not wait for its own signal.
    if (std::exchange(unbind_bind_next, false)) {
        unbind_bind_tick = signal_tick;
    }

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = num_waits,
        .pWaitSemaphoreValues = wait_ticks,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = num_waits,
        .pWaitSemaphores = wait_semas,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    // Queue access is serialized by submit_mutex, which the scheduler no longer holds here.
    std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
    const auto bind_start = std::chrono::steady_clock::now();
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    // sotc: a bind of many ranges (an arena migration rebinds all of them) can take long.
    if (const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - bind_start)
                            .count();
        ms >= 20) {
        size_t num_binds = 0;
        for (const auto& binds : pending_binds) {
            num_binds += binds.binds.size();
        }
        LOG_WARNING(Render, "Sparse bind of {} ranges in {} arenas took {} ms", num_binds,
                    pending_binds.size(), ms);
    }

    pending_binds.clear();
}

namespace {
struct ReadbackBatchState {
    u32 depth = 0;
    Vulkan::Scheduler* pending = nullptr; ///< a readback left its submit to the outer scope
};
thread_local ReadbackBatchState rb_batch;
bool ReadbackBatchEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_RB_BATCH");
        return v && v[0] == '1';
    }();
    return enabled;
}
} // namespace

ReadbackBatchScope::ReadbackBatchScope() {
    ++rb_batch.depth;
}

ReadbackBatchScope::~ReadbackBatchScope() {
    if (--rb_batch.depth == 0 && rb_batch.pending) {
        Vulkan::Scheduler* scheduler = std::exchange(rb_batch.pending, nullptr);
        scheduler->Flush();
        SotcStats::rb_batch_submits.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ReadbackBatchScope::Defer(Vulkan::Scheduler& scheduler) {
    if (rb_batch.depth == 0 || !ReadbackBatchEnabled() ||
        (rb_batch.pending && rb_batch.pending != &scheduler)) {
        return false;
    }
    if (rb_batch.pending) {
        SotcStats::rb_batch_saved.fetch_add(1, std::memory_order_relaxed);
    }
    rb_batch.pending = &scheduler;
    return true;
}

} // namespace VideoCore

// sotc: SOTC_RB_GROUP report line (driver.cpp, SOTC_FPS). The "faults avoided" figure is an upper
// bound: delivered ranges the guest never reads count too.
bool SotcStats::RbGroupTake(char* out, std::size_t out_size) {
    const auto take = [](std::atomic<uint64_t>& counter) {
        return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed));
    };
    const auto delivered = take(rbg_delivered);
    const auto first = take(rbg_first_hit);
    const auto second = take(rbg_second_hit);
    const auto ranges = take(rbg_ranges);
    const auto runs = take(rbg_runs);
    const auto bytes = take(rbg_bytes);
    const auto bytes_max = take(rbg_bytes_max);
    const auto applied = take(rbg_applied);
    const auto applied_bytes = take(rbg_applied_bytes);
    const auto faults = take(rbg_faults);
    const auto late = take(rbg_late);
    const auto learned = take(rbg_learned);
    const auto trimmed = take(rbg_trimmed);
    const auto cold = take(rbg_fb_cold);
    const auto old = take(rbg_fb_old);
    const auto size = take(rbg_fb_size);
    const auto cap = take(rbg_fb_cap);
    const auto empty = take(rbg_fb_empty);
    const auto dirty = take(rbg_fb_dirty);
    const auto expired = take(rbg_expired);
    const auto s_window = take(rbg_skip_window);
    const auto s_owned = take(rbg_skip_owned);
    const auto s_cpu = take(rbg_skip_cpu);
    const auto s_owed = take(rbg_skip_owed);
    const auto s_stale = take(rbg_skip_stale);
    if ((delivered | applied | faults | cold | old | size | cap | empty | dirty | expired) == 0) {
        return false;
    }
    const double n = delivered ? static_cast<double>(delivered) : 1.0;
    std::snprintf(out, out_size,
                  "SOTC RB group: delivered %llu (%llu at the first fault, %llu at the second; "
                  "per group %.1f ranges, %.1f page runs, %.1f KiB, max %.1f KiB), applied %llu "
                  "(%.2f MiB), faults on groups %llu (late %llu), faults avoided <= %llu (est.), "
                  "keys learned %llu; not delivered: cold %llu, old %llu, size %llu, cap %llu, "
                  "empty %llu, dirty %llu, expired %llu; pages left out: window %llu, owned %llu, "
                  "CPU bit %llu, owed %llu, stale %llu; %.1f KiB trimmed",
                  delivered, first, second, ranges / n, runs / n, bytes / n / 1024.0,
                  bytes_max / 1024.0, applied, applied_bytes / 1048576.0, faults, late,
                  ranges > late ? ranges - late : 0ULL, learned, cold, old, size, cap, empty, dirty,
                  expired, s_window, s_owned, s_cpu, s_owed, s_stale, trimmed / 1024.0);
    return true;
}
