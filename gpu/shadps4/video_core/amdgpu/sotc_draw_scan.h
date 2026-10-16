// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: DrawScan, the scanner of speculative draw preparation (docs/sotc/STUTTER-CLAUDE.md).
//
// Every top-level graphics command buffer is copied when it is submitted and numbered under the
// queue lock. A background thread walks the copies in submission order and records, for each
// draw, a running hash of every register and draw packet seen so far (the register history).
// The command processor computes the same hash with the same function while it executes, over
// every packet it processes (nested command buffers too), and perturbs it for writes the scanner
// cannot see (async compute writing graphics registers). Equal hashes at a draw mean the scanner
// saw exactly the packets the command processor applied, so the scanner's own register file,
// replayed with ApplyStatePacket (the same writes the command processor makes), is the command
// processor's. Anything the scanner misses makes the hashes differ: a draw is rejected, never
// wrongly accepted.
//
// SOTC_DRAW_SCAN=1 counts agreement; 2 also compares the scanner's register file with the
// command processor's at every 128th draw of a buffer (SOTC drawscan line). A DrawPreparer
// (SOTC_DRAW_PREP, renderer_vulkan) prepares each draw from the scanner's registers; the command
// processor takes the prepared record of a draw only when the hashes agree.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/regs.h"

namespace AmdGpu {

/// Opcodes that start a graphics draw.
constexpr bool IsDrawOpcode(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}

/// Opcodes whose packet contents feed the register state or the draw parameters.
constexpr bool IsStatePacket(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::SetBase:
    case PM4ItOpcode::EventWrite:
        return true;
    default:
        return IsDrawOpcode(opcode);
    }
}

inline u64 HashWords(u64 hash, const u32* words, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        hash = (hash ^ words[i]) * 0x9E3779B97F4A7C15ULL;
        hash ^= hash >> 29;
    }
    return hash;
}

/// Adds a type-3 packet (header and body) to the register history when it is a state packet.
inline void HashStatePacket(const PM4Header* header, u64& hash) {
    if (IsStatePacket(header->type3.opcode)) {
        hash = HashWords(hash, &header->raw, header->type3.NumWords() + 1);
    }
}

/// The register writes Liverpool::ProcessGraphics makes for a type-3 packet (graphics queue),
/// without its other effects. Keep the two in step: SOTC_DRAW_SCAN=2 compares the results.
/// False for a write past the register file (the scanner stops trusting its copy).
inline bool ApplyStatePacket(Regs& regs, const PM4Header* header) {
    const u32 count = header->type3.NumWords();
    const auto set_regs = [&](u32 reg_addr) {
        const u32 words = count - 1;
        if (count == 0 || reg_addr > Regs::NumRegs || words > Regs::NumRegs - reg_addr) {
            return false;
        }
        std::memcpy(&regs.reg_array[reg_addr], header + 2, words * sizeof(u32));
        return true;
    };
    switch (header->type3.opcode) {
    case PM4ItOpcode::ClearState:
        regs.SetDefaults();
        return true;
    case PM4ItOpcode::SetConfigReg:
        return set_regs(Regs::ConfigRegWordOffset +
                        reinterpret_cast<const PM4CmdSetData*>(header)->reg_offset);
    case PM4ItOpcode::SetContextReg:
        return set_regs(Regs::ContextRegWordOffset +
                        reinterpret_cast<const PM4CmdSetData*>(header)->reg_offset);
    case PM4ItOpcode::SetShReg: {
        const u32 offset = reinterpret_cast<const PM4CmdSetData*>(header)->reg_offset;
        if (offset >= 0x200 && offset <= 0x200 + sizeof(ComputeProgram) / 4) {
            return true; // the graphics queue's compute state, not in the register file
        }
        return set_regs(Regs::ShRegWordOffset + offset);
    }
    case PM4ItOpcode::SetUconfigReg:
        return set_regs(Regs::UconfigRegWordOffset +
                        reinterpret_cast<const PM4CmdSetData*>(header)->reg_offset);
    case PM4ItOpcode::IndexType:
        regs.index_buffer_type.raw = reinterpret_cast<const PM4CmdDrawIndexType*>(header)->raw;
        return true;
    case PM4ItOpcode::DrawIndex2: {
        const auto* draw = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
        regs.max_index_size = draw->max_size;
        regs.index_base_address.base_addr_lo = draw->index_base_lo;
        regs.index_base_address.base_addr_hi = draw->index_base_hi;
        regs.num_indices = draw->index_count;
        regs.draw_initiator = draw->draw_initiator;
        return true;
    }
    case PM4ItOpcode::DrawIndexOffset2: {
        const auto* draw = reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
        regs.max_index_size = draw->max_size;
        regs.num_indices = draw->index_count;
        regs.draw_initiator = draw->draw_initiator;
        return true;
    }
    case PM4ItOpcode::DrawIndexAuto: {
        const auto* draw = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
        regs.num_indices = draw->index_count;
        regs.draw_initiator = draw->draw_initiator;
        return true;
    }
    case PM4ItOpcode::NumInstances:
        regs.num_instances.num_instances =
            reinterpret_cast<const PM4CmdDrawNumInstances*>(header)->num_instances;
        return true;
    case PM4ItOpcode::IndexBase: {
        const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
        regs.index_base_address.base_addr_lo = index_base->addr_lo;
        regs.index_base_address.base_addr_hi = index_base->addr_hi;
        return true;
    }
    case PM4ItOpcode::IndexBufferSize:
        regs.num_indices = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header)->num_indices;
        return true;
    case PM4ItOpcode::EventWrite: {
        const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
        if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
            regs.cp_strmout_cntl.offset_update_done = 1;
        }
        return true;
    }
    default:
        return true;
    }
}

/// The packet fields of a draw that pipeline selection reads besides registers.
struct ScanDrawParams {
    u16 vertex_sgpr_offset{};
    u16 instance_sgpr_offset{};
};

/// Selection read from an indirect draw packet; zero for direct draws (as Rasterizer::Draw).
inline ScanDrawParams DrawParamsOf(const PM4Header* header) {
    ScanDrawParams params{};
    const auto take = [&](const auto* draw) {
        // A packet shorter than its structure (garbage) gives zeros; the command processor's
        // own parameters then differ and the proposal is refused.
        if ((header->type3.NumWords() + 1) * sizeof(u32) < sizeof(*draw)) {
            return;
        }
        params.vertex_sgpr_offset = static_cast<u16>(draw->base_vtx_loc.Value());
        params.instance_sgpr_offset = static_cast<u16>(draw->start_inst_loc.Value());
    };
    switch (header->type3.opcode) {
    case PM4ItOpcode::DrawIndirect:
        take(reinterpret_cast<const PM4CmdDrawIndirect*>(header));
        break;
    case PM4ItOpcode::DrawIndirectMulti:
        take(reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header));
        break;
    case PM4ItOpcode::DrawIndexIndirect:
        take(reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header));
        break;
    case PM4ItOpcode::DrawIndexIndirectMulti:
        take(reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header));
        break;
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        take(reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header));
        break;
    default:
        break;
    }
    return params;
}

/// Prepares draws on the scanner thread (implemented by the pipeline cache, SOTC_DRAW_PREP).
class DrawPreparer {
public:
    virtual ~DrawPreparer() = default;
    /// Scanner thread: storage for the records of one command buffer's draws.
    virtual std::shared_ptr<void> NewBlock(u32 num_draws) = 0;
    /// Scanner thread: prepares record `index` from the register state its draw selects with.
    virtual void Prepare(void* block, u32 index, const Regs& regs, ScanDrawParams params) = 0;
    /// Command processor: record `index`, published before the scanner moved past it.
    virtual const void* Record(const void* block, u32 index) const = 0;
};

class DrawScan {
public:
    /// SOTC_DRAW_SCAN (1, 2 = verify registers) or SOTC_DRAW_PREP.
    static bool Enabled();
    static u32 VerifyMode();
    /// SOTC_DRAW_PREP: 0 off, 1 (default) use prepared selections, 2 verify them.
    static u32 PrepMode();

    explicit DrawScan(const Regs& initial_regs);
    ~DrawScan();

    /// Before the first submission.
    void SetPreparer(DrawPreparer* preparer_) {
        preparer = preparer_;
    }
    /// Ends the scanner thread (before the preparer goes away); draws then get no records.
    void Stop();

    struct Submission {
        static constexpr u32 VerifyEvery = 128;

        u64 seq{};
        std::vector<u32> commands;          ///< private copy, read by the scanner only
        u32 num_draws{};                    ///< top-level draws (set before any is published)
        std::unique_ptr<u64[]> draw_hashes; ///< register history at each top-level draw
        std::unique_ptr<std::unique_ptr<Regs>[]> verify_regs; ///< SOTC_DRAW_SCAN=2
        std::shared_ptr<void> prepared;     ///< DrawPreparer block
        std::atomic<u32> num_ready{0};      ///< draws [0, num_ready) are published
        std::atomic<bool> scanned{false};   ///< the whole buffer was walked
        ~Submission();
    };

    /// Guest submit thread, outside the queue lock: copies the command buffer.
    std::shared_ptr<Submission> Build(std::span<const u32> dcb);
    /// Guest submit thread, under the queue lock: numbers it in queue order.
    u64 Enqueue(std::shared_ptr<Submission> submission);

    /// Command processor: brackets one top-level command buffer.
    void Begin(u64 seq);
    void End();
    /// Command processor: at each top-level draw, after hashing its packet and before applying
    /// it. Returns the prepared record when the scanner's history is the same.
    const void* OnDraw(u64 hash, const Regs& regs);

private:
    void ScanLoop(std::stop_token stop);
    void Scan(Submission& submission);

    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::shared_ptr<Submission>> pending;  ///< not scanned yet (scanner input)
    std::deque<std::shared_ptr<Submission>> recent;   ///< scanned or not, until the CP is done
    std::vector<std::shared_ptr<Submission>> retired; ///< done by the CP, freed by the scanner
    u64 next_seq{};
    u64 scan_hash{};
    std::unique_ptr<Regs> scan_regs; ///< scanner thread only
    bool scan_broken{};              ///< a packet the scanner could not apply: regs untrusted
    DrawPreparer* preparer{};
    std::shared_ptr<Submission> current; ///< command processor's submission
    u32 current_draw{};
    std::jthread thread;
};

} // namespace AmdGpu
