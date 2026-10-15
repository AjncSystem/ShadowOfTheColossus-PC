// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/logging/log.h"

#ifdef _WIN32
extern "C" [[noreturn]] void bb_longjmp(sigjmp_buf buffer, int value);
#endif

/// sotc: set by draw preparation helpers (vk_draw_prep.cpp) around speculative work on data the
/// command processor has not validated yet. An assertion there means the data was not what the
/// command processor will see: the helper drops that proposal (back to its runtime_fault_recover
/// point) instead of stopping the game. Nowhere else is an assertion recovered.
thread_local bool g_bb_speculative_asserts = false;
std::atomic<long long> g_bb_speculative_assert_count{0};

void assert_fail_impl() {
    if (g_bb_speculative_asserts && runtime_fault_recover) {
        auto* recover = runtime_fault_recover;
        runtime_fault_recover = nullptr;
        g_bb_speculative_asserts = false;
        if (g_bb_speculative_assert_count.fetch_add(1, std::memory_order_relaxed) < 16) {
            std::fputs("SOTC prep: the assertion above was in the draw preparation helper (data "
                       "not validated yet); its proposal is dropped\n",
                       stderr);
        }
#ifdef _WIN32
        bb_longjmp(*recover, 2);
#else
        siglongjmp(*recover, 2);
#endif
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
