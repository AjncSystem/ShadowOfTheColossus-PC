#include "bbport_write_log.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "bbport_toggles.h"
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
#include "core/memory.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
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
            if (map_invalidate) r->InvalidateMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
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
    // and guest-thread fault unprotects) paid 2-4 lock xadds on shared cache lines.
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
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
extern "C" int runtime_sotc_neo(void);
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
#ifdef _WIN32
    if (config->user_dir && !std::getenv("BB_GPU_USER_DIR")) _putenv_s("BB_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "Shadow of the Colossus";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("bb:window");
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
    return rasterizer && std::this_thread::get_id() == rasterizer->GetGpuCommandProcessorThread()
               ? 1
               : 0;
}

extern "C" void bbgpu_register_guest_stack(uintptr_t address, uint64_t size) {
    VideoCore::RegisterGuestStack(address, size);
}
extern "C" void bbgpu_register_guest_stack_end(uintptr_t address, uint64_t size) {
    VideoCore::UnregisterGuestStack(address, size);
}
extern "C" int bbgpu_readbacks_mode(void) {
    return static_cast<int>(EmulatorSettings.GetReadbacksMode());
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
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
u32 ConsumeGameOnlyScreenshotRequests() {
    static const char* trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    return !ShotWithOverlay() && trigger && std::remove(trigger) == 0 ? 1 : 0;
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
