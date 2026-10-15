// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: which register words graphics pipeline selection may read (thread-plan 1D).
#pragma once

#include <bitset>
#include <memory>
#include "video_core/amdgpu/regs.h"

namespace SotcSel {

/// Register words pipeline selection never reads (BuildRuntimeInfo, RefreshGraphicsKey,
/// RefreshGraphicsStages, GetProgram): writes to them leave Liverpool::sel_regs_seq unchanged.
/// Written as exclusions, so a word missing here only costs fast-path hits, never correctness.
/// The user data registers are compared per stage through the flattened buffer instead.
inline const std::bitset<AmdGpu::Regs::NumRegs>& IgnoredWords() {
    static const auto words = [] {
        auto set = std::make_unique<std::bitset<AmdGpu::Regs::NumRegs>>();
        const auto regs = std::make_unique<AmdGpu::Regs>();
        const auto* base = reinterpret_cast<const u8*>(regs->reg_array.data());
        const auto mark = [&](const auto& field) {
            const size_t first = (reinterpret_cast<const u8*>(&field) - base) / sizeof(u32);
            const size_t count = (sizeof(field) + sizeof(u32) - 1) / sizeof(u32);
            for (size_t w = first; w < first + count && w < AmdGpu::Regs::NumRegs; ++w) {
                set->set(w);
            }
        };
        mark(regs->ps_program.user_data);
        mark(regs->vs_program.user_data);
        mark(regs->gs_program.user_data);
        mark(regs->es_program.user_data);
        mark(regs->hs_program.user_data);
        mark(regs->ls_program.user_data);
        mark(regs->screen_scissor);
        mark(regs->window_scissor);
        mark(regs->generic_scissor);
        mark(regs->viewport_scissors);
        mark(regs->viewports);
        mark(regs->blend_constants);
        mark(regs->stencil_ref_front);
        mark(regs->stencil_ref_back);
        mark(regs->depth_bounds_min);
        mark(regs->depth_bounds_max);
        return std::move(set);
    }();
    return *words;
}

/// IgnoredWords plus the draw-packet fields (written by draw packets without a sequence bump,
/// never read by selection): the words SOTC_SEL_FAST=2 skips when it compares a whole register
/// file against the one a reused selection saw.
inline const std::bitset<AmdGpu::Regs::NumRegs>& VerifyIgnoredWords() {
    static const auto words = [] {
        auto set = std::make_unique<std::bitset<AmdGpu::Regs::NumRegs>>(IgnoredWords());
        const auto regs = std::make_unique<AmdGpu::Regs>();
        const auto* base = reinterpret_cast<const u8*>(regs->reg_array.data());
        const auto mark = [&](const auto& field) {
            const size_t first = (reinterpret_cast<const u8*>(&field) - base) / sizeof(u32);
            const size_t count = (sizeof(field) + sizeof(u32) - 1) / sizeof(u32);
            for (size_t w = first; w < first + count && w < AmdGpu::Regs::NumRegs; ++w) {
                set->set(w);
            }
        };
        mark(regs->index_buffer_type);
        mark(regs->max_index_size);
        mark(regs->index_base_address);
        mark(regs->num_indices);
        mark(regs->draw_initiator);
        mark(regs->num_instances);
        return std::move(set);
    }();
    return *words;
}

/// True when a write of `count` words at `word` may change what selection reads.
inline bool Relevant(u32 word, u32 count) {
    const auto& ignored = IgnoredWords();
    for (u32 w = word, end = word + count; w < end; ++w) {
        if (w >= AmdGpu::Regs::NumRegs || !ignored.test(w)) {
            return true;
        }
    }
    return false;
}

} // namespace SotcSel
