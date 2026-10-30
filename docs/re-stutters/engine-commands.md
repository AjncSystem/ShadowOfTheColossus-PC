# Engine console commands (Phase 0 reverse engineering)

Target: the EU 1.01 eboot (`out/eboot.elf`). Addresses are eboot virtual addresses, the same as
`tools/re/eboot_re.py` and the `SOTC HOOK` manifest use. Guest calling convention is SysV x86-64
(rdi, rsi, rdx, rcx, r8, r9; float arguments in xmm0-7). Confidence labels: **confirmed** (read in the
decompilation and matched by a unique signature), **inferred** (plausible from the code, not run in
game), **unverified** (needs a live test).

## Summary

- The in-game console is a bpe console. Its per-line executor is `sub_3c9c80(console, bpe::String*)`.
  It echoes the line, splits it into `bpe::Command` objects and dispatches each one to the
  `bpe::ConCommand` found by name. **confirmed** (decompilation).
- The console tick `sub_3c7740(float dt, console*)` runs once per frame from `GameCoreLoop::GameLoop`
  (`sub_25d8b0` -> `sub_261820` -> `sub_3c7740`). **confirmed** (callers via `eboot_re.py callers`).
  Its per-frame position makes it the drain point for injected lines.
- Live execution is implemented by calling `sub_3c9c80` from the tick with a `bpe::String` view of the
  line, through the hook framework's `CallGuest` action. **inferred**, awaiting an in-game run.
- The `shadow_jumpToBoss`, `boss_outro`, `killBoss` and `printPlayerPos` handlers exist as ConCommands.
  The `shadow_horseDebugWarpToPlayer` handler was not located in this pass (the name is referenced by
  the PR's plan, not yet found in the strings list). **confirmed** for the first four names (strings),
  **unverified** for the horse warp.
- A built-in debug camera exists: `toggleDebugCam 0|1`, `ToggleDebugCamera` (string), class
  `CCPLdrCameraDebugCamera`, and `enableDebugCamera`. **confirmed** (strings and callers). This is a
  candidate for the free-cam phase, not yet exercised.

## The command machinery

| Address | Name (inferred from strings and __FUNCTION__ text) | Role | Confidence |
|---|---|---|---|
| `sub_1dff0` | `bpe::Command::ExecuteCmd() const` | Looks up `ConCmdManager`, calls the handler. Prints `Could not find command "%s"` | confirmed |
| `sub_1f920` | `CommandLine::DispatchCmds(EConFlag, EConFlag, options)` | Loops a `vector<Command>` (stride 0x98) and dispatches each | confirmed |
| `sub_1dcf0` | command parser (string -> Command, tokens) | Splits a `bpe::String` into the first token (name) and argument pairs | confirmed |
| `sub_1d9c0` / `sub_1dc80` | Command ctor / copy of a String into a Command | | confirmed |
| `sub_21920` | `ConCmdManager::ExecuteCmd(const Command&)` | Locks the manager mutex, finds the ConCommand by name (`sub_21620`), calls its vtable slot +0x20 | confirmed |
| `sub_21620` | `ConCmdManager::FindConCmd(String)` | Name lookup in a sorted tree (case-insensitive). Before the manager exists it walks a static list | confirmed |
| `sub_20d80` | `ConCmdManager::Register(IConBase*, ConCmdManager*)` | Inserts the command into the tree or the static list | confirmed |
| `sub_217f0`, `sub_219f0` | ExecuteCmd variants taking a request object | | confirmed (callers only) |
| `sub_21da0` | `virtual ConCommand::ExecuteCmd(const Command&)` (vtable slot +0x20) | Default handler dispatch | confirmed (string) |
| `sub_3c9c80` | `console->ExecuteLine(this, bpe::String* line)` | Echo, split, dispatch. The 2-char prefix (0xfe7d4d) is printed before the echo | confirmed structure, inferred name |
| `sub_3c7d90` | console submit: `FUN_003c9c80(this, this+8)`, then clears `this+8` | Runs the text typed in the in-game console | confirmed |
| `sub_3c9a10` | console output append (`this+0x30` list, 256 entries, mutex `this+0xb0`) | Prints lines | confirmed |
| `sub_3c7740` | console tick `(float dt, console*)` | Handles key events, calls `sub_3c7d90` on submit | confirmed |
| `sub_3c3a90` | automation manager (`automate_go`, `automate_loadfile`, ...) | The PR #345 path; plans run from here | confirmed (strings) |
| `sub_124f0` | one-time boot path that dispatches an empty command | Not reusable | confirmed |

### Command object layout (from `sub_1dcf0`, `sub_1dff0`, `sub_1f920`)

- Size 0x98 per Command in a `vector<Command>`.
- `+0x28`: pointer to the first token, the command name (NUL-terminated C string in the parsed text).
- `+0x30 ... +0x40`: vector of argument pairs `{char* ptr, size_t len}` (16 bytes each).
- Dispatch flags: `ConCommand` has flags at `+0x1c`, tested against EConFlag masks.
- `ConCommand` objects: vtable at `+0x00`; list link at `+0x08`; name `char*` at `+0x20`, name length at
  `+0x28` (static registration list); the handler is the virtual at vtable `+0x20`.

### `bpe::String` layout (from `sub_f2f0`, `sub_1dcf0`, `sub_3c9a10`)

Four 8-byte fields: `{char* data, size_t len, size_t cap, refcount_block*}`.
`sub_f2f0(String*, 0, const char*)` builds a non-owning view: `data = s`, `len = strlen(s)`, `cap = 0`,
`refcount = 0`. `sub_f590` (release) does nothing when the refcount block is null. **confirmed**
(decompilation of `sub_f2f0` and `sub_f590`).

## Calling the executor

```
sub_3c9c80(console, &line)     ; rdi = console (the tick's first integer argument), rsi = bpe::String*
```

- `console` is the object passed to `sub_3c7740` in rdi. Confirmed: the tick reads it as `param_2`
  while `param_1` is the float in xmm0.
- The line `String` may be a view (refcount 0), so no allocation or release is needed around the call.
- The call is made on the update thread from the tick's entry hook (the tick is not holding
  `console+0xb0`, because the lock is taken after the hook point). **inferred**.

## Structures used by the commands

- `printPlayerPos_command` (handler at `sub_330`, string `"Player Position: SetPlayerPos %.2f ..."`):
  - the player object comes from `sub_7c9820(&g_player_mgr)` with `g_player_mgr = 0x325e0a8`
    (data), which calls `sub_578810(mgr, 0)` and then `sub_578a00`. **confirmed** call chain,
    **inferred** meaning of the global.
  - the position and rotation come from `sub_b0(player, &pos, &rot)`; inside it `sub_660fc0(player)`
    gives a transform with two float blocks at `+0x150` and `+0x160`. **inferred** (offsets read from
    the decompiled code, not yet checked against a live position).
- Console output sink for the live path: `sub_3c9a10` (see `ENGINE_CMD_PRINT`).

## Command list

Names found in the eboot strings (the `_command(bpe::ConCommand` handlers, plus the `shadow_*`, boss and
debug names referenced by registration tables). Only the ones marked *(boss/camera)* were relevant to
this phase.

| Name | Where | Notes |
|---|---|---|
| `printPlayerPos` | handler `sub_330` | prints the Wander transform. *(boss/camera)* |
| `shadow_jumpToBoss` | registration `sub_4c4a10`, also `sub_534130`, `sub_5c7f50` | arg = boss index 0..15. *(boss/camera)* |
| `boss_outro` | same registration as above | arg = boss index. *(boss/camera)* |
| `killBoss` | menu table `sub_bb0` (also an icon `killboss.png`) | no args. *(boss/camera)* |
| `waitForAreaLoader` | registration `sub_4c92a0` | automation-only wait. |
| `shadow_horseDebugWarpToPlayer` | from the PR plan | not located in this pass. |
| `toggleDebugCam 0|1` | menu table `sub_bb0` | toggles the built-in debug camera. *(boss/camera)* |
| `ToggleDebugCamera`, `enableDebugCamera` | strings, `Cvars::RegisterResourceDependenciesFile` | cvar-side names. *(boss/camera)* |
| `CCPLdrCameraDebugCamera` | class, `sub_211d80` | the debug camera object. *(boss/camera)* |
| `GameCoreLoop::PrintCameraPos` | handler at `0xfc4061` string | prints the camera. *(boss/camera)* |
| `shadow_showPositionInfo`, `shadow_showPositionInfoEx` | registration `sub_c780` | debug HUD. |
| `shadow_playerDebugDrawSlope`, `shadow_printPlayerMotKind`, ... | registration `sub_c780` | many `shadow_print*` / `shadow_player*` debug switches. |
| `shadow_gameflagchk/dump/set/cls` | strings at `0x1289d36`..`0x1289e86` | game flag tools (save data touch). |
| `shadow_EasyMode_*` | strings near `0x1275929` | easy-mode multipliers. |
| `screenshot`, `help`, `getValue`, `uiPushScene`, `uiPopScene`, ... | generic | not needed for this phase. |
| `automate_go`, `automate_loadfile`, `automate_loadstring`, `automate_pause`, `automate_rewind`, `automate_clear`, `automate_userdone`, `automate_throwerror` | registration `sub_3c3a90` | automation manager. |

Free-cam, teleport, set-position, god-mode and noclip commands: **none found** as ConCommands, apart
from the debug camera (`toggleDebugCam`, `ToggleDebugCamera`). A `setpos` or `teleport` name does not
appear in the eboot strings. **confirmed** (string search, `str 'setpos|teleport|noclip|godmode'`).

## Execution strategy

1. **Chosen: guest call of the console executor from the tick.** The tick hook (`ENGINE_CMD_TICK`,
   entry at `0x3c7740`) captures the console and, when a line is queued, returns `HookAction::CallGuest`
   for `sub_3c9c80(console, &line)`. One line per frame. The line text and its String view live in host
   memory of this process (`engine_cmd.cpp`), not in the guest allocator. **inferred**.
2. Not used: the automation manager (`automate_loadstring`), because the PR found chained boss jumps
   hang in it.
3. Not used: writing into the console's own input String (`console+8`). It is refcounted and
   submit-cleared, so a direct write would need a guest allocation.

Boss sequence (`RequestBoss`, in `engine_cmd.cpp`):

1. `BackupSave` for Kill and Outro (copies `<user>/savedata/<uid>/CUSA0880x` to `<user>/save-backups/<ts>/`).
2. `shadow_jumpToBoss <n-1>` (or `boss_outro <n-1>` for Outro), one frame after the call.
3. `shadow_horseDebugWarpToPlayer` after 600 frames when horse is on and the arena is not water
   (colossi 7 and 12). **inferred delay**: the area loader's idle state is not read. Replace with a real
   check when the loader state is found.
4. `killBoss` 300 frames after the warp (Kill only).

Open point: `waitForAreaLoader` (`sub_4c92a0`) probably tests a loader-busy flag. Finding that flag
would replace the fixed frame counts.

## Hooks (scripts/hooks.d/enginecmd.py)

| Hook | Mode | Offset (EU) | Pattern | Use |
|---|---|---|---|---|
| `ENGINE_CMD_TICK` | entry | `0x3c7740` | 36 bytes, unique | drain point on the update thread; captures the console; `CallGuest` |
| `ENGINE_CMD_PRINT` | entry | `0x3c9a10` | 32 bytes, unique | copies console output into `RecentOutput()` |

`tools/re/hooks_check.py` resolves both (24 of 24 hooks resolve). `tests/test_hooks.py` passes (29 tests).

## Confidence per claim

| Claim | Confidence |
|---|---|
| `sub_3c9c80` executes one console line (echo, split, dispatch) | confirmed (structure) |
| `sub_3c7740` is the per-frame console tick from GameLoop | confirmed (callers) |
| The tick's first integer argument is the console object | confirmed (decompiled signature) |
| `bpe::String` view layout and `sub_f2f0` semantics | confirmed (decompiled) |
| Calling `sub_3c9c80` from the tick runs the line in game | inferred, unverified |
| `printPlayerPos` reads the player via `sub_7c9820(0x325e0a8)` then transform `+0x150/+0x160` | inferred (offsets), unverified |
| Boss jump, boss outro and kill command names | confirmed (strings) |
| Behavior of `shadow_jumpToBoss` with an index | inferred from the PR, unverified here |
| `shadow_horseDebugWarpToPlayer` handler | unverified (not located) |
| Fixed frame delays for area load and kill | inferred, not tied to the loader state |
| Debug camera exists (`toggleDebugCam`, `CCPLdrCameraDebugCamera`) | confirmed (strings and callers), behavior unverified |
| Hook offsets for the US 1.00 eboot | unknown (only the EU fingerprint is declared) |

## Names for tools/re/names.txt (not edited here)

Suggested additions: `0x3c7740 console_tick`, `0x3c9c80 console_run_line`, `0x3c9a10 console_print`,
`0x3c7d90 console_submit`, `0x1dff0 Command_ExecuteCmd`, `0x1f920 CommandLine_DispatchCmds`,
`0x21920 ConCmdManager_ExecuteCmd`, `0x21620 ConCmdManager_FindConCmd`, `0x20d80 ConCmdManager_Register`,
`0xf2f0 bpe_String_FromCStr`, `0xf590 bpe_String_Release`, `0x330 printPlayerPos_command`,
`0x4c4a10 register_boss_commands`, `0x7c9820 get_player_object`, `0xb0 get_player_transform`.
