// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <optional>
#include <queue>
#include <unordered_set>
#include "common/elf_info.h"
#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsDivergentInst(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::LaneId:
        return true;
    case IR::Opcode::GetAttributeU32:
        return inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId;
    default:
        return false;
    }
}

static bool IsDivergentCondition(const IR::U1& condition,
                                 const std::unordered_set<const IR::Inst*>* uniform_insts) {
    if (condition.IsImmediate()) {
        return false;
    }
    if (!uniform_insts) {
        return IR::BreadthFirstSearch(condition.Inst(),
                                      [](const IR::Inst* inst) -> std::optional<bool> {
                                          return IsDivergentInst(inst) ? std::optional{true}
                                                                       : std::nullopt;
                                      })
            .value_or(false);
    }
    // The search stops at uniform_insts: their result is the same in the whole workgroup
    std::unordered_set<const IR::Inst*> visited{condition.Inst()};
    std::queue<const IR::Inst*> queue;
    queue.push(condition.Inst());
    while (!queue.empty()) {
        const IR::Inst* inst = queue.front();
        queue.pop();
        if (IsDivergentInst(inst)) {
            return true;
        }
        if (uniform_insts->contains(inst)) {
            continue;
        }
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            const IR::Inst* arg_inst = inst->Arg(arg).TryInst();
            if (arg_inst && visited.insert(arg_inst).second) {
                queue.push(arg_inst);
            }
        }
    }
    return false;
}

std::vector<IR::Block*> FindWave64UniformBlocks(
    const IR::Program& program, const std::unordered_set<const IR::Inst*>* uniform_insts) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<const IR::Block*> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond, uniform_insts);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop:
            loops.push_back(node.data.loop.merge);
            break;
        case Type::Repeat:
            if (loops.empty() || loops.back() != node.data.repeat.merge) {
                return {};
            }
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0 && loops.empty()) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

static IR::Inst* FindBallotForMaskedBitCount(const IR::Inst& mbcnt) {
    IR::Value value = mbcnt.Arg(0);
    if (value.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* inst = value.Inst();
    if (inst->GetOpcode() != IR::Opcode::CompositeExtractU32x2 || inst->Arg(0).IsImmediate()) {
        return nullptr;
    }
    inst = inst->Arg(0).Inst();
    if (inst->GetOpcode() != IR::Opcode::UnpackUint2x32 || inst->Arg(0).IsImmediate()) {
        return nullptr;
    }
    inst = inst->Arg(0).Inst();
    return inst->GetOpcode() == IR::Opcode::Ballot ? inst : nullptr;
}

static const IR::Inst* SkipBitCasts(const IR::Value& value) {
    if (value.IsImmediate()) {
        return nullptr;
    }
    const IR::Inst* inst = value.Inst();
    while (inst->GetOpcode() == IR::Opcode::BitCastU32F32 ||
           inst->GetOpcode() == IR::Opcode::BitCastF32U32) {
        if (inst->Arg(0).IsImmediate()) {
            return nullptr;
        }
        inst = inst->Arg(0).Inst();
    }
    return inst;
}

/// Identity of the reduction step that produced value, when value = op(x, ShuffleXor(x, k)).
/// Based on the wave64_missing_lane_identity workaround of Pink-shadPS4 (luizgustavs).
static std::optional<u32> ReductionIdentity(const IR::Value& value) {
    const IR::Inst* op = SkipBitCasts(value);
    if (!op || op->NumArgs() != 2) {
        return std::nullopt;
    }
    bool is_step = false;
    for (u32 arg = 0; arg < 2; ++arg) {
        const IR::Inst* shuffle = SkipBitCasts(op->Arg(arg));
        const IR::Inst* other = SkipBitCasts(op->Arg(1 - arg));
        is_step |= shuffle && other && shuffle->GetOpcode() == IR::Opcode::ShuffleXor &&
                   SkipBitCasts(shuffle->Arg(0)) == other;
    }
    if (!is_step) {
        return std::nullopt;
    }
    switch (op->GetOpcode()) {
    case IR::Opcode::FPMin32:
        return 0x7f800000u; // +inf
    case IR::Opcode::FPMax32:
        return 0xff800000u; // -inf
    case IR::Opcode::FPMul32:
        return 0x3f800000u; // 1.0
    case IR::Opcode::UMin32:
    case IR::Opcode::BitwiseAnd32:
        return 0xffffffffu;
    case IR::Opcode::SMin32:
        return 0x7fffffffu;
    case IR::Opcode::SMax32:
        return 0x80000000u;
    case IR::Opcode::IMul32:
        return 1u;
    case IR::Opcode::FPAdd32:
    case IR::Opcode::IAdd32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
        return 0u;
    default:
        return std::nullopt;
    }
}

static std::atomic<int> wave64_uniform_default{-1};

void SetWave64UniformBranchesDefault(bool enabled) {
    wave64_uniform_default = enabled ? 1 : 0;
}

bool Wave64UniformBranchesEnabled() {
    // SotC only; SOTC_WAVE64_UNIFORM=0/1 overrides; the renderer sets the default
    // (PipelineCache).
    static const int forced = [] {
        const char* v = std::getenv("SOTC_WAVE64_UNIFORM");
        return v ? (std::atoi(v) != 0 ? 1 : 0) : -1;
    }();
    if (forced >= 0) {
        return forced != 0;
    }
    static const bool sotc = Common::ElfInfo::Instance().GameSerial() == "CUSA08809";
    return sotc && wave64_uniform_default != 0;
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        // The whole workgroup fits in one host subgroup, but the guest code was written for a
        // wave64 whose upper half is inactive. Reads of a fixed lane in that upper half (e.g. the
        // "readlane 32" that merges the two halves of a cross-lane reduction) index past the host
        // subgroup, which is undefined for OpGroupNonUniformBroadcast. When the value read is a
        // reduction step, the missing half contributes the reduction identity (folding it onto
        // the lower half would count the lower lanes twice, e.g. double a sum). Other reads are
        // folded into the lower half so they stay valid.
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::ReadLane || !inst.Arg(1).IsImmediate()) {
                    continue;
                }
                const u32 lane = inst.Arg(1).U32();
                if (lane < profile.subgroup_size) {
                    continue;
                }
                // SOTC_WAVE64_IDENTITY=0 restores the plain fold (for A/B comparisons).
                static const bool identity_enabled = [] {
                    const char* v = std::getenv("SOTC_WAVE64_IDENTITY");
                    return v ? std::atoi(v) != 0 : true;
                }();
                const auto identity =
                    identity_enabled ? ReductionIdentity(inst.Arg(0)) : std::optional<u32>{};
                if (identity) {
                    inst.ReplaceUsesWithAndRemove(IR::Value{*identity});
                } else {
                    inst.SetArg(1, IR::Value{lane % profile.subgroup_size});
                }
            }
        }
        return;
    }

    const auto is_unpack = [](const IR::Use& use) {
        return use.user->GetOpcode() == IR::Opcode::UnpackUint2x32;
    };
    // The instructions lowered below exchange their value through LDS, so it is the same in the
    // whole 64-lane wave (and the workgroup, when it is at most one wave)
    const auto is_lowered = [&](const IR::Inst& inst) {
        return (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate()) ||
               (inst.GetOpcode() == IR::Opcode::Ballot &&
                std::ranges::any_of(inst.Uses(), is_unpack));
    };

    std::vector<IR::Inst*> worklist;
    auto uniform_blocks = FindWave64UniformBlocks(program);
    // In a one-wave workgroup, branches on lowered values are uniform, so lower their ReadLane and
    // Ballot too, expanding the uniform blocks until stable. From Pink-shadPS4 (luizgustavs),
    // wave64_uniform_branches.
    if (num_threads <= 64 && Wave64UniformBranchesEnabled()) {
        std::unordered_set<const IR::Inst*> uniform_insts;
        while (true) {
            const size_t known = uniform_insts.size();
            for (const IR::Block* block : uniform_blocks) {
                for (const IR::Inst& inst : block->Instructions()) {
                    if (is_lowered(inst)) {
                        uniform_insts.insert(&inst);
                    }
                }
            }
            if (uniform_insts.size() == known) {
                break;
            }
            uniform_blocks = FindWave64UniformBlocks(program, &uniform_insts);
        }
    }
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        const auto push_worklist = [&](IR::Inst& inst) {
            if (is_uniform) {
                worklist.push_back(&inst);
            } else {
                LOG_WARNING(Render_Recompiler, "{} instruction in non uniform control flow",
                            inst.GetOpcode());
            }
        };
        for (IR::Inst& inst : block->Instructions()) {
            if (is_lowered(inst)) {
                push_worklist(inst);
            } else if (inst.GetOpcode() == IR::Opcode::MaskedBitCount32) {
                IR::Inst* const ballot = FindBallotForMaskedBitCount(inst);
                if (ballot == nullptr ||
                    std::ranges::contains(uniform_blocks, ballot->GetParent())) {
                    worklist.push_back(&inst);
                }
            }
        }
    }
    if (worklist.empty()) {
        return;
    }

    const u32 scratch_base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u64));
    const u32 scratch_size =
        (Common::AlignUp(num_threads, 64) / profile.subgroup_size) * sizeof(u32);
    program.info.shared_memory_scratch_size =
        scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;

    for (IR::Inst* inst : worklist) {
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64", inst->GetOpcode());
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 subgroup_id = ir.ShiftRightLogical(invocation_index, ir.Imm32(5));
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, mask_low, offset);
            ir.Barrier();
            const IR::U64 mask = IR::U64{ir.LoadShared(64, false, offset)};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            const IR::U32 lane32 = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(31));
            const IR::U32 half = ir.ShiftRightLogical(IR::U32{inst->Arg(1)}, ir.Imm32(5u));
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, lane32), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false,
                                      ir.IAdd(ir.BitwiseAnd(offset, ir.Imm32(~7u)),
                                              ir.ShiftLeftLogical(half, ir.Imm32(2u))))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::MaskedBitCount32) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            const IR::U32 thread_mask{
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(2).U1() ? 1u : 0u)};
            const IR::U32 masked_value{
                ir.BitCount(ir.BitwiseAnd(IR::U32{inst->Arg(0)}, thread_mask))};
            inst->ReplaceUsesWithAndRemove(ir.IAdd(masked_value, IR::U32{inst->Arg(1)}));
        }
    }
}

} // namespace Shader::Optimization
