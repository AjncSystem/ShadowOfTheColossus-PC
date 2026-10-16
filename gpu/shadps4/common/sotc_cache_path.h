// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <string_view>

namespace Common::FS {
inline std::filesystem::path SotcCachePath(const std::filesystem::path& user,
                                         std::string_view configured, bool portable,
                                         std::string_view local_app_data) {
    if (!configured.empty()) return std::filesystem::path(configured);
    if (!portable && !local_app_data.empty())
        return std::filesystem::path(local_app_data) / "ShadowOfTheColossus-PC" / "cache";
    return user / "cache";
}
} // namespace Common::FS
