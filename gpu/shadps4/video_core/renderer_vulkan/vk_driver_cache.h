// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <xxhash.h>

namespace Vulkan::DriverCache {

struct Identity {
    std::uint32_t vendor;
    std::uint32_t device;
    std::array<unsigned char, 16> uuid;
};

inline constexpr std::size_t MaxBytes = 256 * 1024 * 1024;
inline constexpr std::array<unsigned char, 8> Magic{'S', 'O', 'T', 'C', 'V', 'K', 'C', '1'};

// Vulkan header version one is 32 bytes, in little endian. Never pass an unverified
// opaque payload to a driver: UUID/device checks alone cannot detect truncation/bit rot.
inline bool Matches(std::span<const unsigned char> data, const Identity& id) {
    if (data.size() < 32 || data.size() > MaxBytes) return false;
    const auto word = [&](std::size_t offset) {
        std::uint32_t value{};
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    };
    return word(0) == 32 && word(4) == 1 &&
           word(8) == id.vendor && word(12) == id.device &&
           std::memcmp(data.data() + 16, id.uuid.data(), id.uuid.size()) == 0;
}

inline std::vector<unsigned char> Encode(std::span<const unsigned char> payload) {
    if (payload.size() < 32 || payload.size() > MaxBytes) return {};
    std::vector<unsigned char> file(24 + payload.size());
    const std::uint64_t size = payload.size();
    const std::uint64_t hash = XXH3_64bits(payload.data(), payload.size());
    std::memcpy(file.data(), Magic.data(), Magic.size());
    std::memcpy(file.data() + 8, &size, 8);
    std::memcpy(file.data() + 16, &hash, 8);
    std::memcpy(file.data() + 24, payload.data(), payload.size());
    return file;
}

inline std::vector<unsigned char> Decode(std::span<const unsigned char> file, const Identity& id) {
    if (file.size() < 56 || file.size() > MaxBytes + 24 ||
        std::memcmp(file.data(), Magic.data(), Magic.size()) != 0) return {};
    std::uint64_t size{}, hash{};
    std::memcpy(&size, file.data() + 8, 8);
    std::memcpy(&hash, file.data() + 16, 8);
    auto payload = file.subspan(24);
    if (size != payload.size() || !Matches(payload, id) ||
        hash != XXH3_64bits(payload.data(), payload.size())) return {};
    return {payload.begin(), payload.end()};
}

} // namespace Vulkan::DriverCache
