// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: registry of live SOTC_* switches (see sotc_live.h).
#include "sotc_live.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace SotcLive {

// Befriended by Int: the only code that touches the registry map.
struct Registry {
    std::mutex mutex;
    std::map<std::string, Int*, std::less<>> by_name; // sorted, so Snapshot() is already ordered

    static Registry& Get() {
        // Function-local static: constructed on first use, so Int constructors that run during
        // static initialisation of other translation units can register safely.
        static Registry instance;
        return instance;
    }
    static void Store(Int* live, int value) {
        live->value_.store(value, std::memory_order_relaxed);
    }
};

namespace {

/// Parses a base-10 integer covering the whole text. Returns false on any other text.
bool ParseInt(std::string_view text, int* out) {
    if (text.empty()) {
        return false;
    }
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (errno != 0 || end == copy.c_str() || *end != '\0') {
        return false;
    }
    if (value < INT_MIN || value > INT_MAX) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

} // namespace

Int::Int(const char* name, int def) : name_(name), def_(def), value_(def) {
    int parsed = 0;
    const char* env = std::getenv(name);
    if (env && *env && ParseInt(env, &parsed)) {
        value_.store(parsed, std::memory_order_relaxed);
    }
    Registry& registry = Registry::Get();
    std::lock_guard lock(registry.mutex);
    registry.by_name[name] = this;
}

SetResult Set(std::string_view name, std::string_view value) {
    Registry& registry = Registry::Get();
    Int* found = nullptr;
    {
        std::lock_guard lock(registry.mutex);
        const auto it = registry.by_name.find(name);
        if (it == registry.by_name.end()) {
            return SetResult::NotLive;
        }
        found = it->second;
    }
    int parsed = found->default_value();
    if (!value.empty() && !ParseInt(value, &parsed)) {
        return SetResult::BadValue;
    }
    Registry::Store(found, parsed);
    std::fprintf(stderr, "SOTC LIVE %s=%d\n", found->name(), parsed);
    return SetResult::Ok;
}

bool IsLive(std::string_view name) {
    Registry& registry = Registry::Get();
    std::lock_guard lock(registry.mutex);
    return registry.by_name.find(name) != registry.by_name.end();
}

bool Get(std::string_view name, int* out) {
    Registry& registry = Registry::Get();
    std::lock_guard lock(registry.mutex);
    const auto it = registry.by_name.find(name);
    if (it == registry.by_name.end()) {
        return false;
    }
    *out = it->second->get();
    return true;
}

std::vector<Entry> Snapshot() {
    Registry& registry = Registry::Get();
    std::lock_guard lock(registry.mutex);
    std::vector<Entry> entries;
    entries.reserve(registry.by_name.size());
    for (const auto& [name, live] : registry.by_name) {
        entries.push_back(Entry{name, live->get(), live->default_value()});
    }
    return entries;
}

} // namespace SotcLive
