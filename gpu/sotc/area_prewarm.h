// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-area image prewarm (plan step 5d), switch SOTC_AREA_PREWARM (area_prewarm.cpp).
//
//   0 (unset)  off
//   1          area notifications and the area report only (probe_area.cpp), no records
//   2          records: every image the texture cache creates is written to the current area's
//              manifest (user/cache/area_manifest/<area>.bin), once per distinct image
//   3          records and replay: when an area becomes current, its recorded images are created
//              ahead of their draw by the async image path (SOTC_ASYNC_IMAGE_CREATE must be on).
//              Creation only: no data is uploaded, the draw still uploads on first use.
//
// SOTC_AREA_PREWARM_MB (default 256): the VRAM budget of one area's replay, in MiB.
//
// This header is vendor-neutral: the texture cache converts its ImageInfo to an ImageRecord.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SotcAreaPrewarm {

/// What the texture cache needs to plan an image again (TextureCache::PlanImage): the guest range
/// the image was made for and the parameters PlanImage reads. Plain integers, 48 bytes, no padding
/// (the manifest file stores these bytes as they are).
struct ImageRecord {
    std::uint64_t guest_address{};
    std::uint32_t guest_size{};
    std::uint32_t pixel_format{}; ///< the graphics API format value
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint32_t levels{};
    std::uint32_t layers{};
    std::uint32_t props{}; ///< the image property bits (ImageProperties, 4 bytes)
    std::uint32_t type{};  ///< the image type value
    std::uint32_t num_samples{};

    friend bool operator==(const ImageRecord&, const ImageRecord&) = default;
};
static_assert(sizeof(ImageRecord) == 48);

/// SOTC_AREA_PREWARM, read once: 0 to 3 (values above 3 read as 3).
int Mode();

/// SOTC_AREA_PREWARM_MB, read once, in bytes (default 256 MiB, 0 disables replay).
std::uint64_t ReplayBudgetBytes();

/// Recording is on (mode 2 or 3). The texture cache checks this before it builds a record.
inline bool Recording() {
    return Mode() >= 2;
}

/// Value ranges of a record that PlanImage and the device accept: the type is one of the 2D/3D
/// image types PlanImage handles, samples 1 to 16, sizes and counts within sane limits, a
/// non-zero guest address. A record that fails is dropped (not loaded, not replayed).
bool ValidRecord(const ImageRecord& record);

/// Texture cache: an extra check of a record (the exact type, sample and format checks PlanImage
/// needs). Set once by the texture cache constructor; null: ValidRecord only. Must be pure.
void SetRecordCheck(bool (*check)(const ImageRecord&));

/// Called with the area name when the current area changes (probe_area.cpp), and once with "boot"
/// at start. An empty name means the area is unknown: records then go nowhere. seq is the probe's
/// change sequence number (increasing); a call older than one already applied is ignored, so calls
/// that race after the probe's lock is released still apply in order. seq 0: always applied.
/// Saves the previous area's manifest (queued, see RequestSave), loads this area's manifest, and in
/// mode 3 hands its records to the texture cache for replay (TakeReplay). A no-op below mode 2.
void OnAreaChange(const char* area_name, std::uint64_t seq);

/// Texture cache, on every new image (under the cache lock, not the draw path): appends the image
/// to the current area's manifest unless the area already has it. A leaf lock of its own.
void NoteImageCreated(const ImageRecord& record);

/// Set (release) by OnAreaChange on every area change in mode 3 (also when the area has no
/// records, so the previous area's replay images go); read on the command processor thread
/// (TextureCache::PrefetchTexture) with one atomic load.
inline std::atomic<bool> g_replay_pending{false};
inline bool ReplayPending() {
    return g_replay_pending.load(std::memory_order_acquire);
}

/// Texture cache, command processor thread: moves the pending replay records into `out` and clears
/// the pending flag. False when nothing was pending (out is left alone). True when an area change
/// was pending: out holds that area's records, and may be empty.
bool TakeReplay(std::vector<ImageRecord>& out);

/// Counters for the texture cache's replay and for the prefetcher's take.
void NoteReplayed(std::uint64_t bytes);  ///< an image was queued for creation ahead of its draw
void NoteReplaySkipped();                ///< a record was not replayed (mapping, existing image, budget, limits)
void NoteReplayHit();                    ///< a draw took an image that replay created (ImagePrefetcher::Take)

/// One report line every 5 s while the switch is on (driver.cpp wires it): records, replays, hits,
/// skipped, for the current area. Does no file I/O: when the current area has new records its
/// manifest is queued for the save worker (a thread of its own). Appends nothing when mode < 2 or
/// the window saw nothing.
void ReportAreaPrewarm(std::string& out, double seconds);

} // namespace SotcAreaPrewarm
