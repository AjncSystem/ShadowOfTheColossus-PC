// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/sotc_stats.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
// Above both lines before the upstream merge: generation changed, so old caches are dropped.
static constexpr u32 ShaderBinaryVersion = 34u;
// sotc: 33 = the draw thread merge (SRT walker faults read zero instead of patching).
// sotc: 34 = invariant position on barycentric-emulation pipelines (ground-level texture/depth
// mismatch between the pipeline with the geometry shader and the one without).
static constexpr u32 ShaderMetaVersion = 21u;
static constexpr u32 PipelineKeyVersion = 3u;
} // namespace Serialization

namespace Vulkan {

namespace {
// sotc: where the boot-time preload spends its time (logged with "Preloaded N pipelines").
struct WarmUpTimes {
    std::chrono::steady_clock::duration load{}, modules{}, pipelines{};
} warmup_times;

struct WarmUpTimer {
    std::chrono::steady_clock::duration& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WarmUpTimer() {
        total += std::chrono::steady_clock::now() - start;
    }
};

long long Ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

// sotc: a shader permutation is stored under its index in the program ({pgm}_{idx}.spv, and a
// meta named after a hash of the same pair). The index is the order in which this session met the
// permutations, so a session that did not preload all of them can give an index already used on
// disk by another permutation. Writing over it paired the pipelines saved earlier with the wrong
// shader: the driver refused them at the next start (ErrorUnknown on AMD). Such a permutation is
// now left out of the cache, along with every pipeline that uses it; they compile each session.
std::mutex collided_mutex;
std::unordered_set<u64> collided_bins;      // BinaryId of the permutations left out
std::unordered_set<size_t> collided_perms; // perm hashes, as in the pipeline keys

u64 BinaryId(u64 pgm_hash, size_t perm_idx) {
    return pgm_hash ^ (u64(perm_idx) << 56);
}

bool IsCollidedPerm(size_t perm_hash) {
    std::scoped_lock lock{collided_mutex};
    return collided_perms.contains(perm_hash);
}
} // namespace

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened() || IsCollidedPerm(key.value)) {
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
    for (const auto stage_hash : key.stage_hashes) {
        if (stage_hash && IsCollidedPerm(stage_hash)) {
            return;
        }
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
    {
        std::scoped_lock lock{collided_mutex};
        if (collided_bins.contains(BinaryId(info.pgm_hash, perm_idx))) {
            collided_perms.insert(perm_hash);
            return;
        }
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

    const auto name = fmt::format("{:#018x}_{}", pgm_hash, perm_idx);
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        std::vector<u32> stored;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary, name, stored);
        if (!stored.empty() && stored != spv) {
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render_Vulkan,
                            "Shader permutation {} is another one on disk: not cached", name);
            }
            std::scoped_lock lock{collided_mutex};
            collided_bins.insert(BinaryId(pgm_hash, perm_idx));
            return;
        }
    }
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary, name, std::move(spv));
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

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    {
        WarmUpTimer timer{warmup_times.pipelines};
        // sotc: built by the pipeline workers (SOTC_ASYNC_PRELOAD); WarmUp waits for them.
        const bool deferred = AsyncPreloadEnabled();
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, true, deferred);
        if (deferred) {
            auto job = std::make_unique<AsyncPipelineJob>();
            job->compute = it.value().get();
            {
                std::scoped_lock lock{async_mutex};
                async_queue.insert(async_queue.begin() + async_compute_queued++, std::move(job));
            }
            async_cv.notify_one();
            preload_computes.push_back(compute_key);
        }
    }

    infos.fill(nullptr);
    modules.fill(nullptr);

    if (AsyncPreloadEnabled()) {
        return true; // checked by WarmUp once built
    }
    if (!it.value()->Handle()) {
        compute_pipelines.erase(compute_key);
        return false;
    }
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
    if (with_gs) {
        sdata.Write(gs);
    }
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    if (with_gs) {
        sdata.Read(gs);
    }
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

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    ASSERT(is_new);

    // sotc: built by a pipeline worker; the map entry stays null (draws skipped, full-screen
    // passes built on the command processor) until the command processor collects it
    // (SOTC_ASYNC_PRELOAD). Not the ones whose construction reads per-draw state.
    const bool async = AsyncPreloadEnabled() && AsyncPipelineEligible();
    if (async) {
        QueueAsyncPipeline(std::hash<GraphicsPipelineKey>{}(graphics_key), &sdata);
    } else {
        WarmUpTimer timer{warmup_times.pipelines};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
            runtime_infos, fetch_shader, modules, sdata, true);
    }

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    if (async) {
        return true; // a refused one is dropped when collected (CollectAsyncPipelines)
    }
    if (!it.value()->Handle()) {
        graphics_pipelines.erase(graphics_key);
        return false;
    }
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
    {
        WarmUpTimer timer{warmup_times.load};
        Storage::DataBase::Instance().Load(
            Storage::BlobType::ShaderBinary,
            fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx), spv);
    }
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
        {
            WarmUpTimer timer{warmup_times.modules};
            module = CompileSPV(spv, instance.GetDevice());
        }
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
            WarmUpTimer timer{warmup_times.modules};
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
    // A cache made for another profile (another GPU or driver, a new build) cannot be used. It
    // used to be ignored and left in place, so every later run started without a cache and saved
    // nothing: it is set aside and a new one starts with this profile.
    bool compatible = true;
    if (!profile_data.empty()) {
        Shader::Profile cached_profile{};
        if (profile_data.size() == sizeof(Shader::Profile)) {
            // sotc: barycentric_via_gs, the last member, sits where older caches have a padding
            // byte (seen 0xe3, 0x36): anything but 1 there is off, so those caches still match.
            auto& via_gs = profile_data[offsetof(Shader::Profile, barycentric_via_gs)];
            via_gs = via_gs == 1 ? 1 : 0;
            std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        }
        compatible = profile_data.size() == sizeof(Shader::Profile) && cached_profile == profile;
        if (!compatible) {
            LOG_WARNING(Render, "Pipeline cache isn't compatible with current system (profile of "
                                "{} bytes, expected {}). Starting a new cache",
                        profile_data.size(), sizeof(Shader::Profile));
            Storage::DataBase::Instance().Reset();
        }
    }
    if (profile_data.empty() || !compatible) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    const auto warmup_start = std::chrono::steady_clock::now();
    // sotc: progress for the loading screen's bar.
    SotcStats::warmup_done.store(0, std::memory_order_relaxed);
    SotcStats::warmup_total.store(
        static_cast<u32>(Storage::DataBase::Instance().CountBlobs(Storage::BlobType::PipelineKey)),
        std::memory_order_relaxed);

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            SotcStats::warmup_done.fetch_add(1, std::memory_order_relaxed);

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

    // sotc: the compute pipelines the workers built (SOTC_ASYNC_PRELOAD); the graphics ones
    // are still being built while the game starts.
    for (const auto& key : preload_computes) {
        const auto it = compute_pipelines.find(key);
        if (it == compute_pipelines.end()) {
            continue;
        }
        it.value()->WaitBuilt();
        if (!it.value()->Handle()) {
            preload_refused.push_back(std::move(it.value()));
            compute_pipelines.erase(it);
            --num_pipelines;
        }
    }
    preload_computes.clear();

    LOG_INFO(Render,
             "Preloaded {} pipelines in {} ms (SPIR-V reads {} ms, shader modules {} ms, "
             "pipeline creation {} ms)",
             num_pipelines, Ms(std::chrono::steady_clock::now() - warmup_start),
             Ms(warmup_times.load), Ms(warmup_times.modules), Ms(warmup_times.pipelines));
    SotcStats::warmup_total.store(0, std::memory_order_relaxed); // the bar goes away
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(FlatBuf());
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(FlatBuf());

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
