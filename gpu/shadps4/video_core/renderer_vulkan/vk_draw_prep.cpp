// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: draw preparation (see vk_draw_prep.h).

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

#include "../../../shim/bbport_toggles.h"
#include "common/hash.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/sotc_stats.h"

/// gpu/shim/common/assert.cpp: an assertion while set returns to runtime_fault_recover.
extern thread_local bool g_bb_speculative_asserts;

namespace Vulkan {

using Shader::HwStage;
using Shader::SwStage;

u32 DrawPrepMode() {
    static const u32 mode =
        EmulatorSettings.IsShaderCollect() ? 0u : AmdGpu::DrawScan::PrepMode();
    return mode;
}

namespace {
/// Runs f on data nothing has validated yet (the helper's view of guest memory may be older
/// than the command processor's). A guest memory fault (src/probe.c) or an assertion
/// (assert.cpp) inside it gives false instead of stopping the game: the proposal is dropped.
/// f takes no lock and leaves no shared state half-changed.
template <typename F>
[[gnu::noinline]] bool RunSpeculative(F&& f) {
    sigjmp_buf recover;
    runtime_fault_recover = &recover;
    g_bb_speculative_asserts = true;
    if (sigsetjmp(recover, 0) != 0) {
        g_bb_speculative_asserts = false; // the handler already cleared runtime_fault_recover
        return false;
    }
    f();
    g_bb_speculative_asserts = false;
    runtime_fault_recover = nullptr;
    return true;
}
} // namespace

/// The records of one command buffer, and the flattened data and V#s they point to. Chunks are
/// never moved: the command processor reads published records while later ones are written.
struct DrawPrep::Block {
    static constexpr u32 ChunkWords = 1u << 15;

    std::unique_ptr<PreparedDraw[]> draws;
    std::vector<std::unique_ptr<u32[]>> chunks;
    u32 used = 0;
    u32 cap = 0;

    u32* Alloc(u32 words) {
        words = (words + 3) & ~3u; // 16-byte aligned (V#s)
        if (words > cap - used) {
            const u32 size = std::max(words, ChunkWords);
            chunks.push_back(std::make_unique_for_overwrite<u32[]>(size));
            cap = size;
            used = 0;
        }
        u32* out = chunks.back().get() + used;
        used += words;
        return out;
    }
};

DrawPrep::DrawPrep(PipelineCache& cache_) : cache{cache_} {}

DrawPrep::~DrawPrep() {
    Event* event = events.exchange(nullptr, std::memory_order_acquire);
    while (event) {
        Event* next = event->next;
        delete event;
        event = next;
    }
}

std::shared_ptr<void> DrawPrep::NewBlock(u32 num_draws) {
    auto block = std::make_shared<Block>();
    block->draws = std::make_unique<PreparedDraw[]>(num_draws);
    return block;
}

const void* DrawPrep::Record(const void* block, u32 index) const {
    return &static_cast<const Block*>(block)->draws[index];
}

void DrawPrep::Publish(Program& program) {
    const u32 count = static_cast<u32>(program.modules.size());
    if (program.prep_published == count) {
        return;
    }
    auto* event = new Event;
    event->hash = program.info.pgm_hash;
    event->program = &program;
    if (program.prep_published == 0) {
        // The static part of the program's info (walker, resources, fetch shader register);
        // the helper fills user data and the flattened buffer itself.
        event->info = std::make_unique<Shader::Info>(program.info);
    }
    event->first_perm = program.prep_published;
    event->specs.reserve(count - program.prep_published);
    for (u32 i = program.prep_published; i < count; ++i) {
        event->specs.push_back(program.modules[i].spec);
    }
    program.prep_published = count;
    event->next = events.load(std::memory_order_relaxed);
    while (!events.compare_exchange_weak(event->next, event, std::memory_order_release,
                                         std::memory_order_relaxed)) {
    }
}

void DrawPrep::Drain() {
    if (!events.load(std::memory_order_relaxed)) {
        return;
    }
    Event* list = events.exchange(nullptr, std::memory_order_acquire);
    Event* ordered = nullptr; // the list is newest first
    while (list) {
        Event* next = list->next;
        list->next = ordered;
        ordered = list;
        list = next;
    }
    while (ordered) {
        std::unique_ptr<Event> event{ordered};
        ordered = ordered->next;
        if (event->info) {
            auto& helper = programs[event->hash];
            helper.program = event->program;
            helper.info = std::move(event->info);
            helper.specs.clear();
        }
        const auto it = programs.find(event->hash);
        if (it == programs.end()) {
            continue;
        }
        auto& specs = it->second.specs;
        if (specs.size() != event->first_perm) {
            programs.erase(it); // out of step with Program::modules: forget the program
            continue;
        }
        for (auto& spec : event->specs) {
            specs.push_back(std::move(spec));
        }
    }
}

void DrawPrep::Prepare(void* block_ptr, u32 index, const AmdGpu::Regs& regs,
                       AmdGpu::ScanDrawParams params) {
    auto& block = *static_cast<Block*>(block_ptr);
    PreparedDraw& out = block.draws[index];
    out.params = {
        .vertex_sgpr_offset = params.vertex_sgpr_offset,
        .instance_sgpr_offset = params.instance_sgpr_offset,
    };
    Drain();
    out.result = PrepareSelection(block, regs, out);
    SotcStats::Bump(SotcStats::prep_results[out.result]);
}

PreparedDraw::Result DrawPrep::PrepareSelection(Block& block, const AmdGpu::Regs& regs,
                                                PreparedDraw& out) {
    // Only the vertex + pixel configuration with dynamic vertex input (the key then holds no
    // V#-derived vertex format), like the SOTC_SEL_FAST reuse.
    if (regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs ||
        !cache.instance.IsVertexInputDynamicState()) {
        return PreparedDraw::Config;
    }
    auto& key = out.key;
    std::memset(&key, 0, sizeof(GraphicsPipelineKey));
    out.stage_mask = 0;
    if (!RunSpeculative([&] { PipelineCache::KeyTargetsFirstPass(regs, key); })) {
        return PreparedDraw::Fault;
    }

    // RefreshGraphicsStages: pixel first, then vertex (bindings are numbered across stages).
    Shader::Backend::Bindings binding{};
    for (const auto& [hw, sw] : {std::pair{HwStage::Fragment, SwStage::Fragment},
                                 std::pair{HwStage::Vertex, SwStage::Vertex}}) {
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(hw));
        if (regs.stage_enable.IsStageEnabled(static_cast<u32>(hw)) && pgm &&
            pgm->Address<u32*>()) {
            if (const auto result = PrepareStage(block, regs, hw, sw, binding, out);
                result != PreparedDraw::Ready) {
                return result;
            }
        }
        if (sw == SwStage::Fragment) {
            const bool has_fs = out.stage_mask & (1u << static_cast<u32>(SwStage::Fragment));
            key.mrt_mask =
                has_fs ? programs.at(out.stages[static_cast<u32>(SwStage::Fragment)].hash)
                             .info->mrt_mask
                       : 0u;
            key.num_color_attachments = std::bit_width(key.mrt_mask);
        }
    }
    if (!RunSpeculative([&] { cache.KeyTargetsSecondPass(regs, key); })) {
        return PreparedDraw::Fault;
    }
    return PreparedDraw::Ready;
}

namespace {
constexpr u32 MaxVsharps = 32;

/// ParseFetchShader and VertexAttribute::GetSharp, with every guest read through the backing
/// view (the fetch code and the V# tables may share pages the page tracker read-protects).
bool ReadFetch(const Shader::Info& info, Shader::Gcn::FetchShaderData& fetch,
               std::array<AmdGpu::Buffer, MaxVsharps>& vsharps, u32& num_vsharps) {
    auto* memory = Core::Memory::Instance();
    const u32* code = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
    std::array<u32, 512> words;
    size_t num_words = 0;
    for (const size_t want : {size_t{512}, size_t{128}, size_t{32}}) {
        if (memory->TryReadBacking(reinterpret_cast<VAddr>(code), words.data(),
                                   want * sizeof(u32))) {
            num_words = want;
            break;
        }
    }
    if (num_words == 0 || !Shader::Gcn::TryParseFetchShader(words.data(), num_words, fetch) ||
        fetch.attributes.size() > MaxVsharps) {
        return false;
    }
    num_vsharps = 0;
    for (const auto& attrib : fetch.attributes) {
        // Info::ReadUdReg: a pointer in user data, or the user data itself.
        const u32* base = info.user_data.data();
        AmdGpu::Buffer buffer;
        if (attrib.sgpr_base != Shader::IR::NumScalarRegs) {
            std::memcpy(&base, info.user_data.data() + attrib.sgpr_base, sizeof(base));
            base = reinterpret_cast<const u32*>(VAddr(base) & 0xFFFFFFFFFFFFULL);
            if (!memory->TryReadBacking(reinterpret_cast<VAddr>(base + attrib.dword_offset),
                                        &buffer, sizeof(buffer))) {
                return false;
            }
        } else {
            std::memcpy(&buffer, base + attrib.dword_offset, sizeof(buffer));
        }
        buffer.base_address += attrib.inst_offset;
        if (attrib.data_format) {
            buffer.data_format = attrib.data_format;
            buffer.num_format = attrib.num_format;
        }
        vsharps[num_vsharps++] = buffer;
    }
    return true;
}
} // namespace

PreparedDraw::Result DrawPrep::PrepareStage(Block& block, const AmdGpu::Regs& regs, HwStage hw,
                                            SwStage sw, Shader::Backend::Bindings& binding,
                                            PreparedDraw& out) {
    const auto& pgm = *regs.ProgramForStage(static_cast<u32>(hw));
    const u32* code = pgm.Address<u32*>();
    AmdGpu::BinaryInfo bininfo;
    if (!AmdGpu::TryBinaryInfoCached(code, bininfo)) {
        return PreparedDraw::BinInfo;
    }
    const u64 hash = bininfo.shader_hash;
    const auto it = programs.find(hash);
    if (it == programs.end() || it->second.info->hw_stage != hw ||
        it->second.info->sw_stage != sw) {
        return PreparedDraw::NoProgram;
    }
    HelperProgram& helper = it->second;
    Shader::Info& info = *helper.info;

    // BuildRuntimeInfo and GetProgram, on data nothing has validated yet (speculative: a fault or
    // an assertion only drops this proposal).
    Shader::RuntimeInfo runtime_info{};
    info.pgm_base = reinterpret_cast<VAddr>(code);
    info.user_data = pgm.user_data;
    Shader::Gcn::FetchShaderData fetch{};
    std::array<AmdGpu::Buffer, MaxVsharps> vsharps;
    u32 num_vsharps = 0;
    const bool fetch_vs = sw == SwStage::Vertex && info.has_fetch_shader;
    PreparedDraw::Result result = PreparedDraw::Ready;
    u32 perm = 0;
    const bool completed = RunSpeculative([&] {
        runtime_info.Initialize(hw, sw);
        if (hw == HwStage::Vertex) {
            cache.FillHwVertexInfo(runtime_info, regs);
            PipelineCache::FillSwVertexInfo(runtime_info, regs, out.params);
        } else {
            cache.FillHwFragmentInfo(runtime_info, regs, out.key);
        }
        // The SRT walker on this thread's copy: guest memory through the backing view (pages the
        // page tracker read-protects) or directly (anything else).
        info.RefreshFlatBuf(Shader::GetSrtHelperWalkerContext());
        if (fetch_vs && !ReadFetch(info, fetch, vsharps, num_vsharps)) {
            result = PreparedDraw::Fetch;
            return;
        }
        const Shader::StageSpecialization spec(info, runtime_info, cache.profile, binding,
                                               &fetch, std::span{vsharps.data(), num_vsharps});
        // std::ranges::find over Program::modules: the stored spec on the left (a warm-up
        // placeholder compares unequal).
        while (perm < helper.specs.size() && !(helper.specs[perm] == spec)) {
            ++perm;
        }
        if (perm == helper.specs.size()) {
            result = PreparedDraw::NoPerm;
        }
    });
    if (!completed) {
        return PreparedDraw::Fault;
    }
    if (result != PreparedDraw::Ready) {
        return result;
    }

    PreparedStage& stage = out.stages[static_cast<u32>(sw)];
    stage.program = helper.program;
    stage.hash = hash;
    stage.pgm_base = info.pgm_base;
    stage.start = binding;
    stage.perm_idx = perm;
    stage.fetch_vs = fetch_vs;
    stage.flat_size = static_cast<u32>(info.flattened_ud_buf.size());
    u32* flat = block.Alloc(stage.flat_size);
    std::memcpy(flat, info.flattened_ud_buf.data(), stage.flat_size * sizeof(u32));
    stage.flat = flat;
    stage.num_vsharps = num_vsharps;
    if (num_vsharps != 0) {
        u32* copy = block.Alloc(num_vsharps * (sizeof(AmdGpu::Buffer) / sizeof(u32)));
        std::memcpy(copy, vsharps.data(), num_vsharps * sizeof(AmdGpu::Buffer));
        stage.vsharps = reinterpret_cast<const AmdGpu::Buffer*>(copy);
    }
    info.AddBindings(binding);
    out.key.stage_hashes[static_cast<u32>(sw)] = HashCombine(hash, u64{perm});
    out.stage_mask |= 1u << static_cast<u32>(sw);
    return PreparedDraw::Ready;
}

// ------------------------------------------------------------------ command processor side

namespace {
/// The flattened dwords StageSpecialization reads (every sharp of the program's resources, as
/// SharpFetch::Fetch and FMaskResource/SamplerResource read them), sorted and unique. The
/// specialization is a function of these, the program base, the runtime info, the start bindings
/// and, for a vertex shader, the fetch shader and its V#s: per-draw constants and dynamic windows
/// in the flattened buffer do not change the permutation (the command processor's own walk
/// supplies them to the draw).
std::vector<u16> SpecDwords(const Shader::Info& info) {
    std::vector<u16> dwords;
    const auto add_fetch = [&](const auto& fetch, u32 num_dwords, u32 element_off) {
        using Summary = typename std::remove_cvref_t<decltype(fetch)>::Summary;
        if (fetch.summary == Summary::SingleLoad) {
            for (u32 i = 0; i < num_dwords; ++i) {
                dwords.push_back(static_cast<u16>(fetch.offsets[0] + element_off + i));
            }
        } else if (fetch.summary == Summary::MultiLoad) {
            for (u32 i = 0; i < num_dwords; ++i) {
                if (fetch.load_mask & (1u << i)) {
                    dwords.push_back(static_cast<u16>(fetch.offsets[i] + element_off));
                }
            }
        }
    };
    for (const auto& buffer : info.buffers) {
        add_fetch(buffer.sharp_fetch, 4, 0);
    }
    for (const auto& image : info.images) {
        const u32 num_dwords = image.is_r128 ? 4 : 8;
        for (u32 e = 0; e < std::max<u32>(image.array_size, 1); ++e) {
            add_fetch(image.sharp_fetch, num_dwords, e * image.array_stride_dw);
        }
    }
    for (const auto& fmask : info.fmasks) {
        for (u32 i = 0; i < 8; ++i) {
            dwords.push_back(static_cast<u16>(fmask.sharp_idx + i));
        }
    }
    for (const auto& sampler : info.samplers) {
        add_fetch(sampler.sharp_fetch, 4, 0);
        if (sampler.post_op == Shader::SharpFetchPostOp::DisableAnisoIfSingleLod) {
            dwords.push_back(sampler.post_op_tsharp_dw3_off);
        }
    }
    std::sort(dwords.begin(), dwords.end());
    dwords.erase(std::unique(dwords.begin(), dwords.end()), dwords.end());
    return dwords;
}

/// SOTC_PREP_FULL_FLAT=1: compare the whole flattened buffer (as first built; for A/B).
bool FullFlatCompare() {
    static const bool full = std::getenv("SOTC_PREP_FULL_FLAT") != nullptr;
    return full;
}

/// The proposal's flattened data selects the same permutation as the command processor's.
bool SameSpecInputs(Program& program, const std::vector<u32>& cp, const PreparedStage& st) {
    if (cp.size() != st.flat_size) {
        return false;
    }
    if (FullFlatCompare()) {
        return std::memcmp(cp.data(), st.flat, st.flat_size * sizeof(u32)) == 0;
    }
    if (!program.prep_spec_dwords_ready) {
        program.prep_spec_dwords = SpecDwords(program.info);
        program.prep_spec_dwords_ready = true;
    }
    for (const u16 i : program.prep_spec_dwords) {
        if (i < st.flat_size && cp[i] != st.flat[i]) {
            return false;
        }
    }
    return true;
}

/// SOTC_PREP_DIAG=1: why a proposal's flattened data differs. The helper's way of reading is
/// repeated here, now: equal to the command processor's result means the memory changed after
/// the helper read it; equal to the helper's means the two ways of reading differ (GPU-written
/// data the helper reads stale from the backing view).
void DiagnoseFlat(const Shader::Info& info, const PreparedStage& st) {
    static const bool diag = std::getenv("SOTC_PREP_DIAG") != nullptr;
    if (!diag || info.flattened_ud_buf.size() != st.flat_size) {
        return;
    }
    Shader::Info now = info;
    now.RefreshFlatBuf(Shader::GetSrtHelperWalkerContext());
    const auto& cp = info.flattened_ud_buf;
    const bool now_cp = now.flattened_ud_buf == cp;
    const bool now_helper =
        std::memcmp(now.flattened_ud_buf.data(), st.flat, st.flat_size * sizeof(u32)) == 0;
    (now_cp ? SotcStats::prep_diag_later : now_helper ? SotcStats::prep_diag_read
                                                      : SotcStats::prep_diag_other)
        .fetch_add(1, std::memory_order_relaxed);
    u32 num_diff = 0, first = ~0u, in_user_data = 0;
    for (u32 i = 0; i < st.flat_size; ++i) {
        if (cp[i] != st.flat[i]) {
            num_diff++;
            first = std::min(first, i);
            in_user_data += i < info.user_data.size();
        }
    }
    static std::atomic<u32> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 48) {
        std::fprintf(stderr,
                     "SOTC prep diag: %s %#llx flat %u dwords, %u differ (%u in user data), "
                     "first %u: helper %#x cp %#x; now %s\n",
                     info.sw_stage == SwStage::Vertex ? "vs" : "ps",
                     static_cast<unsigned long long>(info.pgm_hash), st.flat_size, num_diff,
                     in_user_data, first, st.flat[first], cp[first],
                     now_cp ? "= cp (changed after the helper read)"
                     : now_helper ? "= helper (read differs)"
                                  : "differs from both");
    }
}
} // namespace

bool PipelineCache::TryPreparedSelection(const PreparedDraw& prepared,
                                         const DrawIndirectParams& params) {
    if (prepared.result != PreparedDraw::Ready) {
        SotcStats::Bump(SotcStats::prep_unready[prepared.result]);
        return false;
    }
    const auto reject = [](u32 check) {
        SotcStats::Bump(SotcStats::prep_reject[check]);
        return false;
    };
    if (prepared.params.vertex_sgpr_offset != params.vertex_sgpr_offset ||
        prepared.params.instance_sgpr_offset != params.instance_sgpr_offset) {
        return reject(0);
    }
    // The registers are the command processor's (equal history hash). Everything read from
    // guest memory is read again here, exactly as the full selection reads it.
    const auto& regs = liverpool->regs;
    Shader::Backend::Bindings binding{};
    // SOTC_PHASE_TIME=1: 32 program parameters, 33 walker, 34 specialization inputs compared,
    // 35 fetch shader parsed and its V#s compared, 36 the rest of the selection.
    const bool timed = SotcStats::PhaseTime();
    u64 last = timed ? __builtin_ia32_rdtsc() : 0;
    const auto mark = [&](int phase) {
        if (timed) {
            const u64 now = __builtin_ia32_rdtsc();
            SotcStats::Bump(SotcStats::phase_cycles[phase], now - last);
            last = now;
        }
    };
    for (const SwStage sw : {SwStage::Fragment, SwStage::Vertex}) {
        const u32 i = static_cast<u32>(sw);
        if ((prepared.stage_mask & (1u << i)) == 0) {
            continue;
        }
        const PreparedStage& st = prepared.stages[i];
        const HwStage hw = sw == SwStage::Fragment ? HwStage::Fragment : HwStage::Vertex;
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(hw));
        if (!pgm || !pgm->Address<u32*>()) {
            return reject(1);
        }
        const auto p = AmdGpu::GetParams(*pgm);
        mark(32);
        if (p.hash != st.hash) {
            return reject(1); // other code at the same address
        }
        if (binding != st.start) {
            return reject(2);
        }
        Program& program = *st.program;
        if (st.perm_idx >= program.modules.size() || !program.modules[st.perm_idx].module) {
            return reject(3);
        }
        auto& info = program.info;
        info.pgm_base = p.Base();
        info.user_data = p.user_data;
        mark(36);
        info.RefreshFlatBuf();
        mark(33);
        if (info.pgm_base != st.pgm_base ||
            !SameSpecInputs(program, info.flattened_ud_buf, st)) {
            DiagnoseFlat(info, st);
            return reject(4);
        }
        mark(34);
        if (st.fetch_vs) {
            Shader::Gcn::FetchShaderData now;
            Shader::Gcn::ParseFetchShader(info, now);
            const auto& spec_fetch = program.modules[st.perm_idx].spec.fetch_shader_data;
            if (!(now == spec_fetch) || now.attributes.size() != st.num_vsharps) {
                return reject(5);
            }
            for (u32 a = 0; a < st.num_vsharps; ++a) {
                const AmdGpu::Buffer sharp = now.attributes[a].GetSharp(info);
                if (std::memcmp(&sharp, &st.vsharps[a], sizeof(sharp)) != 0) {
                    return reject(6);
                }
            }
            mark(35);
        }
        info.AddBindings(binding);
    }
    // Accept: what RefreshGraphicsKey leaves behind for the pipeline lookup and the draw.
    graphics_key = prepared.key;
    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;
    for (const SwStage sw : {SwStage::Fragment, SwStage::Vertex}) {
        const u32 i = static_cast<u32>(sw);
        if ((prepared.stage_mask & (1u << i)) == 0) {
            continue;
        }
        const PreparedStage& st = prepared.stages[i];
        auto& module = st.program->modules[st.perm_idx];
        infos[i] = &st.program->info;
        modules[i] = module.module;
        if (sw == SwStage::Vertex && !module.spec.fetch_shader_data.Empty()) {
            fetch_shader = &module.spec.fetch_shader_data;
        }
    }
    SotcStats::Bump(SotcStats::prep_accept);
    mark(36);
    return true;
}

} // namespace Vulkan
