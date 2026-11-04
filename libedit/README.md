# libedit for zash

This is NetBSD's line-editing library, bundled so zash has line editing
on z/OS, which has no libedit package. It also builds on Linux and BSD,
so the port is exercised on every build.

## Provenance

- **Source:** <https://github.com/NetBSD/src>, commit
  `91af1a5842bfaed91ef5ba5178ffd758613c0e54` (2026-11-03).
- **From `lib/libedit/`:** every `.c` and `.h` file, `makelist`, and the
  manual pages `editline.3`, `editline.7` and `editrc.5`.
- **From NetBSD libc:** `lib/libc/gen/vis.c`, `lib/libc/gen/unvis.c`,
  `include/vis.h`, `common/lib/libc/string/strlcpy.c` and `strlcat.c`.
- **Left out:**
  - `config.h`: replaced by `port/config.h`.
  - The BSD-make `Makefile`.
  - `readline.c` and `readline/`: the GNU readline compatibility layer,
    which zash doesn't use and which needs the non-POSIX `getpwent_r`.
  - `TEST/`, `ChangeLog`, and the packaging files (`edit.expsym`,
    `libedit.pc`, `shlib_version`).

**Files under `netbsd/` are unmodified.** `make verify` checks them
against `netbsd.sha256` and rejects added or missing files. It needs
`sha256sum`, so run it on Linux.

Licenses: see [LICENSE](LICENSE). It covers 3-clause BSD (University of
California), 2-clause BSD (The NetBSD Foundation), and ISC (Todd C.
Miller; Ingo Schwarze). Binary redistributions of zash built with this
library must include those notices.

## How the port works

All porting lives in `port/`, so upstream sources compile as-is:

| File | Purpose |
|---|---|
| `port/config.h` | Replaces NetBSD's `config.h`. Selects the features for z/OS or for Linux/BSD, with no configure step. |
| `port/port.h` | Shared by every file. Defines the NetBSD `<sys/cdefs.h>` macros (`__RENAME`, `__RCSID`, `__arraycount`, `_DIAGASSERT`), turns on the libc helper implementations, and renames them with a `libedit_` prefix. |
| `port/namespace.h`, `port/nbtool_config.h` | Hooks that `vis.c`/`unvis.c` and `strlcpy.c`/`strlcat.c` already include. Both just include `port.h`. |
| `port/compat.c` | `issetugid`, plus `getline` and `wcsdup` on z/OS. |
| `port/zos/sys/cdefs.h`, `port/zos/sys/param.h` | Stand-ins for headers z/OS lacks. Only z/OS builds put this directory on the include path. |

Details worth knowing:

- **The `libedit_` prefix** keeps the vendored helpers from colliding with
  a libc that has its own: glibc 2.38 added `strlcpy`, and macOS has `vis`.
  A static libedit linked into zash exposes all of its symbols, so a clash
  is a link error, not a quiet override.
- **`issetugid()` is approximated** by comparing real and effective user
  and group IDs. libedit uses it only to decide whether to read
  `~/.editrc`. Without it, `el_source()` refuses to read the file at all.
  Unlike the BSD call, the approximation doesn't catch a program that ran
  set-id and has since dropped privileges.
- **`getline` and `wcsdup`** are POSIX.1-2008 functions that older z/OS
  releases may lack. On z/OS the compat versions are always used, under
  their own names. Build with `-DLIBEDIT_COMPAT_ALL` to exercise those
  versions on Linux.
- **ASCII builds only.** `map.c` binds keys through 256-entry tables
  indexed by ASCII code, so an EBCDIC build would bind the wrong keys.
  `port.h` stops the build with an `#error` instead of producing a broken
  library.
- **The termcap functions** (`tgetent`, `tgetflag`, `tgetnum`, `tgetstr`,
  `tgoto`, `tputs`) are libedit's only non-libc dependency. They come from
  ncurses on Linux and from X/Open curses (`-lcurses`) on z/OS.

## Building

The zash Makefile builds this directory automatically. To build it on its
own:

```sh
make CC=clang CFLAGS='-std=c11 -O2 -Wall -Wextra'
```

This produces `libedit.a` and `include/histedit.h`. Only `histedit.h` is
copied to `include/`, because several upstream headers (`search.h`,
`tty.h`) share names with system headers.

`makelist` generates `gen/*.h` with `sh`, `awk` and `sort`, exactly as
NetBSD's own build does. The `-fh` and `-fc` outputs must number functions
in the same order. Both use the same `sort`, so they agree even though
EBCDIC and ASCII sort differently.

Warnings: clang `-Wall -Wextra -pedantic` is clean. gcc reports three
`warn_unused_result` notes from glibc in upstream `vi.c` and `chartype.c`;
they are left as they are, rather than patching upstream.

## z/OS status

Not yet built on z/OS. Things to check there:

- **Feature macros.** z/OS builds use `-D_ALL_SOURCE -D_UNIX03_THREADS`:
  `u_char` and the BSD types need `_ALL_SOURCE`, and `terminal.c` uses a
  pthread mutex.
- **Termcap declarations.** Check that `<curses.h>` and `<term.h>` declare
  the termcap functions. If they don't, use zopen's ncurses:
  `make zos TERMLIB_ZOS='-L$ZOPEN_ROOT/lib -lncurses'`.
- **Terminal conversion.** z/OS terminals are EBCDIC, and libedit in
  zash runs in ASCII mode. It's not yet known whether `_BPXK_AUTOCVT=ON`
  (as zopen sets it) converts tty I/O for zash, or whether zash must turn
  conversion on for fds 0-2 at startup, as zopen's runtime library does
  for its tools. If the prompt or typed input looks garbled, this is the
  cause.

## EBCDIC (in progress)

EBCDIC support is being designed. The right design depends on facts that
must be measured on z/OS:

- how each compiler encodes wide literals (`L'a'` = 0x0081 or 0x0061);
- what `mbrtowc` returns for EBCDIC bytes;
- whether ASCII and EBCDIC compilation units can be linked together;
- whether system curses hands ASCII code EBCDIC escape sequences.

Run the probe on z/OS and keep its output:

```sh
sh libedit/port/probe/probe.sh            # -v shows compiler errors
CURSES_LIB='-L$ZOPEN_ROOT/lib -lncurses' sh libedit/port/probe/probe.sh
```

## Updating from upstream

1. Copy the files listed under Provenance from a newer NetBSD `src`
   checkout into `netbsd/libedit/` and `netbsd/libc/`.
2. Regenerate the checksums:
   `(cd netbsd && sha256sum libedit/* libc/* > ../netbsd.sha256)`.
3. Update the commit hash here and in `LICENSE`.
4. Rebuild, and check `LICENSE` against any new or changed file headers.
