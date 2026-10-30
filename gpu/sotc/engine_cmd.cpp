// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: live engine console commands (gpu/sotc/engine_cmd.h, docs/re-stutters/engine-commands.md).
//
// The engine's in-game console runs one line through sub_3c9c80(console, bpe::String*): it echoes the
// line, splits it into bpe::Command objects and dispatches them to the registered ConCommand handlers.
// sub_3c7740 is the console tick, called once per frame from GameCoreLoop::GameLoop on the update
// thread (hook ENGINE_CMD_TICK). The tick takes the next queued line and returns HookAction::CallGuest
// for sub_3c9c80, so each line runs exactly as if typed into the console, on the update thread.
// Output is taken from sub_3c9a10 (the console's line append, hook ENGINE_CMD_PRINT).
//
// The bpe::String handed to the guest is a non-owning view (data, length, 0, 0), the layout that
// sub_0000f2f0 builds from a C string: no refcount block, so the guest never frees it. Its text lives
// in a block committed below 2 GiB (the game keeps 31-bit pointers), not in the guest allocator: the
// guest reads it with ordinary loads. Inferred from the String layout; not yet confirmed in game.

#include "engine_cmd.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "guest_mem.h"
#include "freecam.h"
#include "hooks.h"

extern "C" const char* runtime_file_user_dir(void);

namespace SotcEngineCmd {
namespace {

// Image offset of the console line executor in the eboot (EU 1.01 fingerprint, see
// scripts/hooks.d/enginecmd.py). sub_3c9c80(console, bpe::String*).
constexpr uint64_t kConsoleLine = 0x3c9c80;

constexpr size_t kMaxLine = 480;
constexpr size_t kMaxQueue = 64;
constexpr size_t kOutputKeep = 200;
// Frames (60 Hz) to wait after a jump before the horse warp, and after the warp before killBoss.
// Inferred from the PR #345 boot flow; the engine's area loader idle state is not read yet.
constexpr uint64_t kAreaLoadFrames = 600;
constexpr uint64_t kKillFrames = 300;

struct Pending {
    uint64_t due;  // frame number at or after which the line may run
    std::string line;
    bool boss;     // part of a RequestBoss sequence (BossJumpPending)
};

std::mutex g_mu;                   // queue, output and sequence state
std::deque<Pending> g_queue;       // FIFO; the head blocks until due
std::deque<std::string> g_output;  // console output and echoed lines, newest last

std::atomic<uint64_t> g_console{0};  // the console object, from the last tick
std::atomic<uint64_t> g_frame{0};    // ticks seen

// The String view and its text. The game keeps 31-bit pointers, so they live in one block committed
// below 2 GiB (LowBuffer), never in this executable's own data. Written only by the tick handler on
// the update thread, immediately before the guest call reads them.
struct LowBlock {
    uint64_t str[4];
    char text[kMaxLine + 16];
};

// Commits one 64 KiB block at the highest free address below 2 GiB (top-down scan, outside what the
// loader already reserved). nullptr when none is free: live commands then stay off.
LowBlock* LowBuffer() {
    static LowBlock* const block = []() -> LowBlock* {
#ifdef _WIN32
        for (uintptr_t a = 0x7fff0000u; a >= 0x01000000u; a -= 0x10000u) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), 0x10000, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
            if (!p) {
                continue;
            }
            if (reinterpret_cast<uintptr_t>(p) + 0x10000 <= 0x80000000u) {
                return static_cast<LowBlock*>(p);
            }
            VirtualFree(p, 0, MEM_RELEASE);
        }
#endif
        std::fprintf(stderr, "SOTC ENGINECMD: no free block below 2 GiB, live commands off\n");
        return nullptr;
    }();
    return block;
}

const Colossus kColossi[kColossusCount] = {
    {"Valus", false},   {"Quadratus", false}, {"Gaius", false},   {"Phaedra", false},
    {"Avion", false},   {"Barba", false},     {"Hydrus", true},   {"Kuromori", false},
    {"Basaran", false}, {"Dirge", false},     {"Celosia", false}, {"Pelagia", true},
    {"Phalanx", false}, {"Cenobia", false},   {"Argus", false},   {"Malus", false},
};

void AddOutputLocked(std::string text) {
    if (text.size() > 1024) {
        text.resize(1024);
    }
    g_output.push_back(std::move(text));
    while (g_output.size() > kOutputKeep) {
        g_output.pop_front();
    }
}

// Tick handler (ENGINE_CMD_TICK, entry, update thread). rdi is the console (first integer argument;
// xmm0 holds the frame dt).
SotcHooks::HookAction OnTick(SotcHooks::GuestRegs& regs, void*) {
    g_console.store(regs.rdi, std::memory_order_relaxed);
    const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

    std::string line;
    bool quiet = false; // free-cam lines: one per frame, not logged
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_queue.empty() && g_queue.front().due <= frame) {
            Pending pending = std::move(g_queue.front());
            g_queue.pop_front();
            line = std::move(pending.line);
            AddOutputLocked("> " + line);
        }
    }
    if (line.empty()) {
        // Nothing queued is due: the free camera may run its line for this frame. xmm0 holds the
        // console tick's frame dt (float).
        float dt = 0.0f;
        std::memcpy(&dt, &regs.xmm[0][0], sizeof dt);
        if (!SotcFreecam::EngineFrameLine(dt, &line) || line.empty()) {
            return SotcHooks::HookAction::Continue;
        }
        quiet = line.compare(0, 13, "SetCameraPos ") == 0 || line.compare(0, 13, "SetPlayerPos ") == 0;
    }
    if (line.size() > kMaxLine) {
        return SotcHooks::HookAction::Continue;
    }

    LowBlock* low = LowBuffer();
    if (!low) {
        return SotcHooks::HookAction::Continue;
    }
    if (!quiet) {
        std::fprintf(stderr, "SOTC ENGINECMD %s\n", line.c_str());
    }
    std::memcpy(low->text, line.data(), line.size());
    low->text[line.size()] = '\0';
    low->str[0] = reinterpret_cast<uint64_t>(low->text); // data
    low->str[1] = line.size();                           // length
    low->str[2] = 0;                                     // no owned capacity
    low->str[3] = 0; // no refcount block: the guest will not free it

    const uint64_t args[6] = {regs.rdi, reinterpret_cast<uint64_t>(low->str), 0, 0, 0, 0};
    SotcHooks::SetGuestCall(SotcHooks::ImageBase() + kConsoleLine, args);
    return SotcHooks::HookAction::CallGuest;
}

// Print handler (ENGINE_CMD_PRINT, entry). rsi is a bpe::String*: data pointer at +0, length at +8.
// Observe only: copies the text into the output ring and continues the original.
SotcHooks::HookAction OnPrint(SotcHooks::GuestRegs& regs, void*) {
    uint64_t data = 0, length = 0;
    if (!SotcGuest::Read(regs.rsi, &data) || !SotcGuest::Read(regs.rsi + 8, &length) || length == 0 ||
        length > 4096) {
        return SotcHooks::HookAction::Continue;
    }
    std::string text(static_cast<size_t>(length), '\0');
    if (!SotcGuest::Read(data, text.data(), text.size())) {
        return SotcHooks::HookAction::Continue;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        if (end > start) {
            AddOutputLocked(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return SotcHooks::HookAction::Continue;
}

// The game's stdout (src/runtime_file.c): keeps only console replies the lab parses ("... Position:
// Set..." from printPlayerPos / PrintCameraPos), so the rest of the log costs one short scan.
void TakeStdout(const char* text, size_t size) {
    static constexpr char kKey[] = "Position: Set";
    constexpr size_t n = sizeof(kKey) - 1;
    if (size < n || size > 4096) {
        return;
    }
    bool found = false;
    for (size_t i = 0; i + n <= size; ++i) {
        if (text[i] == 'P' && std::memcmp(text + i, kKey, n) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    std::string line(text, size);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    std::lock_guard<std::mutex> lock(g_mu);
    AddOutputLocked(std::move(line));
}

std::string Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

} // namespace

void Register() {
    SotcHooks::Handler tick{};
    tick.pre = OnTick;
    SotcHooks::RegisterHandler("ENGINE_CMD_TICK", tick);

    SotcHooks::Handler print{};
    print.pre = OnPrint;
    SotcHooks::RegisterHandler("ENGINE_CMD_PRINT", print);
}

bool Available() {
    return SotcHooks::IsInstalled("ENGINE_CMD_TICK");
}

bool Queue(std::string_view line, std::string* err) {
    if (!Available()) {
        if (err) *err = "engine command hook is not installed (SOTC_ENGINE_CMD=1 and a resolved tick)";
        return false;
    }
    if (g_console.load(std::memory_order_relaxed) == 0) {
        if (err) *err = "the console has not ticked yet";
        return false;
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' ||
                             line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        if (err) *err = "empty command line";
        return false;
    }
    if (line.size() > kMaxLine) {
        if (err) *err = "command line is longer than 480 bytes";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_queue.size() >= kMaxQueue) {
        if (err) *err = "command queue is full";
        return false;
    }
    g_queue.push_back({g_frame.load(std::memory_order_relaxed) + 1, std::string(line), false});
    return true;
}

std::vector<std::string> RecentOutput() {
    std::lock_guard<std::mutex> lock(g_mu);
    return std::vector<std::string>(g_output.begin(), g_output.end());
}

const Colossus* Colossi() {
    return kColossi;
}

bool RequestBoss(int n, BossMode mode, bool horse, std::string* err) {
    if (n < 1 || n > kColossusCount) {
        if (err) *err = "colossus must be 1..16";
        return false;
    }
    if (!Available()) {
        if (err) *err = "live jumps are unavailable (engine command hook not installed)";
        return false;
    }
    if (mode != BossMode::Play) {
        std::string backup_err;
        if (BackupSave(&backup_err).empty()) {
            if (err) *err = "save backup failed, nothing was changed: " + backup_err;
            return false;
        }
    }

    const bool water = kColossi[n - 1].water_arena;
    const uint64_t base = g_frame.load(std::memory_order_relaxed) + 1;
    const std::string index = std::to_string(n - 1);
    std::lock_guard<std::mutex> lock(g_mu);
    const size_t steps = 1 + (horse && !water ? 1 : 0) + (mode == BossMode::Kill ? 1 : 0);
    if (g_queue.size() + steps > kMaxQueue) {
        if (err) *err = "command queue is full";
        return false;
    }
    if (mode == BossMode::Outro) {
        g_queue.push_back({base, "boss_outro " + index, true});
    } else {
        g_queue.push_back({base, "shadow_jumpToBoss " + index, true});
    }
    uint64_t due = base + kAreaLoadFrames;
    if (horse && !water) {
        g_queue.push_back({due, "shadow_horseDebugWarpToPlayer", true});
        due += kKillFrames;
    }
    if (mode == BossMode::Kill) {
        g_queue.push_back({due, "killBoss", true});
    }
    return true;
}

bool BossJumpPending() {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const Pending& pending : g_queue) {
        if (pending.boss) return true;
    }
    return false;
}

std::string BackupSave(std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path user = runtime_file_user_dir();
    const fs::path savedata = user / "savedata";
    const fs::path target = user / "save-backups" / Timestamp();

    bool copied = false;
    for (const fs::directory_entry& uid : fs::directory_iterator(savedata, ec)) {
        if (!uid.is_directory(ec)) continue;
        // Every title folder of this user (the port runs one game, so this is its save).
        for (const fs::directory_entry& title : fs::directory_iterator(uid.path(), ec)) {
            if (!title.is_directory(ec)) continue;
            const fs::path src = title.path();
            fs::copy(src, target / uid.path().filename() / src.filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                if (err) *err = "copy failed: " + ec.message();
                return "";
            }
            copied = true;
        }
    }
    if (!copied) {
        if (err) *err = "no save folder under " + savedata.string();
        return "";
    }
    return target.string();
}

} // namespace SotcEngineCmd

// sotc: called by src/runtime_file.c for every write of the game to stdout/stderr (weak there).
extern "C" void bbgpu_engine_output(const void* buffer, uint64_t size) {
    SotcEngineCmd::TakeStdout(static_cast<const char*>(buffer), static_cast<size_t>(size));
}
