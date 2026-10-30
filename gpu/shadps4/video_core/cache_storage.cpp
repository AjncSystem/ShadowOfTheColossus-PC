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

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

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

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

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
    std::FILE* f = Common::FS::OpenCFile(path, Common::FS::FileAccessMode::Read);
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

static void WriteCachePack(std::filesystem::path path,
                           std::vector<std::pair<std::string, std::vector<u8>>> files) {
    auto tmp = path;
    tmp += L".tmp";
    std::FILE* f = Common::FS::OpenCFile(tmp, Common::FS::FileAccessMode::Create);
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
    std::error_code ec;
    size_t in_pack = 0, on_disk = 0;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        const auto ext = entry.path().extension();
        if (!entry.is_regular_file(ec) || ext == ".tmp" || ext == ".sotcpack") {
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
    // Pack entries whose loose file is gone (a cleared cache) are dropped with `packed`.
    {
        // sotc: the same listing, kept for MayExist once prefetch_active ends below.
        std::scoped_lock lock{known_mutex};
        known_files.clear();
        for (const auto& file : files) {
            known_files.insert(file.filename().string());
        }
        for (const auto& [name, data] : prefetched) { // taken from the pack, on disk too
            known_files.insert(name);
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
    prefetch_active = true;
    if (CachePackEnabled() && (!files.empty() || !packed.empty())) {
        std::vector<std::pair<std::string, std::vector<u8>>> copy;
        copy.reserve(prefetched.size());
        for (const auto& [name, data] : prefetched) {
            copy.emplace_back(name, data);
        }
        std::thread{WriteCachePack, pack_path, std::move(copy)}.detach();
    }
    std::fprintf(stderr, "SOTC shader cache: %zu files, %zu from the pack, %zu read one by one\n",
                 on_disk, in_pack, files.size());
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
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
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
                using namespace Common::FS;
                // sotc: written whole or not at all. The game process is often ended (launcher
                // Stop, a crash) while blobs are being written, and a truncated blob from the
                // last session is read back at the next start.
                auto temporary = path;
                temporary += ".tmp";
                {
                    const auto file = IOFile{temporary, FileAccessMode::Create};
                    file.Write(v);
                }
                std::error_code error;
                std::filesystem::rename(temporary, path, error);
                if (error) {
                    std::filesystem::remove(temporary, error);
                }
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
    if (prefetch_active) {
        path.replace_extension(GetBlobFileExtension(type));
        TakePrefetched(path, data);
        return;
    }
    if (!prefetched.empty()) {
        auto file = path;
        file.replace_extension(GetBlobFileExtension(type));
        if (const auto it = prefetched.find(file.filename().string()); it != prefetched.end()) {
            data = it->second;
            return;
        }
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
    if (prefetch_active) {
        path.replace_extension(GetBlobFileExtension(type));
        std::vector<u8> bytes;
        TakePrefetched(path, bytes);
        data.resize(bytes.size() / sizeof(u32));
        if (!data.empty()) {
            std::memcpy(data.data(), bytes.data(), data.size() * sizeof(u32));
        }
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
    if (!EmulatorSettings.IsPipelineCacheArchived() &&
        !MayExist(std::filesystem::path{path}.replace_extension(GetBlobFileExtension(type)))) {
        data.clear();
        return;
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
        for (const auto& [file_name, data] : prefetched) {
            if (std::filesystem::path{file_name}.extension().string().ends_with(ext)) {
                func(std::vector<u8>{data});
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
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
