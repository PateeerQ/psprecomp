# Contributing to psprecomp

Thanks for your interest in the PSP static-recompilation toolkit. This page
covers building, testing, what a PR needs, and where your code may come from.

## Development environment

- **Windows 10/11 with Visual Studio 2022** (C workload). MSVC is the reference
  compiler, and every change has to build with it.
- **CMake 3.16+**
- **Git**

No other dependencies. The crypto, decoder and runtime are self-contained.

```
git clone https://github.com/sp00nznet/psprecomp.git
cd psprecomp
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The expected result is `100% tests passed, 0 tests failed out of 10`. Every
suite uses synthetic code and needs no game data.

## What a PR needs

- **It builds and passes `ctest` with MSVC.** A runtime change that needs
  POSIX-only APIs (`pthread.h`, `clock_gettime`) needs a Windows path too. Use
  C11 `<threads.h>`, Win32, or a small shim.
- **A test that fails on the old behaviour.** For the emitter, hand-build the
  analysis (see `hand_built()` in `tests/test_emit.c`) rather than relying on
  discovery order. Read generated output from the split units
  (`<prefix>_funcs_000.c`, `<prefix>_funcs.h`).
- **No silent skips.** A check that returns early because a file is missing
  has not passed. Make it fail. Remember that output written on Windows is
  CRLF.
- **Say whether it changes behaviour for every title or is opt-in**, and which
  titles you checked. Discovery and emitter changes affect every game, so give
  before/after numbers (`allegrexrecomp funcs` and `emit`).
- **Game-specific values stay in the game repo.** The toolkit gets the general
  mechanism: a hook, a flag, an environment variable.
- **Measure, don't reason.** The PRs that landed fastest showed the address,
  the instructions and the before/after run. [BRINGUP.md](docs/BRINGUP.md)
  shows why: every wrong turn there came from inference.

## What never goes in a PR

Section 3 of the house rules, at full strength:

- No game data: ISOs, EBOOTs, PRXs, decrypted modules, saves, or assets.
- No keys. Users supply their own ([DECRYPT.md](docs/DECRYPT.md)).
- No generated output: emitted C, disassembly listings, or symbol maps lifted
  from a retail binary, including as test fixtures. Individual instruction
  words quoted in a test, as `tests/test_decode.c` does, are fine. A dump is
  not.

## Where your code comes from

Contributions must be your own work, or come from a source whose licence is
compatible with MIT (MIT, BSD, zlib, Apache-2.0, ISC, public domain).
Specifically:

- **Don't port code from GPL projects.** For the PSP that means PPSSPP and
  JPCSP above all, and other PSP recompilers that carry PPSSPP code. Rewriting
  it line by line doesn't change that. Running an emulator as a black box and
  comparing its output with ours is fine, and so is citing what you observed
  ("checked against PPSSPP's output"). Carrying its code or its structure
  across is not. If you saw a fix in another project, describe the bug in the
  PR and we'll write it fresh.
- **Hardware documentation, SDK headers under a permissive licence (PSPSDK is
  BSD) and measurements of the game itself are all fine.** Say which you used.
- **If something came from elsewhere, say so in the PR**, with a link, and keep
  any copyright header. A licence problem caught in review costs a comment.
  One found after release means untangling history.
- The same applies to AI-assisted code: if it looks like it reproduces an
  existing project, check where it came from before submitting it.

## Merging and credit

Community PRs are merged with a merge commit, never squashed or rebased, so
your commits keep your authorship and you appear in GitHub's contributor list.
If your PR needs changes to integrate, for example because `main` moved under
it, the maintainer adds separate fixup commits on top that name your PR. Your
commits are never rewritten.

Contributors are listed in **[CONTRIBUTORS.md](CONTRIBUTORS.md)**, and that
includes people who only filed an issue. A good bug report is a contribution.
If you land something and your name doesn't appear, that's our mistake: open a
PR against the file, or say so on the issue.

## Where to ask

**[The sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** is the
community hub for these projects. It's the quickest way to find out whether
someone is already working on something, or already stuck on it.

## License

MIT. By submitting a contribution you agree that it is released under the same
licence.
