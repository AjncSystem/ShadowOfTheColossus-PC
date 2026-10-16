// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// Above both lines before the upstream merge: generation changed, so old caches are dropped.
static constexpr u32 ShaderBinaryVersion = 33u;
static constexpr u32 ShaderMetaVersion = 22u;
static constexpr u32 PipelineKeyVersion = 3u;
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    return info.Deserialize(ar);
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    // Stages left over from a pipeline whose load failed half-way must not leak into this one:
    // a stale entry for a stage this key does not use would be bound without user data.
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    if (warmup_pool) {
        if (compute_pipelines.contains(compute_key) ||
            !warmup_compute_keys.try_emplace(compute_key, 0).second) {
            infos.fill(nullptr);
            modules.fill(nullptr);
            return false;
        }
        warmup_pool->Submit([this, key = compute_key, info = infos[0], module = modules[0],
                             sdata]() mutable {
            auto pipeline = std::make_unique<ComputePipeline>(
                instance, scheduler, desc_heap, profile, *pipeline_cache, key, *info, module,
                sdata, true);
            std::scoped_lock lock{warmup_mutex};
            warmup_compute.emplace_back(key, std::move(pipeline));
        });
        infos.fill(nullptr);
        modules.fill(nullptr);
        return true;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *infos[0], modules[0], sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    // Stages left over from a pipeline whose load failed half-way must not leak into this one:
    // a stale entry for a stage this key does not use would be bound without user data.
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;
    graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    if (warmup_pool) {
        if (graphics_pipelines.contains(graphics_key) ||
            !warmup_graphics_keys.try_emplace(graphics_key, 0).second) {
            infos.fill(nullptr);
            modules.fill(nullptr);
            fetch_shader = nullptr;
            return false;
        }
        // Program infos stay unchanged during the warm-up (later stages only add modules), but
        // fetch_shader points into a module list that can grow: copy it.
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        if (fetch_shader) {
            fetch = *fetch_shader;
        }
        warmup_pool->Submit([this, key = graphics_key, stage_infos = infos,
                             stage_runtime = runtime_infos, stage_modules = modules,
                             fetch = std::move(fetch), sdata]() mutable {
            auto pipeline = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, key, *pipeline_cache, stage_infos,
                stage_runtime, fetch ? &*fetch : nullptr, stage_modules, sdata, true);
            std::scoped_lock lock{warmup_mutex};
            warmup_graphics.emplace_back(key, std::move(pipeline));
        });
        infos.fill(nullptr);
        modules.fill(nullptr);
        fetch_shader = nullptr;
        return true;
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    ASSERT(is_new);

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
        runtime_infos, fetch_shader, modules, sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    // Shader patches (user/shader/patch) must also apply to pipelines restored from the cache.
    if (EmulatorSettings.IsPatchShaders()) {
        if (auto patch = GetShaderPatch(program->info.pgm_hash, program->info.hw_stage, perm_idx,
                                        "spv", spv)) {
            LOG_INFO(Loader, "Loaded patch for cached {} shader {:#x}", program->info.hw_stage,
                     program->info.pgm_hash);
            spv = std::move(*patch);
        }
    }

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    // The specialization must reference the cached program's info, not the temporary one.
    spec.info = &it_pgm.value()->info;
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    infos[stage] = &it_pgm.value()->info;
    modules[stage] = module;
    if (auto& fetch = it_pgm.value()->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }

    return true;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render,
                    "Pipeline cache profile has unexpected size ({} != {}). Rebuilding the cache",
                    profile_data.size(), sizeof(Shader::Profile));
        if (Storage::DataBase::Instance().Reset()) {
            profile_data.resize(sizeof(profile));
            std::memcpy(profile_data.data(), &profile, sizeof(profile));
            Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                               std::move(profile_data));
        }
        return;
    }

    Shader::Profile cached_profile{};
    std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    if (cached_profile != profile) {
        LOG_WARNING(Render,
                    "Pipeline cache isn't compatible with current system. Rebuilding the cache");
        if (Storage::DataBase::Instance().Reset()) {
            profile_data.resize(sizeof(profile));
            std::memcpy(profile_data.data(), &profile, sizeof(profile));
            Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                               std::move(profile_data));
        }
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    const auto warmup_start = std::chrono::steady_clock::now();

    // sotc: SOTC_PARALLEL_WARMUP=<threads> (0 = build on this thread, as before). With a cold
    // driver cache (new driver, cleared cache) each pipeline takes 10-40 ms to compile.
    const u32 warmup_threads = [] {
        const char* v = std::getenv("SOTC_PARALLEL_WARMUP");
        const u32 hw = std::max(1u, std::thread::hardware_concurrency());
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10))
                 : std::clamp<u32>(hw > 2 ? hw - 2 : 1, 1, 12);
    }();
    if (warmup_threads > 0) {
        warmup_pool = std::make_unique<AsyncCompiler>(warmup_threads, size_t{1} << 20);
    }

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;

            bool result{};
            try {
                Serialization::Archive ar{std::move(data)};
                Serialization::Reader pldata{ar};

                u32 version{};
                pldata.Read(version);
                if (version != Serialization::PipelineKeyVersion) {
                    return;
                }

                u32 is_compute{};
                pldata.Read(is_compute);
                if (is_compute) {
                    result = LoadComputePipeline(ar);
                } else {
                    result = LoadGraphicsPipeline(ar);
                }
            } catch (const Serialization::CorruptedData&) {
                // A truncated entry (e.g. from a killed session) is just skipped.
                infos.fill(nullptr);
                modules.fill(nullptr);
                fetch_shader = nullptr;
                result = false;
            }

            if (result) {
                ++num_pipelines;
            }
        });

    if (warmup_pool) {
        warmup_pool->Stop(); // drains: every queued pipeline is built
        warmup_pool.reset();
        for (auto& [key, pipeline] : warmup_graphics) {
            graphics_pipelines.try_emplace(key, std::move(pipeline));
        }
        for (auto& [key, pipeline] : warmup_compute) {
            compute_pipelines.try_emplace(key, std::move(pipeline));
        }
        warmup_graphics.clear();
        warmup_compute.clear();
        warmup_graphics_keys.clear();
        warmup_compute_keys.clear();
    }
    std::fprintf(stderr, "SOTC pipeline cache: preloaded %u pipelines in %lld ms (%u threads)\n",
                 num_pipelines,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - warmup_start)
                                            .count()),
                 warmup_threads);
    LOG_INFO(Render, "Preloaded {} pipelines in {} ms", num_pipelines,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - warmup_start)
                 .count());
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    SaveDriverCache(true);
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
    fetch.Write(one_vgprs);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);
    fetch.Read(one_vgprs);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
        if (!walker_func) {
            // No room for the walker: drop the cached program, it is compiled again.
            walker_func_size = 0;
            return false;
        }
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
