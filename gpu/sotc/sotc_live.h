// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_* switches that can change while the game runs (SotC Lab, docs/sotc/LAB.md).
// A live switch is a SotcLive::Int defined at namespace scope; it registers itself during static
// initialisation, reads its environment variable once, and is read with a relaxed atomic load on
// every use. Only switches that meet every live-safe rule may be migrated (launcher/sotc_tuning.py,
// LIVE dict): not part of a pipeline/shader key, recompiler output or descriptor layout; not used at
// device or swapchain creation; no allocation sizing at init; read on the use path.
//
//   // before:  static const bool kFoo = [] { const char* v = getenv("SOTC_FOO"); ... }();
//   // after:   static SotcLive::Int kFoo{"SOTC_FOO", 0};   ...   if (kFoo.get()) { ... }
#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace SotcLive {

class Int {
public:
    /// name: the environment variable; def: the value when it is unset or empty. Values parse as
    /// base-10 integers ("1"/"0" for flags). Register at namespace scope only.
    Int(const char* name, int def);
    int get() const {
        return value_.load(std::memory_order_relaxed);
    }
    explicit operator bool() const {
        return get() != 0;
    }
    const char* name() const {
        return name_;
    }
    int default_value() const {
        return def_;
    }

private:
    friend struct Registry;
    const char* name_;
    int def_;
    std::atomic<int> value_;
};

enum class SetResult {
    Ok,
    NotLive,  ///< known switch, but it needs a restart
    BadValue, ///< not an integer
};

/// Sets a live switch from text ("" = back to its default). Any thread. Logs
/// "SOTC LIVE <NAME>=<value>" on success.
SetResult Set(std::string_view name, std::string_view value);
bool IsLive(std::string_view name);
/// Current value of a live switch; false when the name is not live.
bool Get(std::string_view name, int* out);

struct Entry {
    std::string name;
    int value;
    int def;
};
/// Every registered live switch, sorted by name.
std::vector<Entry> Snapshot();

} // namespace SotcLive
