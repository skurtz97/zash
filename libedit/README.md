# libedit for zash

This is NetBSD's line-editing library, bundled so zash has line editing
on z/OS, which has no libedit package. It builds in ASCII mode on Linux,
BSD and z/OS, and in IBM-1047 EBCDIC mode on z/OS.

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

**`netbsd/` is upstream plus [`patches/zash.patch`](patches/zash.patch)
(9 files, 86 changed lines) and nothing else.** The patch is for EBCDIC
and xlc; in an ASCII build it changes no behaviour. `make verify` proves
the claim: it undoes the patch on a copy, checks every file against
`netbsd.sha256`, and rejects added or missing files. It needs `patch`
and `sha256sum`, so run it on Linux. After editing an upstream file,
regenerate the patch from an unmodified copy with
`make patch PRISTINE=dir`.

Licenses: see [LICENSE](LICENSE). It covers 3-clause BSD (University of
California), 2-clause BSD (The NetBSD Foundation), and ISC (Todd C.
Miller; Ingo Schwarze). Binary redistributions of zash built with this
library must include those notices.

## How the port works

Porting lives in `port/`; the upstream patch only swaps hard-coded ASCII
for `port.h` macros:

| File | Purpose |
|---|---|
| `port/config.h` | Replaces NetBSD's `config.h`. Selects the features for z/OS or for Linux/BSD, with no configure step. |
| `port/port.h` | Shared by every file. Defines the NetBSD `<sys/cdefs.h>` macros (`__RENAME`, `__RCSID`, `__arraycount`, `_DIAGASSERT`), turns on the libc helper implementations, and renames them with a `libedit_` prefix. |
| `port/namespace.h`, `port/nbtool_config.h` | Hooks that `vis.c`/`unvis.c` and `strlcpy.c`/`strlcat.c` already include. Both just include `port.h`. |
| `port/compat.c` | `issetugid`, plus `getline` and `wcsdup` on z/OS. |
| `port/charset.c` | IBM-1047 <-> ISO 8859-1 tables, used only by EBCDIC builds. |
| `port/test/charset_test.c` | Tests the EBCDIC macros and table; built with the library and run by zash's `make check`. |
| `port/probe/` | The z/OS facts the EBCDIC design rests on; `sh port/probe/probe.sh`. |
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
- **The termcap functions** (`tgetent`, `tgetflag`, `tgetnum`, `tgetstr`,
  `tgoto`, `tputs`) are libedit's only non-libc dependency. They come from
  ncurses on Linux and from X/Open curses (`-lcurses`) on z/OS.

## Building

The zash Makefile builds this directory automatically. To build it on its
own:

```sh
make CC=clang CFLAGS='-std=c11 -O2 -Wall -Wextra'
```

This produces `libedit.a` and `include/histedit.h`. The archive's members
are named `le_*.o`. The z/OS binder (as xlc drives it) skips an archive
member whose name matches an object already on the link line, and zash
has its own `prompt.o`, `parse.o` and `history.o`. Only `histedit.h` is
copied to `include/`, because several upstream headers (`search.h`,
`tty.h`) share names with system headers.

`makelist` generates `gen/*.h` with `sh`, `awk` and `sort`, exactly as
NetBSD's own build does. The `-fh` and `-fc` outputs must number functions
in the same order. Both use the same `sort`, so they agree even though
EBCDIC and ASCII sort differently.

`make test-ebcdic` (Linux, needs gcc) checks the EBCDIC side without
z/OS. It runs the charset tests compiled with gcc's EBCDIC literals
(`-fexec-charset=IBM1047 -funsigned-char`), then compiles every source
that way. That catches `case` labels that collide and initializers that
stop being constant once characters are EBCDIC. It can't run libedit
itself: glibc reads strings as ASCII.

Warnings: clang `-Wall -Wextra -pedantic` is clean. gcc reports three
`warn_unused_result` notes from glibc in upstream `vi.c` and `chartype.c`;
they are left as they are, rather than patching upstream.

## EBCDIC

`port/probe/probe.sh` measured these on z/OS with Open XL and xlc in
EBCDIC mode:

- `'a'` is 0x81 and `L'a'` is 0x0081;
- `mbrtowc("a")` returns 0x0081;
- `iswalpha` agrees;
- `'\n'` is 0x15.

So narrow literals, wide literals and the C library all use native
EBCDIC values, and libedit runs on native characters. Only its ASCII
arithmetic is translated, by the `port.h` macros the patch introduces:

- **Key tables** (`map.c`) are written in ASCII order and installed at
  `EL_A2N(i)`. Meta bindings (ASCII 0200-0377) become ESC-prefixed key
  sequences as before, with the key translated.
- **Meta key** (`read.c`): the meta bit is set in ASCII terms
  (`EL_META`); in EBCDIC, letters already have the high bit set.
- **Control characters:** `tty.h`'s `CONTROL()` defaults (`EL_CTRL_C`),
  the `^X` display (`EL_UNCTRL`), DEL and ESC (`chared.c`, `chartype.c`),
  and the arrow-key defaults (`terminal.c`, `EL_WESC`).
- **`.editrc` escapes** (`parse.c`): `\a \b \t \n \v \f \r` are written as
  C escapes, so they're native; `\e` and `^[` are ESC; `\nnn` is an
  ASCII code and is translated. So `.editrc` lines written for Linux,
  such as `bind "\033[A" ed-prev-history`, work unchanged.
- **Constant contexts:** `case` labels and static initializers need
  constants, so `EL_ESC`, `EL_DEL` and `EL_CTRL_C` are constant
  expressions; the rest use the tables.

The table pairs EBCDIC 0x15 (NL) with ASCII LF, as z/OS does. glibc's
IBM1047 table pairs LF with 0x25. On z/OS, `charset_test` compares the
table with the system's own iconv IBM-1047 conversion, so a mismatch
fails `make check`.

Limits:
- **IBM-1047 only.** `port.h` rejects other EBCDIC code pages, and DBCS
  isn't supported.
- **Unverified on z/OS:** EBCDIC line editing hasn't been tried there
  yet.

The patch also fixes two things for both modes on z/OS:
- the `chartype.h` ISO 10646 guard, which z/OS fails even in ASCII mode;
- xlc's rejection of `va_arg(va, const int)` (`history.c`).

## z/OS status

- **ASCII builds use system curses** (`-lcurses`) by default. If that
  library returns EBCDIC escape sequences to ASCII code, cursor movement
  will be garbled; zopen's ncurses would then be needed:
  `make zos TERMLIB_ZOS='-L$ZOPEN_ROOT/lib -lncurses'`. EBCDIC builds
  and system curses always agree.
- **Terminal conversion in ASCII builds.** z/OS terminals are EBCDIC.
  If the prompt or typed input looks garbled in an ASCII-mode zash,
  either `_BPXK_AUTOCVT` isn't converting the tty, or zash must turn
  conversion on for fds 0-2 itself. EBCDIC builds don't have this
  problem.

## Updating from upstream

1. Copy the files listed under Provenance from a newer NetBSD `src`
   checkout into `netbsd/libedit/` and `netbsd/libc/`.
2. Regenerate the checksums:
   `(cd netbsd && sha256sum libedit/* libc/* > ../netbsd.sha256)`.
3. Re-apply the patch with `(cd netbsd && patch -p1 < ../patches/zash.patch)`,
   fix any rejected hunks, and regenerate it with
   `make patch PRISTINE=<the unmodified copy>`.
4. Update the commit hash here and in `LICENSE`.
5. Rebuild, run `make verify` and `make test-ebcdic`, and check `LICENSE`
   against any new or changed file headers.
