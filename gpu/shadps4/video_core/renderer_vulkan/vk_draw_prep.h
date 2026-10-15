// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: draw preparation (SOTC_DRAW_PREP), the multi-threaded part of graphics pipeline selection.
//
// The DrawScan thread (amdgpu/sotc_draw_scan.h) replays every graphics register write ahead of
// the command processor. At each draw it calls DrawPrep::Prepare with its register file, which
// runs the selection the command processor would run (render target key, binary info, runtime
// info, the SRT walker, fetch shader, specialization, permutation lookup) and stores the result.
// The helper only proposes: it reads guest memory through the backing view or under a fault
// guard (runtime_fault_recover), never compiles, never takes a lock the command processor waits
// on, and never touches the command processor's caches. Programs and their permutations reach it
// through a lock-free list (Publish).
//
// The command processor proves a proposal (PipelineCache::TryPreparedSelection) before using it:
// the register history hash equal at the draw (so the registers are equal, SOTC_DRAW_SCAN=2
// checks this), then per stage the code hash, start bindings, the walker re-run on its own
// thread with the flattened dwords the specialization reads compared (per-draw constants change
// after the scanner read them and do not select the permutation), the fetch shader re-parsed and
// every vertex V# re-read (the gap the Bloodborne/GoW3 versions left open). What it saves: building the runtime infos,
// the key, the specialization memo search and, on memo misses, the specialization and the
// permutation search.
//
// SOTC_DRAW_PREP: 0 off, 1 use proposals (default), 2 verify (prepare, run the full selection,
// use it and count differences: SOTC prep line, "SOTC prep verify" messages).
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "video_core/amdgpu/sotc_draw_scan.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace Vulkan {

u32 DrawPrepMode();

struct PreparedStage {
    Program* program{};
    u64 hash{};
    VAddr pgm_base{};
    Shader::Backend::Bindings start{};
    const u32* flat{};
    u32 flat_size{};
    u32 perm_idx{};
    const AmdGpu::Buffer* vsharps{};
    u32 num_vsharps{};
    bool fetch_vs{};
};

struct PreparedDraw {
    /// Why a draw has no proposal (Ready: it has one).
    enum Result : u8 {
        Ready,
        Config,    ///< not vertex + pixel, or no dynamic vertex input
        BinInfo,   ///< the command processor never looked this code up
        NoProgram, ///< program not published yet
        Fault,     ///< the walker faulted on guest memory
        Fetch,     ///< fetch shader or vertex V# unreadable
        NoPerm,    ///< no compiled permutation matches (the command processor will compile)
        NumResults,
    };
    Result result{Config};
    u8 stage_mask{};
    DrawIndirectParams params{};
    std::array<PreparedStage, MaxShaderStages> stages{};
    GraphicsPipelineKey key{};
};

class DrawPrep final : public AmdGpu::DrawPreparer {
public:
    explicit DrawPrep(PipelineCache& cache);
    ~DrawPrep() override;

    std::shared_ptr<void> NewBlock(u32 num_draws) override;
    void Prepare(void* block, u32 index, const AmdGpu::Regs& regs,
                 AmdGpu::ScanDrawParams params) override;
    const void* Record(const void* block, u32 index) const override;

    /// Command processor: makes a program and its permutations so far known to the helper.
    void Publish(Program& program);

private:
    struct Event {
        Event* next{};
        u64 hash{};
        Program* program{};
        std::unique_ptr<Shader::Info> info; ///< set for the first event of a program
        u32 first_perm{};
        std::vector<Shader::StageSpecialization> specs;
    };
    struct HelperProgram {
        Program* program{};
        std::unique_ptr<Shader::Info> info; ///< the helper's own copy (user data, flat buffer)
        std::vector<Shader::StageSpecialization> specs; ///< Program::modules[i].spec, same order
    };
    struct Block;

    void Drain();
    PreparedDraw::Result PrepareSelection(Block& block, const AmdGpu::Regs& regs,
                                          PreparedDraw& out);
    PreparedDraw::Result PrepareStage(Block& block, const AmdGpu::Regs& regs,
                                      Shader::HwStage hw, Shader::SwStage sw,
                                      Shader::Backend::Bindings& binding, PreparedDraw& out);

    PipelineCache& cache;
    std::atomic<Event*> events{nullptr};
    std::unordered_map<u64, HelperProgram> programs; ///< scanner thread only
};

} // namespace Vulkan
