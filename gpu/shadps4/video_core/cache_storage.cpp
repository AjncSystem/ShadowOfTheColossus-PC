// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <algorithm>
#include <atomic>
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
        // do not tell them apart: the launcher's DMA switch gets its own cache folder.
        cache_path = GetUserPath(PathType::CacheDir) /
                     (std::string{game_info.GameSerial()} +
                      (EmulatorSettings.IsDirectMemoryAccessEnabled() ? "" : "_nodma"));
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        Prefetch();
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

// sotc: the boot preload opened ~12k small files one by one (~21 s of a 22 s warm-up on Windows,
// where each open is slow and scanned). Read them all in parallel first; Load and ForEachBlob
// take them from memory until FinishPreload.
void DataBase::Prefetch() {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() != ".tmp") {
            files.push_back(entry.path());
        }
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

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
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

void DataBase::FinishPreload() {
    prefetch_active = false;
    prefetched = {};
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
