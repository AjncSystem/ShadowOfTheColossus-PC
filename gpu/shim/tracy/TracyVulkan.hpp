// sotc: the Vulkan profiler macros stay no-ops (no GPU zones through Tracy; the GPU queue
// spans come from the SOTC_TRACE queue watcher instead). The host zones of Tracy.hpp are unaffected.
#pragma once
#include "Tracy.hpp"
#define TracyVkCtx void*
#define TracyVkCollect(ctx, cmdbuf)
#define TracyVkNamedZoneC(ctx, var, cmd, name, color, active)
#define TracyVkContextName(ctx, name, size)
#define TracyVkContextHostCalibrated(...) nullptr
#define TracyVkDestroy(ctx)
namespace tracy { struct VkCtxScope { template <class... A> VkCtxScope(A&&...) {} }; }
