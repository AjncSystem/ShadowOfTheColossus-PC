// sotc: assertion failures that unwind instead of stopping the port (see assert.cpp).
#pragma once

#include <exception>

namespace Common {

/// While set on a thread, a failed ASSERT/UNREACHABLE there throws AssertFailure. Pipeline
/// workers set it while translating a shader from guest tables the game may already have
/// reused (garbage sharps): the command processor then translates that shader itself.
extern thread_local bool g_assert_throws;

struct AssertFailure : std::exception {
    const char* what() const noexcept override {
        return "GPU library assertion failed";
    }
};

} // namespace Common
