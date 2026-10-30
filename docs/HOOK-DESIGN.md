# Engine hooks that work on every release

How the port hooks the game's engine so that it works on CUSA08809 (EU 1.01) and CUSA08034
(US 1.00), and keeps working, or turns itself off cleanly, on an eboot it has never seen.

## What we know about the releases

- **Observed.** EU 1.01 and US 1.00 run the same eboot, byte for byte in code and data: the EU
  1.01 update ships no eboot, only Arabic text (`scripts/sotc_modes.py`; commit `056b9af`).
  Every existing fixed-address patch (`MODES`, render size, red-zone functions) already applies
  to both.
- **Observed.** Build fingerprint of that eboot: SHA-256 over the virtual address, size and file
  contents of every `PT_LOAD` and `PT_SCE_RELRO` segment of the decrypted ELF =
  `72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225`. The SELF header and the
  `param.sfo` (content ID, title ID) are left out on purpose: they differ between regions while
  the code does not.
- **Unknown.** Only the EU dump is available here. The US fingerprint has not been computed on
  this machine. The first US run logs it, and `tools/re/hooks_check.py` checks a US dump when
  one is given.

So a hook needs one address today. The design below still resolves every hook without trusting
that, because a regional patch, a demo or a later fix would move the code.

## Three kinds of hook, in order of preference

1. **Import hooks (no address at all).** A system function the game imports (`sceGnm*`,
   `sceKernel*`, `sceVideoOut*`, ...) is bound by NID in `runtime_resolve()` (`src/runtime.c`).
   Wrapping the host implementation there works on any build of any region, because NIDs never
   change.
   - Use this kind whenever the behaviour can be changed at the system boundary: command buffer
     submission, flips, event flags, sleeps, direct/flexible memory allocation and mapping,
     file I/O.
   - The index lists the 571 imports and who calls them (`tools/re/eboot_re.py imports`).
2. **Engine code hooks (resolved by signature).** Hooks inside the engine (an allocator, the XPR
   consumer, the resource loader) are declared by a signature, not by an address:
   - **Pattern.** The instruction bytes at the hook site, with rel32 and RIP-relative
     displacements wildcarded. `eboot_re.py sig ADDR` generates the shortest unique one, and
     `eboot_re.py sigfind PATTERN` checks it.
   - **Anchor, when the pattern alone is not unique.** Small, generic functions are like this:
     `0x5bd70` is not unique within 64 bytes. The anchor narrows the search to functions that
     reference a given string (for example `"XPR_OccluderDepth"`), call a given import, or are
     called from an already-resolved hook.
   - **Expected address.** The address for the known fingerprint.
3. **Data hooks (derived, never hard-coded).** Struct field offsets and global addresses a hook
   needs are decoded from the matched instructions (the displacement of `mov rax, [rdi+0x50]`, the
   RIP target of a `lea`), at a declared offset inside the pattern. A rebuilt eboot that moves a
   global still resolves.

## Resolution at load time

For every engine hook, before the game starts (in the same pass as the existing CPU patches,
`sotc_patch_eboot`):

1. **Fingerprint is known:** take the expected address and check the pattern there (one
   compare).
2. **Fingerprint is unknown, or the check fails:** scan the text segment (with the anchor, if
   any) and require exactly one match.
3. **Zero or several matches:** that hook is turned off. Its feature falls back to the current
   port behaviour (page tracking, readbacks, ...), and the log says which hook and why. A hook
   never guesses.

Dependent hooks (B needs A's object layout) list their dependencies and are turned off with
them. At boot the port prints one manifest line per hook: name, resolved address, method
(expected / scanned / off), and the fingerprint.

## How a hook takes over

- **Detour at a function entry.** The first ≥5 bytes become `jmp rel32` to a trampoline in the
  area the loader already reserves right after the image (`src/probe.c`, "the CPU patch
  trampolines follow the image (rel32 jumps reach them)"). The displaced instructions are
  relocated by the existing patcher (`gpu/sotc/cpu_patches.cpp`, which decodes and re-emits
  them the way it does for SSE4a).
- **Guest-to-host thunk.** The trampoline saves the guest registers it must, keeps the 128-byte
  red zone below `rsp` intact (the game relies on it; see the red-zone functions), aligns the
  stack, and calls a host C++ handler with the guest's SysV arguments. The handler returns
  either "run the original" (jump to the relocated prologue) or a result.
- **Timed entry (mode `time`, `SOTC_TRACE_FUNCS`).** A slim detour for measuring, with no handler
  and no `GuestRegs`. The stub saves the registers it clobbers and calls a shared enter thunk,
  which records a frame (the return-slot address, the saved return address, the interned hook name
  and the start time) in a per-thread table of 256 frames and returns 1, or 0 when the table is full
  (the body then runs untimed and its return slot is not swapped). On 1 the stub replaces the return
  address with one shared leave trampoline. The trampoline keeps rax, rdx, xmm0-1, calls
  `sotc_trace_funcs_leave`, which finds the frame by the **exact return-slot address** (not a stack
  order), records a span when it lasts `SOTC_TRACE_FUNC_MIN_US` (default 10) or more, and returns the
  real return address. The table is not a LIFO: fibers run on stacks at arbitrary addresses on one
  host thread, and a longjmp abandons frames, so each live call is matched by its unique return slot
  in any order; a later call at the same slot (after a longjmp) overwrites the dead frame. Cost:
  about 100 bytes of trampoline per function, plus the shared thunks once. Refusals are those of
  Wrap, plus: a return at a non-zero stack depth, a branch that leaves the function (a tail call,
  whose callee would see the leave trampoline as its return), and any value derived from rsp or rbp
  that could reach the return slot. A return that matches no frame stops the process rather than jump
  to a wrong address; the refusals make that unreachable in normal control flow. Off unless its
  switch is set and `SOTC_TRACE` is on. Declared in `scripts/hooks.d/` files with `mode='time'` and
  `switch='SOTC_TRACE_FUNCS'`, one hook per function and no shared group.
- **Call-site redirect.** Only the `call rel32` of one caller changes, when the same function
  must keep its old behaviour for other callers.
- **Immediate/data patch.** As today's `MODES` (`scripts/sotc_modes.py`): address, expected
  bytes and new bytes, built into `out/patches.bin`. It moves to signature resolution when the
  site is resolved by signature.

## Where the declarations live

- `scripts/sotc_hooks.py` declares each hook:
  - name and switch (`SOTC_HOOK_<NAME>`, off by default);
  - kind;
  - pattern and anchor;
  - expected address per fingerprint;
  - derived data fields (instruction offset → meaning);
  - dependencies.
- It writes `out/hooks.bin` in the preparation step, like `patches.bin`. Packaged builds run it
  through `SotC.exe --script`. The loader resolves and checks the hooks at run time, so a
  packaged build with an unknown eboot still scans.
- `tools/re/hooks_check.py` (offline) resolves every hook against `out/eboot.elf`, and against a
  second dump when given (`--eboot path`). It fails when a hook does not resolve to exactly one
  site. It runs in the tests whenever the dump is present.
- The engine names found by the reverse-engineering work go to `tools/re/names.txt` as
  `0xADDR name` lines, for the current fingerprint only. Hooks never read that file; they only
  use their signatures.

## Rules for every hook

- **Off by default.** Behind its own switch, with a verify mode where the original runs too and
  the results are compared, as the bind-path switches do.
- **No silent partial state.** If any part of a hook group fails to resolve, the whole group
  is off.
- **Same behaviour on both releases.** A hook that would need region-specific behaviour (text,
  saves, DLC) keys on the title ID from `param.sfo`, never on the code.
