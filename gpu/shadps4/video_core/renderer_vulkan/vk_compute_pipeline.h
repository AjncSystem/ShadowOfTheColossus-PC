// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Serialization {
struct Archive;
}

namespace Vulkan {

class Instance;
class Scheduler;
class DescriptorHeap;

struct ComputePipelineKey {
    size_t value;

    friend bool operator==(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return lhs.value == rhs.value;
    }
    friend bool operator!=(const ComputePipelineKey& lhs, const ComputePipelineKey& rhs) {
        return !(lhs == rhs);
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

class ComputePipeline : public Pipeline {
public:
    struct SerializationSupport {
        u32 dummy{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    /// sotc: deferred: only the layouts are made here; Build makes the pipeline on a pipeline
    /// worker (SOTC_ASYNC_COMPUTE) and command buffers bind it with BindWhenReady.
    ComputePipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                    const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
                    ComputePipelineKey compute_key, const Shader::Info& info,
                    vk::ShaderModule module, SerializationSupport& sdata, bool preloading,
                    bool deferred = false);
    ~ComputePipeline();

    /// sotc: makes the pipeline of a deferred construction, unless another thread claimed it
    /// first (pipeline worker, or the recording thread when it needs it before any worker
    /// started it: workers may be waiting for GPU work queued behind that bind, since shader
    /// translation reads guest memory on them).
    void Build();

    /// sotc: binds the pipeline in the current command buffer. A deferred pipeline still being
    /// built is bound on the recording thread, which waits for it there, so the command
    /// processor goes on (and other new pipelines build meanwhile).
    void BindWhenReady() const;

private:
    void CreatePipeline();

    ComputePipelineKey compute_key;
    vk::PipelineCache cache;
    vk::ShaderModule module;
    bool from_cache{}; ///< sotc: preloaded from the cache (a refused one is dropped)
    std::atomic<bool> ready{true};
    std::atomic<bool> claimed{false};
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::ComputePipelineKey> {
    std::size_t operator()(const Vulkan::ComputePipelineKey& key) const noexcept {
        return std::hash<size_t>{}(key.value);
    }
};
