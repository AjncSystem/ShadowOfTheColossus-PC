// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <queue>
#include <unordered_set>
#include "common/elf_info.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Workgroup barrier, or a subgroup one for workgroups of several waves (see below).
static void EmitSync(IR::IREmitter& ir, bool subgroup) {
    if (subgroup) {
        ir.SubgroupBarrier();
    } else {
        ir.Barrier();
    }
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block, bool subgroup) {
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                EmitSync(ir, subgroup);
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                EmitSync(ir, subgroup);
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        EmitSync(ir, subgroup);
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block, bool subgroup) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    EmitSync(ir, subgroup);
}

struct DivergenceContext {
    // Blocks whose ReadLane with a constant lane the wave64 lowering turns into a
    // workgroup-uniform value (an exchange through LDS).
    std::unordered_set<const IR::Block*> uniform_readlane_blocks;
    // Synchronize host subgroups only (workgroups larger than one wave).
    bool subgroup{};
};

// A condition is divergent when it depends on LocalInvocationId. With uniform_readlane_blocks,
// the search does not continue through uniform ReadLanes: a wave-wide reduction (a tile min/max
// depth, for example) is the same in every invocation even though its inputs are not.
// From Pink-shadPS4 (luizgustavs), lds_barrier_uniform_readlane.
static bool SearchDivergence(const IR::U1& cond,
                             const std::unordered_set<const IR::Block*>* uniform_readlane_blocks) {
    if (cond.IsImmediate()) {
        return false;
    }
    std::unordered_set<const IR::Inst*> visited{cond.Inst()};
    std::queue<const IR::Inst*> queue;
    queue.push(cond.Inst());
    while (!queue.empty()) {
        const IR::Inst* inst = queue.front();
        queue.pop();
        if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
            inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
            return true;
        }
        if (uniform_readlane_blocks && inst->GetOpcode() == IR::Opcode::ReadLane &&
            inst->Arg(1).IsImmediate() && uniform_readlane_blocks->contains(inst->GetParent())) {
            continue;
        }
        for (size_t arg = inst->NumArgs(); arg--;) {
            const IR::Value value = inst->Arg(arg);
            if (value.IsImmediate()) {
                continue;
            }
            const IR::Inst* arg_inst = value.Inst();
            if (visited.insert(arg_inst).second) {
                queue.push(arg_inst);
            }
        }
    }
    return false;
}

static bool IsDivergent(const IR::U1& cond, const DivergenceContext& ctx) {
    return SearchDivergence(cond, ctx.uniform_readlane_blocks.empty()
                                      ? nullptr
                                      : &ctx.uniform_readlane_blocks);
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth,
                                    const DivergenceContext& ctx) {
    const IR::U1 cond = data.if_node.cond;
    if (IsDivergent(cond, ctx)) {
        if (divergence_depth == 0) {
            EmitBarrierAtBlockStart(data.if_node.merge, ctx.subgroup);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list,
                                  const DivergenceContext& ctx) {
    NodeSet divergent_loops;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (IsDivergent(node.data.repeat.cond, ctx)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (IsDivergent(node.data.break_node.cond, ctx)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

// Workgroups of several waves synchronize with s_barrier between waves, but the last steps of a
// reduction (s[tid] += s[tid + 16] for tid < 16, ...) run inside one wave and rely on its lanes
// executing in lockstep. A host subgroup of 32 does not guarantee that, and the compiler may
// reorder the loads of other lanes' values: SotC's auto-exposure averaged its luminance history
// to 0.78 instead of 2.06. Those workgroups get subgroup barriers at the same points (a workgroup
// barrier there would also wait on the other waves, which the guest code never does).
// SotC only; SOTC_LDS_BARRIERS=0/1 overrides.
static bool MultiWaveLdsBarriersEnabled() {
    static const bool enabled = [] {
        if (const char* v = std::getenv("SOTC_LDS_BARRIERS")) {
            return std::atoi(v) != 0;
        }
        return Common::ElfInfo::Instance().GameSerial() == "CUSA08809";
    }();
    return enabled;
}

// Workgroups smaller than a wave are one partial GCN wave: their lanes write LDS and read the
// values of the other lanes with no s_barrier at all. On the host the compiler may move those
// reads before the writes, so they read stale LDS: SotC's particle count prefix sum (cs
// 0xbb7eb2dd, 16 threads) gave 0 instances to every GPU particle draw (no waterfall spray, foam
// or mist). They get workgroup barriers like a full wave. SotC only; SOTC_LDS_SMALL=0/1 overrides.
static bool SmallGroupLdsBarriersEnabled() {
    static const bool enabled = [] {
        if (const char* v = std::getenv("SOTC_LDS_SMALL")) {
            return std::atoi(v) != 0;
        }
        return Common::ElfInfo::Instance().GameSerial() == "CUSA08809";
    }();
    return enabled;
}

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup.
    const bool multi_wave = threadgroup_size > GcnSubgroupSize &&
                            threadgroup_size % GcnSubgroupSize == 0 &&
                            MultiWaveLdsBarriersEnabled();
    const bool partial_wave = threadgroup_size < GcnSubgroupSize && SmallGroupLdsBarriersEnabled();
    if (shared_memory_size == 0 ||
        (threadgroup_size != GcnSubgroupSize && !multi_wave && !partial_wave) ||
        !profile.needs_lds_barriers) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    DivergenceContext ctx;
    ctx.subgroup = multi_wave;
    // A ReadLane is uniform within one wave, not across the waves of a larger workgroup.
    if (!multi_wave && profile.needs_wave64_uniform_branches) {
        const auto blocks = FindWave64UniformBlocks(program);
        ctx.uniform_readlane_blocks.insert(blocks.begin(), blocks.end());
    }
    u32 divergence_depth{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list, ctx);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth, ctx);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge, ctx.subgroup);
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            EmitBarrierInBlock(node.data.block, ctx.subgroup);
        }
    }
}

} // namespace Shader::Optimization
