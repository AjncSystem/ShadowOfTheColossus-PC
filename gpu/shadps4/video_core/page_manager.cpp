// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>
#include <utility>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <mutex>
#include <atomic>
#include "common/alignment.h"
#include "common/adaptive_mutex.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/multi_level_page_table.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "common/decoder.h"
#include "core/signals.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "video_core/page_manager.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <ucontext.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

namespace {
std::mutex g_guest_stacks_mutex;
// Reference counted: the margin below one stack often covers a neighbouring stack (games pack
// fiber stacks back to back), so finalizing one must not drop the other's protection.
boost::icl::interval_map<VAddr, u32> g_guest_stacks;
// Re-applies the tracker's protection to pages that stop being stack pages.
std::function<void(VAddr, u64)> g_reapply_protection;
// sotc (Linux): one-page queries for the loader's helper-thread read window (src/probe.c).
std::function<bool(VAddr)> g_page_read_watched;
std::function<bool(VAddr)> g_restore_page;
} // namespace

// The exception frame is written below the faulting stack pointer, so a nearly full stack also
// needs the memory just under it to stay accessible.
constexpr u64 GuestStackMargin = 16_KB;

void RegisterGuestStack(VAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    // Whole pages: protection is page granular, so a page shared with a stack is a stack page
    // for the tracker too (never protected, never written back over).
    const VAddr begin = Common::AlignDown(address - GuestStackMargin, VAddr{4096});
    const VAddr end = Common::AlignUp(address + size, VAddr{4096});
    {
        std::scoped_lock lk{g_guest_stacks_mutex};
        g_guest_stacks += std::make_pair(boost::icl::interval<VAddr>::right_open(begin, end), 1u);
    }
    // Memory the guest hands over as a stack may already be tracked; make it accessible now.
    Core::Memory::Instance()->GetAddressSpace().Protect(begin, end - begin,
                                                        Core::MemoryPermission::ReadWrite);
}

void UnregisterGuestStack(VAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    const VAddr begin = Common::AlignDown(address - GuestStackMargin, VAddr{4096});
    const VAddr end = Common::AlignUp(address + size, VAddr{4096});
    {
        std::scoped_lock lk{g_guest_stacks_mutex};
        g_guest_stacks -= std::make_pair(boost::icl::interval<VAddr>::right_open(begin, end), 1u);
    }
    // The pages were left accessible while they were a stack: give the tracker its protection
    // back, or CPU writes there would go unnoticed (pages still used by another stack stay).
    if (g_reapply_protection) {
        g_reapply_protection(begin, end - begin);
    }
}

void ForEachNonStackRange(VAddr address, u64 size, const std::function<void(VAddr, u64)>& func) {
    if (size == 0) {
        return;
    }
    boost::icl::interval_set<VAddr> ranges;
    const auto window = boost::icl::interval<VAddr>::right_open(address, address + size);
    bool overlaps = false;
    {
        std::scoped_lock lk{g_guest_stacks_mutex};
        const auto [first, last] = g_guest_stacks.equal_range(window);
        if (first != last) {
            overlaps = true;
            ranges += window;
            for (auto it = first; it != last; ++it) {
                ranges -= it->first;
            }
        }
    }
    if (!overlaps) {
        // Common case: nothing of the range is a stack.
        func(address, size);
        return;
    }
    static std::atomic<u32> reported{0};
    if (reported++ < 8) {
        LOG_WARNING(Render, "Not writing GPU data back over guest stack pages in {:#x}+{:#x}",
                    address, size);
    }
    for (const auto& range : ranges) {
        func(range.lower(), range.upper() - range.lower());
    }
}

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers;
        u8 num_read_watchers;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <bool is_read>
        u8 GetPage() const {
            if constexpr (is_read) {
                return num_read_watchers;
            } else {
                return num_write_watchers;
            }
        }

        constexpr Core::MemoryPermission Update(PageOp write_op, bool update_write = true,
                                                PageOp read_op = PageOp::None,
                                                bool update_read = false) {
            if (update_read) {
                if (read_op == PageOp::Track) {
                    ASSERT_MSG(num_read_watchers == 0, "Too many watchers");
                } else if (read_op == PageOp::Untrack) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                }
                num_read_watchers += std::to_underlying(read_op);
            }
            if (update_write) {
                if (write_op == PageOp::Track) {
                    ASSERT_MSG(num_write_watchers < 255, "Too many watchers");
                } else if (write_op == PageOp::Untrack) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                }
                num_write_watchers += std::to_underlying(write_op);
            }
            return Perms();
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / NUM_REGION_PAGES;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
        EnsurePages(address, address + size);
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    /// Re-applies the protection implied by the watchers of every page in the range, after the
    /// host mapping was recreated with its default protection.
    void ReapplyProtection(VAddr address, u64 size) {
        const u64 page_start = address >> PM_PAGE_BITS;
        const u64 page_end = std::min<u64>(Common::DivCeil(address + size, PM_PAGE_SIZE),
                                           NUM_ADDRESS_PAGES);
        std::vector<u64> locked_pages;
        u64 run_begin{};
        u64 run_pages{};
        Core::MemoryPermission run_perms{};
        const auto flush = [&] {
            if (run_pages != 0) {
                Protect(run_begin << PM_PAGE_BITS, run_pages << PM_PAGE_BITS, run_perms);
                run_pages = 0;
            }
        };
        for (u64 page = page_start; page < page_end; ++page) {
            PageState* state = cached_pages.find(page);
            if (!state) {
                flush();
                continue;
            }
            locks[page].lock();
            locked_pages.push_back(page);
            const auto perms = state->Perms();
            if (perms == Core::MemoryPermission::ReadWrite) {
                flush();
                continue;
            }
            if (run_pages != 0 && perms == run_perms && run_begin + run_pages == page) {
                ++run_pages;
            } else {
                flush();
                run_begin = page;
                run_pages = 1;
                run_perms = perms;
            }
        }
        flush();
        for (const u64 page : locked_pages) {
            locks[page].unlock();
        }
    }

    /// sotc (Linux): true when the tracker read-protects the page holding address.
    bool PageReadWatched(VAddr address) {
        const PageState* state = cached_pages.find(address >> PM_PAGE_BITS);
        return state && state->num_read_watchers != 0;
    }

    /// sotc (Linux): sets the page holding address to exactly the protection its watchers imply
    /// (ReadWrite included, unlike ReapplyProtection). False when the tracker has no state.
    bool RestorePage(VAddr address) {
        const u64 page = address >> PM_PAGE_BITS;
        PageState* state = cached_pages.find(page);
        if (!state) {
            return false;
        }
        locks[page].lock();
        Protect(page << PM_PAGE_BITS, PM_PAGE_SIZE, state->Perms());
        locks[page].unlock();
        return true;
    }

    void EnsurePages(VAddr begin, VAddr end) {
        end = std::min(end, VAddr{1} << ADDRESS_BITS) - 1;
        const size_t start_page = begin >> PM_PAGE_BITS;
        const size_t end_page = end >> PM_PAGE_BITS;
        cached_pages.reserve(start_page, end_page);
        locks.reserve(start_page, end_page);
    }

    void UpdatePageWatchers(VAddr addr, u64 size, PageOp write_op) {
        const u64 page_start = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        Core::MemoryPermission perms{};
        u64 range_begin = page_start;
        u64 range_pages = 0;
        u64 potential_pages = 0;

        const auto release_pending = [&] {
            if (range_pages > 0) {
                Protect(range_begin << PM_PAGE_BITS, range_pages << PM_PAGE_BITS, perms);
                range_pages = 0;
                potential_pages = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page_start << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            // sotc: frequent while the game streams; the first few are enough.
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render,
                            "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                            aligned_addr, aligned_end);
            }
            EnsurePages(aligned_addr, aligned_end);
        }

        for (u64 page = page_start; page != page_end; ++page) {
            PageState* state = cached_pages.find(page);
            if (!state) {
                continue;
            }

            locks[page].lock();

            const auto old_perms = state->Perms();
            if (page == page_start) {
                perms = old_perms;
            }

            // Apply the change to the page state
            const auto new_perms = state->Update(write_op);
            if (new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_pages != 0) {
                ++potential_pages;
            }

            // If the page must be (un)protected
            if (new_perms != old_perms) {
                if (range_pages == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_pages = 1;
                }
                // Extend current range up to potential range
                range_pages = potential_pages;
            }
        }

        // Add pending (un)protect action
        release_pending();

        for (u64 page = page_start; page != page_end; ++page) {
            if (auto* lock = locks.find(page)) {
                lock->unlock();
            }
        }
    }

    void UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                     const RegionBits& write_mask, const RegionBits& read_mask,
                                     PageOp write_op, PageOp read_op) {
        const u64 base_page = base_addr >> PM_PAGE_BITS;
        const u64 page_start = bounds.start_word * PAGES_PER_WORD + bounds.start_page;
        const u64 page_end = bounds.end_word * PAGES_PER_WORD + bounds.end_page + 1;

        Core::MemoryPermission perms{};
        u64 range_begin = base_page + page_start;
        u64 range_pages = 0;
        u64 potential_pages = 0;

        const auto release_pending = [&] {
            if (range_pages > 0) {
                Protect(range_begin << PM_PAGE_BITS, range_pages << PM_PAGE_BITS, perms);
                range_pages = 0;
                potential_pages = 0;
            }
        };

        for (u64 page = page_start; page != page_end; ++page) {
            PageState* state = cached_pages.find(base_page + page);
            if (!state) {
                continue;
            }

            locks[base_page + page].lock();

            const auto old_perms = state->Perms();
            if (page == page_start) {
                perms = old_perms;
            }

            // Apply the change to the page state
            const bool update_write = write_op != PageOp::None && write_mask.GetPage(page);
            const bool update_read = read_op != PageOp::None && read_mask.GetPage(page);
            const auto new_perms = state->Update(write_op, update_write, read_op, update_read);

            if (new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_pages != 0) {
                // If the protection did not change, extend the potential range
                ++potential_pages;
            }

            // If the page must be (un)protected
            if (new_perms != old_perms) {
                if (range_pages == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_pages = 1;
                }
                // Extend current rango up to potential range
                range_pages = potential_pages;
            }
        }

        // Add pending (un)protect action
        release_pending();

        for (u64 page = page_start; page != page_end; ++page) {
            if (auto* lock = locks.find(base_page + page)) {
                lock->unlock();
            }
        }
    }

    struct PageTraits {
        using Entry = PageState;
        static constexpr size_t ADDRESS_SPACE_BITS = ADDRESS_BITS;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = PM_PAGE_BITS;
        static constexpr bool NULL_CHECK = false;
    };
    Common::MultiLevelPageTable<PageTraits> cached_pages;
    struct MutexTraits {
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
        using Entry = Common::AdaptiveMutex;
#else
        using Entry = std::mutex;
#endif
        static constexpr size_t ADDRESS_SPACE_BITS = ADDRESS_BITS;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = PM_PAGE_BITS;
        static constexpr bool NULL_CHECK = false;
    };
    Common::MultiLevelPageTable<MutexTraits> locks;
};

#ifdef __linux__
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Request uffdio features from kernel.
        uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_THREAD_ID;
        const int ret = ioctl(uffd, UFFDIO_API, &api);
        if (ret != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("uffdio_api");
        }

        // Create uffd handler thread
        ufd_thread = std::jthread([&](std::stop_token token) { UffdHandler(token); });
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        PageManager::Impl::OnMap(address, size);
        uffdio_register reg;
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_REGISTER, &reg);
        ASSERT_MSG(ret != -1, "Uffdio register failed with error: {}", Common::GetLastErrorMsg());
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range;
        range.start = address;
        range.len = size;
        const int ret = ioctl(uffd, UFFDIO_UNREGISTER, &range);
        ASSERT_MSG(ret != -1, "Uffdio unregister failed with error: {}", Common::GetLastErrorMsg());
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        bool allow_write = True(perms & Core::MemoryPermission::Write);
        uffdio_writeprotect wp;
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
        ASSERT_MSG(ret != -1, "Uffdio writeprotect failed with error: {}",
                   Common::GetLastErrorMsg());
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("shadPS4:Uffd");

        auto regions = Core::Memory::Instance()->GetAddressSpace().GetUsableRegions();
        for (auto& region : regions) {
            OnMap(region.lower(), region.upper());
        }
        LOG_INFO(Common_Memory, "registered reserved memory with userfaultfd");

        while (!token.stop_requested()) {
            pollfd pollfd;
            pollfd.fd = uffd;
            pollfd.events = POLLIN;

            // Block until the descriptor is ready for data reads.
            const int pollres = poll(&pollfd, 1, -1);
            switch (pollres) {
            case -1:
                perror("Poll userfaultfd");
                continue;
                break;
            case 0:
                continue;
            case 1:
                break;
            default:
                UNREACHABLE_MSG("Unexpected number of descriptors {} out of poll", pollres);
            }

            // We don't want an error condition to have occured.
            ASSERT_MSG(!(pollfd.revents & POLLERR), "POLLERR on userfaultfd");

            // We waited until there is data to read, we don't care about anything else.
            if (!(pollfd.revents & POLLIN)) {
                continue;
            }

            // Read message from kernel.
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                LOG_ERROR(Common_Memory, "Unexpected result of uffd read: {}",
                          Common::GetLastErrorMsg());
                break;
            }
            ASSERT_MSG(readret == sizeof(msg), "Unexpected short read, exiting");
            ASSERT(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);

            // Notify rasterizer about the fault.
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            rasterizer->InvalidateMemory(addr, 1,
                                         ptid == rasterizer->GetGpuCommandProcessorThreadId());

            // Some calls to InvalidateMemory never reach the UFFDIO_WRITEPROTECT ioctl in
            // ::Protect, therefore we use MODE_DONTWAKE and wake the thread with UFFDIO_WAKE here
            uffdio_range wake;
            wake.start = msg.arg.pagefault.address;
            wake.len = PageManager::PM_PAGE_SIZE;
            const int ret = ioctl(uffd, UFFDIO_WAKE, &wake);
            ASSERT_MSG(ret != -1, "Waking thread {} failed with: {}", ptid,
                       Common::GetLastErrorMsg());
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Splitting a mapped placeholder on Windows recreates the neighbouring views with their
        // default protection, silently dropping the write/read tracking of those pages.
        // sotc port: the C runtime re-applies protections itself when it splits views.
        g_reapply_protection = [this](VAddr address, u64 size) {
            ReapplyProtection(address, size);
        };
        g_page_read_watched = [this](VAddr address) { return PageReadWatched(address); };
        g_restore_page = [this](VAddr address) { return RestorePage(address); };

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
        Shader::SetSrtCleanLoad(&SrtWalkerCleanLoad);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        // sotc: the walker's clean-load bitmap follows the read protection: set before
        // protecting, cleared after unprotecting (a stale bit only costs a clean load).
        const bool read_protected = False(perms & Core::MemoryPermission::Read);
        if (read_protected) {
            Shader::SetSrtReadProtectedPages(address, size, true);
        }
        const auto clear_after = [&] {
            if (!read_protected) {
                Shader::SetSrtReadProtectedPages(address, size, false);
            }
        };
        if (perms != Core::MemoryPermission::ReadWrite) {
            // Keep guest stacks accessible, only protect the rest of the range.
            boost::icl::interval_set<VAddr> ranges;
            ranges += boost::icl::interval<VAddr>::right_open(address, address + size);
            {
                std::scoped_lock lk{g_guest_stacks_mutex};
                const auto window = boost::icl::interval<VAddr>::right_open(address, address + size);
                const auto [first, last] = g_guest_stacks.equal_range(window);
                if (first != last) {
                    for (auto it = first; it != last; ++it) {
                        ranges -= it->first;
                    }
                    static std::atomic<u32> reported{0};
                    if (reported++ < 8) {
                        LOG_ERROR(Render, "Not protecting guest stack pages in {:#x}+{:#x}",
                                    address, size);
                    }
                }
            }
            for (const auto& range : ranges) {
                impl.Protect(range.lower(), range.upper() - range.lower(), perms);
            }
            clear_after();
            return;
        }
        impl.Protect(address, size, perms);
        clear_after();
    }

    // sotc: SOTC_RB_STATS=1 prints, every 10 s, the read faults that cost a readback, grouped by
    // reader (guest or GPU thread), faulting instruction and 64 KiB region.
    // SOTC_RB_STATS=w: the write faults instead.
    static void NoteReadFault(void* context, VAddr addr, bool is_gpu_thread, bool write = false) {
        static const char* const mode = std::getenv("SOTC_RB_STATS");
        if (!mode || (mode[0] == 'w') != write) {
            return;
        }
        static std::mutex stats_mutex;
        static std::map<std::tuple<bool, u64, VAddr>, u64> counts;
        static auto last = std::chrono::steady_clock::now();
#ifdef _WIN32
        const u64 rip = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord->Rip;
#elif defined(__linux__)
        const u64 rip = static_cast<u64>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#else
        const u64 rip = 0;
#endif
        std::scoped_lock lk{stats_mutex};
        ++counts[{is_gpu_thread, rip, addr & ~VAddr{0xFFFF}}];
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(10)) {
            return;
        }
        last = now;
        std::vector<std::pair<u64, std::tuple<bool, u64, VAddr>>> sorted;
        u64 total = 0;
        for (const auto& [key, count] : counts) {
            sorted.emplace_back(count, key);
            total += count;
        }
        std::sort(sorted.rbegin(), sorted.rend());
        std::fprintf(stderr, "SOTC readback faults in 10 s: %llu\n", static_cast<unsigned long long>(total));
        for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
            const auto& [count, key] = sorted[i];
            char where[512] = "?";
#ifdef _WIN32
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(std::get<1>(key)), &module)) {
                char path[MAX_PATH];
                GetModuleFileNameA(module, path, MAX_PATH);
                const char* name = std::strrchr(path, '\\');
                std::snprintf(where, sizeof(where), "%s+%#llx", name ? name + 1 : path,
                              static_cast<unsigned long long>(std::get<1>(key) -
                                                              reinterpret_cast<u64>(module)));
            } else {
                MEMORY_BASIC_INFORMATION info{};
                VirtualQuery(reinterpret_cast<void*>(std::get<1>(key)), &info, sizeof(info));
                std::snprintf(where, sizeof(where), "alloc %p+%#llx", info.AllocationBase,
                              static_cast<unsigned long long>(
                                  std::get<1>(key) - reinterpret_cast<u64>(info.AllocationBase)));
            }
#endif
            std::fprintf(stderr, "  %6llu %s rip=%#llx (%s) region=%#llx\n",
                         static_cast<unsigned long long>(count), std::get<0>(key) ? "gpu  " : "guest",
                         static_cast<unsigned long long>(std::get<1>(key)), where,
                         static_cast<unsigned long long>(std::get<2>(key)));
        }
        counts.clear();
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        const auto size = std::min<u64>(8, PageManager::GetNextPageAddr(addr) - addr);
        const auto is_gpu_thread =
            std::this_thread::get_id() == rasterizer->GetGpuCommandProcessorThread();
        if (!Common::IsWriteError(context) && is_gpu_thread && TryCleanWalkerLoad(context)) {
            SotcStats::fault_walker.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (Common::IsWriteError(context)) {
            SotcStats::fault_writes.fetch_add(1, std::memory_order_relaxed);
            NoteReadFault(context, addr, is_gpu_thread, true);
            if (!rasterizer->InvalidateMemory(addr, size, is_gpu_thread)) {
                return false;
            }
            rasterizer->MarkWriteAhead(addr);
            return true;
        } else {
            if (is_gpu_thread) {
                SotcStats::fault_gpu_reads.fetch_add(1, std::memory_order_relaxed);
            }
            NoteReadFault(context, addr, is_gpu_thread);
            return rasterizer->ReadMemory(addr, size, is_gpu_thread);
        }
        return false;
    }

#ifdef _WIN32
    static DWORD64* ContextRegister(CONTEXT* ctx, ZydisRegister reg) {
        switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
        case ZYDIS_REGISTER_RAX: return &ctx->Rax;
        case ZYDIS_REGISTER_RCX: return &ctx->Rcx;
        case ZYDIS_REGISTER_RDX: return &ctx->Rdx;
        case ZYDIS_REGISTER_RBX: return &ctx->Rbx;
        case ZYDIS_REGISTER_RSI: return &ctx->Rsi;
        case ZYDIS_REGISTER_RDI: return &ctx->Rdi;
        case ZYDIS_REGISTER_R8: return &ctx->R8;
        case ZYDIS_REGISTER_R9: return &ctx->R9;
        case ZYDIS_REGISTER_R10: return &ctx->R10;
        case ZYDIS_REGISTER_R11: return &ctx->R11;
        case ZYDIS_REGISTER_R12: return &ctx->R12;
        case ZYDIS_REGISTER_R13: return &ctx->R13;
        case ZYDIS_REGISTER_R14: return &ctx->R14;
        case ZYDIS_REGISTER_R15: return &ctx->R15;
        default: return nullptr;
        }
    }
#elif defined(__linux__)
    static u64* ContextRegister(ucontext_t* ctx, ZydisRegister reg) {
        auto* g = reinterpret_cast<u64*>(ctx->uc_mcontext.gregs);
        switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
        case ZYDIS_REGISTER_RAX: return &g[REG_RAX];
        case ZYDIS_REGISTER_RCX: return &g[REG_RCX];
        case ZYDIS_REGISTER_RDX: return &g[REG_RDX];
        case ZYDIS_REGISTER_RBX: return &g[REG_RBX];
        case ZYDIS_REGISTER_RSI: return &g[REG_RSI];
        case ZYDIS_REGISTER_RDI: return &g[REG_RDI];
        case ZYDIS_REGISTER_R8: return &g[REG_R8];
        case ZYDIS_REGISTER_R9: return &g[REG_R9];
        case ZYDIS_REGISTER_R10: return &g[REG_R10];
        case ZYDIS_REGISTER_R11: return &g[REG_R11];
        case ZYDIS_REGISTER_R12: return &g[REG_R12];
        case ZYDIS_REGISTER_R13: return &g[REG_R13];
        case ZYDIS_REGISTER_R14: return &g[REG_R14];
        case ZYDIS_REGISTER_R15: return &g[REG_R15];
        default: return nullptr;
        }
    }
#endif

    /// Clean-load callback of the generated walkers (GPU command processor thread).
    static bool SrtWalkerCleanLoad(u64 address, u32 size, u64* out) {
        // No GPU mapping check: without one the page is not protected and the backing holds
        // what a plain load would read.
        if (std::this_thread::get_id() != rasterizer->GetGpuCommandProcessorThread()) {
            return false;
        }
        *out = 0;
        if (!rasterizer->ReadCleanMemory(address, out, size)) {
            return false;
        }
        return true;
    }

    /// SRT walkers read user data tables that often share a page with bytes the GPU wrote. The
    /// page is read-protected for those bytes, but the backing already holds every byte the GPU
    /// did not write: complete such a load from the backing instead of a full GPU readback. The
    /// page stays protected, so reads of the GPU-written bytes still fault and read back.
    /// Idea from srt_walker_clean_reads of Pink-shadPS4 (luizgustavs). SOTC_CLEAN_READS=0 = off.
    static bool TryCleanWalkerLoad(void* context) {
#if defined(_WIN32) || defined(__linux__)
        static const bool enabled = [] {
            const char* v = std::getenv("SOTC_CLEAN_READS");
            return v ? std::atoi(v) != 0 : true;
        }();
        const void* rip = Common::GetRip(context);
        if (!enabled || !Shader::IsSrtWalkerCode(rip)) {
            return false;
        }
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        const auto status = Common::Decoder::Instance()->decodeInstruction(
            instruction, operands, const_cast<void*>(rip), 15);
        if (!ZYAN_SUCCESS(status) || instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
            operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
            return false;
        }
#ifdef _WIN32
        auto* ctx = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord;
        using RegisterValue = DWORD64;
#else
        auto* ctx = static_cast<ucontext_t*>(context);
        using RegisterValue = u64;
#endif
        const auto& mem = operands[1].mem;
        const u32 size = operands[0].size / 8;
        RegisterValue* dst = ContextRegister(ctx, operands[0].reg.value);
        if (!dst || (size != 4 && size != 8) || mem.segment != ZYDIS_REGISTER_DS ||
            mem.base == ZYDIS_REGISTER_NONE) {
            return false;
        }
        const RegisterValue* base = ContextRegister(ctx, mem.base);
        if (!base) {
            return false;
        }
        VAddr address = *base + static_cast<u64>(mem.disp.value);
        if (mem.index != ZYDIS_REGISTER_NONE) {
            const RegisterValue* index = ContextRegister(ctx, mem.index);
            if (!index) {
                return false;
            }
            address += *index * mem.scale;
        }
        if (!rasterizer->IsMapped(address, size) || rasterizer->IsRangeGpuWritten(address, size)) {
            return false;
        }
        u64 value = 0;
        if (!Core::Memory::Instance()->TryCopySparseMemory(address, reinterpret_cast<u8*>(&value),
                                                           size)) {
            return false;
        }
        // A 32-bit destination zero-extends into the full register, as the mov would.
        *dst = value;
#ifdef _WIN32
        ctx->Rip += instruction.length;
#else
        ctx->uc_mcontext.gregs[REG_RIP] += instruction.length;
#endif
        // Later walker loads in this page try the clean path before faulting.
        Shader::MarkSrtCleanPages(address, size);
        return true;
#else
        return false;
#endif
    }
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    if (EmulatorSettings.IsUserfaultfdTracking()) {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

void PageManager::UpdatePageWatchers(VAddr addr, u64 size, PageOp write_op) const {
    impl->UpdatePageWatchers(addr, size, write_op);
}

void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                              const RegionBits& write_mask,
                                              const RegionBits& read_mask, PageOp write_op,
                                              PageOp read_op) const {
    impl->UpdatePageWatchersForRegion(base_addr, bounds, write_mask, read_mask, write_op, read_op);
}

} // namespace VideoCore

extern "C" int bbgpu_page_read_watched(uintptr_t address) {
    return VideoCore::g_page_read_watched && VideoCore::g_page_read_watched(address) ? 1 : 0;
}

extern "C" int bbgpu_page_restore(uintptr_t address) {
    return VideoCore::g_restore_page && VideoCore::g_restore_page(address) ? 1 : 0;
}
