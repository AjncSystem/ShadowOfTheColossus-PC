// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: engine hook loader and dispatcher (hooks.h, docs/HOOK-DESIGN.md). Reads out/hooks.bin
// (SOTCHOOK1), resolves each enabled hook by its expected address or by a unique pattern scan,
// installs the detours (Core::InstallGuestDetour) and runs the registered host handlers.
// A hook that does not resolve exactly once, or whose dependency or group fails, is off.
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpu_patches.h"
#include "hook_layout.h"
#include "hook_stats.h"
#include "hooks.h"

namespace SotcHooks {
namespace {

constexpr char kMagic[8] = {'S', 'O', 'T', 'C', 'H', 'O', 'O', 'K'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 8 + 4 + 4 + 32 + 4 + 4;
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kMaxPattern = 4096;
constexpr uint32_t kMaxDeps = 256;
constexpr uint32_t kMaxDerived = 64;

struct DerivedDecl {
    std::string field;
    uint32_t offset{};
    uint32_t size{};
    uint32_t kind{};     // 0 imm, 1 disp, 2 rip_target
    uint32_t insn_end{}; // pattern offset of the end of the instruction
};

struct Record {
    // From the file.
    std::string name;
    std::string switch_name;
    std::string group;
    uint32_t mode{};
    std::vector<uint8_t> pattern;
    std::vector<uint8_t> mask;
    uint64_t expected{};
    uint64_t range_len{};
    uint32_t anchor_kind{};
    uint32_t anchor_index{};
    std::vector<uint32_t> deps;
    std::vector<DerivedDecl> derived_decl;

    // Set at install.
    int switch_value{};
    bool enabled{};           // switch set to a non-zero value (and SOTC_HOOKS not 0)
    std::string off_reason;   // empty while the hook is on
    int resolve_state{};      // 0 not visited, 1 resolving, 2 done
    bool resolved{};          // pattern matched exactly once (or at the expected address)
    bool expected_hit{};      // matched at the expected address
    uint64_t match{};         // image offset of the matched bytes
    std::vector<std::pair<std::string, uint64_t>> derived_values;
    bool installed{};         // detour written (modes 0-2)
    bool active{};            // on, resolved and installed: the handler may run
    bool has_handler{};
    Handler handler{};
};

struct Layout {
    std::vector<CodeRange> code;
    std::vector<uint64_t> function_starts; // sorted, image offsets
};

std::vector<Record> g_records;
std::map<std::string, Handler, std::less<>> g_handlers;
Layout g_layout;
uintptr_t g_image = 0;
uint64_t g_module_size = 0;
bool g_fingerprint_known = false;
std::string g_fingerprint_hex;

// ---------------------------------------------------------------------------
// Reading out/hooks.bin
// ---------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}

    bool Has(size_t n) const { return n <= data_.size() - pos_; }

    bool U32(uint32_t& v) {
        if (!Has(4)) {
            return false;
        }
        v = static_cast<uint32_t>(data_[pos_]) | (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
            (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
            (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }

    bool U64(uint64_t& v) {
        uint32_t lo = 0, hi = 0;
        if (!U32(lo) || !U32(hi)) {
            return false;
        }
        v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
        return true;
    }

    bool Bytes(size_t n, std::vector<uint8_t>& out) {
        if (!Has(n)) {
            return false;
        }
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                   data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return true;
    }

    // A NUL-padded field of n bytes.
    bool Text(size_t n, std::string& out) {
        if (!Has(n)) {
            return false;
        }
        size_t len = 0;
        while (len < n && data_[pos_ + len] != 0) {
            ++len;
        }
        out.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
        pos_ += n;
        return true;
    }

    bool Hex(size_t n, std::string& out) {
        if (!Has(n)) {
            return false;
        }
        static const char kDigits[] = "0123456789abcdef";
        out.clear();
        for (size_t i = 0; i < n; ++i) {
            out.push_back(kDigits[data_[pos_ + i] >> 4]);
            out.push_back(kDigits[data_[pos_ + i] & 15]);
        }
        pos_ += n;
        return true;
    }

    bool Skip(size_t n) {
        if (!Has(n)) {
            return false;
        }
        pos_ += n;
        return true;
    }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_{0};
};

bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }
    bool ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(file) : -1;
    ok = ok && size >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(static_cast<size_t>(size));
        ok = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    return ok;
}

// Parses SOTCHOOK1 (format at the end of hooks.h). Any malformed field rejects the whole file:
// a partly read table would misplace hooks.
bool ParseFile(const std::vector<uint8_t>& data, std::string& error) {
    Reader rd(data);
    if (!rd.Has(kHeaderSize)) {
        error = "short header";
        return false;
    }
    if (std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
        error = "bad magic";
        return false;
    }
    rd.Skip(sizeof kMagic);
    uint32_t version = 0, count = 0, known = 0, reserved = 0;
    rd.U32(version);
    rd.U32(count);
    if (version != kVersion) {
        error = "unsupported version";
        return false;
    }
    if (count > kMaxRecords) {
        error = "too many records";
        return false;
    }
    rd.Hex(32, g_fingerprint_hex);
    rd.U32(known);
    rd.U32(reserved);
    g_fingerprint_known = known != 0;

    std::vector<Record> records;
    records.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Record rec;
        if (!rd.Text(48, rec.name) || !rd.Text(64, rec.switch_name) || !rd.Text(32, rec.group)) {
            error = "truncated record";
            return false;
        }
        uint32_t pattern_len = 0, dep_count = 0, derived_count = 0;
        if (!rd.U32(rec.mode) || !rd.U32(pattern_len) || !rd.U64(rec.expected) ||
            !rd.U64(rec.range_len) || !rd.U32(rec.anchor_kind) || !rd.U32(rec.anchor_index) ||
            !rd.U32(dep_count)) {
            error = "truncated record";
            return false;
        }
        if (rec.name.empty() || rec.mode > 3 || pattern_len == 0 || pattern_len > kMaxPattern ||
            rec.anchor_kind > 1 || dep_count > kMaxDeps) {
            error = "bad record " + rec.name;
            return false;
        }
        rec.deps.resize(dep_count);
        for (uint32_t& dep : rec.deps) {
            if (!rd.U32(dep) || dep >= count) {
                error = "bad dependency in " + rec.name;
                return false;
            }
        }
        if (!rd.U32(derived_count) || derived_count > kMaxDerived) {
            error = "bad derived count in " + rec.name;
            return false;
        }
        for (uint32_t k = 0; k < derived_count; ++k) {
            DerivedDecl dd;
            if (!rd.Text(32, dd.field) || !rd.U32(dd.offset) || !rd.U32(dd.size) ||
                !rd.U32(dd.kind) || !rd.U32(dd.insn_end)) {
                error = "truncated derived field in " + rec.name;
                return false;
            }
            const bool size_ok = dd.size == 1 || dd.size == 2 || dd.size == 4 || dd.size == 8;
            if (!size_ok || dd.kind > 2 || uint64_t{dd.offset} + dd.size > pattern_len ||
                dd.insn_end > pattern_len) {
                error = "bad derived field " + dd.field + " in " + rec.name;
                return false;
            }
            rec.derived_decl.push_back(std::move(dd));
        }
        if (!rd.Bytes(pattern_len, rec.pattern) || !rd.Bytes(pattern_len, rec.mask)) {
            error = "truncated pattern in " + rec.name;
            return false;
        }
        records.push_back(std::move(rec));
    }

    // Anchors name another record; checked once all records are read.
    for (const Record& rec : records) {
        if (rec.anchor_kind == 1) {
            if (rec.anchor_index >= count) {
                error = "bad anchor in " + rec.name;
                return false;
            }
        }
    }

    g_records = std::move(records);
    for (Record& rec : g_records) {
        const auto it = g_handlers.find(rec.name);
        if (it != g_handlers.end()) {
            rec.handler = it->second;
            rec.has_handler = true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Switches
// ---------------------------------------------------------------------------

// The integer value of an environment switch; unset, empty or not a number reads as 0.
int ReadSwitch(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return 0;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end == value ? 0 : static_cast<int>(parsed);
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

// Whether the record's pattern matches the image at the image offset `at`.
bool MatchAt(const Record& rec, uint64_t at) {
    const size_t len = rec.pattern.size();
    if (at > g_module_size || len > g_module_size - at) {
        return false;
    }
    const auto* img = reinterpret_cast<const uint8_t*>(g_image) + at;
    for (size_t k = 0; k < len; ++k) {
        if (((img[k] ^ rec.pattern[k]) & rec.mask[k]) != 0) {
            return false;
        }
    }
    return true;
}

uint64_t SignExtend(uint64_t raw, uint32_t size) {
    if (size >= 8) {
        return raw;
    }
    const unsigned shift = 64 - 8 * size;
    return static_cast<uint64_t>(static_cast<int64_t>(raw << shift) >> shift);
}

// Reads the derived fields from the matched bytes. Immediates are zero-extended, displacements
// sign-extended; rip_target is the image offset of the RIP-relative target.
void Derive(Record& rec) {
    rec.derived_values.clear();
    const auto* img = reinterpret_cast<const uint8_t*>(g_image);
    for (const DerivedDecl& dd : rec.derived_decl) {
        const uint64_t at = rec.match + dd.offset;
        if (at + dd.size > g_module_size) {
            continue;
        }
        uint64_t raw = 0;
        for (uint32_t k = 0; k < dd.size; ++k) {
            raw |= static_cast<uint64_t>(img[at + k]) << (8 * k);
        }
        uint64_t value = raw;
        if (dd.kind == 1) {
            value = SignExtend(raw, dd.size);
        } else if (dd.kind == 2) {
            const uint32_t end = dd.insn_end != 0 ? dd.insn_end : dd.offset + dd.size;
            value = rec.match + end + SignExtend(raw, dd.size);
        }
        rec.derived_values.emplace_back(dd.field, value);
    }
}

void ResolveRecord(size_t index);

// Resolves one enabled record: the expected address when the fingerprint is known, else a
// unique match in the code (inside the anchor's function when it has one).
void ResolveOne(size_t index) {
    Record& rec = g_records[index];
    uint64_t lo = 0;
    uint64_t hi = g_module_size;
    if (rec.anchor_kind == 1) {
        const size_t anchor = rec.anchor_index;
        if (!g_records[anchor].enabled) {
            rec.off_reason = "anchor off";
            return;
        }
        ResolveRecord(anchor);
        if (!g_records[anchor].resolved) {
            rec.off_reason = "anchor not resolved";
            return;
        }
        if (g_layout.function_starts.empty()) {
            rec.off_reason = "anchor: no function table";
            return;
        }
        const uint64_t at = g_records[anchor].match;
        const auto& starts = g_layout.function_starts;
        const auto next = std::upper_bound(starts.begin(), starts.end(), at);
        if (next == starts.begin()) {
            rec.off_reason = "anchor outside functions";
            return;
        }
        lo = *(next - 1);
        hi = next == starts.end() ? g_module_size : *next;
    }

    if (g_fingerprint_known && rec.expected != 0) {
        // sotc: an anchored hook's expected site must also lie inside the anchor's function.
        const bool in_anchor = rec.anchor_kind != 1 ||
                               (rec.expected >= lo && rec.expected + rec.pattern.size() <= hi);
        if (in_anchor && MatchAt(rec, rec.expected)) {
            rec.resolved = true;
            rec.expected_hit = true;
            rec.match = rec.expected;
            Derive(rec);
            return;
        }
        std::fprintf(stderr, "SOTC HOOK %s: bytes at expected 0x%llx differ, scanning\n",
                     rec.name.c_str(), static_cast<unsigned long long>(rec.expected));
    }

    uint64_t found = 0;
    uint32_t count = 0;
    const auto scan = [&](uint64_t begin, uint64_t end) {
        end = std::min(end, g_module_size);
        for (uint64_t at = begin; at + rec.pattern.size() <= end; ++at) {
            if (MatchAt(rec, at)) {
                if (count == 0) {
                    found = at;
                }
                ++count;
            }
        }
    };
    if (rec.anchor_kind == 1) {
        scan(lo, hi);
    } else if (g_layout.code.empty()) {
        scan(0, g_module_size);
    } else {
        for (const CodeRange& range : g_layout.code) {
            scan(range.begin, range.end);
        }
    }

    if (count == 0) {
        rec.off_reason = "pattern not found";
        return;
    }
    if (count > 1) {
        rec.off_reason = "ambiguous: " + std::to_string(count) + " matches";
        return;
    }
    rec.resolved = true;
    rec.expected_hit = false;
    rec.match = found;
    Derive(rec);
}

// Resolves a record once. resolve_state 1 marks a record in progress, so an anchor cycle makes
// the anchored record fail ("anchor not resolved") instead of recursing.
void ResolveRecord(size_t index) {
    Record& rec = g_records[index];
    if (rec.resolve_state != 0) {
        return;
    }
    rec.resolve_state = 1;
    if (rec.enabled) {
        ResolveOne(index);
    }
    rec.resolve_state = 2;
}

// Turns off, until nothing changes: a hook whose dependency is off, and every enabled member
// of a group when one of its enabled members has failed.
void Propagate() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (Record& rec : g_records) {
            if (!rec.enabled || !rec.off_reason.empty()) {
                continue;
            }
            for (const uint32_t dep : rec.deps) {
                const Record& other = g_records[dep];
                if (!other.enabled || !other.off_reason.empty()) {
                    rec.off_reason = "dep " + other.name;
                    changed = true;
                    break;
                }
            }
            // sotc: the anchor is a dependency too: a hook placed by its anchor's function is off
            // when the anchor is.
            if (rec.off_reason.empty() && rec.anchor_kind == 1 &&
                rec.anchor_index < g_records.size()) {
                const Record& anchor = g_records[rec.anchor_index];
                if (!anchor.enabled || !anchor.off_reason.empty()) {
                    rec.off_reason = "anchor " + anchor.name;
                    changed = true;
                }
            }
        }
        for (const Record& failed : g_records) {
            if (failed.group.empty() || !failed.enabled || failed.off_reason.empty()) {
                continue;
            }
            for (Record& member : g_records) {
                if (member.group == failed.group && member.enabled && member.off_reason.empty()) {
                    member.off_reason = "group " + failed.group + ": " + failed.name;
                    changed = true;
                }
            }
        }
    }
}

// Writes the detour of an enabled, resolved hook. On refusal the hook is off with the reason.
void InstallDetour(size_t index) {
    Record& rec = g_records[index];
    Core::DetourRequest request;
    request.address = static_cast<uint64_t>(g_image) + rec.match;
    request.mode = rec.mode == 0   ? Core::DetourMode::Entry
                   : rec.mode == 1 ? Core::DetourMode::Wrap
                                   : Core::DetourMode::Site;
    request.hook_id = static_cast<uint32_t>(index);
    request.pattern = rec.pattern;
    request.mask = rec.mask;
    Core::DetourResult result;
    if (Core::InstallGuestDetour(request, &result) && result.installed) {
        rec.installed = true;
        return;
    }
    rec.off_reason = std::string("detour: ") + (result.reason && *result.reason ? result.reason : "refused");
}

Record* FindRecord(std::string_view name) {
    for (Record& rec : g_records) {
        if (rec.name == name) {
            return &rec;
        }
    }
    for (Record& rec : g_records) {
        if (rec.switch_name == name) {
            return &rec;
        }
    }
    return nullptr;
}

// The manifest's method column: expected, scanned, or off(reason).
std::string ManifestStatus(const Record& rec) {
    if (rec.active) {
        return rec.expected_hit ? "expected" : "scanned";
    }
    return "off(" + (rec.off_reason.empty() ? std::string("unresolved") : rec.off_reason) + ")";
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

struct DispatchCall {
    GuestRegs* regs{};
    uint32_t hook_id{};
    uint32_t phase{};
    uint32_t action{};
};

// sotc: the guest call a pre handler requested with SetGuestCall (hooks.h). Per thread: the
// handler and the stub's sotc_hook_take_guest_call run on the same guest thread.
struct PendingGuestCall {
    bool set{};
    uint64_t words[7]{}; // fn, then the six arguments
};
thread_local PendingGuestCall t_guest_call;

// Runs on the host stack (runtime_call_on_host_stack). Nothing may escape: a throw would unwind
// into the guest's frames.
void RunDispatch(void* arg) {
    auto& call = *static_cast<DispatchCall*>(arg);
    call.action = static_cast<uint32_t>(HookAction::Continue);
    t_guest_call.set = false;
    try {
        if (call.hook_id >= g_records.size()) {
            return;
        }
        const Record& rec = g_records[call.hook_id];
        // sotc: a stub left behind by a hook made inert later (group/dependency) does nothing.
        if (!rec.active) {
            return;
        }
        if (call.phase == static_cast<uint32_t>(Phase::Pre)) {
            HookAction action = HookAction::Continue;
            if (rec.has_handler && rec.handler.pre) {
                action = rec.handler.pre(*call.regs, rec.handler.user);
            }
            // sotc: Return is only valid at a function entry (hooks.h).
            if (action == HookAction::Return && rec.mode != 0) {
                action = HookAction::Continue;
            }
            // sotc: CallGuest needs the call the handler set (hooks.h); the stub runs it.
            if (action == HookAction::CallGuest && !t_guest_call.set) {
                action = HookAction::Continue;
            }
            SotcHookStats::CountPre(call.hook_id, action == HookAction::Return);
            call.action = static_cast<uint32_t>(action);
        } else if (call.phase == static_cast<uint32_t>(Phase::Post)) {
            if (rec.has_handler && rec.handler.post) {
                rec.handler.post(*call.regs, rec.handler.user);
            }
            SotcHookStats::CountPost(call.hook_id);
        }
    } catch (...) {
        call.action = static_cast<uint32_t>(HookAction::Continue);
    }
    // A pending call survives only into the CallGuest return it belongs to.
    if (call.action != static_cast<uint32_t>(HookAction::CallGuest)) {
        t_guest_call.set = false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void RegisterHandler(std::string_view name, const Handler& handler) {
    g_handlers.insert_or_assign(std::string(name), handler);
    for (Record& rec : g_records) {
        if (rec.name == name) {
            rec.handler = handler;
            rec.has_handler = true;
        }
    }
}

void SetCodeLayout(std::span<const CodeRange> code, std::span<const uint64_t> function_starts) {
    g_layout.code.assign(code.begin(), code.end());
    g_layout.function_starts.assign(function_starts.begin(), function_starts.end());
    std::sort(g_layout.function_starts.begin(), g_layout.function_starts.end());
    g_layout.function_starts.erase(
        std::unique(g_layout.function_starts.begin(), g_layout.function_starts.end()),
        g_layout.function_starts.end());
}

bool InstallHooks(const char* hooks_path, void* image, uint64_t module_size, InstallStats* stats) {
    InstallStats totals{};
    if (stats) {
        *stats = {};
    }
    g_image = reinterpret_cast<uintptr_t>(image);
    g_module_size = module_size;
    g_records.clear();
    g_fingerprint_known = false;
    g_fingerprint_hex.clear();

    if (!hooks_path || !*hooks_path) {
        std::fprintf(stderr, "SOTC HOOKS: no hooks file\n");
        return false;
    }
    std::vector<uint8_t> data;
    if (!ReadFile(hooks_path, data)) {
        std::fprintf(stderr, "SOTC HOOKS: cannot read %s\n", hooks_path);
        return false;
    }
    std::string error;
    if (!ParseFile(data, error)) {
        std::fprintf(stderr, "SOTC HOOKS: %s rejected: %s\n", hooks_path, error.c_str());
        return false;
    }
    std::fprintf(stderr, "SOTC HOOKS: %zu hooks, fingerprint %s (%s)\n", g_records.size(),
                 g_fingerprint_hex.c_str(), g_fingerprint_known ? "known" : "unknown");

    // Switches are read once, here.
    const char* master = std::getenv("SOTC_HOOKS");
    const bool master_off = master && *master && ReadSwitch("SOTC_HOOKS") == 0;
    std::vector<std::string> names;
    for (Record& rec : g_records) {
        rec.switch_value = ReadSwitch(rec.switch_name.c_str());
        rec.enabled = !master_off && rec.switch_value != 0;
        if (master_off) {
            rec.off_reason = "SOTC_HOOKS=0";
        } else if (!rec.enabled) {
            rec.off_reason = "switch";
        }
        names.push_back(rec.name);
    }
    SotcHookStats::Configure(names);

    // Resolution, then dependencies and groups, then the detours. A detour refused after an
    // earlier member of its group was written leaves that member inert: its dispatch sees
    // active == false and returns HookAction::Continue, so the original code still runs.
    for (size_t i = 0; i < g_records.size(); ++i) {
        ResolveRecord(i);
    }
    Propagate();
    for (size_t i = 0; i < g_records.size(); ++i) {
        const Record& rec = g_records[i];
        if (rec.enabled && rec.off_reason.empty() && rec.resolved && rec.mode != 3 &&
            !rec.installed) {
            InstallDetour(i);
            Propagate();
        }
    }

    uint32_t installed = 0;
    for (Record& rec : g_records) {
        rec.active = rec.enabled && rec.off_reason.empty() && rec.resolved;
        if (rec.active) {
            ++installed;
        }
    }
    // sotc: one manifest line per enabled hook; hooks whose switch is unset are listed only with
    // SOTC_HOOKS_LOG=1 (every boot would print them otherwise).
    static const bool log_all = [] {
        const char* v = std::getenv("SOTC_HOOKS_LOG");
        return v && v[0] == '1';
    }();
    for (const Record& rec : g_records) {
        if (!rec.enabled && !log_all) {
            continue;
        }
        std::fprintf(stderr, "SOTC HOOK %s 0x%llx %s %s=%d\n", rec.name.c_str(),
                     static_cast<unsigned long long>(rec.resolved ? rec.match : 0),
                     ManifestStatus(rec).c_str(), rec.switch_name.c_str(), rec.switch_value);
    }
    totals.installed = installed;
    totals.off = g_records.size() - installed;
    std::fprintf(stderr, "SOTC HOOKS: %llu installed, %llu off, trampoline %llu bytes\n",
                 static_cast<unsigned long long>(totals.installed),
                 static_cast<unsigned long long>(totals.off),
                 static_cast<unsigned long long>(Core::TrampolineBytesUsed()));
    if (stats) {
        *stats = totals;
    }
    return true;
}

uintptr_t ImageBase() {
    return g_image;
}

bool IsInstalled(std::string_view name) {
    const Record* rec = FindRecord(name);
    return rec && rec->active;
}

int HookMode(std::string_view hook) {
    const Record* rec = FindRecord(hook);
    return rec ? rec->switch_value : 0;
}

bool Derived(std::string_view hook, std::string_view field, uint64_t* value) {
    const Record* rec = FindRecord(hook);
    if (!rec || !rec->active) {
        return false;
    }
    for (const auto& [name, v] : rec->derived_values) {
        if (name == field) {
            if (value) {
                *value = v;
            }
            return true;
        }
    }
    return false;
}

bool ResolvedRange(std::string_view hook, uint64_t* begin, uint64_t* end) {
    const Record* rec = FindRecord(hook);
    // sotc: the range must be non-empty and inside the module.
    if (!rec || !rec->active || rec->mode != 3 || rec->range_len == 0 ||
        rec->match > g_module_size || rec->range_len > g_module_size - rec->match) {
        return false;
    }
    if (begin) {
        *begin = rec->match;
    }
    if (end) {
        *end = rec->match + rec->range_len;
    }
    return true;
}

} // namespace SotcHooks

extern "C" uint32_t sotc_hook_dispatch(SotcHooks::GuestRegs* regs, uint32_t hook_id, uint32_t phase) {
    SotcHooks::DispatchCall call{regs, hook_id, phase, 0};
    runtime_call_on_host_stack(SotcHooks::RunDispatch, &call);
    return call.action;
}

namespace SotcHooks {

void SetGuestCall(uint64_t fn, const uint64_t args[6]) {
    t_guest_call.words[0] = fn;
    std::memcpy(&t_guest_call.words[1], args, 6 * sizeof(uint64_t));
    t_guest_call.set = true;
}

} // namespace SotcHooks

// Runs on the guest stack from the detour stub (hooks.h). Must not throw: it only copies.
extern "C" uint32_t sotc_hook_take_guest_call(uint64_t out[7]) {
    if (!SotcHooks::t_guest_call.set) {
        return 0;
    }
    std::memcpy(out, SotcHooks::t_guest_call.words, sizeof(SotcHooks::t_guest_call.words));
    SotcHooks::t_guest_call.set = false;
    return 1;
}
