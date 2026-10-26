// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_OP_STATS=1, op composition counters (docs/sotc/ESCENA-TEMPLO.md). Diagnostic only, no
// behaviour change: with the variable unset every hook is one load and one predictable branch
// (kOn is read once at static initialisation, like SotcStats::kHashStats), nothing is hashed,
// stored or printed.
//
// Draw thread (ExecuteBackOp, Rasterizer::Draw/DrawIndirect): ops by kind, draws by target kind
// (depth-only / color / both), by pass (bound depth + color targets), sun shadow cascade share,
// run lengths of consecutive draws sharing pipeline + state at three levels (see Run levels in
// sotc_opstats.cpp), draws equal to a draw of the previous two frames. Command processor
// (Liverpool::ProcessGraphics/ProcessCompute): PM4 packets and dwords per opcode. The draw thread
// prints the block every ~5 s from its own thread, at a frame boundary (no cross-thread reads
// except the CP packet counters, which are atomics).
#pragma once
#include <cstdint>
#include <cstdlib>

namespace SotcOps {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_OP_STATS");
    return v && v[0] != '\0' && v[0] != '0';
}();

/// Target kinds of a draw.
enum : uint32_t { KindDepthOnly, KindColor, KindColorDepth, KindNone, NumKinds };

/// What Rasterizer::Draw/DrawIndirect tell the counters about one issued draw. The hashes are
/// computed by the rasterizer (it owns the types they cover):
///  h_pipe  pipeline and RenderState
///  h_desc  h_pipe + image infos + buffer handles (no offsets) + push transform + vertex buffer
///          handles/strides + index type: what a multi-draw-indirect with a per-draw table could
///          share (offsets and arguments free)
///  h_exact h_desc + every buffer offset, push buffer offsets, vertex/index buffer addresses:
///          draws that differ only in their arguments (what VK_EXT_multi_draw can merge as is)
///  h_args  arguments (count, instances, first index/vertex, vertex offset, first instance)
///  h_stat  h_pipe + image infos + vertex/index addresses + arguments, without descriptor buffer
///          offsets: the "same draw in the previous frame" test when constant buffers move
struct DrawNote {
    bool indexed;
    bool gpu_args; ///< real indirect draw (arguments written by the GPU): no argument fields
    uint32_t kind;
    bool shadow; ///< depth-only into a square target >= 512 (sun shadow cascade slice)
    uint32_t slice, width, height, num_color;
    uint64_t pass_key, depth_addr;
    uint64_t h_pipe, h_desc, h_exact, h_args, h_stat;
    uint32_t count, instances, first_index, vertex_offset, first_instance;
    uint32_t n_buf, n_img; ///< descriptor buffers and images bound
    // Depth pre-pass analysis (docs/sotc/ESCENA-TEMPLO.md): raw register words of the draw state.
    uint32_t dctl, sctl, dsc, pctl, rctl, rovr, cctl; ///< depth/stencil/depth-shader/polygon/
                                                      ///< render control/override/color control
    uint32_t wmask, blend_en, mrt_mask; ///< 4 write-mask bits per RT, blend-enable bit per RT
    uint8_t num_samples, depth_samples;
    bool has_ps, ps_discard, ps_zexport, ps_other_export, bias, ps_uav;
    uint64_t vs_hash, ps_hash;
    uint64_t depth_read_addr, depth_write_addr;
    uint32_t depth_pitch, depth_height;
    uint64_t vp_hash, bias_hash; ///< viewport/scissor registers, polygon offset registers
    uint64_t g1, g2;             ///< geometry keys: IB+range / + vertex buffers + instance index
    uint64_t in_pos, in_mat, in_vp; ///< position inputs of the known position-only/G-buffer VS
    uint64_t in_posc;               ///< hash of the position bytes (first 16 KiB of the stream)
    uint32_t sref_f, sref_b;        ///< raw stencil reference/mask registers (front, back)
    uint64_t vsbuf[12];             ///< guest buffers of the vertex stage: hash of base, size, stride
    uint32_t n_vsbuf;
    uint64_t in_lvp, in_flat;       ///< view matrix at +0x80 of the VS view constants, VS flat buffer
};

/// Scratch the vertex/index binders fill for the draw being recorded (draw thread only), reset
/// by NoteDraw.
inline uint64_t vb_desc = 0, vb_exact = 0, ib_desc = 0, ib_exact = 0;

/// Command processor: one PM4 type 3 packet of `dwords` dwords (compute queue: compute=true).
void CpPacket(uint32_t opcode, uint32_t dwords, bool compute);
/// Draw thread: an op is about to execute (kind = Liverpool::BackOp::Kind as an int).
void NoteBackOp(uint32_t kind);
/// Command processor: a Nop packet with payload dword `payload` (first data dword, 0 if none).
void CpNop(uint32_t dwords, uint32_t payload);
/// Draw thread: a compute dispatch with this shader hash and this many thread groups.
void NoteDispatch(uint64_t hash, uint64_t groups);
/// Draw thread: a draw was recorded.
void NoteDraw(const DrawNote& n);
/// Draw thread: a draw was dropped after the op started (no pipeline, bind failure).
void NoteDrawDropped();
/// Draw thread: a resource of a draw/dispatch/copy covers [addr, addr+size). Recorded only when it
/// overlaps a depth target seen by a draw (what reads or writes the depth between the passes).
enum : uint32_t { TouchImage, TouchBuffer, TouchCopySrc, TouchCopyDst, TouchFill, TouchDma,
                  NumTouchKinds };
void NoteTouch(uint32_t what, uint64_t shader_hash, uint32_t sw_stage, bool written, uint64_t addr,
               uint64_t size);
/// Any thread: the guest CPU read (write=false, readback of GPU-modified memory) or wrote
/// (write=true, write fault) [addr, addr+size). Counted only when it overlaps a known depth target.
void NoteCpuAccess(bool write, uint64_t addr, uint64_t size);

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 32;
    return h;
}
inline uint64_t MixBytes(uint64_t h, const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t w;
        __builtin_memcpy(&w, p + i, 8);
        h = Mix(h, w);
    }
    uint64_t tail = 0;
    if (i < bytes) {
        __builtin_memcpy(&tail, p + i, bytes - i);
        h = Mix(h, tail);
    }
    return h;
}

} // namespace SotcOps
