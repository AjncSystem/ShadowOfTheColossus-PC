// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_HLE_SHADOW_CULL=1 (sotc_shadow_cull.h).
//
// The shader, as its SPIR-V translation (0x4defcf7f_0.spv) and eboot 0x19c1a0 define it. One
// thread per section, 32 threads per group, ceil(sections / 32) groups, no texture.
//  Buffers (guest buffers in binding order): 0 constants (0x160 bytes the CPU memcpy's in),
//  1 sections (64-byte records), 2 objects (64-byte records), 3 object fades (32-byte records),
//  4 Shadow bitset, 5 ShadowFadeOut bitset (both 4-byte elements, one word per group).
//  Constants (dwords): [0] section count, [1] section mask A, [2] plane count, [3] fade slot
//  (cascade + 1, 0 for a spot light), [4 + 4k .. 7 + 4k] plane k (x, y, z, w), [70] section mask
//  B (the cascade or spot bit). The identity matrix at 0x120 is not read by this program.
//  Section s (dwords): rows r0 = [0..3], r1 = [4..7], r2 = [8..11]; [12] object, [13] object
//  generation, [14] mask A, [15] mask B.
//  Candidate: s < count, (A & sec[14]) != 0, (B & sec[15]) != 0, sec[13] == objects[sec[12]][1].
//  Geometry: per row i, the segment ends P_i = r_i.w - r_i.x and Q_i = r_i.x + r_i.w (homogeneous
//  weights wP = wQ = 1), swept by the box of half axes (r_i.y) and (r_i.z). Per plane:
//  R = |n.(r.y)| + |n.(r.z)|, d(X) = n.X + R + w.wX. Both ends outside (d < 0): culled (the
//  visibility flag only goes down). One end inside: the segment is clipped, the outside end
//  moving to the crossing point at t = dQ / n.(Q - P) from Q. Both inside: the ends swap (the
//  same formulas with t = 0).
//  A visible candidate reads f = fades[object] dword [slot]: Shadow bit when f > 0.99, the
//  ShadowFadeOut bit when 0.01 < f <= 0.99 (none when f <= 0.01 or NaN).
//  Group g stores word g of each bitset (bit l = section 32g + l) when g < that buffer's
//  num_records (the shader compares with the V#'s dword 2). Nothing else is written: no
//  indirect arguments, counters or atomics.
//
// Exactness. The arithmetic is replayed in single precision in the shader's own operation order,
// without contraction (the SPIR-V marks every product and sum NoContraction; this file is built
// with -ffp-contract=off). On a conforming device (correctly rounded add, sub and mul) every
// value is then the GPU's, except two: the reciprocal 1/den (OpFDiv: 2.5 ulp in Vulkan, v_rcp_f32
// on the PS4) and denormals (the SPIR-V declares no denorm mode, so a device may flush them). The
// reciprocal only matters on a plane the segment straddles, where it moves the clipped end along
// the segment and so changes the ends later planes see. So no decision is trusted to bit
// equality: each sign the shader takes (dQ >= 0, dP >= 0) must clear an error bound covering a
// device whose every operation is off by up to 2 ulp (kRel, our own rounding included), whose
// reciprocal is off by up to 8 ulp (kRcp), which flushes denormals (kAbs), and whose clipped ends drifted from ours by what
// the earlier clips allow (dev, propagated through every clip). When every decision of a
// section clears its bound, the GPU took the same decisions and its bit is ours.
// Conservativeness. When a decision does not clear its bound, the section switches to a
// superset test: the GPU's later segments lie within the drift bound of the current one (a clip
// keeps one end and moves the other along the segment) and d is affine, so the section is culled
// only when both current ends are outside a later plane by more than the bound; otherwise it is
// kept. A kept section gets the bit its fade selects (the fade test is an exact comparison of
// the same bytes), so the bitsets are a superset of the GPU's: at worst a shadow caster outside
// the light frustum is drawn, never is a caster the GPU draws dropped. Non-finite values never
// clear a bound: kept.
// Inputs. They are read from guest memory, which must be authoritative: any input range with
// GPU-written pages runs the shader on the GPU (this also covers inputs written by the GPU
// earlier in the same frame, which the CPU cannot see without waiting). So do accesses outside
// a buffer (robust-access results are the device's business), unmapped ranges and unknown
// layouts.
// Ordering. This runs where the dispatch would be recorded (the draw thread, in stream order),
// so the shadow group's end-of-pipe label, written later on the same thread, is written after
// the bitsets: a guest that waited for the label reads them. The outputs go through
// BufferCache::TryWriteThroughFromCpu: guest memory now, the resident GPU copy by a copy at this
// point of the command stream (later GPU readers see it), or the range marked CPU-written when
// not resident.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_shadow_cull.h"
#include "video_core/sotc_stats.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace SotcShadowCull {
namespace {

constexpr double kRel = 0x1p-22;  ///< per-operation error of the device (2 ulp at worst)
constexpr double kRcp = 0x1p-20;  ///< reciprocal error of the device (8 ulp at worst)
constexpr double kAbs = 0x1p-100; ///< denormal flushing, per unit of operand magnitude (below)
constexpr double kChain = 24.0;   ///< operations behind one value (19) x (device + our rounding)
constexpr u32 kMaxPlanes = 21;    ///< planes the 0x160-byte constants can hold (dwords 4..87)

float F(u32 v) {
    return std::bit_cast<float>(v);
}

double A(float v) {
    return std::fabs(static_cast<double>(v));
}

struct Plane {
    float x, y, z, w;
};

enum class Result { Culled, Visible, Kept };

/// The plane distances of both ends and the clip denominator, in the shader's operation order
/// (temporaries named after the SPIR-V ids), with the magnitudes their errors scale with.
struct PlaneEval {
    float dQ, dP, den;
    double lQ, lP;
};

/// The magnitudes the error of the clip denominator scales with (only needed on a straddle).
double DenScale(const Plane& pl, const float* p, const float* q, float wp, float wq) {
    return A(pl.x) * (A(p[0]) + A(q[0])) + A(pl.y) * (A(p[1]) + A(q[1])) +
           A(pl.z) * (A(p[2]) + A(q[2])) + A(pl.w) * (A(wp) + A(wq));
}

PlaneEval EvalPlane(const Plane& pl, const float* ax, const float* bx, const float* p,
                    const float* q, float wp, float wq) {
    PlaneEval e;
    const float t309 = pl.x * ax[0];
    const float t312 = pl.y * ax[1];
    const float t313 = t312 + t309;
    const float t315 = pl.x * bx[0];
    const float t318 = pl.z * ax[2];
    const float t319 = t318 + t313;
    const float t321 = pl.y * bx[1];
    const float t322 = t321 + t315;
    const float t324 = pl.z * bx[2];
    const float t325 = t324 + t322;
    const float r = std::fabs(t319) + std::fabs(t325);
    const float t330 = q[0] - p[0];
    const float t331 = pl.x * t330;
    const float t288 = q[1] - p[1];
    const float t333 = pl.y * t288;
    const float t334 = t333 + t331;
    const float t332 = q[2] - p[2];
    const float t335 = pl.z * t332;
    const float t336 = t335 + t334;
    const float t339 = wq - wp;
    const float t341 = pl.w * t339;
    e.den = t341 + t336;
    const float t343 = pl.x * p[0];
    const float t344 = t343 + r;
    const float t345 = pl.x * q[0];
    const float t346 = t345 + r;
    const float t347 = pl.y * q[1];
    const float t348 = t347 + t346;
    const float t349 = pl.y * p[1];
    const float t350 = t349 + t344;
    const float t351 = pl.z * q[2];
    const float t352 = t351 + t348;
    const float t353 = pl.z * p[2];
    const float t354 = t353 + t350;
    const float t355 = pl.w * wq;
    e.dQ = t355 + t352;
    const float t357 = pl.w * wp;
    e.dP = t357 + t354;
    // Every intermediate of a sum is bounded by the sum of the magnitudes of its terms.
    const double lr = A(t309) + A(t312) + A(t315) + A(t318) + A(t321) + A(t324);
    e.lQ = lr + A(t345) + A(t347) + A(t351) + A(t355);
    e.lP = lr + A(t343) + A(t349) + A(t353) + A(t357);
    return e;
}

double MaxCoord(const float* p, const float* q) {
    double m = 0.0;
    for (int i = 0; i < 3; ++i) {
        m = std::max({m, A(p[i]), A(q[i])});
    }
    return m;
}

double MaxAxis(const float* ax, const float* bx) {
    return std::max({A(ax[0]), A(ax[1]), A(ax[2]), A(bx[0]), A(bx[1]), A(bx[2])});
}

/// Absolute error of one plane evaluation from denormal flushing. A device that flushes also
/// flushes denormal operands (GCN v_mul / v_mad with FTZ): an operand moving by < 2^-126 moves a
/// product by < 2^-126 times the other operand, and a result moving by < 2^-126 moves the sums.
/// kAbs (2^-100) times the largest operand magnitudes bounds all of them with a wide margin.
double FlushError(const Plane& pl, double m, double axes) {
    return kAbs * (1.0 + A(pl.x) + A(pl.y) + A(pl.z) + A(pl.w) + m + axes);
}

/// After an undecidable plane `k0`: the GPU's segments at the later planes lie within `dev0` plus
/// the rounding of their clips of [p, q]. Culled only when both ends are outside a later plane
/// by more than that bound.
Result Superset(const Plane* planes, u32 k0, u32 count, const float* ax, const float* bx,
                const float* p, const float* q, float wp, float wq, double dev0) {
    const double m = MaxCoord(p, q) + dev0;
    const double axes = MaxAxis(ax, bx);
    if (!std::isfinite(m) || !std::isfinite(dev0) || !std::isfinite(axes)) {
        return Result::Kept;
    }
    for (u32 j = k0 + 1; j < count; ++j) {
        const Plane& pl = planes[j];
        const PlaneEval e = EvalPlane(pl, ax, bx, p, q, wp, wq);
        const double n1 = A(pl.x) + A(pl.y) + A(pl.z);
        // Each GPU clip since k0 rounds (8 kRel m) and may flush (kAbs (1 + m), s and P - Q).
        const double dev = dev0 + double(j - k0) * (8.0 * kRel * m + kAbs * (1.0 + 2.0 * m));
        const double tol = kChain * kRel * (2.0 * std::max(e.lQ, e.lP) + n1 * dev) + n1 * dev +
                           kChain * FlushError(pl, m + dev, axes);
        if (std::isfinite(tol) && double(e.dQ) < -tol && double(e.dP) < -tol) {
            return Result::Culled;
        }
    }
    return Result::Kept;
}

/// The plane loop of one candidate section: Culled / Visible exactly as the GPU decides, or
/// Kept when the GPU's decision could not be established (it may have culled).
Result EvaluateSection(const u32* sec, const Plane* planes, u32 count) {
    float p[3], q[3], ax[3], bx[3];
    double dev = 0.0;
    for (int i = 0; i < 3; ++i) {
        const float x = F(sec[4 * i + 0]);
        const float y = F(sec[4 * i + 1]);
        const float z = F(sec[4 * i + 2]);
        const float w = F(sec[4 * i + 3]);
        p[i] = w - x; // %259 / %262 / %265
        q[i] = x + w; // %266 / %267 / %268
        ax[i] = y;
        bx[i] = z;
        dev = std::max(dev, 2.0 * kRel * (A(x) + A(w)) + kAbs * (1.0 + A(x) + A(w)));
    }
    const double axes = MaxAxis(ax, bx);
    // Bounds on |GPU end - our end|, per coordinate.
    double dev_p = dev;
    double dev_q = dev;
    float wp = 1.0f;
    float wq = 1.0f;
    for (u32 k = 0; k < count; ++k) {
        const Plane& pl = planes[k];
        const PlaneEval e = EvalPlane(pl, ax, bx, p, q, wp, wq);
        const double n1 = A(pl.x) + A(pl.y) + A(pl.z);
        const double m = MaxCoord(p, q) + std::max(dev_p, dev_q);
        const double flush = kChain * FlushError(pl, m, axes);
        // The GPU evaluates d at its own ends: its rounding scales with its magnitudes, which
        // exceed ours by at most n1 dev (the second-order term).
        const double tol_q = kChain * kRel * (e.lQ + n1 * dev_q) + n1 * dev_q + flush;
        const double tol_p = kChain * kRel * (e.lP + n1 * dev_p) + n1 * dev_p + flush;
        const double dq = e.dQ;
        const double dp = e.dP;
        // NaN compares false: never inside nor outside, so undecidable.
        const bool in_q = dq > tol_q;
        const bool out_q = dq < -tol_q;
        const bool in_p = dp > tol_p;
        const bool out_p = dp < -tol_p;
        if (out_q && out_p) {
            return Result::Culled;
        }
        if (!(in_q || out_q) || !(in_p || out_p) || !std::isfinite(tol_q + tol_p)) {
            return Superset(planes, k, count, ax, bx, p, q, wp, wq, std::max(dev_p, dev_q));
        }
        const bool straddle = in_q != in_p; // %372
        float s = 0.0f;                     // %374
        double next_dev_p = dev_q + kAbs;   // both inside: P' = Q (flushed if denormal)
        if (straddle) {
            const double b =
                kChain * kRel * (DenScale(pl, p, q, wp, wq) + 2.0 * n1 * (dev_p + dev_q)) +
                n1 * (dev_p + dev_q) + flush;
            if (!std::isfinite(e.den) || !(std::fabs(double(e.den)) > 2.0 * b)) {
                return Superset(planes, k, count, ax, bx, p, q, wp, wq, std::max(dev_p, dev_q));
            }
            const float rcp = 1.0f / e.den; // %362
            const float t = rcp * e.dQ;     // %366
            // NMax / NMin: a NaN operand yields the other one.
            const float lo = std::isnan(t) ? 0.0f : std::max(t, 0.0f); // %367
            s = 1.0f * std::min(lo, 1.0f);                              // %368, %374
            // |t_gpu - t| for dQ off by tol_q, den off by b, and the device's reciprocal.
            const double at = A(t);
            // A flushed (denormal) dQ moves t by up to |rcp| 2^-126.
            const double dt = 1.001 * (tol_q + at * b) / (std::fabs(double(e.den)) - b) +
                              at * (kRcp + 2.0 * kRel) + A(rcp) * 0x1p-126 + kAbs;
            if (!std::isfinite(at) || !(dt < 1.0)) {
                return Superset(planes, k, count, ax, bx, p, q, wp, wq, std::max(dev_p, dev_q));
            }
            double diff = 0.0;
            for (int i = 0; i < 3; ++i) {
                diff = std::max(diff, std::fabs(double(p[i]) - double(q[i])));
            }
            const double s_max = std::min(1.0, double(s) + dt);
            // Rounding of the GPU's three operations scales with its magnitudes (ours + dev);
            // flushing s or P - Q moves the product by < 2^-126 (1 + diff).
            next_dev_p = (dev_p + dev_q) * s_max + diff * dt + dev_q + 8.0 * kRel * m +
                         kAbs * (1.0 + diff);
        }
        // %379: Q is kept when the segment straddles and Q is the inside end (dQ >= dP).
        const bool keep_q = straddle && dq >= dp;
        const float nwp = (wp - wq) * s + wq; // %375..%377
        const float nwq = keep_q ? wq : wp;   // %384
        for (int i = 0; i < 3; ++i) {
            const float np = (p[i] - q[i]) * s + q[i]; // %385..%387, %389..%392, %393..%395
            const float nq = keep_q ? q[i] : p[i];     // %388, %396, %397
            p[i] = np;
            q[i] = nq;
        }
        wp = nwp;
        wq = nwq;
        const double next_dev_q = keep_q ? dev_q : dev_p;
        dev_p = next_dev_p;
        dev_q = next_dev_q;
        // %390: (in_q || in_p) holds here, the visibility flag stays up.
    }
    return Result::Visible;
}

struct Range {
    VAddr base;
    u64 size;
    bool Overlaps(const Range& o) const {
        return size && o.size && base < o.base + o.size && o.base < base + size;
    }
};

bool Fallback(SotcStats::ShadowCullFallback reason) {
    SotcStats::shadow_cull_fallbacks[reason].fetch_add(1, std::memory_order_relaxed);
    return false;
}

} // namespace

bool Execute(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
             Vulkan::Rasterizer& rasterizer) {
    using namespace SotcStats;
    struct Timer {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~Timer() {
            shadow_cull_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count(),
                                     std::memory_order_relaxed);
        }
    } const timer;

    // SOTC_RB_PREV serves guest reads from an older version and releases those pages (pf_owed):
    // guest memory may then be older than the GPU copy while no page is GPU-modified, so the
    // inputs cannot be trusted. The other readback modes keep a page GPU-modified until the
    // bytes applied to it are the GPU's current ones (SOTC_RB_EXACT owed ranges count as
    // GPU-modified; SOTC_RB_GROUP and label downloads are trimmed of any page changed after
    // their copy, see BufferCache::RbgAtFault; prefetches are applied only when clean).
    static const bool rb_prev = [] {
        const char* v = std::getenv("SOTC_RB_PREV");
        return v && v[0] == '1';
    }();
    if (rb_prev) {
        return Fallback(ShadowCullRbPrev);
    }

    // The six guest buffers, in binding order (the SPIR-V bindings 0-5); only the last two
    // written. The two outputs must be the V#s at flatbuf dwords 16 and 20: the program's store
    // guards read their num_records from flatbuf dwords 18 and 22.
    std::array<AmdGpu::Buffer, 6> v{};
    u32 n = 0;
    for (const auto& desc : info.buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        if (n == v.size() || desc.is_written != (n >= 4)) {
            return Fallback(ShadowCullLayout);
        }
        if (n >= 4) {
            const auto& fetch = desc.sharp_fetch;
            if (fetch.summary != std::remove_cvref_t<decltype(fetch)>::Summary::SingleLoad ||
                fetch.offsets[0] != (n == 4 ? 16 : 20)) {
                return Fallback(ShadowCullLayout);
            }
        }
        v[n++] = desc.GetSharp(info);
    }
    if (n != v.size()) {
        return Fallback(ShadowCullLayout);
    }
    const auto& flat = info.FlatBuf();
    if (flat.size() < 24) {
        return Fallback(ShadowCullLayout);
    }
    const u32 guard_shadow = flat[18]; // %18 > group: the Shadow store
    const u32 guard_fade = flat[22];   // %462 > group: the ShadowFadeOut store
    const AmdGpu::Buffer& cb = v[0];
    const AmdGpu::Buffer& sections = v[1];
    const AmdGpu::Buffer& objects = v[2];
    const AmdGpu::Buffer& fades = v[3];
    const AmdGpu::Buffer& out_shadow = v[4];
    const AmdGpu::Buffer& out_fade = v[5];
    // The strides the program's addressing was compiled for; plain (not swizzled) buffers.
    if (sections.GetStride() != 64 || objects.GetStride() != 64 || fades.GetStride() != 32 ||
        out_shadow.GetStride() != 4 || out_fade.GetStride() != 4) {
        return Fallback(ShadowCullLayout);
    }
    for (const auto& b : v) {
        if (b.swizzle_enable || b.add_tid_enable || (b.base_address & 3) != 0) {
            return Fallback(ShadowCullLayout);
        }
    }
    // The plain dispatch: full groups of 32 (no partial last group), no ordered append, group
    // counts (not thread counts) in dim_x. DISPATCH_INITIATOR bits 1 PARTIAL_TG_EN,
    // 3 ORDERED_APPEND_ENBL, 4 ORDERED_APPEND_MODE, 5 USE_THREAD_DIMENSIONS.
    const auto one = [](u32 x) { return x <= 1; };
    constexpr u32 kInitiatorUnusual = (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5);
    if (cs_program.dim_y != 1 || cs_program.dim_z != 1 || cs_program.start_x != 0 ||
        cs_program.start_y != 0 || cs_program.start_z != 0 ||
        cs_program.num_thread_x.full != 32 || !one(cs_program.num_thread_y.full) ||
        !one(cs_program.num_thread_z.full) || cs_program.num_thread_x.partial != 0 ||
        cs_program.num_thread_y.partial != 0 || cs_program.num_thread_z.partial != 0 ||
        (cs_program.dispatch_initiator & kInitiatorUnusual) != 0) {
        return Fallback(ShadowCullLayout);
    }

    auto& buffer_cache = rasterizer.GetBufferCache();
    const auto readable = [&](const Range& r) {
        return r.size == 0 || rasterizer.IsMapped(r.base, r.size);
    };
    const Range cb_range{cb.base_address, cb.GetSize()};
    if (cb_range.size < 71 * 4 || !readable(cb_range)) {
        return Fallback(ShadowCullBounds);
    }
    if (buffer_cache.IsRegionGpuModified(cb_range.base, cb_range.size)) {
        return Fallback(ShadowCullInputGpu);
    }
    const u32* constants = std::bit_cast<const u32*>(cb_range.base);
    const u32 count = constants[0];
    const u32 mask_a = constants[1];
    const u32 num_planes = constants[2];
    const u32 slot = constants[3];
    const u32 mask_b = constants[70];
    if (num_planes > kMaxPlanes || 4 + 4 * u64{num_planes} > cb_range.size / 4) {
        return Fallback(ShadowCullBounds);
    }
    const u64 groups = cs_program.dim_x;
    const u64 evaluated = std::min<u64>(count, groups * 32);
    if (evaluated > (u64{1} << 26)) { // the program's u32 index arithmetic would wrap
        return Fallback(ShadowCullLayout);
    }
    const Range sec_range{sections.base_address, evaluated * 64};
    const Range obj_range{objects.base_address, objects.GetSize()};
    const Range fade_range{fades.base_address, fades.GetSize()};
    if (sec_range.size > sections.GetSize()) {
        return Fallback(ShadowCullBounds);
    }
    if (!readable(sec_range) || !readable(obj_range) || !readable(fade_range)) {
        return Fallback(ShadowCullBounds);
    }
    if ((sec_range.size && buffer_cache.IsRegionGpuModified(sec_range.base, sec_range.size)) ||
        (obj_range.size && buffer_cache.IsRegionGpuModified(obj_range.base, obj_range.size)) ||
        (fade_range.size && buffer_cache.IsRegionGpuModified(fade_range.base, fade_range.size))) {
        return Fallback(ShadowCullInputGpu);
    }
    // Group g stores its word when g < the guard (the V#'s num_records, read from the flatbuf).
    const u64 words_shadow = std::min<u64>(groups, guard_shadow);
    const u64 words_fade = std::min<u64>(groups, guard_fade);
    // Words past the sections hold zeros only; a bound before any allocation (2^21 words cover
    // the 2^26 sections allowed above).
    if (words_shadow > (u64{1} << 21) || words_fade > (u64{1} << 21)) {
        return Fallback(ShadowCullLayout);
    }
    const Range shadow_range{out_shadow.base_address, words_shadow * 4};
    const Range fade_out_range{out_fade.base_address, words_fade * 4};
    if (!readable(shadow_range) || !readable(fade_out_range)) {
        return Fallback(ShadowCullBounds);
    }
    for (const Range& in : {cb_range, sec_range, obj_range, fade_range}) {
        if (shadow_range.Overlaps(in) || fade_out_range.Overlaps(in)) {
            return Fallback(ShadowCullLayout);
        }
    }
    if (shadow_range.Overlaps(fade_out_range)) {
        return Fallback(ShadowCullLayout);
    }

    std::array<Plane, kMaxPlanes> planes{};
    for (u32 k = 0; k < num_planes; ++k) {
        const u32* d = constants + 4 + 4 * k;
        planes[k] = {F(d[0]), F(d[1]), F(d[2]), F(d[3])};
    }
    static thread_local std::vector<u32> bits_shadow;
    static thread_local std::vector<u32> bits_fade;
    // Sized to what is stored and to what the sections can set; bits past the stored words are
    // computed but not written, as the GPU's ballots of those groups are not stored.
    const u64 words = std::max({words_shadow, words_fade, (evaluated + 31) / 32});
    bits_shadow.assign(words, 0u);
    bits_fade.assign(words, 0u);
    const u32* secs = std::bit_cast<const u32*>(sec_range.base);
    const u32* objs = std::bit_cast<const u32*>(obj_range.base);
    const u32* fade_data = std::bit_cast<const u32*>(fade_range.base);
    const u64 obj_dwords = obj_range.size / 4;
    const u64 fade_dwords = fade_range.size / 4;
    u64 kept = 0;
    for (u64 s = 0; s < evaluated; ++s) {
        const u32* sec = secs + s * 16;
        if ((mask_a & sec[14]) == 0 || (mask_b & sec[15]) == 0) { // %165, %178
            continue;
        }
        const u32 object = sec[12];
        const u32 generation_index = object * 16u + 1u; // u32 arithmetic, as the program's
        if (generation_index >= obj_dwords) {
            return Fallback(ShadowCullBounds);
        }
        if (sec[13] != objs[generation_index]) { // %196
            continue;
        }
        const Result result = EvaluateSection(sec, planes.data(), num_planes);
        if (result == Result::Culled) {
            continue;
        }
        const u32 fade_index = (object * 32u + (slot << 2u)) >> 2u;
        if (fade_index >= fade_dwords) {
            return Fallback(ShadowCullBounds);
        }
        const float f = F(fade_data[fade_index]);
        const bool full = 0.99f < f && 0.01f < f;     // %421 && %423
        const bool fading = 0.01f < f && !(0.99f < f); // %423 && !%421
        if (!full && !fading) {
            continue;
        }
        (full ? bits_shadow : bits_fade)[s >> 5] |= 1u << (s & 31);
        kept += result == Result::Kept;
    }

    // Both outputs, each as one write. A refused second write leaves the first one done: the
    // GPU then runs the shader and writes both in full, so nothing stale remains either way.
    const auto write = [&](const Range& r, const std::vector<u32>& words) {
        if (r.size == 0) {
            return true;
        }
        if (!buffer_cache.TryWriteThroughFromCpu(r.base, words.data(), r.size)) {
            return false;
        }
        if (kCmdStats) {
            inv_hle.fetch_add(r.size, std::memory_order_relaxed);
        }
        rasterizer.GetTextureCache().InvalidateMemory(r.base, r.size);
        if (kBindCacheRanges) {
            rasterizer.BindCacheNoteWrite(r.base, r.size);
        }
        return true;
    };
    if (!write(shadow_range, bits_shadow) || !write(fade_out_range, bits_fade)) {
        return Fallback(ShadowCullReplace);
    }
    shadow_cull_dispatches.fetch_add(1, std::memory_order_relaxed);
    shadow_cull_sections.fetch_add(evaluated, std::memory_order_relaxed);
    shadow_cull_kept.fetch_add(kept, std::memory_order_relaxed);
    return true;
}

} // namespace SotcShadowCull
