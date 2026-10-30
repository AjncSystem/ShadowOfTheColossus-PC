// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-area image prewarm (plan step 5d). See area_prewarm.h for the switch and its modes.
//
// A manifest is the list of distinct images one area's first load created. It is kept in memory per
// area (so an area that is left and entered again keeps its records) and written to
// user/cache/area_manifest/<area>.bin by a save worker: an area change queues the previous area, and
// a 10 s tick saves the current area when it is dirty, and an atexit handler saves it at exit. The
// worker copies the records under g_mutex and writes the file without it. The file is a 20-byte header (magic "SOTCAREA",
// version, record size, count) and the records as they are in memory. A file with another version,
// size or a short body is ignored; records that fail ValidRecord or the texture cache's check are
// dropped when the file is read. Area names are lower case (NTFS is case-insensitive).
//
// Locks: ChangeMutex (serializes OnAreaChange; the manifest read happens under it, before g_mutex),
// then g_mutex (short sections, no file I/O). The save queue has its own lock and is never held
// with g_mutex.
//
// Mode 3 replays: OnAreaChange copies the area's records into a pending list and sets a flag. The
// command processor reads the flag in TextureCache::PrefetchTexture and takes the list there, so the
// creation runs on that thread with the same checks as a prefetch (no cache lock is taken).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/path_util.h"
#include "common/thread.h"
#include "area_prewarm.h"

namespace SotcAreaPrewarm {
namespace {

constexpr char kMagic[8] = {'S', 'O', 'T', 'C', 'A', 'R', 'E', 'A'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxRecordsPerArea = 4096;
constexpr std::size_t kMaxNameChars = 64;

struct ImageRecordHash {
    std::size_t operator()(const ImageRecord& r) const noexcept {
        std::uint64_t h = r.guest_address * 0x9E3779B97F4A7C15ULL;
        const std::uint32_t words[] = {r.guest_size, r.pixel_format, r.width,  r.height,
                                       r.depth,      r.levels,       r.layers, r.props,
                                       r.type,       r.num_samples};
        for (const std::uint32_t w : words) {
            h = (h ^ w) * 0xC2B2AE3D27D4EB4FULL;
            h ^= h >> 29;
        }
        return static_cast<std::size_t>(h);
    }
};

struct Manifest {
    std::vector<ImageRecord> records;
    std::unordered_set<ImageRecord, ImageRecordHash> seen;
    bool loaded{};
    bool dirty{};
};

std::mutex g_mutex;
// One manifest write at a time: the save worker, the exit save. Never taken with g_mutex held.
std::timed_mutex g_write_mutex;
std::string g_area;                                       // current area (lower case); empty: unknown
std::unordered_map<std::string, Manifest> g_areas;         // by area name, kept for the session
std::vector<ImageRecord> g_pending;                        // mode 3: records for the next replay
std::atomic<bool (*)(const ImageRecord&)> g_check{nullptr}; // texture cache's exact record check

// Window counters (reset by ReportAreaPrewarm).
std::atomic<std::uint64_t> w_recorded{0};
std::atomic<std::uint64_t> w_full{0};     // records dropped at the per-area cap
std::atomic<std::uint64_t> w_replayed{0};
std::atomic<std::uint64_t> w_replay_bytes{0};
std::atomic<std::uint64_t> w_hits{0};
std::atomic<std::uint64_t> w_skipped{0};

// Serializes OnAreaChange. The probe's sequence numbers are applied in order (see OnAreaChange).
struct ChangeState {
    std::mutex mutex;
    std::uint64_t applied_seq{};
};
ChangeState& Change() {
    static ChangeState* state = new ChangeState; // leaked: outlives the save thread
    return *state;
}

std::uint64_t ParseSwitch(const char* name, std::uint64_t fallback, bool& set) {
    const char* v = std::getenv(name);
    set = v != nullptr;
    if (!v) {
        return fallback;
    }
    char* end = nullptr;
    const long long n = std::strtoll(v, &end, 10);
    return (end == v || n < 0) ? 0 : static_cast<std::uint64_t>(n);
}

// Area name in lower case (NTFS is case-insensitive: "Area" and "area" must share one file).
std::string NormalizeArea(const char* name) {
    std::string out{name};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// Area name to a file name: letters, digits, '_' and '-' kept, anything else '_'. A name that had
// to change, or is long, gets a hash of the whole name so two names cannot share a file.
std::string SafeFileName(const std::string& area) {
    std::string safe;
    safe.reserve(area.size());
    bool changed = false;
    for (const char c : area) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        safe.push_back(ok ? static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c) : '_');
        changed |= !ok;
    }
    if (safe.size() > kMaxNameChars) {
        safe.resize(kMaxNameChars);
        changed = true;
    }
    if (changed) {
        std::uint64_t h = 0xCBF29CE484222325ULL; // FNV-1a
        for (const char c : area) {
            h = (h ^ static_cast<unsigned char>(c)) * 0x100000001B3ULL;
        }
        char tail[24];
        std::snprintf(tail, sizeof tail, "_%016llx", static_cast<unsigned long long>(h));
        safe += tail;
    }
    return safe.empty() ? std::string{"_"} : safe;
}

std::filesystem::path ManifestPath(const std::string& area) {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "area_manifest" /
           (SafeFileName(area) + ".bin");
}

void PutU32(char* out, std::uint32_t v) {
    std::memcpy(out, &v, sizeof v); // little-endian host (Windows x86-64)
}

std::uint32_t GetU32(const char* in) {
    std::uint32_t v = 0;
    std::memcpy(&v, in, sizeof v);
    return v;
}

// Reads an area's file and returns the records that pass ValidRecord and the texture cache's check.
// No g_mutex: the caller holds ChangeMutex. A file that fails the header or the size checks gives
// nothing.
std::vector<ImageRecord> ReadManifest(const std::string& area) {
    std::vector<ImageRecord> valid;
    std::error_code ec;
    const auto path = ManifestPath(area);
    if (!std::filesystem::exists(path, ec)) {
        return valid;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return valid;
    }
    char header[20];
    if (!in.read(header, sizeof header) || std::memcmp(header, kMagic, sizeof kMagic) != 0 ||
        GetU32(header + 8) != kVersion || GetU32(header + 12) != sizeof(ImageRecord)) {
        return valid;
    }
    const std::uint32_t count = GetU32(header + 16);
    if (count > kMaxRecordsPerArea) {
        return valid;
    }
    std::vector<ImageRecord> records(count);
    if (count != 0 &&
        !in.read(reinterpret_cast<char*>(records.data()),
                 static_cast<std::streamsize>(count * sizeof(ImageRecord)))) {
        return valid; // short body: ignore the whole file
    }
    const auto check = g_check.load(std::memory_order_acquire);
    valid.reserve(count);
    for (const ImageRecord& r : records) {
        if (ValidRecord(r) && (!check || check(r))) {
            valid.push_back(r);
        }
    }
    return valid;
}

// Adds records to m: no duplicates, at most kMaxRecordsPerArea. Under g_mutex.
void AddLocked(Manifest& m, const std::vector<ImageRecord>& records) {
    for (const ImageRecord& r : records) {
        if (m.records.size() >= kMaxRecordsPerArea) {
            return;
        }
        if (m.seen.insert(r).second) {
            m.records.push_back(r);
        }
    }
}

// "<path>: <what failed> (errno N: text)" for the log.
std::string Describe(const std::filesystem::path& path, const char* what, int code) {
    return path.string() + ": " + what + " (errno " + std::to_string(code) + ": " +
           std::strerror(code) + ")";
}

// Writes records to the area's file (a temporary file with a unique name, then rename). Only the
// save worker and the exit save call it, under g_write_mutex. False on any failure, with the reason in
// error.
bool WriteManifest(const std::string& area, const std::vector<ImageRecord>& records,
                   std::string& error) {
    std::error_code ec;
    const auto path = ManifestPath(area);
    std::filesystem::create_directories(path.parent_path(), ec);
    static std::atomic<std::uint64_t> counter{0};
    auto tmp = path;
    tmp += "." + std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    {
        errno = 0;
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = Describe(tmp, "open failed", errno);
            return false;
        }
        char header[20];
        std::memcpy(header, kMagic, sizeof kMagic);
        PutU32(header + 8, kVersion);
        PutU32(header + 12, sizeof(ImageRecord));
        PutU32(header + 16, static_cast<std::uint32_t>(records.size()));
        out.write(header, sizeof header);
        if (!records.empty()) {
            out.write(reinterpret_cast<const char*>(records.data()),
                      static_cast<std::streamsize>(records.size() * sizeof(ImageRecord)));
        }
        out.close();
        if (!out) {
            error = Describe(tmp, "write failed", errno);
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        error = Describe(path, "rename failed", ec.value());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// Save queue: one worker thread, started by the first RequestSave. Leaked on purpose: the thread
// is detached and can still be waiting when the statics are destroyed at exit.
struct SaveQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> areas; // areas whose save is queued (each once)
    bool started{};
};
SaveQueue& Queue() {
    static SaveQueue* queue = new SaveQueue;
    return *queue;
}

// Writes one area (caller holds g_write_mutex): the records are copied under g_mutex, then written
// without it. A failed write leaves the area dirty, so the next save retries it.
void SaveAreaLocked(const std::string& area) {
    std::vector<ImageRecord> records;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_areas.find(area);
        if (it == g_areas.end() || !it->second.dirty) {
            return;
        }
        it->second.dirty = false;
        records = it->second.records;
    }
    std::string error;
    if (WriteManifest(area, records, error)) {
        std::fprintf(stderr, "SOTC AREA PREWARM: saved %s (%zu images)\n",
                     ManifestPath(area).string().c_str(), records.size());
    } else {
        std::fprintf(stderr, "SOTC AREA PREWARM: save failed: %s\n", error.c_str());
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto it = g_areas.find(area); it != g_areas.end()) {
            it->second.dirty = true;
        }
    }
}

// Saves one area, serialized with the other writers.
void SaveArea(const std::string& area) {
    std::lock_guard<std::timed_mutex> write(g_write_mutex);
    SaveAreaLocked(area);
}

// Current area, copied under g_mutex ("" when unknown).
std::string CurrentArea() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_area;
}

void SaveLoop() {
    Common::SetCurrentThreadName("sotc:AreaSave");
    SaveQueue& q = Queue();
    for (;;) {
        std::string area;
        bool queued = false;
        {
            std::unique_lock<std::mutex> lock(q.mutex);
            queued = q.cv.wait_for(lock, std::chrono::seconds(10), [&] { return !q.areas.empty(); });
            if (queued) {
                area = std::move(q.areas.back());
                q.areas.pop_back();
            }
        }
        if (queued) {
            SaveArea(area);
            continue;
        }
        // Tick without a queued save: the current area is saved if it is dirty, so a session that
        // ends without an area change (or without a report) keeps its records.
        const std::string current = CurrentArea();
        if (!current.empty()) {
            SaveArea(current);
        }
    }
}

// Exit save of the current area. Waits at most 1 s for a write in progress; the 10 s tick covers
// a process that ends without running atexit (TerminateProcess).
void SaveAtExit() {
    const std::string area = CurrentArea();
    if (area.empty()) {
        return;
    }
    std::unique_lock<std::timed_mutex> write(g_write_mutex, std::chrono::seconds(1));
    if (!write.owns_lock()) {
        std::fprintf(stderr, "SOTC AREA PREWARM: exit save skipped (writer busy)\n");
        return;
    }
    SaveAreaLocked(area);
}

// Queues the save of an area. Called without g_mutex. Without a worker thread (creation failed) the
// save runs on the caller.
void RequestSave(const std::string& area) {
    SaveQueue& q = Queue();
    bool threaded = false;
    {
        std::lock_guard<std::mutex> lock(q.mutex);
        if (!q.started) {
            try {
                std::thread(SaveLoop).detach();
                q.started = true;
            } catch (...) {
                q.started = false;
            }
        }
        threaded = q.started;
        if (threaded && std::find(q.areas.begin(), q.areas.end(), area) == q.areas.end()) {
            q.areas.push_back(area);
        }
    }
    if (threaded) {
        q.cv.notify_one();
    } else {
        SaveArea(area);
    }
}

} // namespace

int Mode() {
    static const int mode = [] {
        bool set = false;
        const std::uint64_t n = ParseSwitch("SOTC_AREA_PREWARM", 0, set);
        return static_cast<int>(std::min<std::uint64_t>(n, 3));
    }();
    return mode;
}

std::uint64_t ReplayBudgetBytes() {
    static const std::uint64_t bytes = [] {
        bool set = false;
        const std::uint64_t mb = ParseSwitch("SOTC_AREA_PREWARM_MB", 256, set);
        return std::min<std::uint64_t>(mb, 65536) << 20;
    }();
    return bytes;
}

bool ValidRecord(const ImageRecord& r) {
    constexpr std::uint64_t kMaxGuestAddress = std::uint64_t{1} << 40; // GPU address space
    constexpr std::uint32_t kMaxGuestSize = 1u << 30;
    constexpr std::uint32_t kMaxExtent = 16384;
    constexpr std::uint32_t kMaxLevels = 16; // mip levels of a 16384 texel side
    constexpr std::uint32_t kMaxLayers = 2048;
    constexpr std::uint32_t kMaxFormat = 256; // core Vulkan format values are below this

    if (r.guest_address == 0 || r.guest_address >= kMaxGuestAddress) {
        return false;
    }
    if (r.guest_size == 0 || r.guest_size > kMaxGuestSize) {
        return false;
    }
    // AmdGpu::ImageType Color1D (8) to Color2DMsaa (14): the types PlanImage converts (Color2DMsaaArray
    // 15 has no case in ConvertImageType).
    if (r.type < 8 || r.type > 14) {
        return false;
    }
    switch (r.num_samples) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
        break;
    default:
        return false;
    }
    if (r.pixel_format == 0 || r.pixel_format >= kMaxFormat) {
        return false;
    }
    if (r.width == 0 || r.height == 0 || r.depth == 0 || r.width > kMaxExtent ||
        r.height > kMaxExtent || r.depth > kMaxExtent) {
        return false;
    }
    if (r.levels == 0 || r.levels > kMaxLevels || r.layers == 0 || r.layers > kMaxLayers) {
        return false;
    }
    return true;
}

void SetRecordCheck(bool (*check)(const ImageRecord&)) {
    g_check.store(check, std::memory_order_release);
}

void OnAreaChange(const char* area_name, std::uint64_t seq) {
    if (Mode() < 2 || !area_name) {
        return;
    }
    static std::once_flag exit_flag;
    std::call_once(exit_flag, [] { std::atexit(SaveAtExit); });
    const std::string name = NormalizeArea(area_name); // empty: unknown

    // One change at a time. The probe numbers its changes; a change older than the last applied one
    // lost the race after the probe's lock was released and is dropped.
    ChangeState& change = Change();
    std::lock_guard<std::mutex> change_lock(change.mutex);
    if (seq != 0) {
        if (seq <= change.applied_seq) {
            return;
        }
        change.applied_seq = seq;
    }

    bool need_load = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (name == g_area) {
            return;
        }
        if (!name.empty()) {
            const auto it = g_areas.find(name);
            need_load = it == g_areas.end() || !it->second.loaded;
        }
    }
    // The file is read with no g_mutex held (ChangeMutex only).
    std::vector<ImageRecord> from_file;
    if (need_load) {
        from_file = ReadManifest(name);
    }

    const bool replay = Mode() >= 3 && ReplayBudgetBytes() != 0;
    std::string save_name;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_area.empty()) {
            const auto it = g_areas.find(g_area);
            if (it != g_areas.end() && it->second.dirty) {
                save_name = g_area;
            }
        }
        g_area = name;
        g_pending.clear();
        if (!name.empty()) {
            Manifest& m = g_areas[name];
            if (!m.loaded) {
                AddLocked(m, from_file); // records made while the file was read stay first
                m.loaded = true;
            }
            if (replay) {
                g_pending = m.records;
            }
        }
        // Every change in mode 3 sets the flag, also when the list is empty: the texture cache then
        // drops the previous area's replay images. Unknown area: an empty list.
        g_replay_pending.store(replay, std::memory_order_release);
    }
    if (!save_name.empty()) {
        RequestSave(save_name);
    }
}

void NoteImageCreated(const ImageRecord& record) {
    if (Mode() < 2) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_area.empty()) {
        return; // unknown area: nothing is recorded into another area
    }
    Manifest& m = g_areas[g_area];
    if (m.records.size() >= kMaxRecordsPerArea) {
        w_full.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!m.seen.insert(record).second) {
        return;
    }
    m.records.push_back(record);
    m.dirty = true;
    w_recorded.fetch_add(1, std::memory_order_relaxed);
}

bool TakeReplay(std::vector<ImageRecord>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_replay_pending.load(std::memory_order_acquire)) {
        return false;
    }
    out.swap(g_pending);
    g_pending.clear();
    g_replay_pending.store(false, std::memory_order_release);
    return true;
}

void NoteReplayed(std::uint64_t bytes) {
    w_replayed.fetch_add(1, std::memory_order_relaxed);
    w_replay_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteReplaySkipped() {
    w_skipped.fetch_add(1, std::memory_order_relaxed);
}

void NoteReplayHit() {
    w_hits.fetch_add(1, std::memory_order_relaxed);
}

void ReportAreaPrewarm(std::string& out, double seconds) {
    if (Mode() < 2) {
        return;
    }
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    const std::uint64_t recorded = w_recorded.exchange(0, std::memory_order_relaxed);
    const std::uint64_t full = w_full.exchange(0, std::memory_order_relaxed);
    const std::uint64_t replayed = w_replayed.exchange(0, std::memory_order_relaxed);
    const std::uint64_t replay_bytes = w_replay_bytes.exchange(0, std::memory_order_relaxed);
    const std::uint64_t hits = w_hits.exchange(0, std::memory_order_relaxed);
    const std::uint64_t skipped = w_skipped.exchange(0, std::memory_order_relaxed);

    std::string save_name;
    std::string area;
    std::size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        area = g_area.empty() ? "?" : g_area;
        const auto it = g_areas.find(g_area);
        if (it != g_areas.end()) {
            total = it->second.records.size();
            if (it->second.dirty) {
                save_name = g_area; // written by the save worker, not on this (flip) thread
            }
        }
    }
    if (!save_name.empty()) {
        RequestSave(save_name);
    }
    if (recorded == 0 && full == 0 && replayed == 0 && hits == 0 && skipped == 0) {
        return;
    }

    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC AREA PREWARM (mode %d): area %s, manifest %zu images; recorded %llu "
                  "(full %llu), replayed %llu (%.1f MB), replay hits %llu, skipped %llu in %.0f s\n",
                  Mode(), area.c_str(), total, static_cast<unsigned long long>(recorded),
                  static_cast<unsigned long long>(full), static_cast<unsigned long long>(replayed),
                  static_cast<double>(replay_bytes) / (1024.0 * 1024.0),
                  static_cast<unsigned long long>(hits), static_cast<unsigned long long>(skipped),
                  seconds);
    out += line;
}

} // namespace SotcAreaPrewarm
