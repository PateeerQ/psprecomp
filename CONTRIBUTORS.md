# Contributors

Thank you to everyone who has contributed code, fixes, testing, or a hard-won
debugging insight. This file is the canonical record of who did what. The
[CHANGELOG](CHANGELOG.md) tells the story version by version, but credit lives
here.

If you've contributed and aren't listed, or a line is wrong, open a PR against
this file. We want every name right.

---

## Maintainer

### Ned Heller — [@sp00nznet](https://github.com/sp00nznet)
Project creator and maintainer. The container stack, decryption, the Allegrex
decoder, function discovery, the C emitter, the runtime and HLE layer, and the
WTF: Work Time Fun bring-up recorded in [BRINGUP.md](docs/BRINGUP.md).

---

## Contributors

### andy — [@andyst-dev](https://github.com/andyst-dev)
Brought LocoRoco (UCES00304) up on the toolkit and fixed, at the root, what
stood in the way. Each PR came with the measurement that found the problem, a
test that fails on the old behaviour, and a clear statement of what it does
*not* fix. Landed in **v0.4.0 "Handed Over"**:

*Emitter correctness*
- **Fall-out continuation (#7).** A body whose last instruction falls through
  into a label owned by another body used to end its C function, which is a
  silent return. That cut a 656-byte-frame state machine off before its shared
  epilogue, so the caller reloaded `$s5` from the wrong slot and died on
  `jalr`. The body now continues through the label's thunk. Making that work
  needed two things: the label map built module-wide before any body is
  emitted, and forward declarations for the thunks. 579 sites on LocoRoco.
  This PR replaced their own #6 after they measured that #6's rule ran the
  continuation twice. They closed #6 themselves and explained why.
- **Delay slots belong to their branch (#10).** When discovery gives a slot's
  address to another body, the walk no longer steps over it, so the fall-out is
  found where control actually arrives. The branch's taken path now runs the
  slot too. Only slots that don't themselves transfer control are repeated,
  since those are architecturally unpredictable. This removed ten negative
  stack leaks, and two reads of the module's own code as a data table. The PR
  also lists what is still open: calls whose slot belongs to the next body, and
  likely branches on the not-taken path.
- **Body-ending transfers stay tail jumps (#11).** A stack observation placed
  after the final call cost a host frame per guest transfer. They showed this
  with a two-line `clang -O2` reduction and with `lldb` on a worker thread whose
  stack reached 16,385 KB of 16,396 KB. Without the observation the same loop
  ran 371 million iterations at 9 KB. The second commit gives the hand-over its
  own invariant (`PSP_SP_LEAVE`): `$sp` below the entry is expected mid-frame,
  and only `$sp` above it is a defect. That explains BRINGUP's unexplained
  `-64` group, and they verified it in the disassembly at three sites.
- **`viim`/`vfim` (#3).** The destination field was computed and then
  discarded, so the emitter wrote the wrong VFPU register. They cross-checked
  the field layout against an independent sleigh spec. Non-finite immediates
  had been emitted as `nanf`/`inff`, which do not compile.

*Runtime and instruments*
- **`psp_syscall`/`psp_unimplemented` defined in the runtime (#4).** Emitted C
  used to compile and then fail to link: 15,646 trap sites on LocoRoco, with
  nothing defining the symbol.
- **Rate-limited unimplemented-call log and a 64-bit zero-return counter
  (#5).** One run wrote 1.37 GB of identical lines. The `int` counter wrapped
  negative and indexed the ring out of bounds. The log now says when it goes
  quiet, so a silenced message can't be read as one that stopped happening.
- **Stack imbalances grouped by site (#8).** One row per `(site, delta)`,
  largest magnitude first, with the largest kept even when the table is full.
  A `-656` leak had been hidden behind 24 printed lines of `+16`.

Landed in **v0.5.0 "Taking Turns"**:
- **Real threads with an explicit CPU hand-over (#9).** One host thread per
  guest thread and a single baton, so `sceKernelStartThread` can return before
  the worker finishes. Sub-interrupt handlers and callbacks are delivered where
  the guest waits, on the owning thread. When asked, they put it behind
  `PSPRECOMP_REAL_THREADS`, wrote a native Win32 adapter alongside pthreads, and
  extended CI to Windows, Linux and macOS in both modes. An independent review
  they ran found three lifecycle and masking defects, and each now has a test
  that failed before the fix.

### Invictus-potens — [@Invictus-potens](https://github.com/Invictus-potens)
Brought up Monster Hunter Portable 3rd (NPJB40001). Landed in **v0.4.0
"Handed Over"**:
- **Thread entry points built as `lui`+`addiu` immediates (#2).** The entry
  passed to `sceKernelCreateThread` is constructed as data right before the
  call and is never a `jal` target, so discovery missed it and
  `sceKernelStartThread` dispatched to an address nothing had registered. They
  confirmed it in the disassembly (`lui $s1,0x882 / addiu $a1,$s1,0x182C`). On
  NPJB40001 the pass added 2,045 functions.
- **`sceKernelSleepThread` / `sceKernelSleepThreadCB` (#2)**, with NIDs
  verified against the SHA-1 of the name, as the rest of the file is. A game
  calling either one used to stall in the dispatch-miss loop.

### Tzeentchnet — [@Tzeentchnet](https://github.com/Tzeentchnet)
Measured the toolkit against Killzone: Liberation (`KZL.PRX`, v1.20 US). Every
PR states which emitted lines change on that module, carries a test that fails
on the old code, and names its sources. Landed in **v0.5.0 "Taking Turns"**:
- **VFPU load/store register fields (#12).** `lv.s`/`sv.s`/`lv.q`/`sv.q` took
  their register from the arithmetic field, which overlaps the base GPR. 10,019
  of 15,444 emitted VFPU loads and stores on KZL were using the wrong register.
- **`jr`/`jalr` target read before the delay slot (#13)**, and `jalr` links its
  encoded `rd`. Found in a hand-written ladder that steps its target in the slot.
- **`jal .+8` as get-PC (#14).** The phantom function had made the rest of the
  routine run twice.
- **VFPU conversion scale and saturation (#17), and `vf2in`/`vf2iu`/`vf2id`
  (#18).** 30 of KZL's 37 `vi2f` sites had a nonzero scale that was being
  dropped. Expected values are hardware measurements.
- Reported **#15** (forcing labels for computed jumps) and **#16** (`bgezal`
  to another function drops the code after the call).

*In review:* #19, jump-table operands traced to their definitions.

---

## A note on AI-assisted contributions

Parts of this project, and some contributions to it, were developed with the
help of AI coding tools. That's welcome here. What matters is that a human
understands, reviews and verifies every change before it lands. If you used an
assistant, say so in your PR and make sure you can stand behind the result.
