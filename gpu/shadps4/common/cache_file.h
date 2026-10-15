// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Common::FS {

// Publish only a complete file. Distinct temporary names allow concurrent sessions;
// replacing an existing file on Windows requires MoveFileEx, not CRT rename.
inline bool WriteCacheFile(const std::filesystem::path& path, std::span<const unsigned char> data) {
    static std::atomic<unsigned long long> sequence{};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    auto temporary = path;
    temporary += ".tmp." + std::to_string(pid) + "." + std::to_string(sequence.fetch_add(1));
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.close();
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
#ifdef _WIN32
    const bool published = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    const bool published = !error;
#endif
    if (!published) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return published;
}

} // namespace Common::FS
