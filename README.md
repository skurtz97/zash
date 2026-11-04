# zash

An Almquist-family POSIX shell for z/OS UNIX System Services that also
builds on Linux and BSD. It has line editing through a bundled port of
NetBSD libedit, bash-compatible
prompts and history, and z/OS support for `spawn()`, file tagging, MVS
data sets and TSO.

The line-editing library in `libedit/` is NetBSD code. Apart from the
port layer, the only change is a small, verified patch for EBCDIC and
xlc; see [libedit/README.md](libedit/README.md).
FreeBSD sh, oksh and BusyBox ash were used as references only. The shell is
new, and nothing was copied from BusyBox (GPLv2).

## Status

Early, but usable for scripting on Linux:

- The test suite passes with gcc and clang (`-std=c11 -Wall -Wextra
  -pedantic`), with or without libedit.
- Output matches dash on the portable tests.
- In-shell work (loops, arithmetic, expansion, `read`) is as fast as dash
  or faster; see [Performance](#performance).

**The z/OS platform layer has never been compiled on z/OS.** It was written
from IBM documentation and checked only against stub headers. Calls still
to be confirmed on a real system are marked `VERIFY` in the source.

Missing: job control (`fg`, `bg`, `jobs`, `^Z`), `trap`, `kill`, `getopts`,
`printf`, `command`, `alias`, `ulimit`. See [Limitations](#limitations).

## Building

Any POSIX `make` works (tested with GNU make and bmake). There are three
toolchains:

| Command | Platform | Compiler |
|---|---|---|
| `make` (or `make linux`) | Linux, BSD | `clang` |
| `make zos` | z/OS | IBM Open XL C/C++ (`ibm-clang -m64`) |
| `make xlc` | z/OS | IBM z/OS XL C (`xlc -q64`) |

```sh
make                                     # Linux/BSD, clang
make CC=gcc                              # if clang isn't installed
make EDIT_CPPFLAGS= EDIT_LIBS= EDIT_DEP= # no line editing (any target)
make check                               # run the tests
make install PREFIX=$HOME/.local         # installs bin/zash
make dist                                # zash.tar for transfer to z/OS
```

Line editing uses the bundled NetBSD libedit in `libedit/`, which is built
automatically and linked statically. Its one outside dependency is the
termcap functions from curses: ncurses on Linux (`ncurses-devel` on
Fedora, `libncurses-dev` on Debian/Ubuntu), and system curses
(`-lcurses`) on z/OS. Override the library with `TERMLIB=` on Linux or
`TERMLIB_ZOS=` on z/OS. To use the system libedit instead:

```sh
make EDIT_CPPFLAGS=-DWITH_LIBEDIT EDIT_LIBS=-ledit EDIT_DEP=
```

`check` and `install` use the `zash` already built and never rebuild it.
That way `make zos` followed by `make check` tests the z/OS binary.

Changing the compiler or flags deletes the old objects and rebuilds
everything, so the same tree can switch between `make zos` and `make xlc`.

### z/OS

Both z/OS builds default to ASCII mode, the zopen convention. Clear the
ASCII macro for an EBCDIC build:

```sh
make zos                     # Open XL, ASCII
make zos OXL_ASCII=          # Open XL, EBCDIC
make xlc                     # XL C, ASCII
make xlc XLC_ASCII=          # XL C, EBCDIC
```

- **Open XL** builds with `-m64`. `ibm-clang` defaults to 31-bit
  non-XPLINK (`-m32n`), which rejects `-fzos-le-char-mode=ascii`.
- **XL C** builds with `-q64 -qxplink -qlanglvl=extc1x`; `extc1x` is xlc's
  C11 level. xlc has no `-Wall` or `-Wextra`. The ASCII flags
  (`-qascii -D_ENHANCED_ASCII_EXT=0xFFFFFFFF`) haven't been checked on a
  real system yet.
- **Overrides:** use `OXL_CC`, `OXL_CFLAGS`, `OXL_LDFLAGS`, `XLC_CC`,
  `XLC_CFLAGS`, `XLC_LDFLAGS` or `ZOS_CPPFLAGS`. Plain `CFLAGS` only
  applies to the Linux build.

To copy the source to z/OS, run `make dist` on Linux, which writes
`zash.tar` (ustar format). Transfer it in binary mode: `sftp` does that by
default; with FTP, use `binary`. On z/OS:

```sh
pax -rf zash.tar -ofrom=ISO8859-1,to=IBM-1047 -o setfiletag
cd zash && make zos && make check
```

The `pax` options convert the files to EBCDIC and tag them as IBM-1047
text. That works for both compilers, for `/bin/sh` and, through automatic
conversion, for an ASCII-mode zash. All shipped files are plain ASCII, so
the conversion is lossless.

Cloning with git on z/OS (zopen or IBM Open Enterprise git) gives the
same result: `.gitattributes` asks for an IBM-1047 working tree, so
`git clone` and then `make zos` work directly with no `pax` step. Run
`make verify` in `libedit/` on Linux only: it checksums the vendored
sources as ASCII bytes.

Line editing works in both modes. In EBCDIC mode libedit runs on
native IBM-1047 characters, and `make check` also runs a test comparing
its character table with z/OS's own conversion. Other EBCDIC code pages
aren't supported.

To use zopen's ncurses instead of system curses:

```sh
make zos TERMLIB_ZOS='-L$ZOPEN_ROOT/lib -lncurses'
```

Neither mode has been fully built on z/OS yet;
[libedit/README.md](libedit/README.md) lists what to check.

## Usage

```sh
zash script [arg...]
zash -c 'command' [name [arg...]]
zash -s [arg...]         # read commands from stdin
zash -i                  # force interactive
```

Options follow `set`: `-C -e -f -n -u -v -x`, plus `-o` with
`noclobber errexit noglob noexec nounset verbose xtrace vi emacs`.

When invoked as a login shell (argv[0] starts with `-`), zash reads
`/etc/profile` and then `~/.profile`. An interactive shell then reads the
file named by `$ENV`.

### Interactive features

**Line editing.** libedit provides emacs bindings by default, or vi with
`set -o vi`. It reads `~/.editrc` for custom bindings.

**Completion.** TAB completes the word under the cursor:

- **First word of a command:** keywords, builtins, functions and PATH
  programs. This also applies after `|`, `&&`, `;`, `$(`, `then`, `do`,
  `sudo`, `env` or a `NAME=value` assignment.
- **`$` prefix:** variable names.
- **Anything else:** files. Directories get a trailing `/`, and `~/`
  expands.

Completions are quoted for where the cursor is: backslashes outside
quotes, the right escapes inside `"..."` or `'...'`. With several matches,
TAB fills in the common prefix and then lists the rest in columns.

**Prompts.** `PS1` and `PS2` use bash's escapes:

| Escape | Meaning |
|---|---|
| `\u` `\h` `\H` | user, short host, full host |
| `\w` `\W` | working directory (with `~`), its basename |
| `\$` | `#` for root, otherwise `$` |
| `\t` `\T` `\@` `\A` `\d` `\D{fmt}` | time and date |
| `\j` `\!` `\#` | job count, history number, command number |
| `\e` `\nnn` `\n` `\\` | escape, octal byte, newline, backslash |
| `\[ \]` | wrap non-printing text such as color codes |

After the escapes, the prompt is expanded like a variable, so `$VAR` and
`$(cmd)` work. An existing bash prompt should work unchanged:

```sh
PS1='\[\e[32m\]\u@\h\[\e[0m\]:\[\e[34m\]\w\[\e[0m\]\$ '
```

Names produced by `\w`, `\u` and `\h` are quoted before that expansion, so
a directory named `$(cmd)` is displayed, not run.

**History.**

- Saved to `HISTFILE` (default `~/.zash_history`) in bash's plain format,
  so the file can be shared with bash.
- At exit, a shell appends only its own new commands. Concurrent shells
  keep each other's history.
- The file is trimmed by writing a temporary file and renaming it, so an
  interrupted save can't truncate the file. New files are created mode
  0600, since history often contains secrets.
- Closing the terminal (SIGHUP) saves before exiting.

| Variable | Default | Meaning |
|---|---|---|
| `HISTFILE` | `~/.zash_history` | history file; empty disables saving |
| `HISTSIZE` | 500 | entries kept in memory |
| `HISTFILESIZE` | `HISTSIZE` | entries kept in the file |
| `HISTCONTROL` | unset | `ignorespace`, `ignoredups` or `ignoreboth` |
| `FCEDIT` | `$EDITOR`, then `ed` | editor for `fc` |

`fc` works as POSIX describes: `-l`, `-n`, `-r`, `-s old=new` and
`-e editor`. `history` takes `[n]`, `-c` (clear), `-a` (append to the
file), `-w` (write the file) and `-r` (read the file).

### Builtins

`. : [ break cd chtag continue echo eval exec exit export extattr false fc
hash history pwd read readonly return set shift test true tso type umask
unset wait`

`chtag`, `extattr` and `tso` are z/OS only. Elsewhere they exit with
status 2.

## z/OS features

- **No fork for external commands.** They are started with `spawn()`,
  passing redirections as its `fd_map`. `_BPX_SHAREAS=YES` (run the child
  in the shell's address space) and `STEPLIB` take effect through the
  environment, with no extra shell code. Children sharing the address space
  cannot change user identity, and they end if the shell ends. Scripts
  without `#!` are run by zash itself, unless `_BPX_SPAWN_SCRIPT=YES` lets
  spawn handle them first.
- **Tagging.** Each build follows the conventions of the tools around it.
  The ASCII build (the default) follows zopen: pipes are tagged ISO8859-1,
  so output from EBCDIC programs is converted for zash, and new files made
  by a redirection take the tag named by `_ENCODE_FILE_NEW` (`ISO8859-1`,
  the default, `IBM-1047` or `BINARY`). The EBCDIC build follows `sh` in
  the z/OS UNIX Command Reference: `_TAG_REDIR_OUT` and `_TAG_REDIR_ERR`
  (`TXT` or `BIN`) give untagged output files a deferred tag, set at the
  first write. In both builds `_TAG_REDIR_IN` turns conversion of input
  files on or off, and a one-command form like `_TAG_REDIR_OUT=TXT cmd
  >file` works.
- **MVS data sets.** `cmd >"//'HLQ.DATA.SET'"` and `cmd <"//DD:NAME"` go
  through a small helper process, because `open()` cannot open data sets.
  The shell does not wait for that helper before running the next command.
- **Builtins.** `chtag -p` runs inside the shell. Other `chtag` and
  `extattr` options are passed to `/bin/chtag` and `/bin/extattr`. `tso`
  runs `tsocmd`.
- **ASCII or EBCDIC.** Words are stored without in-band marker bytes; the
  markers in ash-derived shells are letters in IBM-1047. Tables are sorted
  at startup, and the escape character is chosen at compile time, so either
  build mode works.

Line editing, completion and colors need a real terminal, such as ssh or
rlogin. They do not work under TSO `OMVS`, which is 3270 line mode.

## Performance

External commands are spawned, never forked and then exec'd. The shell
forks only when shell code must run in a child: a subshell, or a builtin or
compound command in a pipeline. Parse trees and expansion buffers come from
a per-command arena, PATH lookups are hashed, and the exported environment
is rebuilt only after an exported variable changes.

Linux x86-64, best of 3 runs (`bench/run.sh`):

| Workload | zash | dash | bash | ksh93 |
|---|---|---|---|---|
| arithmetic loop, 200k iterations | 0.17 s | 0.25 s | 0.59 s | 0.20 s |
| function calls, 100k | 0.16 s | 0.20 s | 0.68 s | 0.25 s |
| `${p##*/}` and similar, 100k | 0.31 s | 0.36 s | 0.96 s | 0.44 s |
| `while read`, 100k lines | 0.19 s | 0.69 s | 0.57 s | 0.18 s |
| 3000 x `/bin/true` | 2.89 s | 2.99 s | 3.67 s | 3.19 s |
| 2000 x `$(/bin/echo)` | 1.98 s | 2.26 s | 2.98 s | 2.38 s |
| 2000 x `sh -c :` | 2.69 s | 2.16 s | 3.86 s | 3.29 s |

Startup is slower than dash, probably because libedit is linked even for
non-interactive use; this hasn't been profiled. zash also lacks traps and
job control, so part of its lead is less bookkeeping. On z/OS the gap
should be larger, since `fork()` copies the address space there, but this
hasn't been measured.

To compare other shells, such as oksh:

```sh
bench/run.sh ./zash dash bash oksh   # -n N runs, -v per-run times
```

## Testing

`make check` runs `tests/run.sh ./zash`. Each `tests/*.sh` script is run in
a temporary directory, and its output is compared with the matching `.out`
file. Use `tests/run.sh -v ./zash` to see diffs.

Most expected output was generated with dash. `stdin.sh` uses bash's
output, because dash handles a script read from stdin incorrectly.
`history.sh` was checked by hand.

`make check` also runs `libedit/charset_test`, which checks the EBCDIC
character handling (it does nothing in an ASCII build). On Linux,
`cd libedit && make test-ebcdic` checks the EBCDIC code paths with gcc's
EBCDIC literals, and `make verify` confirms the vendored sources are
upstream plus the patch.

## Editor and tooling

- **`make compdb`** writes `compile_commands.json` for clangd and
  clang-tidy. It covers every zash and libedit source with the flags the
  build actually uses; rerun it after changing flags.
- **`.clang-format`** (clang-format 21 or later) and **`.clang-tidy`** set
  the house style and checks:
  `clang-format -i *.c *.h libedit/port/*.[ch]`,
  `clang-tidy -p . *.c libedit/port/*.c`.
- **Vendored code is exempt.** `libedit/netbsd/` has its own
  `.clang-format` and `.clang-tidy` that switch both tools off, because it
  is kept as upstream plus one patch.
- **`.gitattributes`** keeps LF line endings everywhere (the tests, the
  vendored sources and their patch are compared byte for byte) and, on
  z/OS, checks the text out as IBM-1047 and tags it.

## Source layout

| File | Role |
|---|---|
| `shell.h` | shared declarations |
| `main.c` | startup, options, errors, top-level loop |
| `input.c` | input sources; keeps stdin offsets correct for scripts on stdin |
| `zparse.c` | lexer and recursive-descent parser, here-documents |
| `expand.c` | expansion, field splitting, globbing, arithmetic |
| `eval.c` | evaluation, pipelines, background jobs, `$(...)` |
| `exec.c` | PATH hash, functions, redirections, fd plans |
| `var.c` | variables and the cached environment |
| `jobs.c` | background process table |
| `builtins.c` | POSIX builtins |
| `lineedit.c` `complete.c` `zprompt.c` `zhistory.c` | interactive features |
| `sys_posix.c` `sys_zos.c` | platform layer: spawn, opening redirections, tagging |
| `zos_builtins.c` | `tso`, `chtag`, `extattr` |
| `mem.c` | arena allocator and growable buffers |
| `libedit/` | bundled NetBSD libedit: upstream files in `netbsd/`, port layer in `port/` |

[DESIGN.md](DESIGN.md) explains the design decisions in more detail.

## Limitations

- No job control, `trap`, `kill`, `getopts`, `printf`, `command`, `alias`,
  `ulimit` or `times`.
- Arithmetic has no assignment operators (`=`, `+=`, `++`) and no `?:`.
- `cd` always resolves physical paths (no `-L`). There is no `$LINENO`,
  and error messages have no line numbers.
- No programmable completion (`complete`, `compgen`). The bash-completion
  scripts are written in bash itself and can't be reused.
- No `!!` history expansion. A multi-line command is stored as several
  history entries.
- `$(...)` containing builtins or compound commands still forks.

## License

zash is BSD 3-Clause; see [LICENSE](LICENSE).

`libedit/netbsd/` is third-party code under its original licenses:
3-clause BSD (University of California), 2-clause BSD (The NetBSD
Foundation) and ISC. These are reproduced in
[libedit/LICENSE](libedit/LICENSE). Binary distributions of zash built
with the bundled libedit must include those notices.
