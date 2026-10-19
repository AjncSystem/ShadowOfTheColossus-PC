// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    void Clear();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// Sets the current cache aside (renamed with ".incompatible") and opens an empty one.
    void Reset();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);
    /// sotc: how many blobs of a type there are (the loading screen's progress).
    size_t CountBlobs(BlobType type);

private:
    void Prefetch();
    bool TakePrefetched(const std::filesystem::path& path, std::vector<u8>& data);

    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    bool opened{};
    /// sotc: every loose cache file read up front (in parallel) for the boot preload, by file
    /// name; ordered like an NTFS directory listing. Emptied by FinishPreload.
    std::map<std::string, std::vector<u8>> prefetched{};
    bool prefetch_active{};
};

} // namespace Storage
