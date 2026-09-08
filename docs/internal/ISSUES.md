# PSXRecomp v4 — Open Issues

## Issue #1 — Per-card directory load never runs in recomp (Phase 4 blocker)

**Status:** open, root cause narrowed
**Date opened:** 2026-05-03
**Phase:** 4 (BIOS shell + memory-card screen)

### Symptom

Recomp and Beetle both reach the same MEMORY CARD menu screen with the
same correct PSX rendering. **Beetle renders 3 save-block icons in
CARD 1 + 1 face icon in CARD 2; recomp renders zero icons.** The
divergence is the per-card directory load — Beetle reads it from the
on-disk card files, recomp does not.

(Note: the rainbow buttons are the actual PSX BIOS rendering — both
sides have it. Not a bug.)

### Hard data — gate-mode write distribution

Captured side-by-side, both runtimes driven into the memcard screen
with a 240-frame CROSS hold via `press_into_card_view*.py`:

| Mode | Beetle | Recomp | Notes |
|------|--------|--------|-------|
| 0x01 | 122    | 21     | chain reset / BUSY clear |
| 0x02 | **96** | 16     | R-step install (read-step) |
| 0x04 | 2      | 6      | step-3 install |
| 0x08 | 22     | 3      | D-step install (detect) |
| 0x21 | **0**  | **6**  | init+flag — recomp-only path |

R-step (mode 0x02) caller breakdown:

| caller (`ra`) | Beetle | Recomp |
|----|----|----|
| 0xBFC08F28 (icon-load loop) | **40** | **0** |
| 0xBFC09250                  | **16** | **0** |
| 0xBFC08D08                  | 30     | 6     |
| 0xBFC08A0C                  | 8      | 4     |
| 0xBFC08B98                  | 2      | 6     |

**Recomp never reaches the icon-load loop.** That is the divergence.

### What we proved is *not* the cause

- `mem[0x80007520]` ("chain-restart discriminator") is **not** the
  divergence. Both sides have it at 0 in steady state. The earlier
  hypothesis that recomp=0 / Beetle=non-zero at the decision point was
  wrong — both sides clear it in the JAL-delay-slot at PC `0xBFC14CE0`
  (= RAM 0x51E0) every 2 frames before calling `func_00004D6C`.
- The texture corruption on the menu buttons is correct PSX
  rendering, not a bug.
- The per-slot FP table at `mem[0x7528+slot*4]` IS swapped
  dynamically by recomp too (between `0x5B64`, `0x5688`, `0x51F4`) —
  the swap mechanism works. Beetle just performs additional swaps from
  callers `BFC08F28` and `BFC09250` that recomp never reaches.
- `func_00006380` (writes `mem[0x75C0]=1`) and the B-table trampolines
  at `BFC0DA30` / `BFC0DA40` (B0:0x58, B0:0x4D `_card_status`) DO run in
  recomp — `BFC0DA30` 620×, `BFC0DA40` 618×. The trampoline machinery
  works for those.

### Suspected root cause (not yet confirmed)

Recomp **never enters `func_1FC08B3C`** — the per-card directory load
function. The icon-load loop at PC `0xBFC08F1C..0xBFC08F7C` lives
inside this function. The loop does `jal 0xBFC0DA00` (= B0:0x4F
`_card_read`) once per directory entry; Beetle hits this 20× per
session.

Wtrace activity that *appears* to put recomp inside `func_1FC08B3C`
(mode 0x02 writes with `ra=0xBFC08B98`) is reached via the FP-table
swap path, not via the icon loop. The wider function entry at
`label_BFC08B3C` is never logged in `fn_entry_dump` for the entire
[0x1FC08000, 0x1FC0A000) range across 134M captured ring entries.

`func_1FC08B3C` IS in the recomp dispatch table (entry 5449,
`{ 0x1FC08B3Cu, func_1FC08B3C }`) plus 20+ continuation entries. So
the function is recompiled and dispatchable; it's the *caller* that
isn't reaching it.

### Open questions for the next session

1. **What calls `func_1FC08B3C`?** Find the static caller (or callers)
   of ROM `0xBFC08B3C`. Use Ghidra `xrefs 0xBFC08B3C` to enumerate.
2. **Does recomp ever reach `func_000005E0` (B0 dispatcher) and
   `func_000005C4` (A0 dispatcher)?** These live in the recomp
   dispatch table; if recomp doesn't enter them, B-table dispatch is
   broken. Was about to check this when the runtime froze on a
   270M-entry `fn_entry_dump`.
3. **Counter-mismatch hypothesis:** in `func_00004D6C`, the FP-call
   chooses between several outcomes based on `mem[0x7514]`-1.
   `func_00005B64` (the dispatcher FP) returns -1 when counter=0,
   forcing the 0x21 path. Both sides clear `mem[0x7514]` in the same
   place every 2 frames, but Beetle's writes show counter cycles
   1-2-3-4-5-6 inside a single window (132 cycles × 6 calls), while
   recomp's data showed cycles up to 13 with different clear sources.
   The cycle structure differs — needs deeper analysis.

### Diagnostic infrastructure built / fixed this session

- `tools/summarize_gate_modes.py` — tallies gate writes by value
  (mode 0x01/0x02/0x04/0x08/0x21) per caller, recomp + beetle
- `tools/_dump_gate_seq.py` — dump gate writes in seq order with
  `pc/fn/ra/w` fields (extends earlier handoff tool)
- `tools/_dump_addrs.py` — group wtrace by addr, show distinct
  writers per addr
- `tools/_dump_7520_beetle.py` — filter beetle wtrace by addr range
- `tools/_dump_ram_words.py` — RAM dump as 4-byte words, both backends
- `tools/_read_7520.py` — sample mem[0x7520], 0x755A, 0x7568
- `tools/_screenshot_meta.py` — call screenshot/screenshot_file/
  emu_screenshot
- `tools/check_pc_dispatch.py` — group `fn_entry_dump` by `func_addr`
  in a range
- `tools/press_into_card_view_recomp.py` — mirror Beetle's
  press_into_card_view but for recomp
- `tools/arm_gate_trace.py` extended with `[0x7520..0x7524)`,
  `[0x7514..0x7518)`, `[0x7528..0x7538)`, `[0x7258..0x7260)` ranges

### Tooling fix needed (CLAUDE.md Rule 15)

`fn_entry_dump` iterates the entire ring (up to 270M entries) before
applying the addr filter, which **freezes the debug server thread for
seconds-to-minutes** on a populated ring. Both `handle_fn_entry_dump`
and `handle_fn_exit_dump` in `runtime/src/debug_server.c` need the
addr filter applied early, plus a `seq_lo/seq_hi` window cap, so a
filtered query for a small range returns immediately.

### Files with new memories on disk

- `memory/MEMORY.md` (will be updated next session with the
  "0x7520 is NOT the divergence — directory-load function never
  entered" finding)

### Update 2026-05-03 (continued)

**Static call chain to the directory load (via Ghidra xrefs):**

```
????
  -> FUN_bfc24640 / FUN_bfc24a48 / FUN_bfc1cdd8       (3 of the 5 _card_load callers)
       jal 0xB005A9B0  (= jal 0xBFC42BB0 = B0:0x15 _card_load)
  -> FUN_bfc0dac0  (kernel B0:0x15 trampoline body)
       jr $t2 with $t1=0x15 → kernel B0 dispatcher
  -> _card_load (B0:0x15 body in kernel image)
       calls FUN_bfc09914
  -> FUN_bfc09914
       FUN_bfc08b3c(0)   ; load card-1 directory
       FUN_bfc08b3c(0x10); load card-2 directory
  -> FUN_bfc08b3c (the icon-load loop at BFC08F1C..BFC08F7C)
       jal 0xBFC0DA00  (= B0:0x4F _card_read)  per directory entry
```

**On recomp NONE of `FUN_bfc24640`, `FUN_bfc24a48`, `FUN_bfc1cdd8`,
`FUN_bfc09914`, `FUN_bfc08b3c`, `FUN_bfc0c2e8` are ever called.**
The shell never reaches the call sites. Per
`memory/phase4_b0_trampoline_findings.md`: "The shell has a conditional
code path for card init. On DuckStation, the shell enters the card
detection path. In our runtime, this condition fails, so card init is
skipped." This is the same blocker.

**Static caller of FUN_bfc0c2e8 (BootInitMemcards) is NOT static.** It
has zero static xrefs, and the literal `0xBFC0C2E8` does not appear
anywhere in ROM. It's invoked via a kernel function pointer table
populated at runtime.

**Update late 2026-05-03 — actual call path identified via Beetle
fn_trace:**

Beetle's fn_trace ring (with `0x1FC09914` armed as a target) captures:
- `seq=904 JR caller=0x000005D8 ra=0x8003202C` jumping to FUN_bfc09914
- caller PC `0x000005D8` is INSIDE the kernel A0/B0 dispatcher area
  (A0 dispatcher entry is `0x000005C4`, B0 dispatcher is `0x000005E0`)
- `ra=0x8003202C` means the JAL that started this chain was at
  `0x80032024` — which translates to ROM `0xBFC1A024`
- Disassembly at ROM 0xBFC1A024:
  ```
  bfc1a000: li $t8, 1
  bfc1a004: bne $t9, $zero, 0xbfc1a040  ; SKIP if already initialized
  bfc1a008:   sw $t8, -0x5880($at)       ; (delay slot) set init flag
  bfc1a00c: jal 0xb005a960               ; B0:0x4A InitCARD($a0=1)
  bfc1a010:   li $a0, 1
  bfc1a014: jal 0xb005a970               ; B0:0x4B StartCARD($a0=0)
  bfc1a018:   nop
  bfc1a01c: jal 0xb005a980               ; B0:0x5B ChangeClearPad
  bfc1a020:   clear $a0
  bfc1a024: jal 0xb005a990               ; A0:0x70 (_card_load equivalent)
  bfc1a028:   nop
  bfc1a02c: jal 0xb005a9a0               ; A0:0xAD
  bfc1a030:   li $a0, 1
  bfc1a034: li $t0, 1
  bfc1a038: lui $at, 0x8006
  bfc1a03c: sw $t0, 0x66f8($at)          ; mark mem[0x800666F8]=1 (init done)
  bfc1a040: lw $ra, ...
  ```
- Shell calls `jal 0xB005A990` at PC `0xBFC1A024`. The trampoline at
  `0xBFC42990` is **A0:0x70**, which the kernel A-table dispatches to
  `FUN_bfc09914` (the directory loader caller).
- So the actual chain is **A0:0x70**, not B0:0x15. The 5 static
  callers of `FUN_bfc0c2e8` (FUN_bfc24640 etc.) are red herrings —
  they handle COPY/DELETE actions, not boot-time card load.

**Recomp blocker:** the shell function `FUN_bfc1a000` is reached via
indirect dispatch (no static caller in ROM). It is in recomp's
dispatch table (`{0x1FC1A000u, func_1FC1A000}`), but the shell never
dispatches to it. OR — the shell DOES dispatch to it, but the
`bne $t9, $zero, 0xBFC1A040` branch at PC 0xBFC1A004 skips the init
because `$t9 != 0` somehow.

**Confirmed already-known issue.** This matches
`memory/phase4_b0_trampoline_findings.md`: "The shell has a
conditional code path for card init. ... In our runtime, this
condition fails, so card init is skipped and the shell goes directly
to the main menu (state 0x37)."

### Concrete next step

1. Find what calls `FUN_bfc1a000` on Beetle. Arm `0x1FC1A000` in
   Beetle fn_trace and capture the caller. (One JAL produces the
   chain.)
2. On recomp, check whether `func_1FC1A000` is ever entered. If yes:
   capture `$t9` value at entry. If $t9!=0, find what set it.
   If no: trace the upstream call chain to find why the JAL to
   `func_1FC1A000` doesn't fire.
3. Fix the recompiler's emit / generator for whatever instruction or
   branch is producing the divergent state. NEVER hand-patch the
   generated code or hand-call `func_1FC1A000`.

### Update 2026-05-03 (continued, late session)

**Step 1's address range was wrong** — `0x1FC1A000` is a label
INSIDE `func_1FC19FD0`, not the function entry. Querying for the
correct entry `0x1FC19FD0` shows recomp DOES enter:
- `func_1FC19FD0` (BootInitMemcardsShell) at frame 617/2344 with
  `$a0=0x00F000F0` (matches the AND-mask gate at BFC19FDC).
- The full init chain runs: `mem[0x800666F8]` (init done flag) is set
  to `1`, meaning all 5 inner JALs (InitCARD, StartCARD,
  ChangeClearPad, A0:0x70, A0:0xAD) executed.
- `FUN_bfc09914` is entered at frame 617/2344 with `$a0=0`
  (`ra=0x8003202C` — confirming the JAL chain from BootInitMemcardsShell).
- `FUN_bfc08b3c` is entered **12 times** across both slots:
  frame 2344/2350 (initial calls from FUN_bfc09914 with `ra=BFC09934`,
  `BFC0993C`), then frames 2372/2385/2426/2433 (subsequent calls from
  `FUN_bfc089c0` `ra=BFC08A90`).

**Beetle fn_trace confirms the call chain**:
```
JALR @ 0x8004673C → 0x80031FD0 (BootInitMemcardsShell)
  $a0=0x00F000F0, $a1=0x00F000F0, ra=0x80030968
```
The caller PC `0x8004673C` is inside a function at shell-RAM
`0x800466B8` (= ROM `0xBFC2E6B8`), entered via `JAL` from PC
`0x80030960`. The function reads a function-pointer table at
`mem[$a3 + idx*0x88 + 0x34]` and dispatches devices. For card
init, `$t2` = pointer to BootInitMemcardsShell. Recomp ALSO reaches
this function (per `func_1FC2E6B8` having entries) — the BIOS shell
init chain IS running on recomp end-to-end.

**The actual root cause is one level deeper.** `FUN_bfc08b3c`
runs but **bails out in the 15-sector init loop before reaching the
icon-load loop** at PC `0xBFC08F1C..BFC08F7C`:

- `_card_read` (B0:0x4F via trampoline `BFC0DA00`) is called 32 times
  on recomp — `ra=BFC08B98` (sector 0 from FUN_bfc08b3c entry),
  `ra=BFC08D08` (sectors 1+ from the 15-sector loop), `ra=BFC08A0C`
  (FUN_bfc089c0's first read).
- Sector 0 read SUCCEEDS — `mem[0xA000BE48]` shows `'M' 'C' 0 0 …
  0 0 0 0x0E` — the MC magic AND a valid XOR checksum (XOR of bytes
  0..126 = 0x0E = byte 127). The buffer is correctly filled by
  recomp's SIO/card simulation for sector 0.
- The 15-sector loop runs only ~4 iterations per slot before bailing
  to `LAB_bfc08f8c`. Confirmed: `mem[0xA000B9E8..]=0xFFFFFFFF` is
  exactly the bail-clear pattern (`for (i=0; i<0x14; i++) puVar10[i]=
  0xFFFFFFFF`). And `mem[0xA000BA88]=0` is the bail-clear for the
  directory area.
- The validator `FUN_bfc08720` is **never entered** — confirming the
  bail happens BEFORE the checksum check, in the read-gate:

```c
iVar6 = FUN_bfc0da00(slot, sector, buf);
if ((iVar6 != 1) || (iVar6 = FUN_bfc09144(), iVar6 == 0)) {
    goto LAB_bfc08f8c;          // ← bail
}
iVar6 = FUN_bfc08720(buf);      // ← never called
```

Either:
- (a) `_card_read` returns `!= 1` for sector ≥ 1 (i.e. recomp's SIO
  card simulation only completes sector-0 reads correctly), OR
- (b) `FUN_bfc09144` returns 0 — meaning one of the error flags
  `mem[0xA000B9D4..B9E0]` was set instead of the success flag
  `mem[0xA000B9D0]`. The error flags are set by the recomp's
  SIO IRQ / chain coordinator on read failure.

**Concrete next step (next session):** capture `_card_read` return
values via `fn_exit_dump` of `0x1FC0DA00` while card init is happening
(window from frame ~1029-1100), and capture the wtrace lifecycle of
`mem[0xA000B9D0]` and `mem[0xA000B9D4..B9E0]` during the same window
to determine which flag fires for the failing sector.

**Or simpler**: The fix is in `runtime/src/memcard.c` /
`runtime/src/sio.c`. The SIO/card simulation handles sector 0
correctly but fails on sector ≥ 1. The issue is in hardware
simulation, NOT recompiler/codegen. Inspect how the runtime simulates
the multi-sector read protocol.

**This session's findings invalidate the handoff's primary
hypothesis** (`recomp never enters func_1FC1A000`). The shell init
chain IS reaching directory load. The blocker is the runtime's SIO
card simulation failing sector ≥ 1 reads. Per the handoff: "Cycle-paced
SIO works well enough for card detection. Sector-0 detection works."
That hypothesis is correct, but sector ≥ 1 is the new blocker.

**Failed attempt: 67 trampoline seeds (regressed shell main-menu
transition).** I added all 67 missing A0/B0/C0 trampolines (BFC0D8E0..
BFC0D940, BFC16550..BFC16750, BFC42xxx including BFC42BB0=B0:0x15) to
`recompiler/seeds/dispatch_miss_seeds.json`, regenerated, rebuilt.
Result: recomp never advanced past the MAIN MENU — pressing CROSS no
longer transitioned to the memcard menu. Reverted seeds via
`git checkout HEAD -- recompiler/seeds/`. Conclusion: dispatching the
shell trampolines through recompiled C functions broke shell flow.
The existing dirty_ram_dispatch handles them correctly; do not reseed.

**Tool fix landed:** `runtime/src/debug_server.c::fn_dump_parse` now
defaults to a 1M-entry sliding window instead of the full 128M ring,
preventing the multi-second freeze that previously made `fn_entry_dump`
unusable on populated rings.

### Next session — laser focus

The blocker is "find what kernel/shell function call chain *should*
invoke `FUN_bfc0c2e8` (BootInitMemcards) at boot, and why recomp's
shell skips it." Suggested approach:

1. Find the kernel function pointer table that contains
   `0xBFC0C2E8`. The address must appear *somewhere* — maybe in a
   table populated at runtime by `lui+addiu` instructions in another
   function. Search Ghidra for `BFC0` `C2E8` halfword pairs.
2. Look at SystemInit / boot chain (around `0xBFC00150` reset
   vector). Compare DuckStation/Beetle's static fn_trace through boot
   vs recomp's — find where the divergence first occurs.
3. The goal is a single decision point in shell init code where
   recomp takes the "skip card init" branch and Beetle takes the
   "do card init" branch. Compare register state at that branch.

---

## Issue #2 — `fn_entry_dump` / `fn_exit_dump` freeze the debug server on populated rings

**Status:** open
**Date opened:** 2026-05-03

`runtime/src/debug_server.c::handle_fn_entry_dump` and
`handle_fn_exit_dump` walk every entry in the 64K-cap ring (or rather
the entire `[seq_lo, seq_hi)` window, which can be the full ring)
applying the addr filter only AFTER constructing the per-entry buffer
position. With 270M cumulative entries and a 1GB output buffer, a
filtered query for a 0x200-byte addr range still iterates 134M
entries before returning anything. Debug server is single-threaded so
all other commands stall (including `ping`).

**Fix:** apply `addr_lo/hi` filter as the first check inside the
loop before any string formatting; cap `seq_hi - seq_lo` to a
sensible default (e.g. 2M) when the caller doesn't pass an explicit
window.

---

## Issue #3 — BIOS "PlayStation" disc-detected screen is missing the PS logo glyph

**Status:** open
**Date opened:** 2026-05-11
**Phase:** 4 (BIOS shell render)

### Symptom

After the Sony logo, the BIOS shows the second boot screen (the
"PlayStation" disc-detected screen rendered when a disc is present).
On this screen the top region — which on real hardware shows the
stylized PS logo bitmap — is blank. The text below it ("PlayStation",
license string, etc.) renders correctly.

Cosmetic only; boot continues, the disc loads, and Tomba's FMVs start.
Logged here so we don't lose track of it once the FMV cluster is fixed.

### Likely areas

- GPU VRAM upload for the logo bitmap (CopyRectangle / CPU->VRAM
  transfer) may be dropping a region. The other tiles on the same
  screen render, so it isn't a wholesale VRAM/clut wipe.
- Or the logo is sourced from a CD raw-sector / sector-header data
  path that the current `cdrom.c` whole-sector mode doesn't expose
  the way the BIOS expects.

### Concrete next step

Take a Beetle screenshot of the same boot screen as oracle, diff the
VRAM region the logo lives in (Beetle vs recomp at the same frame),
and walk back from the missing pixels via `wtrace` on the source
RAM/VRAM coordinates.

---

## Issue #5 — Mid-function split targets not registered in dispatch table

**Status:** open, audit-surfaced
**Date opened:** 2026-05-12
**Phase:** platform (recompiler emit, full_function_emitter path)
**Bug class:** same shape as 2026-05-12 jump-table cross-function fix
(`full_function_emitter.cpp:777`).

### Symptom

When the recompiler emits a branch/jump whose target is a
mid-function address (neither a known block in the current CFG nor a
known function start), it emits `call_by_address(cpu, 0xX); return;`
to defer dispatch to the runtime — but does NOT register `0xX` in the
dispatch table. At runtime, the dispatch binary-search misses, the
target page isn't dirty, so `psx_unknown_dispatch` fires.

### Affected emit sites in `recompiler/src/code_generator.cpp`

| Line | Pattern                                                |
|------|--------------------------------------------------------|
| 970  | conditional-branch taken-arm to mid-func target        |
| 981  | conditional-branch fall-through to mid-func target     |
| 998  | unconditional jump to mid-func target                  |

All three should call `register_cross_function_target(branch_target)`
the same way the jump-table emitter does (line 777 of
`full_function_emitter.cpp`).

### Evidence — Tomba audit, 2 manifestations

From `codegen_audit_game.py --config game.toml`:

```
[2] literal call_by_address targets: 333 unique, 459 sites
    dispatch table size: 1921 entries
    targets in declared code regions: 2
    targets in RAM (dirty-RAM interpreter domain): 331
    in-code targets MISSING from dispatch: 2
      0x800905E4  (1 site)
      0x80090600  (1 site)
```

Both addresses appear in `generated/SCUS_942.36_full.c` with comments
`/* taken: split (mid-func) */` and `/* not taken: split (mid-func) */`
— the recompiler EXPLICITLY knows these are mid-function splits but
fails to register them.

The 331 RAM-domain targets are not bugs — they're runtime-loaded
overlays correctly handled by `dirty_ram_dispatch`.

### Concrete next step

1. Add `register_cross_function_target(branch_target)` at the three
   call sites in `code_generator.cpp` (lines 970, 981, 998).
2. Regen Tomba.
3. Re-run `codegen_audit_game.py` — expect "in-code call_by_address
   misses: 0".
4. Re-run on BIOS to confirm no regression.

---

## Issue #4 — 7 unemitted GTE / COP2 instructions in BIOS Shell code

**Status:** open, audit-surfaced
**Date opened:** 2026-05-12
**Phase:** 4 (BIOS shell render)
**Likely related to:** Issue #3 (missing PS logo on disc-detected screen)

### Symptom

The Phase B1 `gte_audit` (now generic, code-region-filtered) reports
4 missing `gte_execute` emits and 3 missing LWC2/SWC2 emits in the
BIOS Shell code region. None of the affected PCs appear anywhere in
`generated/SCPH1001_full.c`.

| PC          | Word         | Class          |
|-------------|--------------|----------------|
| 0xBFC34FF8  | 0x4A480012   | GTE MVMVA      |
| 0xBFC3502C  | 0x4A480012   | GTE MVMVA      |
| 0xBFC35064  | 0x4A480012   | GTE MVMVA      |
| 0xBFC350C4  | 0x4A480012   | GTE MVMVA      |
| (3 sites)   | LWC2 / SWC2  | GTE load/store |

These were previously masked by 73 data-region false positives in the
old unfiltered tool. The B1 code-region filter surfaced them.

### Likely cause

MVMVA is matrix-vector-multiply-and-add — the BIOS only uses GTE/3D
math in two narrow places: the boot logo intro and the
disc-detected/PlayStation-logo screen. We boot past both, but
**Issue #3's missing PS-logo glyph is consistent with these
unemitted MVMVA + LWC2/SWC2 sites**: if the recompiler skipped the
3D math that draws the logo, the logo would render as nothing or
garbage but the surrounding text would be fine — exactly Issue #3's
symptom.

Two possible root causes (not yet distinguished):
- **Discovery gap:** the function containing these PCs was never
  identified, so nothing got emitted for it.
- **Emit gap:** the function was discovered but the recompiler
  skipped these specific instructions when translating.

### Concrete next step

Check whether ANY PC near `0xBFC34FF8` appears in
`generated/SCPH1001_full.c`. If neighbors are emitted but the GTE
sites aren't, it's an emit gap (fix in code_generator.cpp). If
neighbors are absent too, it's a discovery gap (fix in function
discovery seeds). Either way, then close Issue #3 alongside.

## Issue #7 — sljit live execution is unvalidated (pure-live save-load wedge)

**Status:** CLOSED — superseded, not fixed. The sljit Tier-2 backend this issue
describes was removed wholesale in `5b7e69b4` (2026-07-15), which deleted
`lib/sljit/`, `overlay_sljit.{c,h}` and the sljit paths in `code_provider.c` /
`overlay_loader.c`. The toolchain-free role it filled is now served by the
bundled TinyCC tier (introduced `c2a3f6ad`, 2026-06-25), which compiles to a DLL
through a subprocess rather than JITting in-process, so it does not share this
defect's live-execution/save-load shape. Nothing here is actionable; retained as
history.
**Date opened:** 2026-06-15
**Date closed:** 2026-07-15 (by removal of the subsystem)
**Area:** overlay Tier-2 sljit backend (`runtime/src/overlay_loader.c`,
`overlay_sljit.c`, `code_provider.c`, `overlay_sljit.c` resolution)

### Symptom

With `PSX_OVERLAY_SLJIT_LIVE=1` (the prototype toolchain-less production path),
save/load **wedges the guest**: `reason=atexit, pc=0`, dispatch looping a
BIOS/kernel address (`0xB0`/`0x650`). Reproduces only in live mode — the
dev differential path (`overlay_diff_on`) is clean (0 divergences / 395 shadow
calls). Flagged in commit `81cf21b` as Known Bug #1.

### Root cause (NOT the emitter)

The MIPS→sljit emitter + block-local register allocator are **proven correct**
this session: 66/66 emitter unit, 22/22 `$sp`-balance probe, and byte-identical to
the pre-regalloc emitter across 313 overlay functions
(`runtime/tests/sljit_{emit_test,sp_probe,offline_diff}.c`). The crash is in the
**live-execution wiring**, which bypasses the validation harness:

- `run_shadow_diff` (the device-touch detector + diff gate) is gated on
  `s_diff_mode`. Live mode sets `s_sljit_live`, not `s_diff_mode`, so in live mode
  **the diff never runs.** Therefore:
  - `device_touch` is **never computed** for a live shard → a device-touching
    function runs its shard and **double-executes** its SIO/memory-card/DMA I/O
    against real hardware state, corrupting the kernel byte-handler path → the
    `atexit, pc=0`, kernel-`0xB0/0x650` livelock. *(Known Bug #3 — leading cause.)*
  - Shards run with `diff_passes == 0`, i.e. **completely unvalidated**.
    *(Known Bug #2.)*
- Separately, the verify budget counts **cumulative** (not consecutive) clean
  passes — `diff_passes` never resets on a divergence — so even in dev mode an
  intermittently-wrong shard could reach budget on lucky passes.

The historical "`$sp` off by 0x18 at 0x12E478" crash was a **different, already-fixed
bug** (diff over-scoping the call tree, fixed by per-function isolation in
`81cf21b`) — not this one, and not a codegen bug.

### Fix (SLJIT.md §11 Phase A) — runtime-only, no regen

Collapse blind-live into validated-live: route live-mode shards through the diff
gate (`(s_diff_mode || s_sljit_live)` at the dispatch gate), so `device_touch` is
computed and a shard runs native live ONLY after a clean verify budget; and reset
`diff_passes` to 0 on any divergence (consecutive-clean = "0 divergences"). After
this there is no blind path: device + diverging shards stay on the interpreter.

### Related gaps (same area, tracked in SLJIT.md §11 Phases B–C)

- **gcc>sljit precedence not enforced** (LIFO chain order can put an sljit shard
  ahead of a gcc shard for the same region). → Phase B.
- **Non-dev machine never triggers sljit generation in normal play** (on-miss hook
  gated behind a dead env-only flag; `auto` resolves to gcc on every machine because
  `autocompile_configured()` tests for a config *string*, not a real toolchain). →
  Phase C.

### End-to-end success criteria (per `docs/internal/DEBUG.md` end-to-end rule)

`PSX_OVERLAY_SLJIT_LIVE=1` (or `auto`→sljit on a toolchain-less box): play through
save-load, FMV, menus, and combat with **0 divergences, 0 wedges**, shards
validated-then-promoted, device functions correctly pinned to the interpreter.
"Diff is clean" or "shard validated" is NOT success until the full save-load loop
closes live.

---

## Issue #8 — Tomba2 P0: wild-jump FAIL-FAST fatal after coverage-wave native promotion

**Status:** OPEN — root-cause hunt in flight, currently blocked on Issue #9
**Date opened:** 2026-07-06 (carried from session handoff)
**Area:** overlay native execution (codegen OR dispatch/call-contract layer)
**Rule:** root-cause via first-divergence; do NOT ship a block/disable.
`overlay_native_block` on 0x80073328 acceptable for ATTRIBUTION only.

### Symptom

Two FAIL-FAST unknown-dispatch fatals in attract soaks (~15–20 min apart per
run) after the 2026-07-06 entry-based-coverage wave promoted new PCs to native
(the convergence fixes work; they exposed a latent defect):

- **Crash 1:** addr=ra=0x0000F514 (zeroed kernel heap, page not dirty),
  4 seconds after 0x80073328 went native for the first time (its only-new-entry
  DLL `00038000_0A16928B` built 09:04:14). 0x80073328 fired ra=0x80139400 every
  2 frames all scene.
- **Crash 2:** addr=0x009D6830, ra=0x00000001 (beyond 4× RAM mirror), pre-fatal
  register soup: target==ra; interp dispatches into 0x13xxxx drivers with
  garbage args (a0=0xDF73E104, a1=0xE2000318 = raw GP0 word as pointer).
- Both: same kernel event-delivery dispatch chain (…0x20D4, 0x650, 0x2104)
  immediately before; a0=0x800F42E0 (persistent struct) at both fatals.
  Corruption PRECEDES the fatal; fail-fast catches the landing.

Evidence: memory `tomba2_f514_wildjump_first_native.md` + prior session's
scratchpad `crash_f514/` (psx_crash.txt, psx_last_run_report.json,
psx_freeze_heartbeat.json).

### Suspect set

The post-08:44 coverage wave DLLs in
`build-t2/cache/SCUS-94454/gcc/win-x64/cg4_d44a063d/`: new region DLLs +
island fragments (incl. F 80073328 in `00038000_0A16928B`; F 80106D7C /
F 80106E58 / F 80024548; jump-table-ladder aliases F 8011040C/43C/45C/478/4CC
in `00106000_1675417D`).

### Plan (two independent paths)

1. **Instrumented path (preferred):** finish the shadow-diff segment redesign
   (Issue #9), soak with `PSX_OVERLAY_DIFF=1` from boot. Expect either a named
   divergence record (→ shard codegen defect, fix recompiler) or clean segments
   everywhere + crash persists (→ exit/call-contract class, bugA/bugD family —
   fix the dispatch contract layer).
2. **Bisection path (no instrument needed):** quarantine halves of the
   post-08:44 DLL wave out of the cache dir, ~20-min attract soak per round
   (crash repro interval). First single-candidate probe: `overlay_native_block`
   on 0x80073328 (attribution only, never a fix).

---

## Issue #9 — Overlay shadow-diff instrument is structurally unsound (never engages)

**Status:** ROOT-CAUSED; partial fixes in tree (uncommitted, `_wt-tomba2-ipr`);
segment-granular redesign DESIGNED but NOT implemented
**Date opened:** 2026-07-06
**Area:** `runtime/src/overlay_loader.c` (run_shadow_diff + dispatch gates),
`runtime/src/dirty_ram_interp.c`, `runtime/src/interrupts.c`, `runtime/src/traps.c`
**Blocks:** Issue #8 path 1. Full detail + design in memory
`tomba2_shadow_diff_unsound_segment_redesign.md`.

### Symptom

With `overlay_diff_on` / `PSX_OVERLAY_DIFF=1`, shadow_calls=1 over 51K+ frames
(two independent runs) — the diff instrument validates essentially nothing while
dispatch_native runs millions of calls. The game LOOKS fine, so the failure is
silent.

### Root cause chain (three stacked defects, found in order)

1. **CPS interior re-entry path had no diff gate** — continuation re-entries
   (the dominant dispatch class under CPS) ran native blind in diff mode.
   FIXED in tree: gate mirrors the entry chain (`interior_gated` counter in
   `overlay_shadow_dump`).
2. **Longjmp-escape hazard** — a shadow started inside an exception dispatch is
   unwound by the guest RFE's longjmp (setjmp frame below run_shadow_diff);
   epilogue skipped; s_in_shadow/s_native_exec/s_suppress_irq stuck forever.
   HARDENED in tree: `g_exc_setjmp_epoch` (interrupts.c) + 
   `overlay_loader_shadow_escape_fixup()` at all 3 longjmp sites; dump fields
   `escapes`/`escapes_native` must stay 0.
3. **THE WEDGE (design flaw, predates this session): run_shadow_diff never
   terminates for non-returning functions.** Interp pass = one
   dirty_ram_dispatch call (returns at first exit-to-elsewhere, NOT at stop_ra);
   native pass chases via psx_dispatch_call until pc==stop_ra — NEVER for a
   main-loop-shaped function. The live game then runs FOREVER inside the
   abandoned shadow native pass: **speculative native state becomes the live
   timeline**, IRQ-suppress stuck on, all downstream native disabled
   (native_exec=0). Verified live both runs: in_shadow=1, native_exec=0,
   escapes=0, wedge at frame ~1000. ⇒ Any pace/behavior data from a diff-mode
   run is garbage; the instrument was never capable of validating this game.

### The fix (designed, ready to implement): CPS segment-granular diff

Diff exactly ONE `c->fn` invocation extent (entry/interior PC → first
jal/jalr/jr/region-exit). Verified against generated DLL C: CPS codegen
surfaces at EVERY call/return/indirect jump (no local jump-table switches), so
a lean interp segment runner (in dirty_ram_interp.c; must intercept
jal/jalr/jr/j BEFORE exec_one — exec_one nests calls inline) can run an
identical extent. Both passes bounded by construction; no chasing, no fiber/RFE
exposure; validates interior re-entries directly; compares pc (boundary target)
as a first-class divergence signal. Abandon (no diff, interp state stands) on:
syscall/break/rfe, unsupported insn, insn cap, MMIO (device_touch as today).
Asymmetry guards: psx_slice_block no-fire in shadow (currently moot — slicing
default OFF); cycle-counter snapshot/restore around the native pass;
post-dispatch IRQ pump shadow guard (already in tree).

### Uncommitted worktree state (this session, runtime-only, cg tag intact)

- `overlay_loader.c`: CPS interior diff gate + `interior_gated`; in-exception
  shadow-start detour (superseded by segment design — remove when implementing);
  `s_shadow_cand` own-interior native route for shadow native pass; escape-fixup
  mirrors + `overlay_loader_shadow_escape_fixup()`; shadow dump fields
  `in_shadow`/`native_exec`/`escapes`/`escapes_native`; IRQ pump shadow guard.
- `interrupts.c`: `g_exc_setjmp_epoch` + getter; fixup calls in
  deferred_exception_longjmp + psx_rfe_escape_check.
- `traps.c`: fixup call at the deferred-honor longjmp site.

## Issue #10 — Access-violation crash after ~6-7h continuous runtime (overnight session)

**Status:** open, not yet investigated
**Date opened:** 2026-09-08

### Symptom

`VagrantStory_Recompiled.exe` crashed overnight after being left running
unattended for several hours (machine left on; game was idling, not
under active input). `psx_last_run_report.json` (written by the SEH
crash handler) recorded:

- `reason: "seh"`, exit at `2026-09-08T09:30:03Z`
- Guest frame counter at crash: **1,395,540** (~6-7h at 59.9 Hz)
- `seh.code: 0xC0000005` (access violation), **write** access
- `fault_addr: 0x00007FF4FCDC6EB4` — a large heap-range address, not
  near the module base (`module_base: 0x00007FF6FAF40000`), consistent
  with a dangling/corrupted pointer or an out-of-bounds write into a
  large heap allocation rather than a null/near-null deref
- Crash RIP: `module_offset 0x59EB1` in `VagrantStory_Recompiled.exe`
- `stack_scan` shows the same handful of offsets recurring
  (`0x83296`, `0x8C89C`/`0x8C6B8`, `0xBD70CE`/`0xBD6BF8`) — worth
  symbolizing first since they repeat both directly and via the deeper
  frames

### What's already ruled out

- Not a same-session regression from the alpha-normalization fix or
  the widescreen menu-PC fix landed earlier the same day — the crash
  happened many hours later, deep into idle overnight runtime, and
  `frame: 1,395,540` is far beyond where either of those code paths
  would misbehave on the first few thousand frames if they were wrong.

### Suspected contributing factor (unconfirmed)

`gpu_gl_renderer.c`'s HD-texture GL cache (`s_hd_tex_cache`, see
`hd_gl_cache_insert`) is documented in-tree as "Never shrinks/evicts —
a whole session's worth of HD textures for one game is not expected to
be a meaningful budget concern." That assumption was scoped to a normal
play session, not an unattended multi-hour idle run; worth checking
whether cache growth (or some other unbounded per-frame allocation)
correlates with the crash once the offsets above are symbolized.

### Next steps

- Symbolize `module_offset 0x59EB1` and the repeating `stack_scan`
  offsets against a matching build (`nightly-2-g1d4646f3-dirty`,
  2026-09-06 23:43:52 build) to identify the actual faulting function.
- Try to reproduce with a long unattended/idle soak test rather than
  active play, since that's the condition that triggered it.
- Check whether HD-texture GL cache size (`gl_renderer_hd_tex_cache_count`)
  or any other per-frame-growing structure correlates with uptime in a
  long soak.

## Issue #11 — Beetle-format HD pack: visible seam where partial mesh coverage meets native fallback

**Status:** FIXED (opt-in) via fused-page compositing, 2026-09-08
**Date opened:** 2026-09-08

### Symptom

A thin but clearly visible line/seam appears on the character's face (and
was separately observed on wall/door panels and the game's opening
cutscene backdrop) exactly where a mesh has PARTIAL Beetle-format HD
coverage: some triangles of the mesh match a replacement PNG, the rest
fall back to native rendering (confirmed via
`PSXRECOMP_HD_TEXTURE_DEBUG_MISSING=1`, which paints unmatched opaque
prims solid violet — the missing patch for the reproduction case is
palette hash `fedcf2e0`, four small skin/hair fragments exported to
`missing_textures/` and never added to the pack).

### Confirmed via live A/B (`hd_backend none|beetle`, same static scene,
same camera, same frame)

- `hd_backend none` (100% native, no HD replacement at all): **no seam**.
- `hd_backend beetle` (partial HD coverage + native fallback for the
  missing patch): **seam visible**, right at the HD/native boundary.

This conclusively ties the seam to mixing HD-replaced and native-rendered
triangles within one mesh — it is not present when the whole mesh
renders through a single path.

### What was tried and ruled out (both isolated via live rebuild + A/B,
same reproduction scene)

1. **Geometric sub-pixel overdraw** — pushed each HD triangle's vertices
   ~0.5 native-VRAM-px outward from its own centroid (cheap, safe given
   this renderer has no depth test anywhere — painter's-order only) on
   the theory that two independent GL draw calls (the immediate
   single-triangle HD draw vs. the batched native draw) could leave a
   sub-pixel rasterization gap at the shared edge despite using
   bit-identical vertex coordinates. **No visible change at all** —
   ruled out a geometric/coverage gap as the cause.
2. **Force full opacity in `HD_FS`** (`frag = vec4(rgb, 1.0)` instead of
   `vec4(rgb, c.a)`, blend left enabled) on the theory that the
   replacement PNG's own edge alpha (soft/anti-aliased border, real
   blending enabled since the earlier alpha-normalization fix) was
   bleeding into whatever was already in the framebuffer at that pixel.
   **Made the seam MORE visible, not less** — this is informative: the
   partial alpha blend that existed before was mildly *softening* a
   harder discontinuity underneath, not causing one. Ruled out
   edge-alpha framebuffer bleeding as the primary cause.

### Current best explanation (unconfirmed, not yet fixed)

Given both a geometry-level and a blend-level fix independently failed
(one making things worse when the blend was removed), the seam is most
likely a **color/brightness mismatch between the pack's replacement art
and the native/original texture** at the exact boundary where coverage
is incomplete — i.e. a content-authoring limitation of partial-coverage
HD packs, not a rendering pipeline bug. The Beetle-format matcher
(`gpu_hd_texture_pack_match`) is a plain exact `(texture_hash,
palette_hash)` dictionary lookup with 0 ambiguous keys (confirmed via
`hd_backend`'s live stats) — no approximate/cross-palette matching or
tint-compensation path exists for it (unlike DuckStation-format's
`hd_texture_dump_match`, which does derive a per-scene tint — see
`u_tint` in `HD_FS` — specifically so a lighting change still shows
correctly on an approximately-matched HD texture). Whether Beetle
lacking that tint path is *also* a contributing factor here was not
tested this session.

### Also checked: UV crop/off-by-one in `gpu_hd_texture_pack_match`

User hypothesis: a missing pixel in the HD texture's crop (`u_scale`/
`u_offset`/`v_scale`/`v_offset`, `gpu.c` lines ~307-380) could leave a
1-texel sliver unsampled at a panel edge. That formula already has an
extensive comment documenting a prior fix for a related-sounding bug
("sliver of a neighboring image in the same upload" from a truncating
division). Checked 3 already-matched pack entries' PNG pixel
dimensions against their native upload size reported by
`hd_match_ring`: `66c22116-94c4440b.png` (native 64x128 -> PNG
512x512), `24051f2c-d48e9846.png` (native 64x128 -> PNG 512x512),
`6d7af0a0-27781d7c.png` (native 32x64 -> PNG 256x256) — all three are
clean, consistent 8x-width/4x-height integer ratios with no fractional
remainder, so no general off-by-one crop bug is evident from this
sample. Could not check the actual file used at the Sydney-scene face
seam itself (never pinned down live — see above), so this does not
rule out an off-by-one specific to that file/fragment.

### Next steps

- Add the same derived-tint compensation Beetle format currently lacks
  (`hd_tint_r/g/b` are hardcoded to `1.0,1.0,1.0` for `hd_backend==1` in
  `gpu_textured_triangle`, see `gpu_gl_renderer.c`) and re-run the same
  A/B to see if it narrows or removes the seam.
- Failing that, the real fix is completing the pack's coverage for the
  specific missing patch(es) rather than a renderer change — paint/add
  the exported `missing_textures/*fedcf2e0*.png` fragments (and their
  equivalents for the wall/door-panel and intro-backdrop cases) into the
  active pack.
- The opening-cutscene backdrop lines are a SEPARATE, already-confirmed
  issue: `hd_backend none` removes them completely with FULL coverage
  (not partial), meaning that specific artifact is baked directly into
  one or more of the pack's own PNGs for that scene (see the session
  notes; the exact file(s) were never pinned down — `hd_match_ring`'s
  buffer is too small and evicts too fast under the intro's font-glyph
  draw traffic to catch a rarely-redrawn backdrop texture live). Not
  addressed by the fix below (it does not apply -- that scene has full
  coverage, not partial).

### The fix: fused-page compositing (opt-in, real-Beetle-inspired)

Researched the actual Beetle PSX HW Vulkan renderer's source
(`libretro/beetle-psx-libretro`, `HD_TEXTURE_CACHE.md`) for how it
avoids this class of seam. It never draws a primitive split across two
GL pipelines: when a draw's sample region spans multiple VRAM uploads
with mixed HD/native coverage, it composites everything into one
"fused page" texture first and draws the primitive once. Implemented
the same idea here, scoped down to the dominant real case (one query
rect, no UV wrap; one upload/entry whose fragments partially cover it)
rather than a fully general multi-upload compositor:

- `hd_texture_pack.cpp`: `hd_texture_pack_match_fused()` /
  `_match_fused_draw()` — reports each covered (HD) and uncovered
  (native) piece of a query hd_texture_pack_match already rejected as
  not-fully-covered, via the same exact rectangle-subtraction
  `covered_by_upload` uses (now keeping the remainder instead of
  discarding it). `hd_texture_pack_decode_native_rgba()` — a
  C-callable wrapper around the existing native-VRAM decode (shared
  with the missing-texture exporter) for the uncovered pieces.
- `gpu.c`: `gpu_hd_texture_pack_match_fused()` — texel-space wrapper,
  decodes native pieces up front (`GpuHdFusedPiece.native_rgba`, capped
  at `GPU_HD_FUSED_PIECE_MAX_DIM` 128px/axis — a piece bigger than that
  just makes the whole call return 0, falling back to today's
  behavior). `gpu_hd_texture_fusion_set/enabled()` — the opt-in flag.
- `gpu_gl_renderer.c`: `build_fused_composite()` — builds the composite
  in a dedicated small FBO (`FUSED_COMPOSITE_SCALE` 8 texels per native
  texel; see its comment for why), each native piece uploaded as its
  own tiny GL texture and drawn in, each HD piece drawn from the SAME
  cached GL texture the normal path already uses (no duplicate
  decode/upload), native pieces padded 1px into their neighbours as
  cheap insurance against real cracking (see that code's comment — NOT
  what fixed the actual reported seam; the composite scale did). No
  caching of the composite itself: this path is opt-in and rare enough
  (only partial-coverage triangles) that rebuilding it every draw was
  fine in testing. Wired into `gpu_textured_triangle` right after the
  plain HD match fails, before the `s_hd_debug_missing` violet-paint
  fallback.
- Config: `[video] hd_texture_page_fusion = true` in game.toml
  (`config_loader.h/.cpp`, `RuntimeGameConfig`), or
  `PSX_HD_TEXTURE_PAGE_FUSION=1` env override — off by default, exactly
  the "depende del juego si se activa" the user asked for. Deliberately
  NOT wired into the launcher's live-settings UI (seed/us/ls structs) —
  boot-time/per-title only, to keep the change small.

Confirmed live (2026-09-08): the "Have you found Sydney?" face seam is
gone with the feature on, both at 1080p and a real 3840x2160 window (4x
supersampling, the config's cap). A fainter artifact appeared when
`FUSED_COMPOSITE_SCALE` was dropped to 4 for comparison, understood to
be an inherent detail mismatch between the native piece (real
native-resolution content) and its much-more-detailed HD neighbours,
not a gap — see that constant's comment. 8 was kept as the shipped
default (16 looked identical at 4K, just costs more texture memory).

### Also done same day: matched real Beetle's hard alpha-cutoff behavior

Read real Beetle PSX HW's actual `command_fragment.glsl.h` (GL RHI
backend, `libretro/beetle-psx-libretro`) to look for other rendering
differences. Confirmed it uses a hard `if (opacity < 0.5) discard;`
with no real alpha blending anywhere, including for its own HD-texture-
replacement path (`hd_sample_nearest`/`hd_sample_trilinear` feed the
same opacity< 0.5 test). `HD_FS` here previously did real blending
(`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`, `frag = vec4(rgb, c.a)`) —
added earlier the same session to fix the credits-screen replacement's
anti-aliased text looking jagged under an 0.5 cutoff, but BEFORE the
`hd_normalize_alpha` fix (this pack's alpha commonly capped at
~128-140, not 255) landed. Re-tested with both fixes in place: reverted
`HD_FS` to `if (c.a < 0.5) discard;` / `frag = vec4(rgb, 1.0)` (matches
Beetle exactly) and confirmed live — no regression on the credits
screen or the Sydney-face fused-composite scene. Kept as the new
default: one less source of divergence from the reference renderer.

### Noted, not investigated further: vertex/texture color scale mismatch

While comparing against Beetle's shader, noticed this project's own
GL renderer uses two different 5-bit-to-float conventions in the same
color pipeline: texture colors (`col5()`, `gpu_gl_renderer.c`) scale by
`/31.0` (proportional, reaches 1.0 at max); vertex/Gouraud shading
colors (multiple call sites, e.g. lines building `cs[i]` vertex data)
scale via `(v5 << 3) / 255.0` (never quite reaches 1.0 at max, ~0.973).
These get multiplied together in `TEX_FS`/`HD_FS` (`rgb * v_col *
2.0`). Pre-existing code, not touched this session, and NOT a
candidate for today's HD-pack seam investigation specifically — it is
shared by every rendering path (native, DuckStation-format, and
Beetle-format all hit the same `v_col`/`col5` code), so it cannot
explain a difference that only shows up on Beetle-format partial
coverage. Left uninvestigated per the user's call — worth a live A/B
(a scene with an obvious shading gradient) in a future session, but no
evidence yet that it is visibly wrong.

- All inert when diff mode is off; normal play unaffected. Builds clean.
