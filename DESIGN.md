# zash design

An Almquist-family POSIX shell for z/OS UNIX that also builds on Linux/BSD.
Fresh code: FreeBSD sh, oksh and BusyBox ash were used as references for
structure only (BusyBox is GPLv2; nothing was copied from it).

Status: design + working skeleton. Passes its own tests and matches dash on
them on Linux. **The z/OS layer has not been compiled on z/OS yet.**

## Build

POSIX `make`, flat tree, no generated sources apart from libedit's
headers (below). The Makefile has no conditionals. The three toolchains are separate targets: `make` (clang on
Linux/BSD), `make zos` (Open XL `ibm-clang -m64`) and `make xlc` (XL C
`xlc -q64 -qxplink -qlanglvl=extc1x`). The z/OS targets call `$(MAKE)`
again with their own `CC`, flags and `SYSOBJ=sys_zos.o`. ASCII mode is on
by default and controlled by the `OXL_ASCII` and `XLC_ASCII` macros.
Line editing is controlled by `EDIT_CPPFLAGS`, `EDIT_LIBS` and
`EDIT_DEP`. By default they select the bundled libedit, which `libedit/`
builds with the parent's `CC` and `CFLAGS`. Clearing all three disables
line editing; `-DWITH_LIBEDIT`, `-ledit` and an empty `EDIT_DEP` select
the system library.

`.flags` records the toolchain. When it changes, the old objects are
deleted outright instead of relying on timestamps. bmake compares whole
seconds, so objects built in the same second as the new `.flags` would
otherwise look current and be linked with output from the wrong compiler.
`check` and `install` don't depend on `zash`, so they can't rebuild it
with the Linux toolchain right after a z/OS build.

zash's history API is `zhist_*`. A static libedit exposes its internal
`hist_*` functions to the linker, and an earlier `hist_init` in zash
collided with one.

## Source map

| File | Role |
|---|---|
| `shell.h` | All shared declarations |
| `mem.c` | Bump arena (+1 spare block), heap helpers, `sbuf` |
| `libedit/` | Bundled NetBSD libedit; see below and `libedit/README.md` |
| `input.c` | Input source stack: strings, fds (4 KB reads), interactive |
| `lineedit.c` | libedit setup: emacs/vi, history, `~/.editrc`, prompt width escapes |
| `complete.c` | TAB completion: commands, files, `$VAR`, quote-aware |
| `zprompt.c` | bash-compatible `PS1`/`PS2` escapes and expansion |
| `zhistory.c` | Numbered history, `HISTFILE`, `fc`, `history` |
| `zparse.c` | Lexer + recursive-descent parser, here-docs, function-body deep copy |
| `expand.c` | Parameter, command, arithmetic, tilde expansion; field split; glob |
| `var.c` | Variable hash table, cached exported environment |
| `exec.c` | PATH hash, functions, redirection, fd plans |
| `eval.c` | Tree evaluation, pipelines, background, `$(...)` |
| `jobs.c` | Background pid table (no job control yet) |
| `builtins.c` | POSIX builtins |
| `zos_builtins.c` | `tso`, `chtag`, `extattr` |
| `sys_posix.c` / `sys_zos.c` | Platform layer (spawn, redirect open, tagging) |

## Performance decisions

1. **Spawn, never fork-to-exec.** Every external command is started with
   `sys_spawn()` (`posix_spawn` on Linux, `spawn()` on z/OS). Redirections
   compile into an *fd plan*: the parent opens targets close-on-exec and
   records which parent fd becomes child fd 0-9. The parent never `dup2`s
   its own fds for an external. This covers simple commands, each external
   pipeline stage, background simple commands, and `$(cmd)` when `cmd` is a
   single external.
2. **fork only for shell code in a child**: subshells, builtin/function/
   compound pipeline stages, compound background jobs, `$(...)` with
   anything but one external. On z/OS fork copies the address space, so
   these paths are deliberately narrow.
3. **exec in place** for the last simple command of `sh -c`.
4. **Arena per simple command and per loop iteration.** Parse trees and
   expansion buffers are pointer bumps; one reset frees them. A spare block
   avoids malloc churn at block boundaries.
5. **Cached lookups.** PATH results are hashed (cleared on `PATH=` or
   `hash -r`). Variables are stored as `NAME=value` strings, so the export
   environment is an array of pointers rebuilt only when an exported
   variable changes.
6. **No filesystem work for `[`.** A `[` with no closing `]` is not a glob
   pattern, so the `test` command never triggers `glob()`.
7. **Chunked `read` on regular files.** POSIX forbids `read` consuming past
   the line. On pipes and terminals that means one byte per syscall; on a
   regular file zash reads 256 bytes and `lseek()`s back over the rest (as
   bash does). On z/OS this is disabled when auto-conversion is active on
   the fd, since a multibyte conversion makes file offsets and converted
   byte counts differ.
8. **Script on stdin keeps POSIX offsets cheaply.** When the script is fd 0,
   a command that reads stdin must see the input after it. A pipe is read
   one byte per syscall (unavoidable; bash does the same). A regular file
   is read in 4 KB blocks and seeked back lazily, only before something
   that can read fd 0: a spawn or fork, `read`, `exec`, or an in-process
   redirection of fd 0 (synced first, so the seek hits the script and not
   the redirected file). Builtin-only lines cost nothing. dash does not do
   this and gets it wrong.

Measured on Linux x86-64, best of 3 (`bench/run.sh`). ksh is ksh93, not
oksh; oksh was not available in the test environment.

| Workload | zash | dash | bash | ksh93 |
|---|---|---|---|---|
| arith loop, 200k | 0.17 s | 0.25 s | 0.59 s | 0.20 s |
| function calls, 100k | 0.16 s | 0.20 s | 0.68 s | 0.25 s |
| `${p##*/}` etc., 100k | 0.31 s | 0.36 s | 0.96 s | 0.44 s |
| `case`, 300k | 0.23 s | 0.24 s | 0.78 s | 0.26 s |
| `while read`, 100k lines | 0.19 s | 0.69 s | 0.57 s | 0.18 s |
| 3000 x `/bin/true` | 2.89 s | 2.99 s | 3.67 s | 3.19 s |
| 2000 x `$(/bin/echo)` | 1.98 s | 2.26 s | 2.98 s | 2.38 s |
| 3-stage pipeline, 1000x | 2.19 s | 2.22 s | 2.50 s | 2.56 s |
| 2000 x `sh -c :` | 2.69 s | 2.16 s | 3.86 s | 3.29 s |

Interactive start and exit, per start: 2.9 ms (bash 3.4 ms) with no
history; 5.1 ms (bash 5.6 ms) with a 10,000-line `HISTFILE`.

Startup is slower than dash, probably from dynamically linking libedit
even for non-interactive runs (unprofiled). zash also lacks traps and job
control, so some of its lead is less bookkeeping.

Linux fork is cheap, so the spawn advantage there is small; the z/OS case
is expected to differ more but is unmeasured.

## Character sets

Build is either ASCII (`-fzos-le-char-mode=ascii`) or EBCDIC by compiler
flag; the code is written to be charset-neutral:

- **No in-band control bytes in words.** ash marks quoting with high bytes;
  FreeBSD's `CTLVAR` (0xC1) and `CTLENDVAR` (0xF9) are `A` and `9` in
  IBM-1047, BusyBox's `CTLESC` (0x81) is `a`. zash keeps words as raw text
  and expansion records each output byte's origin (literal / quoted /
  expansion / field break) in a parallel mark array.
- **No ASCII ranges or hand-sorted tables.** Character classes use
  `<ctype.h>`; the builtin table is `qsort`ed at startup because `.`, `:`,
  `[` and letters collate differently in EBCDIC.

## z/OS layer (`sys_zos.c`, `zos_builtins.c`)

Lines marked `VERIFY` in the source are written from documentation and must
be checked on a real system.

- **spawn():** `fd_map` gets the plan directly (`SPAWN_FDCLOSED` for closed);
  `struct inheritance` resets signal mask and dispositions. `_BPX_SHAREAS`
  and `STEPLIB` are honored by spawn from the environment passed, so
  `export _BPX_SHAREAS=YES` gives same-address-space spawns with no shell
  code. ENOEXEC (script without `#!`) reruns the script with zash itself
  unless `_BPX_SPAWN_SCRIPT=YES` handles it first. VERIFY: ASCII-mode entry
  point and conversion of argv/envp.
- **ASCII builds:** `sys_pipe()` tags every pipe IBM-1047 text (zoslib uses
  ISO8859-1, which leaves EBCDIC readers seeing ASCII), and files zash
  creates get the zopen `_ENCODE_FILE_NEW` tag (ISO8859-1 by default).
  `sys_input_text()` sniffs untagged input files for EBCDIC. `tools/cvtprobe.c`
  measures the behaviour this relies on. EBCDIC builds:
- **`_TAG_REDIR_OUT/ERR=TXT|BIN`:** untagged regular targets get a deferred
  `F_SETTAG` (CCSID 0, `ft_deferred`), matching the documented "program
  CCSID at the time of the first write". **`_TAG_REDIR_IN`:** `F_CONTROL_CVT`
  on (TXT, needs a tag CCSID) or off (BIN). Assignments are applied before
  the fd plan, so `_TAG_REDIR_OUT=TXT cmd >f` works.
- **MVS data sets:** redirection to `//'HLQ.DS'` or `//DD:NAME` uses a pump
  child that `fopen`s the data set. Costs a fork; output pumps are not
  waited on before the next command (can race).
- **Builtins:** `chtag -p` is native; other `chtag` forms and `extattr` defer
  to `/bin/chtag` and `/bin/extattr`; `tso` defers to `tsocmd`. Off z/OS
  all three exit 2.

## Bundled libedit

z/OS has no libedit package, so zash carries NetBSD's `lib/libedit`, plus
the libc helpers it needs (`vis.c`, `unvis.c`, `strlcpy.c`, `strlcat.c`).
They come from NetBSD `src` commit `91af1a58`. FreeBSD's
`contrib/libedit` tracks the same code.

- **Upstream plus one patch.** `libedit/patches/zash.patch` (9 files, 86
  lines) replaces hard-coded ASCII with `port.h` macros, and fixes two
  z/OS build errors. `make verify` undoes the patch on a copy, checks the
  result against `netbsd.sha256`, and rejects added files.
- **Porting lives in `libedit/port/`.**
  - `config.h` replaces NetBSD's and selects features per platform, with
    no configure step.
  - `port.h` supplies the NetBSD `<sys/cdefs.h>` macros. It reaches the
    libc files through the `namespace.h` and `nbtool_config.h` hooks they
    already include.
  - z/OS-only stand-ins for `<sys/cdefs.h>` and `<sys/param.h>` sit on a
    separate include path, so they never shadow real headers elsewhere.
- **Helpers are renamed with a `libedit_` prefix** (`strlcpy`, all of
  `vis`, `issetugid`, and on z/OS `getline` and `wcsdup`). Statically
  linked, a name shared with libc would be a link error or a silent
  override.
- **Only `histedit.h` is exposed** to zash (copied to `libedit/include/`).
  Upstream also has headers named `search.h` and `tty.h`, which would
  shadow system headers.
- **ASCII and EBCDIC.** The probe (`libedit/port/probe`) showed that z/OS
  EBCDIC builds use native values everywhere: `'a'` == `L'a'` ==
  `mbrtowc("a")` == 0x81. So libedit runs on native characters, and only
  its ASCII arithmetic is translated: key-table slots, the meta bit,
  `^X`, DEL, ESC, and `.editrc` escapes. Constant forms (`EL_ESC`,
  `EL_DEL`, `EL_CTRL_C`) serve `case` labels and static initializers. The
  IBM-1047 table follows z/OS in pairing NL 0x15 with LF; on z/OS,
  `make check` compares it with iconv. On Linux, `make test-ebcdic`
  compiles everything with gcc's EBCDIC literals.
- **Omitted:** `readline.c`, the GNU-readline compatibility layer. zash
  doesn't use it, and it needs the non-POSIX `getpwent_r`.
- **Warnings:** clang `-Wall -Wextra -pedantic` is clean. That needed one
  trick: `__RCSID` expands to a forward `struct` declaration, which
  legally absorbs the semicolon upstream writes after it.

## Interactive features

**Prompt** (`zprompt.c`): bash's escapes (`\u \h \H \w \W \$ \t \T \@ \A
\d \D{fmt} \j \! \# \s \v \V \n \e \nnn \\`) are decoded, then the result
gets parameter expansion and `$(...)`, as with bash's `promptvars`. `\[ \]`
become libedit `EL_PROMPT_ESC` markers so colour codes take no width; a bash
`PS1` works unchanged. Text from `\w \W \u \h \H` is quoted before the
expansion pass, so a directory named `$(cmd)` is displayed, not executed.
A failing expansion shows the decoded prompt instead of looping, and `$?`
is preserved. ESC is produced in the build charset (`\e`, and `\033` as a
convenience for prompts copied from ASCII systems). An inherited `PWD` is
kept only if it names the current directory.

**Completion** (`complete.c`): the word under the cursor is found by a
forward, quote-aware scan. First word of a command (after `; | & ( $( {`,
keywords like `then`/`do`, `exec`/`sudo`/`env`, or `NAME=value`
assignments) completes keywords, builtins, functions and PATH executables;
`$prefix` completes variables; everything else completes files, with
directories getting `/`, `~/` expanded, and dotfiles shown only for a
leading `.`. Insertions are escaped for their context (backslashes
unquoted, `$ \` " \\` inside double quotes, quote-closing inside single
quotes), and a unique match closes an open quote and adds a space.
Ambiguous matches insert the common prefix, then list in columns sized to
the terminal, asking first past 100. PATH directories are cached and
rescanned only when their mtime changes.

**History** (`zhistory.c`): zash owns the numbered list; libedit gets a copy
for arrow-key recall. `HISTFILE` (default `~/.zash_history`) is plain text,
one entry per line, compatible with bash's file. At exit, only this
session's new entries are appended with a single `O_APPEND` write, then the
file is trimmed to `HISTFILESIZE` via temp file + `rename()`, so concurrent
shells keep each other's history and a crash cannot truncate the file. New
files are mode 0600. SIGHUP (terminal closed) saves and exits 129. Load and
trim read the file in one call and locate line boundaries backwards from
the end, so a 10,000-line file costs about 2 ms at startup. `HISTSIZE`,
`HISTFILESIZE`, `HISTCONTROL` (`ignorespace`, `ignoredups`, `ignoreboth`).
Builtins: POSIX `fc` (`-l -n -r`, `-s old=new`, `-e editor`, editing via
`FCEDIT`, `EDITOR`, then `ed`) and `history [n|-c|-a|-w|-r]`. Each physical
input line is one entry; multi-line commands are not joined.

## Implemented

Simple commands, pipelines, `&&`/`||`, `!`, `;`/`&`, subshells, brace
groups, `if`/`while`/`until`/`for`/`case`, functions, all POSIX redirection
operators including here-docs (`<<`, `<<-`, quoted delimiters), `$((...))`
(no assignment operators), `$(...)` and backquotes, `${...}` with
`- = ? +` (colon forms), `# ## % %%`, `${#v}`, tilde, IFS splitting,
`"$@"`, globbing, `set -Cefnuvx`, `-o` names, `set -o vi|emacs`.

Builtins: `. : [ break cd chtag continue echo eval exec exit export extattr
false fc hash history pwd read readonly return set shift test true tso type
umask unset wait`.

## Not yet

- Job control (`fg`, `bg`, `jobs`, `^Z`, process groups), `trap`, `kill`
  builtin, `getopts`, `printf` builtin, `command`, `alias`, `ulimit`, `times`.
- Arithmetic assignment (`=`, `+=`, `++`) and `?:`.
- `cd -L` logical paths (always physical), `$LINENO`, line numbers in
  errors.
- Programmable completion (`complete`/`compgen`); bash-completion scripts
  need bash itself and cannot be reused. `!!` history expansion, aliases,
  timestamps in `HISTFILE`. Named prompt colours (`\c{green}`) on top of the bash syntax.
- Builtins/compounds in `$(...)` still fork; running pure builtins
  in-process with captured output is the next performance step.
- Large here-docs (> `PIPE_BUF`) fork a writer; a temp file avoids that
  on z/OS.
- In-process TSO (IKJTSOEV) and `extattr` via `chattr()`.
- ksh-isms from z/OS `sh` (`typeset`, `whence`, `print`, `let`): undecided.
