# Native worker launcher (Windows)

`dsh-shim.exe` is the launcher `src/infra/environment.ts` copies into `<stateDir>/worker-bin` once per
command name when `process.platform === 'win32'`. Windows cannot execute a `#!/bin/sh` script from `PATH`,
so the POSIX shims that path writes elsewhere do not run there: `spawn gh ENOENT`.

Each copy reads `<its own directory>/shims.xml` and, for its own file name without the extension:

- `<run>PATH</run>` starts `PATH` with the caller's arguments, after removing the shim directory from the
  child's `PATH` so a wrapper that calls `gh` by name reaches the real binary instead of itself;
- `<refuse>MESSAGE</refuse>` prints `MESSAGE` to standard error and exits 1, for `lark-cli` without a
  Feishu bot and for every blocked command.

Exit code is the child's own, `1` for a refusal, `70` for a missing or incomplete `shims.xml`, and `124`
when the child outlives `DSH_SHIM_TIMEOUT_MS` (default ten minutes). Nothing waits without a bound.

The mapping is XML rather than a key-value file because the shim contract must not drift while workers
run; a document with a fixed shape survives concurrent rewrites that a mutable map does not.

## Build

Any Windows C toolchain works. With MSYS2 / MinGW-w64:

```sh
gcc -O2 -municode -Wall -Wextra -o native/build/dsh-shim.exe native/dsh-shim.c
```

`native/build/` is produced by the build step; `src/infra/environment.ts` resolves it relative to the
compiled `lib/index.js`.
