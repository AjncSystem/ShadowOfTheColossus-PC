#include "bbport_write_log.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_copy.h"
#include "bbport_thread_cpu.h" // sotc: SOTC_STALL_DETAIL guest CPU
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "bbport_toggles.h"
#include "video_core/sotc_fps_cap.h"
#include "video_core/sotc_stats.h"
#include "video_core/page_manager.h" // sotc: bbgpu_declare_cpu_write (SOTC_WRITE_DECL)
#include "../sotc/write_decl.h"      // sotc: SOTC_WRITE_DECL statistics
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#ifdef _WIN32
#include <windows.h>
#include "../third_party/renderdoc/renderdoc_app.h"
#endif
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../bbgpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "video_core/buffer_cache/async_upload.h"
#include "common/signal_context.h" // sotc: SOTC_FENCE_PIECES (read faults)
#include "core/memory.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/cache_storage.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_trace.h" // sotc: SOTC_TRACE

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_copy_out(uintptr_t source, unsigned char* dest, uint64_t size);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_read_tsc(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
            // sotc: images over a range mapped now (the fine mips Shadow of the Colossus streams
            // into memory it maps later) were uploaded and tracked without these pages; the data
            // the game writes there next would go unnoticed. Reupload them on their next use.
            static const bool map_invalidate = [] {
                const char* v = std::getenv("SOTC_MAP_INVALIDATE");
                return !v || v[0] != '0';
            }();
            if (SotcStats::kCmdStats) SotcStats::inv_map.fetch_add(size, std::memory_order_relaxed);
            if (map_invalidate) r->InvalidateMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            if (SotcStats::kCmdStats) SotcStats::inv_protect.fetch_add(size, std::memory_order_relaxed);
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
// sotc: SOTC_COPY_LOCK_ONCE=1: one lookup pass per copy (runtime_memory_copy_out) instead of two
// table lookups per mapping run, each of which could wait for a map batch at an area change.
// Read at static initialisation: copies also run inside the fault handler.
static const bool kCopyLockOnce = [] {
    const char* v = std::getenv("SOTC_COPY_LOCK_ONCE");
    return v && v[0] == '1';
}();
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    if (kCopyLockOnce && size && runtime_memory_copy_out(source, dest, size)) {
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        // sotc: read through the backing view, never through the guest address. With precise
        // readbacks GPU-written pages are read-protected, and a fault on a copy thread or the
        // recording thread would wait for the command processor, which waits for them.
        const u8* backing = mapped ? runtime_memory_backing_pointer(source) : nullptr;
        if (backing) std::memcpy(dest, backing, n);
        else if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // bbport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) {
    return runtime_memory_is_mapped(virtual_addr, size ? size : 1) != 0;
}
bool MemoryManager::IsMappedAddress(VAddr virtual_addr) {
    return runtime_memory_is_mapped(virtual_addr, 1) != 0;
}
bool MemoryManager::TryCopySparseMemory(VAddr source, u8* dest, u64 size) {
    // Through the backing view: the guest address may be protected by the GPU page tracker.
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped) || !mapped) {
            return false;
        }
        const u8* backing = runtime_memory_backing_pointer(source);
        if (!backing) {
            return false;
        }
        const u64 n = std::min<u64>(size, end - source);
        std::memcpy(dest, backing, n);
        source += n;
        dest += n;
        size -= n;
    }
    return true;
}
const u8* MemoryManager::GetBackingRun(VAddr address, VAddr* run_start, u64* run_size) const {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return nullptr;
    }
    const u8* backing = runtime_memory_backing_pointer(start);
    if (!backing) {
        return nullptr;
    }
    *run_start = start;
    *run_size = end - start;
    return backing;
}
const u8* MemoryManager::GetBackingPointer(VAddr address) const {
    return runtime_memory_backing_pointer(address);
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size, u32 source) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size,
                     static_cast<BbWriteLog::Source>(10 + source));
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    // sotc: no protect call/page counters: nothing read them, and every protect (CP re-protects
    // and guest-thread fault unprotects) paid 2-4 lock xadds on shared cache lines. Counted
    // only for the SOTC sys line (SOTC_CMD_STATS).
    if (SotcStats::kCmdStats) {
        SotcStats::sys_protects.fetch_add(1, std::memory_order_relaxed);
        SotcStats::sys_protect_bytes.fetch_add(size, std::memory_order_relaxed);
        if (True(perms & MemoryPermission::Write)) {
            SotcStats::sys_unprotects.fetch_add(1, std::memory_order_relaxed);
        }
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

extern "C" int runtime_savedata_mounted(void); // src/runtime_savedata.c

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
// bbport: the runtime's TSC, which is QPC when the cores disagree on the TSC.
u64 PS4_SYSV_ABI sceKernelReadTsc() { return runtime_read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
extern "C" int runtime_sotc_neo(void);
extern "C" int bbgpu_warmup_finished(void) {
    return SotcStats::warmup_finished.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" void bbgpu_request_texture_flush(void) {
    SotcStats::load_flush.store(true, std::memory_order_relaxed);
}

extern "C" uint64_t bbgpu_flip_count(void) {
    return SotcStats::flip_count.load(std::memory_order_relaxed);
}
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return runtime_sotc_neo(); } // sotc: SOTC_NEO=1, a PS4 Pro
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 BbDisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

#if defined(BB_PGO_GENERATE) && defined(BB_PGO_CLANG)
// sotc: Clang instrumentation (compiler-rt profile runtime): write (merging into the %m file)
// and reset, so the file sums the intervals like the GCC path below.
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_reset_counters(void);
static void __gcov_dump(void) { __llvm_profile_write_file(); }
static void __gcov_reset(void) { __llvm_profile_reset_counters(); }
#elif defined(BB_PGO_GENERATE)
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
#endif
#ifdef BB_PGO_GENERATE
// Instrumented build (BB_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
    SotcTrace::SetUserDir(config->user_dir); // sotc: SOTC_TRACE files go to <user dir>/trace
#ifdef _WIN32
    if (config->user_dir && !std::getenv("BB_GPU_USER_DIR")) _putenv_s("BB_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "Shadow of the Colossus";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("sotc:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        // sotc: a save being written (a save directory mounted for writing) gets up to 5 s to
        // finish: quitting mid-write left a truncated save behind.
        if (runtime_savedata_mounted() > 0) {
            std::fprintf(stderr, "Runtime: waiting for the save to finish before quitting\n");
            for (int i = 0; i < 100 && runtime_savedata_mounted() > 0; ++i) {
                SDL_Delay(50);
            }
        }
        // sotc: write the pipelines still queued for the cache before leaving.
        Storage::DataBase::Instance().Close();
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void bbgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int bbgpu_is_gpu_side_thread(void) {
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    return rasterizer && rasterizer->IsGpuSideThread() ? 1 : 0;
}

extern "C" void bbgpu_register_guest_stack(uintptr_t address, uint64_t size) {
    VideoCore::RegisterGuestStack(address, size);
}
extern "C" void bbgpu_register_guest_stack_end(uintptr_t address, uint64_t size) {
    VideoCore::UnregisterGuestStack(address, size);
}
// sotc: SOTC_CARVE_NOSTOP (runtime_memory.c carve_host): is a registered guest (fiber) stack in
// the range? The registry's writers never block on its lock, so this cannot deadlock.
extern "C" int bbgpu_guest_stack_in_range(uintptr_t address, uint64_t size) {
    return VideoCore::IntersectsGuestStack(address, size) ? 1 : 0;
}
extern "C" int bbgpu_readbacks_mode(void) {
    return static_cast<int>(EmulatorSettings.GetReadbacksMode());
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    // sotc: SOTC_ASYNC_UPLOAD write fence. A write fault unprotects the page and, with the
    // write-ahead and the fault granule, pages up to 64 KiB around it: their pending copies
    // finish first (the copy threads take no lock, so waiting here cannot deadlock).
    // sotc: SOTC_FENCE_PIECES: read faults go on without waiting. A read fault only runs a
    // readback (page_manager HandleGuestFault): the GPU bit is cleared and the page keeps its
    // write protection (it only becomes writable when it is CPU-modified too, and a pending
    // copy's pages are CPU-clean: TryUploadAsync; only a fenced write marks them). The readback
    // writes the backing after its download's submit, which waits for every job queued before
    // it (WaitUpTo), so it cannot overtake a copy that was already pending at the fault.
    // sotc: the readbacks ahead of the queue (SOTC_RB_AHEAD's SubmitAheadAndWait,
    // SOTC_RB_ASYNC_AHEAD's SubmitAheadAsync) skip WaitUpTo. They are taken only for bytes no
    // command of the tick being recorded wrote (current_writes), while every pending job of an
    // older tick finished before that tick's submit; a buffer job's arena copy is in
    // current_writes (TryUploadAsync), and a texture job's pages were not GPU-modified when it
    // was queued, so a GPU write that makes them so is in current_writes too.
    // sotc: SOTC_FPS: times the fault (SotcStats::FaultStat; counted when it is a handled guest
    // write fault).
    if (SotcStats::kFpsLog) {
        SotcStats::FaultStat::Begin();
    }
    const u64 trace_t0 = SotcTrace::kOn ? SotcStats::NowNs() : 0; // sotc: SOTC_TRACE
    if (auto* async_upload = VideoCore::AsyncUpload::Get();
        async_upload && async_upload->AnyPending() && VideoCore::AsyncUpload::FencePieces() &&
        !Common::IsWriteError(ucontext)) {
        SotcStats::fence_read_skips.fetch_add(1, std::memory_order_relaxed);
    } else if (async_upload && async_upload->AnyPending()) {
        const SotcStats::FaultStat::Scope fence_timed{&SotcStats::FaultStat::Tl::fence_ns};
        // The fault granule is at most 64 KiB (kFaultGranule); the write-ahead covers
        // SOTC_WRITE_AHEAD_PAGES pages (15 by default) after the faulting one.
        static const u64 span = [] {
            const char* v = std::getenv("SOTC_WRITE_AHEAD_PAGES");
            const u64 ahead = (v ? std::strtoull(v, nullptr, 10) : 15ULL) * 4096 + 4096;
            return std::max<u64>(ahead, 64 * 1024);
        }();
        const u64 window = reinterpret_cast<u64>(address) & ~u64{0xFFFF};
        if (SotcStats::kWriteAheadAdapt) {
            // sotc: SOTC_WRITE_AHEAD_ADAPT: the window can reach SOTC_WRITE_AHEAD_MAX_PAGES pages
            // either side of the faulting page (rounded out to 64 KiB, as the fixed span is).
            const u64 reach = (u64{SotcStats::kWriteAheadMaxPages} + 16) * 4096;
            const u64 begin = (window > reach ? window - reach : 0) & ~u64{0xFFFF};
            async_upload->WaitVa(begin, window - begin + std::max(span, reach) + 64 * 1024);
        } else {
            async_upload->WaitVa(window, span + 64 * 1024);
        }
    }
    const int handled = Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
    if (SotcStats::kFpsLog) {
        SotcStats::FaultStat::End();
    }
    if (trace_t0 && handled) {
        // a = the faulting address, b = the faulting instruction
        u64 rip = 0;
#ifdef _WIN32
        rip = static_cast<EXCEPTION_POINTERS*>(ucontext)->ContextRecord->Rip;
#endif
        // Never allocates here (the exception handler): threads get their ring beforehand.
        SotcTrace::SpanIfReady(Common::IsWriteError(ucontext) ? "write fault" : "read fault",
                               trace_t0, SotcStats::NowNs(), reinterpret_cast<u64>(address), rip);
    }
    return handled;
}

// sotc: SOTC_ASYNC_UPLOAD write fence for the runtime's own writes to guest memory (zeroing
// freed memory, backing writes): offsets in the backing view (runtime_memory.c).
extern "C" void bbgpu_upload_wait_phys(uint64_t phys, uint64_t size) {
    const SotcTrace::Scope trace_wait{"upload fence (runtime write)", phys, size}; // sotc
    if (auto* async_upload = VideoCore::AsyncUpload::Get()) {
        async_upload->WaitPhys(phys, size);
    }
}

// sotc: SOTC_WRITE_DECL verify stamp: a run the declare settles (SOTC_FPS only).
static void NoteDeclaredRun(VAddr addr, uint64_t size) {
    SotcWriteDecl::NoteDeclared(addr, size);
}

// sotc: SOTC_WRITE_DECL (bbgpu.h). The runtime calls this before its memcpy/memset/memmove write a
// span of guest memory. Off unless SOTC_WRITE_DECL is 1 or 2; spans below SOTC_WRITE_DECL_MIN, or
// above SOTC_WRITE_DECL_MAX, are not declared. The pre-check is lock-free and runs once: a span
// with no write-protected page is only counted.
extern "C" uint64_t bbgpu_declare_cpu_write(void* address, uint64_t size, uint64_t return_address) {
    static const uint32_t mode = SotcWriteDecl::Mode();
    static const uint64_t min_bytes = SotcWriteDecl::MinBytes();
    static const uint64_t max_bytes = SotcWriteDecl::MaxBytes();
    if (mode == 0 || size == 0 || size < min_bytes) {
        return 0;
    }
    // The guest address of a host pointer is the pointer itself, as in the fault handler.
    const VAddr addr = reinterpret_cast<VAddr>(address);
    if (max_bytes && size > max_bytes) {
        SotcWriteDecl::NoteCall(return_address, size, 0); // counted, not looked at
        return 0;
    }
    const bool maybe = VideoCore::PageManager::MaybeWriteProtected(addr, size);
    const uint64_t pages = maybe ? VideoCore::PageManager::CountWriteProtectedPages(addr, size) : 0;
    SotcWriteDecl::NoteCall(return_address, size, pages);
    if (!maybe) {
        return pages;
    }
    if (mode == 1) {
        VideoCore::PageManager::DeclareCpuWrite(addr, size,
                                                SotcStats::kFpsLog ? NoteDeclaredRun : nullptr);
    } else if (SotcStats::kFpsLog) {
        // Count only: stamp the runs a declare would settle, so the verify line counts the
        // write faults it would remove.
        VideoCore::PageManager::VisitDeclarableRuns(addr, size, NoteDeclaredRun);
    }
    return pages;
}

extern "C" void bbgpu_set_fps_target_slot(void* slot) {
    SotcFpsCap::target_slot.store(slot, std::memory_order_relaxed);
}

// sotc: SOTC_STALL_DETAIL: a game guest thread records its CPU time under SotcStats::RoleGuest (the
// SOTCSTALL "guest cpu" figure). Called by host_start (src/runtime_thread.c). tl_role is left as it
// is: the trace recorder reads RoleOther as "guest". Only with SOTC_STALL_DETAIL: SOTC_FPS alone
// keeps sampling the GPU threads only, as before.
void bbgpu_register_guest_thread(void) {
    if (SotcStats::kStallDetail) {
        BbThreadCpu::RegisterLowPriority(SotcStats::RoleGuest);
    }
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// sotc port: RenderDoc captures of one GPU submission when the game runs under RenderDoc
// (renderdoccmd capture ...): SOTC_RDOC_TRIGGER=<file> appears -> the next submission the
// command processor runs is captured (Liverpool calls StartCapture/EndCapture around it).
namespace VideoCore {
#ifdef _WIN32
static RENDERDOC_API_1_6_0* rdoc_api;
static int rdoc_state; // 0 idle, 1 triggered, 2 capturing
void LoadRenderDoc() {
    static bool tried;
    if (std::exchange(tried, true)) return;
    if (HMODULE mod = GetModuleHandleA("renderdoc.dll")) {
        const auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
        if (get_api && get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&rdoc_api)) == 1) {
            rdoc_api->SetCaptureKeys(nullptr, 0);
            std::fprintf(stderr, "sotc: RenderDoc attached (SOTC_RDOC_TRIGGER captures a submission)\n");
        }
    }
}
void StartCapture() {
    static const char* trigger = std::getenv("SOTC_RDOC_TRIGGER");
    if (!trigger && rdoc_state == 0) return;
    LoadRenderDoc();
    if (!rdoc_api) return;
    if (trigger && rdoc_state == 0 && std::remove(trigger) == 0) rdoc_state = 1;
    if (rdoc_state == 1) {
        rdoc_api->StartFrameCapture(nullptr, nullptr);
        rdoc_state = 2;
    }
}
void EndCapture() {
    // SOTC_RDOC_SUBMITS=N: the capture spans N submissions (a whole frame is split in several).
    static int left = 0;
    if (rdoc_api && rdoc_state == 2 && left == 0) {
        const char* v = std::getenv("SOTC_RDOC_SUBMITS");
        left = v ? std::max(1, std::atoi(v)) : 1;
    }
    if (rdoc_api && rdoc_state == 2 && --left == 0) {
        const bool ok = rdoc_api->EndFrameCapture(nullptr, nullptr) == 1;
        std::fprintf(stderr, "sotc: RenderDoc capture %s\n", ok ? "saved" : "FAILED");
        rdoc_state = 0;
    }
}
// F12 under RenderDoc (window.cpp): capture the next submissions (SOTC_RDOC_SUBMITS).
void TriggerCapture() {
    LoadRenderDoc();
    if (rdoc_api && rdoc_state == 0) {
        rdoc_state = 1;
        std::fprintf(stderr, "sotc: RenderDoc capture armed (F12)\n");
    }
}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return rdoc_api != nullptr; }
#else
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
#endif
void RequestScreenshot(ScreenshotRequest) {}
// sotc port: BB_FRAME_DUMP_TRIGGER=<file>: when the file appears it is removed and the next
// presented game frame is saved as a PNG screenshot (user screenshots directory).
// SOTC_SHOT_OVERLAY=1: the screenshot includes the overlay (the save/load list).
static bool ShotWithOverlay() {
    static const bool with = std::getenv("SOTC_SHOT_OVERLAY") != nullptr;
    return with;
}
// SOTC_SHOT_COUNT=<n>: the trigger saves the next n game frames in a row (checking flicker).
u32 ConsumeGameOnlyScreenshotRequests() {
    static const char* trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static const int count = [] {
        const char* v = std::getenv("SOTC_SHOT_COUNT");
        return v ? std::max(1, std::atoi(v)) : 1;
    }();
    static int remaining = 0;
    if (remaining > 0) {
        --remaining;
        return 1;
    }
    if (!ShotWithOverlay() && trigger && std::remove(trigger) == 0) {
        remaining = count - 1;
        return 1;
    }
    return 0;
}
bool ConsumeImageDumpRequest() { return false; } // sotc: SOTC_DUMP needs RenderDoc hooks
u32 ConsumeWithOverlaysScreenshotRequests() {
    static const char* trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    return ShotWithOverlay() && trigger && std::remove(trigger) == 0 ? 1 : 0;
}
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int bbgpu_overlay_captures_input(void) {
    return BbOverlay::CapturesInput() ? 1 : 0;
}

// sotc: the game's save/load slot list (sceSaveDataDialog LIST, runtime_services.c).
extern "C" int bbgpu_save_list_open(const char* title, const char* const* items, int count,
                                     int focus, int first_deletable) {
    std::vector<std::string> list;
    for (int i = 0; i < count; ++i) {
        list.emplace_back(items[i] ? items[i] : "");
    }
    return BbOverlay::OpenSaveList(title ? title : "", list, focus, first_deletable) ? 1 : 0;
}
extern "C" void bbgpu_save_list_icons(const char* const* paths, int count) {
    std::vector<std::string> list;
    for (int i = 0; i < count; ++i) {
        list.emplace_back(paths && paths[i] ? paths[i] : "");
    }
    BbOverlay::SetSaveListIcons(std::move(list));
}
extern "C" int bbgpu_save_list_poll(void) {
    return BbOverlay::PollSaveList();
}
extern "C" void bbgpu_save_list_close(void) {
    BbOverlay::CloseSaveList();
}

extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
