# Changelog

Versions follow [SemVer](https://semver.org/). v0.4.0 is the first tagged
release. Entries v0.1.0 through v0.3.0 were reconstructed from the commit
history and are dated by when the work landed. No tags existed for them at
the time, and none were added afterwards.

### Unreleased

*The first game frame: WTF's publisher notice, drawn by the software
renderer.*

**Rendering**
- **Textures.** 565/5551/4444/8888 and CLUT 4/8/16/32-bit, linear or
  swizzled, with the CLUT captured at CLOAD as the hardware does. Texture
  functions modulate, decal, blend, replace and add. Alpha test, blending
  (every factor and equation), clear mode with its colour/alpha flags, scissor,
  16-bit framebuffers, and vertices with no colour take the material colour.
  DXT is drawn magenta and counted.
- **Transformed geometry.** World, view and projection matrices, viewport and
  offset, UV scale and offset, and normalised 8/16-bit vertex formats. A
  primitive with a vertex behind the eye is dropped and counted, since there is
  no clipping yet.
- **`psp_ge_dump_stats()` lists every undecoded GE command** by opcode, with
  its last argument, instead of printing the first 64 to stderr.

**VFPU**
- **`vmmul` transposes its first operand.** `vmmul.q M000, E100, M200` is
  `M100 * M200`. That is how WTF multiplies its matrices, and without the
  transpose its projection matrix reached the GE transposed. The orientation was
  flagged as unverified in the source; it is now pinned by a hand-worked test.

**Runtime and HLE**
- **The GE framebuffer address is VRAM-relative.** FBP `0x88000` drew into
  main RAM at `0x00088000`, which for a vaddr-0 module is its own code. On WTF
  Lumberjack every clear overwrote a jump table and the screen stayed empty.
- **`sceAudioOutputBlocking`/`sceAudioOutputPannedBlocking` block** for one
  buffer's play time, through a new `psp_threadman_delay()`. Returning at once
  let a mixer thread spin and starve every other thread under real threads.

### v0.5.0 — *"Taking Turns"* (October 2026)

*Six pull requests from two people. Guest threads can now really take turns,
behind a build flag. A new contributor, measuring against Killzone: Liberation,
fixed five places where the decoder, the emitter or discovery read something at
the wrong moment or from the wrong field.*

**Runtime and HLE**
- **Opt-in cooperative threads (`-DPSPRECOMP_REAL_THREADS=ON`).** One host
  thread per guest thread and a single CPU baton, handed over at blocking
  calls. `sceKernelStartThread` no longer has to run the entry to completion,
  so a caller can post work to a worker that waits on it. Sub-interrupt
  handlers and callbacks are delivered where the guest waits, on the thread
  that owns them. Native Win32 and pthreads adapters. The default build keeps
  the inline model. Also `sceKernelMaxFreeMemSize` and a deeper trace history.
  See [THREADING.md](docs/THREADING.md) —
  *[@andyst-dev](https://github.com/andyst-dev)* (#9)
- **`vi2f`/`vf2iz` apply their 5-bit scale, and the float-to-int conversions
  saturate** to int32, with NaN giving `0x7FFFFFFF`. `vi2f.q ..., 16` was
  coming out 65536 times too large. Checked against hardware values recorded
  on a PSP-3000 —
  *[@Tzeentchnet](https://github.com/Tzeentchnet)* (#17)
- **`vf2in`/`vf2iu`/`vf2id` are emitted** (nearest-even, up, down) instead of
  trapping —
  *[@Tzeentchnet](https://github.com/Tzeentchnet)* (#18)

**Decoder**
- **`lv.s`/`sv.s`/`lv.q`/`sv.q` read their register from the load/store
  fields.** It used to take bits from the base GPR. 10,019 of 15,444 VFPU
  loads and stores on KZL changed register —
  *[@Tzeentchnet](https://github.com/Tzeentchnet)* (#12)

**Emitter**
- **`jr`/`jalr` read their target before the delay slot runs**, and `jalr`
  links the `rd` it encodes (nothing for `$zero`). A slot that steps the
  target register no longer moves the jump —
  *[@Tzeentchnet](https://github.com/Tzeentchnet)* (#13)

**Discovery**
- **`jal .+8` is the get-PC idiom, not a call.** Its target is no longer made
  a function, so the rest of the routine no longer runs twice —
  *[@Tzeentchnet](https://github.com/Tzeentchnet)* (#14)

**Known open**
- `bgezal`/`bltzal` to another function drops the code after the call (#16).
- Computed jumps into unlabelled blocks need a way to force labels (#15).
- Jump-table operands traced to their definitions (#19) is in review.

### v0.4.0 — *"Handed Over"* (October 2026)

*The first outside contributions: eight pull requests from two people. Most of
it came from bringing LocoRoco (UCES00304) and Monster Hunter Portable 3rd
(NPJB40001) up on the toolkit, and it fixed the open lead in
[BRINGUP.md](docs/BRINGUP.md). On the WTF Lumberjack module the object pointer
that was coming out as zero is now correct. The run makes no bad memory
accesses where it used to make 253 million, and it now stops at the thread
model rather than on corrupted state.*

**Emitter**
- **A body that falls out into another body's label continues there**, through
  that label's thunk, instead of silently returning. 579 bodies on LocoRoco
  ended this way. The label map is now built module-wide before any body is
  emitted, and the thunk declarations go in the shared header —
  *[@andyst-dev](https://github.com/andyst-dev)* (#7)
- **A delay slot belongs to its branch.** A slot that another body owns is no
  longer stepped over, so the fall-out is looked for at the right address. The
  branch's taken path now runs the slot too. On LocoRoco this removed ten
  negative stack leaks and two reads of the module's own code as a table —
  *[@andyst-dev](https://github.com/andyst-dev)* (#10)
- **A body-ending transfer stays a tail jump.** Nothing is emitted between the
  call and the `return`, so the compiler can reuse the frame. A guest loop
  whose back-edge crosses a body boundary used to exhaust a 16 MB host stack.
  It now runs at 9 KB —
  *[@andyst-dev](https://github.com/andyst-dev)* (#11)
- **`viim`/`vfim` write the register the encoding names** (bits 16..22, not the
  generic `vd`), and non-finite immediates are spelled `NAN`/`INFINITY` rather
  than the invalid tokens `nanf`/`inff` —
  *[@andyst-dev](https://github.com/andyst-dev)* (#3)

**Discovery**
- **Code pointers built with `lui`+`addiu`/`ori` are harvested as entries.**
  This catches the thread entry handed to `sceKernelCreateThread`, which is
  never a `jal` target —
  *[@Invictus-potens](https://github.com/Invictus-potens)* (#2)

**Runtime and HLE**
- **`psp_syscall()` and `psp_unimplemented()` are defined by the runtime**
  (`src/traps.c`), so emitted C links without a host shim. They come with
  counters and a reset —
  *[@andyst-dev](https://github.com/andyst-dev)* (#4)
- **The unimplemented-call message is rate-limited.** One 30-second LocoRoco
  run wrote 1.37 GB of it. The message now prints a bounded number of times,
  then one line saying further calls are only counted. The zero-return ring
  counter is 64-bit, so it no longer wraps negative and reads out of bounds —
  *[@andyst-dev](https://github.com/andyst-dev)* (#5)
- **Stack imbalances are grouped by site.** `psp_sp_hist()` and
  `psp_sp_call_hist()` return one row per distinct `(site, delta)`, largest
  magnitude first, so a `-656` leak no longer hides behind a crowd of `+16`s —
  *[@andyst-dev](https://github.com/andyst-dev)* (#8)
- **A hand-over is judged by its own sign.** `PSP_SP_LEAVE` /
  `psp_trace_sp_leave` treat `$sp` below the entry as expected at a fall-out,
  and report it only when it is above. This explains BRINGUP's `-64` group —
  *[@andyst-dev](https://github.com/andyst-dev)* (#11)
- **`sceKernelSleepThread` and `sceKernelSleepThreadCB`** are registered, with
  the same immediate-return approximation as `sceKernelDelayThread` —
  *[@Invictus-potens](https://github.com/Invictus-potens)* (#2)

**Maintainer fixups during integration**

All eight PRs were written against `caca759`, before the corpus was split into
translation units (`39f1f4b`). These fixups are mine, one commit each:
- #2's harvest now only accepts targets inside the scanned range. Without that
  restriction it lifted `.data` as code: 1,282 extra functions and 1,125 extra
  VFPU trap sites on Lumberjack. With it, #2 adds 78 functions there and
  87–214 on each of the other five WTF modules.
- The #7 merge resolution puts the thunk declarations in the shared header, so
  that every unit sees them, and runs the module-wide label pass in `dump`
  too.
- #3, #7 and #11's tests read split units. #11's checks had been skipping
  silently on that and also failed on Windows line endings; both are fixed.
- #10's fall-out check expects #11's `PSP_SP_LEAVE`, and #11's high hand-overs
  are recorded in #8's site report.

**Not in this release:** #9 (real threads with an explicit CPU hand-over) is
still under review. It needs a build that works on MSVC.

### v0.3.0 — *"Units"* (September 2026)

- **The emitted corpus is split into translation units**: a shared header, an
  imports file, a registration file, and numbered `<prefix>_funcs_NNN.c` files
  of 64 functions each. A unit whose contents did not change is not rewritten,
  so a build system rebuilds only what changed. (`39f1f4b`)
- **`allegrexrecomp dump <file> <addr>`** prints one function's emitted C. An
  address inside a function dumps the function that owns it. (`39f1f4b`)

### v0.2.0 — *"Measure, Don't Reason"* (July 2026)

*Bring-up on WTF: Work Time Fun's Lumberjack module, one fault at a time. Every
fix here came from logging an address or reading the instructions. Every wrong
turn, and there were about eight, came from inference.
[BRINGUP.md](docs/BRINGUP.md) keeps all of it, retractions included.*

**Emitter**
- 43% of functions were entered at the wrong address. When a backward jump
  pulls a block below the entry, the body now dispatches to the real entry.
- Fall-through into the next function is emitted (1,548 functions had been
  silently truncated). An interior gap owned by another function gets an
  explicit transfer (1,351 sites).
- Every label is dispatchable through a thunk, so computed jumps resolve by
  construction (13,682 labels on Lumberjack).
- `jal` and `jalr` set `$ra`.
- A stack-balance checker (`PSP_SP_ENTER`/`PSP_SP_CHECK`) catches leaks at the
  function that causes them.

**Discovery**
- Discovery iterates to a fixpoint, with zero dispatch misses on Lumberjack.
- Backward-`j` targets claimed by another walk are promoted to entries, as
  branch targets already were.
- The export table is parsed correctly, and executable code outside `.text`
  is walked.
- Jump-table resolution was added.

**Runtime and HLE**
- Static constructors are discovered and run (`src/ctors.c`); the loader
  normally does this, and nothing was doing it.
- `psp_arg` reads firmware arguments 5–8 from `$t0`–`$t3`, not from the stack.
- `sceKernelAllocPartitionMemory` no longer rejects valid attributes, so the
  game's 15.9 MB heap allocation succeeds.
- Import stubs are traceable. `psp_hle_register_unnamed` was added, along with
  `psp_trace_watch`, bad-access logging and a dispatch budget.
- GE display lists are rasterized: triangles, strips and sprites in
  through-mode. Rendering is split into interpretation and presentation behind
  a backend interface.
- VFPU gains `vidt`, `vcst`, `viim` and `vfim`.

**Docs**
- The README was rewritten in the house style, and corrected: a PSP static
  recompiler already existed. `docs/BRINGUP.md` records the investigation.

### v0.1.0 — *"Phase One"* (July 2026)

*A retail PSP game, from disc image to C that compiles, links and runs, in one
day.*

- **Container stack**: ISO9660, PBP, `~PSP`/`~SCE`, ELF/PRX and PARAM.SFO.
  `info` recurses the whole stack.
- **Decryption**: self-contained AES-128/192/256 and AES-CMAC, checked against
  FIPS-197, SP 800-38A and RFC 4493, and KIRK CMD1 decrypt-and-verify. Keys are
  never bundled ([DECRYPT.md](docs/DECRYPT.md)).
- **Allegrex decoder**: MIPS32r2, the Sony extensions, COP1 and the VFPU, with
  analysis flags on every instruction.
- **Function discovery**: recursive descent plus a linear `jal` harvest, and
  relocation mining, which took coverage from 75% to 89%.
- **C emitter**: a real PSP module compiles, links and runs. Imports are named
  and grouped into per-library stub sections.
- **HLE**: ThreadManForUser, SysMemUserForUser, sceDisplay, sceGe_user,
  sceSasCore, file I/O, Kernel_Library, Utils, Stdio, Ctrl, Audio and
  ModuleMgr. Every NID is verified against the SHA-1 of its name.
- **VFPU**: the register file, load/store, and the common arithmetic and
  matrix ops. Trap sites went from 199 to 30 on the first module.
- **CLI**: `info`, `ls`, `extract`, `dis`, `cover`, `funcs`, `emit`,
  `decrypt`, `kirk1`.
