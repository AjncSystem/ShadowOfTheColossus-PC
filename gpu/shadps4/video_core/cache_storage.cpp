// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>
#ifdef _WIN32
#include <windows.h> // sotc: SetThreadPriority (background mode of the cache writer)
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace {

std::mutex submit_mutex{};
// sotc: atomic. The writer increments under submit_mutex but the IO thread decrements without
// it; a lost decrement left the IO loop spinning on an empty queue (one core) for the session.
std::atomic<u32> num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

// sotc: the session's new cache blobs go into ONE append-only journal file instead of ~12k small
// files (each a create + write + rename, which Windows/Defender make slow enough to show as
// stutter on a cold cache). Format: "SOTCJN1" + NUL, then records of a 16-byte header (u32 magic,
// u16 name length, u16 blob type, u32 data length, u32 CRC-32 of the first 12 header bytes + name
// + data), the file name (with extension, as the loose file would be called) and the data. A
// record that is cut short or fails its checksum ends the file for the reader (a killed session).
constexpr char JournalMagic[8] = {'S', 'O', 'T', 'C', 'J', 'N', '1', '\0'};
constexpr u32 JournalRecMagic = 0x43524A53;
constexpr size_t JournalFlushBytes = 1u << 20;
constexpr long long JournalFlushMs = 2000;
constexpr std::string_view JournalPrefix = "cache.sotcjournal.";

struct JournalRecHeader {
    u32 magic;
    u16 name_len;
    u16 type;
    u32 data_len;
    u32 crc;
};
static_assert(sizeof(JournalRecHeader) == 16);

using BlobPtr = std::shared_ptr<const std::vector<u8>>;

u32 JournalCrc(const JournalRecHeader& h, const char* name, const u8* data) {
    u32 crc = static_cast<u32>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const mz_uint8*>(&h), 12));
    crc = static_cast<u32>(mz_crc32(crc, reinterpret_cast<const mz_uint8*>(name), h.name_len));
    if (h.data_len) {
        crc = static_cast<u32>(mz_crc32(crc, data, h.data_len));
    }
    return crc;
}

class Journal {
public:
    void Begin(std::filesystem::path p) {
        std::scoped_lock io{io_mtx};
        std::scoped_lock lk{mtx};
        ResetLocked();
        path = std::move(p);
        active = true;
        last_flush = std::chrono::steady_clock::now();
    }
    bool Active() {
        std::scoped_lock lk{mtx};
        return active;
    }
    /// Caller thread: the blob is readable from now on, before the writer got to it.
    void Submit(const std::string& name, const BlobPtr& data) {
        std::scoped_lock lk{mtx};
        pending[name] = data;
    }
    /// Writer thread. False: not written (journal unusable), the caller writes a loose file.
    /// The caller threads only ever wait for `mtx`, which is never held across the CRC, a memory
    /// copy of a whole blob or a disk write (`io_mtx` serializes those among writer-side calls).
    bool Append(const std::string& name, Storage::BlobType type, const BlobPtr& data) {
        std::scoped_lock io{io_mtx};
        {
            std::scoped_lock lk{mtx};
            const auto it = pending.find(name);
            if (it == pending.end() || it->second != data) {
                return true; // dropped by Discard, or a newer blob of this name is queued behind
            }
            if (!active || failed) {
                pending.erase(it);
                return false;
            }
        }
        if (!wfile && !OpenIo()) {
            std::scoped_lock lk{mtx};
            if (const auto it = pending.find(name); it != pending.end() && it->second == data) {
                pending.erase(it);
            }
            return false;
        }
        JournalRecHeader h{JournalRecMagic, static_cast<u16>(name.size()), static_cast<u16>(type),
                           static_cast<u32>(data->size()), 0};
        h.crc = JournalCrc(h, name.data(), data->data());
        std::vector<u8> rec(sizeof(h) + name.size() + data->size());
        std::memcpy(rec.data(), &h, sizeof(h));
        std::memcpy(rec.data() + sizeof(h), name.data(), name.size());
        if (!data->empty()) {
            std::memcpy(rec.data() + sizeof(h) + name.size(), data->data(), data->size());
        }
        bool flush_now = false;
        {
            std::scoped_lock lk{mtx};
            const u64 base = logical_end;
            buf.insert(buf.end(), rec.begin(), rec.end());
            logical_end += rec.size();
            index[name] = {base + sizeof(h) + name.size(), h.data_len};
            if (const auto it = pending.find(name); it != pending.end() && it->second == data) {
                pending.erase(it);
            }
            const auto now = std::chrono::steady_clock::now();
            flush_now = buf.size() >= JournalFlushBytes ||
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush)
                                .count() >= JournalFlushMs;
        }
        if (flush_now) {
            FlushIo();
        }
        return true;
    }
    bool Dirty() {
        std::scoped_lock lk{mtx};
        return !buf.empty();
    }
    void Flush() {
        std::scoped_lock io{io_mtx};
        FlushIo();
    }
    /// Flushes and closes the files; the journal stays on disk for the next start to merge.
    void End() {
        std::scoped_lock io{io_mtx};
        FlushIo();
        std::scoped_lock lk{mtx};
        ResetLocked();
        active = false;
    }
    /// Clear(): forget everything. A new file is started by the next Append.
    void Discard() {
        std::scoped_lock io{io_mtx};
        std::scoped_lock lk{mtx};
        const bool was_active = active;
        ResetLocked();
        active = was_active;
    }
    bool Lookup(const std::string& name, std::vector<u8>& out) {
        std::scoped_lock lk{mtx};
        if (const auto p = pending.find(name); p != pending.end()) {
            out = *p->second;
            return true;
        }
        const auto it = index.find(name);
        if (it == index.end()) {
            return false;
        }
        const auto [off, len] = it->second;
        out.resize(len);
        const u64 inflight_size = inflight ? inflight->size() : 0;
        if (off >= flushed + inflight_size) {
            std::memcpy(out.data(), buf.data() + (off - flushed - inflight_size), len);
            return true;
        }
        if (off >= flushed) {
            std::memcpy(out.data(), inflight->data() + (off - flushed), len);
            return true;
        }
        if (!rfile) {
            rfile = _wfopen(path.c_str(), L"rb");
            if (rfile) {
                std::setvbuf(rfile, nullptr, _IONBF, 0);
            }
        }
        if (!rfile || _fseeki64(rfile, static_cast<long long>(off), SEEK_SET) != 0 ||
            std::fread(out.data(), 1, len, rfile) != len) {
            out.clear();
            return false;
        }
        return true;
    }
    /// Names (with extension) of the blobs written this session whose name ends in `ext`.
    std::vector<std::string> Names(const std::string& ext) {
        std::scoped_lock lk{mtx};
        std::vector<std::string> names;
        const auto match = [&](const std::string& n) {
            return n.size() >= ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0;
        };
        for (const auto& [n, loc] : index) {
            if (match(n)) {
                names.push_back(n);
            }
        }
        for (const auto& [n, data] : pending) {
            if (!index.contains(n) && match(n)) {
                names.push_back(n);
            }
        }
        return names;
    }

private:
    // io_mtx held.
    bool OpenIo() {
        std::filesystem::path p;
        {
            std::scoped_lock lk{mtx};
            p = path;
        }
        wfile = _wfopen(p.c_str(), L"wb");
        std::scoped_lock lk{mtx};
        if (!wfile) {
            failed = true;
            return false;
        }
        std::setvbuf(wfile, nullptr, _IONBF, 0);
        buf.assign(JournalMagic, JournalMagic + sizeof(JournalMagic));
        logical_end = sizeof(JournalMagic);
        flushed = 0;
        return true;
    }
    // io_mtx held. The disk write happens outside `mtx`: the bytes stay readable through
    // `inflight` meanwhile.
    void FlushIo() {
        std::shared_ptr<std::vector<u8>> out;
        {
            std::scoped_lock lk{mtx};
            last_flush = std::chrono::steady_clock::now();
            if (buf.empty() || !wfile) {
                return;
            }
            out = std::make_shared<std::vector<u8>>(std::move(buf));
            buf.clear();
            inflight = out;
        }
        const size_t n = std::fwrite(out->data(), 1, out->size(), wfile);
        std::fflush(wfile);
        std::scoped_lock lk{mtx};
        if (n != out->size()) {
            // Disk full or similar: what is not on disk is forgotten, later blobs go loose.
            failed = true;
            for (auto it = index.begin(); it != index.end();) {
                it = (it->second.first + it->second.second > flushed + n) ? index.erase(it)
                                                                           : std::next(it);
            }
            buf.clear();
            logical_end = flushed + n;
            std::fclose(wfile);
            wfile = nullptr;
        }
        flushed += n;
        inflight.reset();
    }
    // Both mutexes held.
    void ResetLocked() {
        if (wfile) {
            std::fclose(wfile);
            wfile = nullptr;
        }
        if (rfile) {
            std::fclose(rfile);
            rfile = nullptr;
        }
        active = false;
        failed = false;
        buf.clear();
        inflight.reset();
        index.clear();
        pending.clear();
        flushed = 0;
        logical_end = 0;
    }

    std::mutex io_mtx{}; // writer-side calls; taken before `mtx`
    std::mutex mtx{};    // everything below except `wfile`
    std::filesystem::path path{};
    std::FILE* wfile{};  // io_mtx
    std::FILE* rfile{};
    bool active{};
    bool failed{};
    std::vector<u8> buf{};                       // appended, not handed to the disk yet
    std::shared_ptr<std::vector<u8>> inflight{}; // being written right now, at `flushed`
    u64 flushed{};                               // bytes on disk
    u64 logical_end{};                           // flushed + inflight + buf
    std::chrono::steady_clock::time_point last_flush{};
    std::unordered_map<std::string, std::pair<u64, u32>> index{}; // name -> data offset, size
    std::unordered_map<std::string, BlobPtr> pending{};            // queued, not appended yet
};

Journal journal{};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");
#ifdef _WIN32
    // sotc: SOTC_CACHE_IO_BACKGROUND=0 leaves the writer at normal priority. By default it runs in
    // background mode (low CPU, I/O and memory priority): on a cold shader cache it writes ~12k small
    // files in the first minutes, which must not compete with the game's own file reads.
    static const bool background = [] {
        const char* v = std::getenv("SOTC_CACHE_IO_BACKGROUND");
        return !v || v[0] != '0';
    }();
    if (background) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    }
#endif

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests.load() != 0; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }

        // sotc: the journal's buffered tail goes to disk once the writer has been quiet for a
        // moment (more blobs usually follow within milliseconds; they join the same write).
        while (!stoken.stop_requested() && journal.Dirty()) {
            std::unique_lock lk{submit_mutex};
            const bool more = request_cv.wait_for(lk, std::chrono::milliseconds(250),
                                                  [&] { return num_requests.load() != 0; });
            lk.unlock();
            if (more) {
                break;
            }
            journal.Flush();
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

static bool CachePackEnabled();

// sotc: SOTC_CACHE_JOURNAL=0 writes every blob as its own file again. The journal needs the pack
// (the next start merges it there), so it is off when SOTC_CACHE_PACK=0.
static bool CacheJournalEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_CACHE_JOURNAL");
        return (!v || v[0] != '0') && CachePackEnabled();
    }();
    return enabled;
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        // sotc: shaders compiled with and without direct memory access differ, and the cache keys
        // do not tell them apart: the launcher's DMA switch gets its own cache folder. The same
        // for SOTC_NEO: a cache filled in PS4 Pro mode gave a black picture in the normal mode.
        cache_path = GetUserPath(PathType::CacheDir) /
                     (std::string{game_info.GameSerial()} +
                      (EmulatorSettings.IsDirectMemoryAccessEnabled() ? "" : "_nodma") +
                      (EmulatorSettings.IsNoInt64() ? "_noi64" : "") +
                      (Libraries::Kernel::sceKernelIsNeoMode() ? "_neo" : ""));
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        Prefetch();
        if (CacheJournalEnabled()) {
            const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            char name[64];
            std::snprintf(name, sizeof(name), "%.*s%016llu", static_cast<int>(JournalPrefix.size()),
                          JournalPrefix.data(), static_cast<unsigned long long>(stamp));
            journal.Begin(cache_path / name);
        }
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

// sotc: all cache files in one file (cache.sotcpack in the cache folder), read in one go. On
// a hard disk the ~12k small files took 30-100 s whenever Windows had dropped them from its file
// cache (the game's 13.7 GB of archives read into memory push them out): the loading screen
// changed between 1 s and 100 s from one start to the next. The loose files stay the cache
// (written as before, whole or not at all); the pack is a copy, rewritten in the background
// when files were missing from it. Format: "SOTCPK1" + NUL, u32 count, then per file u16 name
// length, name, u32 size, data. SOTC_CACHE_PACK=0 turns it off.
static bool CachePackEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_CACHE_PACK");
        return !v || v[0] != '0';
    }();
    return enabled;
}

static constexpr char PackMagic[8] = {'S', 'O', 'T', 'C', 'P', 'K', '1', '\0'};

static void ReadCachePack(const std::filesystem::path& path,
                          std::map<std::string, std::vector<u8>>& out) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        return;
    }
    std::vector<u8> all;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 12) {
        all.resize(static_cast<size_t>(size));
        if (std::fread(all.data(), 1, all.size(), f) != all.size()) {
            all.clear();
        }
    }
    std::fclose(f);
    if (all.size() < 12 || std::memcmp(all.data(), PackMagic, 8) != 0) {
        return;
    }
    size_t pos = 8;
    const auto take = [&](void* dst, size_t n) {
        if (pos + n > all.size()) {
            return false;
        }
        std::memcpy(dst, all.data() + pos, n);
        pos += n;
        return true;
    };
    u32 count = 0;
    take(&count, 4);
    for (u32 i = 0; i < count; ++i) {
        u16 name_len = 0;
        u32 data_len = 0;
        std::string name;
        if (!take(&name_len, 2) || pos + name_len > all.size()) {
            break;
        }
        name.assign(reinterpret_cast<const char*>(all.data() + pos), name_len);
        pos += name_len;
        if (!take(&data_len, 4) || pos + data_len > all.size()) {
            break;
        }
        out.emplace(std::move(name),
                    std::vector<u8>(all.begin() + pos, all.begin() + pos + data_len));
        pos += data_len;
    }
}

// sotc: reads a session journal (format above `Journal`) into `out`, later records replacing
// earlier ones of the same name. A bad or cut-short record ends the file: it can only be the tail
// of a session that was killed, as a journal is never appended to by a later session.
static size_t ReadJournal(const std::filesystem::path& path,
                          std::map<std::string, std::vector<u8>>& out) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        return 0;
    }
    std::vector<u8> all;
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (size > static_cast<long long>(sizeof(JournalMagic))) {
        all.resize(static_cast<size_t>(size));
        if (std::fread(all.data(), 1, all.size(), f) != all.size()) {
            all.clear();
        }
    }
    std::fclose(f);
    if (all.size() < sizeof(JournalMagic) ||
        std::memcmp(all.data(), JournalMagic, sizeof(JournalMagic)) != 0) {
        return 0;
    }
    size_t pos = sizeof(JournalMagic);
    size_t records = 0;
    while (pos + sizeof(JournalRecHeader) <= all.size()) {
        JournalRecHeader h;
        std::memcpy(&h, all.data() + pos, sizeof(h));
        const size_t body = pos + sizeof(h);
        if (h.magic != JournalRecMagic || h.name_len == 0 || h.name_len > 512 ||
            body + h.name_len > all.size() || body + h.name_len + h.data_len > all.size()) {
            break;
        }
        const char* name = reinterpret_cast<const char*>(all.data() + body);
        const u8* data = all.data() + body + h.name_len;
        if (JournalCrc(h, name, data) != h.crc) {
            break;
        }
        out.insert_or_assign(std::string{name, h.name_len}, std::vector<u8>(data, data + h.data_len));
        pos = body + h.name_len + h.data_len;
        ++records;
    }
    if (pos != all.size()) {
        std::fprintf(stderr, "SOTC shader cache: journal %s ends with a broken record, %zu bytes ignored\n",
                     path.filename().string().c_str(), all.size() - pos);
    }
    return records;
}

static void WriteCachePack(std::filesystem::path path,
                           std::vector<std::pair<std::string, std::vector<u8>>> files,
                           std::vector<std::filesystem::path> merged_journals) {
    auto tmp = path;
    tmp += L".tmp";
    std::FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        return;
    }
    bool ok = std::fwrite(PackMagic, 1, 8, f) == 8;
    const u32 count = static_cast<u32>(files.size());
    ok = ok && std::fwrite(&count, 4, 1, f) == 1;
    for (const auto& [name, data] : files) {
        const u16 name_len = static_cast<u16>(name.size());
        const u32 data_len = static_cast<u32>(data.size());
        ok = ok && std::fwrite(&name_len, 2, 1, f) == 1 &&
             std::fwrite(name.data(), 1, name.size(), f) == name.size() &&
             std::fwrite(&data_len, 4, 1, f) == 1 &&
             (data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size());
    }
    ok = (std::fclose(f) == 0) && ok;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
    }
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        return;
    }
    // sotc: the journals are in the pack now (and only now): they can go.
    for (const auto& j : merged_journals) {
        std::filesystem::remove(j, ec);
    }
}

// sotc: the boot preload opened ~12k small files one by one (~21 s of a 22 s warm-up on Windows,
// where each open is slow and scanned). Read them all in parallel first; Load and ForEachBlob
// take them from memory until FinishPreload.
void DataBase::Prefetch() {
    const auto start = std::chrono::steady_clock::now();
    const auto pack_path = cache_path / "cache.sotcpack";
    std::map<std::string, std::vector<u8>> packed;
    if (CachePackEnabled()) {
        ReadCachePack(pack_path, packed);
    }
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> journals;
    std::error_code ec;
    size_t in_pack = 0, on_disk = 0;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        const auto ext = entry.path().extension();
        if (!entry.is_regular_file(ec) || ext == ".tmp" || ext == ".sotcpack") {
            continue;
        }
        if (entry.path().filename().string().starts_with(JournalPrefix)) {
            journals.push_back(entry.path());
            continue;
        }
        ++on_disk;
        const auto it = packed.find(entry.path().filename().string());
        if (it != packed.end()) {
            ++in_pack;
            prefetched.emplace(it->first, std::move(it->second));
            packed.erase(it);
        } else {
            files.push_back(entry.path());
        }
    }
    {
        // sotc: the same listing, kept for MayExist once prefetch_active ends below.
        std::scoped_lock lock{known_mutex};
        known_files.clear();
        for (const auto& file : files) {
            known_files.insert(file.filename().string());
        }
        known_valid = !ec;
    }
    const u32 num_threads =
        std::clamp<u32>(std::thread::hardware_concurrency(), 1, 16);
    std::vector<std::vector<std::pair<std::string, std::vector<u8>>>> results(num_threads);
    std::atomic<size_t> next{0};
    {
        std::vector<std::jthread> workers;
        for (u32 t = 0; t < num_threads; ++t) {
            workers.emplace_back([&, t] {
                using namespace Common::FS;
                for (size_t i = next++; i < files.size(); i = next++) {
                    const IOFile file{files[i], FileAccessMode::Read};
                    if (!file.IsOpen()) {
                        continue;
                    }
                    std::vector<u8> data(file.GetSize());
                    if (file.Read(data) != data.size()) {
                        continue;
                    }
                    results[t].emplace_back(files[i].filename().string(), std::move(data));
                }
            });
        }
    }
    u64 bytes = 0;
    for (auto& list : results) {
        for (auto& [name, data] : list) {
            bytes += data.size();
            prefetched.emplace(std::move(name), std::move(data));
        }
    }
    // sotc: blobs only the pack has (no loose file: they came from a journal that was merged)
    // are part of the cache too.
    const size_t pack_only = packed.size();
    for (auto& [name, data] : packed) {
        prefetched.emplace(name, std::move(data));
    }
    // sotc: the journals of earlier sessions, oldest first (their names carry a timestamp); they
    // win over a loose file or pack entry of the same name.
    std::sort(journals.begin(), journals.end());
    size_t journal_records = 0;
    for (const auto& j : journals) {
        std::map<std::string, std::vector<u8>> blobs;
        journal_records += ReadJournal(j, blobs);
        for (auto& [name, data] : blobs) {
            prefetched.insert_or_assign(name, std::move(data));
        }
    }
    prefetch_active = true;
    if (CachePackEnabled() && (!files.empty() || !journals.empty())) {
        std::vector<std::pair<std::string, std::vector<u8>>> copy;
        copy.reserve(prefetched.size());
        for (const auto& [name, data] : prefetched) {
            copy.emplace_back(name, data);
        }
        std::thread{WriteCachePack, pack_path, std::move(copy), journals}.detach();
    }
    std::fprintf(stderr,
                 "SOTC shader cache: %zu files, %zu from the pack, %zu read one by one, %zu only "
                 "in the pack, %zu records from %zu journals\n",
                 on_disk, in_pack, files.size(), pack_only, journal_records, journals.size());
    LOG_INFO(Render, "Prefetched {} cache files ({} MiB) in {} ms with {} threads",
             prefetched.size(), bytes >> 20,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count(),
             num_threads);
}

bool DataBase::TakePrefetched(const std::filesystem::path& path, std::vector<u8>& data) {
    if (!prefetch_active) {
        return false;
    }
    const auto it = prefetched.find(path.filename().string());
    if (it == prefetched.end()) {
        // Not on disk at prefetch time: the same answer the file system would give.
        data.clear();
        return true;
    }
    // Copied, not moved: a shader's .meta and .spv are loaded once per pipeline that uses it.
    data = it->second;
    return true;
}

bool DataBase::TakePrefetched(const std::filesystem::path& path, std::vector<u32>& data) {
    if (!prefetch_active) {
        return false;
    }
    const auto it = prefetched.find(path.filename().string());
    if (it == prefetched.end()) {
        data.clear();
        return true;
    }
    // sotc: copied once, straight into the u32 vector. It went through a temporary byte vector
    // first (two copies of every SPIR-V blob, the second one freed right away but still a
    // transient heap spike per shader while the preload ran).
    data.resize(it->second.size() / sizeof(u32));
    if (!data.empty()) {
        std::memcpy(data.data(), it->second.data(), data.size() * sizeof(u32));
    }
    return true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();
    // sotc: the worker leaves when asked to stop, with writes still queued: pipelines compiled
    // shortly before the game was closed were dropped and compiled again (a stutter) next time.
    {
        std::scoped_lock lock{m_request};
        while (!req_queue.empty()) {
            auto request = std::move(req_queue.front());
            req_queue.pop();
            if (request.valid()) {
                request();
            }
        }
        num_requests = 0;
    }
    // sotc: the journal's tail to disk; the file stays for the next start to merge into the pack.
    journal.End();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

// sotc: restored from bbport. Deletes the loose cache files so an incompatible cache (another
// GPU, a driver or build change) is rebuilt instead of being ignored by every later session.
void DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    prefetched.clear();
    {
        std::scoped_lock lock{known_mutex};
        known_files.clear();
    }
    journal.Discard(); // sotc: closes this session's journal so it is removed below too
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

static void Enqueue(std::packaged_task<void()>&& request) {
    {
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }
    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
}

// sotc: one loose file per blob, written whole or not at all (the legacy path, and the fallback
// when the journal cannot be written).
static void WriteLooseFile(std::filesystem::path path, const u8* data, size_t size) {
    using namespace Common::FS;
    auto temporary = path;
    temporary += ".tmp";
    {
        const auto file = IOFile{temporary, FileAccessMode::Create};
        file.WriteRaw<u8>(data, size);
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
    }
}

// sotc: the blob goes to the journal (readable at once through `pending`); the writer thread
// appends it.
template <typename T>
static bool WriteJournaled(const BlobType type, const std::filesystem::path& path_,
                           std::vector<T>&& v) {
    std::shared_ptr<std::vector<u8>> bytes;
    if constexpr (std::is_same_v<T, u8>) {
        bytes = std::make_shared<std::vector<u8>>(std::move(v));
    } else {
        bytes = std::make_shared<std::vector<u8>>(v.size() * sizeof(T));
        if (!bytes->empty()) {
            std::memcpy(bytes->data(), v.data(), bytes->size());
        }
    }
    std::filesystem::path path{path_};
    path.replace_extension(GetBlobFileExtension(type));
    const std::string name = path.filename().string();
    const BlobPtr blob = std::move(bytes);
    journal.Submit(name, blob);
    Enqueue(std::packaged_task<void()>{[type, path, name, blob] {
        if (!journal.Append(name, type, blob)) {
            WriteLooseFile(path, blob->data(), blob->size());
        }
    }});
    return true;
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    if (!EmulatorSettings.IsPipelineCacheArchived() && journal.Active()) {
        return WriteJournaled(type, path_, std::move(v));
    }
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                // sotc: written whole or not at all. The game process is often ended (launcher
                // Stop, a crash) while blobs are being written, and a truncated blob from the
                // last session is read back at the next start.
                WriteLooseFile(path, reinterpret_cast<const u8*>(v.data()), v.size() * sizeof(T));
            }
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

// sotc: a blob this session wrote to the journal (queued, buffered or on disk).
static bool JournalLookup(BlobType type, std::filesystem::path path, std::vector<u8>& data) {
    if (!journal.Active()) {
        return false;
    }
    path.replace_extension(GetBlobFileExtension(type));
    return journal.Lookup(path.filename().string(), data);
}

bool DataBase::MayExist(const std::filesystem::path& path) {
    std::scoped_lock lock{known_mutex};
    return !known_valid || known_files.contains(path.filename().string());
}

void DataBase::AddKnown(const std::filesystem::path& path) {
    std::scoped_lock lock{known_mutex};
    known_files.insert(path.filename().string());
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        AddKnown(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)));
    }
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        AddKnown(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)));
    }
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    if (JournalLookup(type, path, data)) {
        return;
    }
    if (prefetch_active) {
        path.replace_extension(GetBlobFileExtension(type));
        TakePrefetched(path, data);
        return;
    }
    if (!EmulatorSettings.IsPipelineCacheArchived() &&
        !MayExist(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)))) {
        data.clear();
        return;
    }
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    {
        std::vector<u8> bytes;
        if (JournalLookup(type, path, bytes)) {
            data.resize(bytes.size() / sizeof(u32));
            if (!data.empty()) {
                std::memcpy(data.data(), bytes.data(), data.size() * sizeof(u32));
            }
            return;
        }
    }
    if (prefetch_active) {
        path.replace_extension(GetBlobFileExtension(type));
        TakePrefetched(path, data);
        return;
    }
    if (!EmulatorSettings.IsPipelineCacheArchived() &&
        !MayExist(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)))) {
        data.clear();
        return;
    }
    if (!prefetched.empty()) {
        auto file = path;
        file.replace_extension(GetBlobFileExtension(type));
        if (const auto it = prefetched.find(file.filename().string()); it != prefetched.end()) {
            data.resize(it->second.size() / sizeof(u32));
            if (!data.empty()) {
                std::memcpy(data.data(), it->second.data(), data.size() * sizeof(u32));
            }
            return;
        }
    }
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else if (prefetch_active) {
        // sotc: each blob of this type is handed over once, so it is moved out (it was copied)
        // and its map node is dropped before `func` runs: the ~tens of MB of pipeline keys do
        // not stay in RAM, or exist twice, while the preload builds the pipelines. Only this
        // thread touches `prefetched` during the preload, and `func` (the Load calls) never
        // looks for a blob of this type, so erasing here is safe.
        for (auto it = prefetched.begin(); it != prefetched.end();) {
            if (std::filesystem::path{it->first}.extension().string().ends_with(ext)) {
                std::vector<u8> data = std::move(it->second);
                it = prefetched.erase(it);
                func(std::move(data));
            } else {
                ++it;
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
        }
        // sotc: and the blobs this session put in the journal.
        for (const auto& journal_name : journal.Names(ext)) {
            std::vector<u8> data;
            if (journal.Lookup(journal_name, data)) {
                func(std::move(data));
            }
        }
    }
}

size_t DataBase::CountBlobs(BlobType type) {
    const auto& ext = GetBlobFileExtension(type);
    size_t count = 0;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            count += std::string{file_name.data()}.ends_with(ext);
        }
    } else if (prefetch_active) {
        for (const auto& [file_name, data] : prefetched) {
            count += std::filesystem::path{file_name}.extension().string().ends_with(ext);
        }
    } else {
        std::error_code ec;
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path, ec}) {
            count += file_name.path().extension().string().ends_with(ext);
        }
        count += journal.Names(ext).size(); // sotc: this session's journal blobs
    }
    return count;
}

void DataBase::Reset() {
    Close();
    // Open() sets cache_path: a cache that was never opened has nothing to set aside.
    if (!cache_path.empty()) {
        auto old_path = cache_path;
        old_path += ".incompatible";
        std::error_code error;
        std::filesystem::remove_all(old_path, error);
        std::filesystem::rename(cache_path, old_path, error);
        if (error) {
            // Could not be renamed (e.g. a file in use): an empty cache matters more than the copy.
            std::filesystem::remove_all(cache_path, error);
        }
    }
    Open();
}

void DataBase::FinishPreload() {
    // sotc: the preloaded files stay in memory for the session (~220 MB): a load reads them from
    // there. Dropping them sent every later lookup to the disk, one small file at a time, while
    // the game loaded a save from the same hard disk: the game stood still for 27 s (22 Oct).
    prefetch_active = false;
    // sotc: swap with an empty map, so the nodes and every blob buffer are freed right here
    // (a plain `= {}` keeps nothing either, but this states the intent and cannot be turned
    // into a clear() that keeps capacity).
    std::map<std::string, std::vector<u8>>{}.swap(prefetched);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
