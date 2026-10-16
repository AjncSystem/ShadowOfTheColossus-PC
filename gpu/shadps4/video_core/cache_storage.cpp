// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/cache_file.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_driver_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

namespace {

std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};
bool accept_requests{}; // protected by m_request

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    for (;;) {
        std::packaged_task<void()> request{};
        {
            std::unique_lock lock{m_request};
            Common::CondvarWait(request_cv, lock, stoken, [&] { return !req_queue.empty(); });
            // Stop accepting work before Close; drain every accepted request before joining.
            if (req_queue.empty()) {
                if (stoken.stop_requested()) break;
                continue;
            }
            request = std::move(req_queue.front());
            req_queue.pop();
        }
        request();
        try {
            request.get_future().get();
        } catch (const std::exception& error) {
            LOG_ERROR(Render, "Cache write failed: {}", error.what());
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
    case BlobType::DriverBinary: {
        return "vkcache";
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
    }

    io_worker = std::jthread{ProcessIO};
    {
        std::scoped_lock lock{m_request};
        accept_requests = true;
    }
    opened = true;
    LOG_INFO(Render, "Pipeline cache directory: {}", cache_path.string());
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    {
        std::scoped_lock lock{m_request};
        accept_requests = false;
    }
    opened = false;
    io_worker.request_stop();
    request_cv.notify_all();
    io_worker.join(); // ProcessIO drains every accepted request before it leaves

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    LOG_INFO(Render, "Cache dumped");
}

bool DataBase::Reset() {
    Close();
    auto preserved = cache_path;
    preserved += ".incompatible." + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code error;
    std::filesystem::rename(cache_path, preserved, error);
    if (error) {
        LOG_WARNING(Render, "Cannot preserve incompatible cache {}: {}", cache_path.string(),
                    error.message());
        return false;
    }
    LOG_WARNING(Render, "Preserved incompatible cache at {}; rebuilding", preserved.string());
    Open();
    FinishPreload();
    return true;
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[path_ = std::move(path_), v = std::move(v), type]() {
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
                auto bytes = std::span<const u8>{reinterpret_cast<const u8*>(v.data()),
                                                 v.size() * sizeof(T)};
                std::vector<u8> driver_file;
                if (type == BlobType::DriverBinary) {
                    // Checksum/envelope creation runs on the IO worker, not the GPU thread.
                    driver_file = Vulkan::DriverCache::Encode(bytes);
                    if (driver_file.empty()) return;
                    bytes = driver_file;
                }
                if (!WriteCacheFile(path, bytes)) {
                    LOG_WARNING(Render, "Could not publish cache file {}", path.string());
                }
            }
        }};
        std::scoped_lock lock{m_request};
        if (!accept_requests) return false;
        req_queue.emplace(std::move(request));
    }

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
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return;
        const auto file = IOFile{path, FileAccessMode::Read};
        if (!file.IsOpen()) return;
        const auto size = file.GetSize();
        if (size > 256 * 1024 * 1024 + 24 || size % sizeof(T)) {
            LOG_WARNING(Render, "Ignoring oversized/misaligned cache file {}", path.string());
            return;
        }
        v.resize(size / sizeof(T));
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
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
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
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
