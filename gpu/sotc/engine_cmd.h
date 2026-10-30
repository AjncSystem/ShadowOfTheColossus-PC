// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: runs Bluepoint engine console commands (bpe::ConCommand, e.g. shadow_jumpToBoss,
// printPlayerPos, killBoss) while the game is running. Hooks: scripts/hooks.d/enginecmd.py,
// switch SOTC_ENGINE_CMD (set by SOTC_LAB=1). Commands are queued from any thread and executed on
// the game's update thread from inside a hook, never from the queueing thread.
// Reverse engineering notes: docs/re-stutters/engine-commands.md.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SotcEngineCmd {

/// Registers the handlers of the ENGINE_CMD_* hooks. Called from RegisterAllHandlers().
void Register();

/// True when the executor hook resolved and live commands can run.
bool Available();

/// Queues one command line exactly as the engine console would take it ("printPlayerPos",
/// "shadow_jumpToBoss 0"). Any thread. False (and *err set) when unavailable or the queue is full.
bool Queue(std::string_view line, std::string* err = nullptr);

/// Lines the engine printed for the last executed commands (newest last, bounded). Any thread.
std::vector<std::string> RecentOutput();

/// How many times the game printed "st_gameflow() starting game" (save loaded / gameplay begins),
/// and the steady-clock time of the last one in ms (0 before the first). Any thread.
uint64_t GameStarts();
int64_t LastGameStartMs();

// --- Colossi -----------------------------------------------------------------------------------
struct Colossus {
    const char* name;  ///< English name (Valus, Quadratus, ...)
    bool water_arena;  ///< the horse warp crashes there (colossi 7 and 12)
};
/// The 16 colossi in game order; index 0 = colossus 1 (Valus).
const Colossus* Colossi();
inline constexpr int kColossusCount = 16;

enum class BossMode : uint32_t {
    Play = 0,  ///< jump to the arena, boss alive
    Kill = 1,  ///< jump, then killBoss (changes the save: back it up first)
    Outro = 2, ///< boss_outro (aftermath at the Shrine; changes the save)
};

/// Requests a jump to colossus n (1..16). horse: warp Agro to the player afterwards (ignored for
/// water arenas). Kill/Outro back up the save folder first (BackupSave). Any thread. Returns false
/// with *err when live jumps are unavailable (the caller may then fall back to a relaunch).
bool RequestBoss(int n, BossMode mode, bool horse, std::string* err = nullptr);

/// True while a requested jump has not finished (area loader busy). Any thread.
bool BossJumpPending();

/// Copies the current save folder (<user>/savedata/<user id>/<title id>/) to
/// user/save-backups/<timestamp>/. Returns the backup path, or empty with *err.
std::string BackupSave(std::string* err = nullptr);

} // namespace SotcEngineCmd
