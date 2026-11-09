# Boss runs: launch into any colossus, hands-off

`tools/sotc_route.py --boss` boots the installed port straight into a boss encounter with no
input from you: it skips the intro movies, walks through the title menu by itself, and uses the
game's own debug machinery (shipped in the retail eboot) to jump into the arena. It was built
for performance analysis and for eyeballing boss animations and cinematics without playing the
30 minutes that normally sit in front of them.

## Usage

```
python tools/sotc_route.py --install DIR --measure SECONDS --boss N [--kill|--outro] [--intro]
```

- `--install DIR` — the port folder (the one with `launcher/` and `user/`).
- `--boss N` — colossus 1-16. Jumps into the encounter through the engine's automation manager
  and `shadow_jumpToBoss`: scripted arena spawn, boss active, the real encounter.
- `--kill` — kill the boss shortly after the jump, so the death-and-collapse cinematic plays.
- `--outro` — skip the fight instead: mark the boss defeated and play the aftermath (wake up at
  the Shrine). `--kill` and `--outro` are different endings; pick one.
- `--intro` — let the encounter's intro cutscene play out. Without it the intro is skipped
  (held X), which is what you want for clean frame measurements but not for checking the
  animations themselves. With `--intro` the kill fires 110 s after the arena loads; without, 40 s.
- `--measure SECONDS` — sit in the encounter that long, then exit. The run's frame log is kept
  as `user/measure-<timestamp>.log` for `tools/spike_report.py`.
- `--play` — instead of measuring, hand the game over once the jump is done: nothing is killed
  or closed, the boss stays alive, and you fight it yourself. The quickest way to playtest one
  encounter.
- `--record FILE` / `--replay FILE` — instead of measuring, record or replay a pad route after
  the jump (see the tool's docstring; routes pair with a save snapshot).

Example — the Eel's death cinematic, measured for two minutes:

```
python tools/sotc_route.py --install "D:\SotC-PC" --measure 120 --boss 7 --kill
```

Playtest Phalanx yourself, intro included:

```
python tools/sotc_route.py --install "D:\SotC-PC" --play --boss 13 --intro
```

All twelve early bosses back to back, intros and death cinematics included (one game launch per
boss; chaining two jumps in one session hangs the engine's automation):

```
for b in $(seq 1 12); do
    python tools/sotc_route.py --install "D:\SotC-PC" --measure 50 --boss $b --kill --intro
done
```

## What it does under the hood

1. Writes an automation plan (`route_bossjump.txt`) into the game dump — the engine loads plans
   from `/app0` — and injects `+nointro +RetailBuild=0 +automate_loadfile(...) +automate_go`
   through `SOTC_EXTRA_CVARS`, which the launcher appends to `SOTC_CVARS` for this run only.
2. Holds X from the first frame via the pad file. That both triggers the engine's `+nointro`
   auto-detect (intro movies skipped, save loading starts ~6 s after boot) and walks the title
   menu (press any button → Continue). The press is released the moment gameplay begins, so no
   cutscene after that sees any input.
3. The plan waits for the game to start, runs `shadow_jumpToBoss N-1`, waits for the area
   loader, and (with `--kill`) fires `killBoss` after the intro window.
4. The tool watches `user/last_run.log` for the automation markers, measures, and closes the
   game. The plan file is deleted afterwards.

## Caveats

- **The save's progress is mutated.** `--kill` and `--outro` flag that colossus as defeated
  (each defeated colossus adds a shadow ghost at the Shrine). Use a throwaway save, or keep a
  snapshot and copy it back.
- A boss you already defeated on the save may skip parts of its first-encounter ceremony.
- Bosses 7 and 12 are water arenas: the plan leaves out the horse stage there
  (`shadow_horseDebugWarpToPlayer` crashes the game over water).
- One jump per game session. The sequential loop above is the reliable way to do several.
- Plan filenames must not contain hyphens — the boot cvar parser rejects them.
- Cvars with vector values use the parens form on the command line: `startupLocation(-854,23,-2088)`
  (`=` values cannot contain commas). For the record: `startupLocation`/`startupArea` do not skip
  the title flow and do not override the save's spawn — the jump is the way in.
