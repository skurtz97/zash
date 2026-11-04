.POSIX:
.SUFFIXES:
.SUFFIXES: .c .o

# zash build.  Three toolchains:
#
#   make            Linux/BSD, clang (same as `make linux`)
#   make zos        z/OS, IBM Open XL C/C++ (ibm-clang, 64-bit)
#   make xlc        z/OS, IBM z/OS XL C (xlc, 64-bit)
#
# z/OS builds are ASCII mode (zopen convention).  For EBCDIC (IBM-1047,
# line editing included):
#   make zos OXL_ASCII=        make xlc XLC_ASCII=
#
# Line editing uses the bundled NetBSD libedit (libedit/) by default.
#   system libedit:  make EDIT_CPPFLAGS=-DWITH_LIBEDIT EDIT_LIBS=-ledit EDIT_DEP=
#   none:            make EDIT_CPPFLAGS= EDIT_LIBS= EDIT_DEP=
#   other curses:    make zos TERMLIB_ZOS='-L$$ZOPEN_ROOT/lib -lncurses'
#
# Override the Linux build with CC, CFLAGS, LDFLAGS as usual (e.g.
# `make CC=gcc` if clang is not installed).  Override the z/OS builds
# with the OXL_* and XLC_* macros below.
#
# Changing compiler or flags rebuilds everything (see .flags), so one
# tree can switch between `make zos` and `make xlc`.  check and install
# use the zash already built and never rebuild it.

PREFIX   = /usr/local

LIBEDIT       = libedit/libedit.a
EDIT_CPPFLAGS = -DWITH_LIBEDIT -Ilibedit/include
EDIT_LIBS     = $(LIBEDIT) $(TERMLIB)
EDIT_DEP      = $(LIBEDIT)
# termcap entry points (tgetent...) for libedit, and the feature macros
# libedit is compiled with (relative to libedit/)
TERMLIB       = -lncurses
LIBEDIT_PLAT  = -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE

# Linux/BSD: clang
CC       = clang
CFLAGS   = -std=c11 -O2 -Wall -Wextra
CPPFLAGS = -D_XOPEN_SOURCE=700 $(EDIT_CPPFLAGS)
LDFLAGS  =
LIBS     = $(EDIT_LIBS)
SYSOBJ   = sys_posix.o

# z/OS feature macros, shared by both z/OS compilers.  libedit uses
# system X/Open curses; port/zos holds <sys/cdefs.h>, <sys/param.h>
# stand-ins.  VERIFY on z/OS: the _ALL_SOURCE/_UNIX03_THREADS set.
ZOS_CPPFLAGS = -D_XOPEN_SOURCE=600 -D_OPEN_SYS_FILE_EXT=1 $(EDIT_CPPFLAGS)
TERMLIB_ZOS  = -lcurses
LIBEDIT_ZOS  = -D_ALL_SOURCE -D_UNIX03_THREADS -Iport/zos

# IBM Open XL C/C++.  -m64 is required: ibm-clang defaults to 31-bit
# non-XPLINK (-m32n), which rejects -fzos-le-char-mode=ascii.
OXL_CC      = ibm-clang
OXL_ASCII   = -fzos-le-char-mode=ascii
# -Wno-char-subscripts: z/OS <stdio.h>'s putchar() macro indexes a table
# with (char)(c), so every putchar() call warns; the code is fine.
OXL_CFLAGS  = -m64 -std=c11 -O2 -Wall -Wextra -Wno-char-subscripts \
	$(OXL_ASCII)
OXL_LDFLAGS = -m64

# IBM z/OS XL C.  -qlanglvl=extc1x is its C11 level; -q64 is LP64,
# which needs XPLINK.  xlc has no -Wall/-Wextra; add -qinfo=... to taste.
# VERIFY on z/OS: -qascii and _ENHANCED_ASCII_EXT for ASCII mode.
XLC_CC      = xlc
XLC_ASCII   = -qascii -D_ENHANCED_ASCII_EXT=0xFFFFFFFF
XLC_CFLAGS  = -q64 -qxplink -qlanglvl=extc1x -O2 $(XLC_ASCII)
XLC_LDFLAGS = -q64 -qxplink

# No zash source may share a basename with a libedit source: xlc names
# an object's code and static sections after the source file, and the
# z/OS binder keeps only the first section of a given name, so the
# second file's functions silently vanish (IEW2456E at link time).
# Hence zprompt.c, zparse.c, zhistory.c.  "make names" checks this.
OBJS = main.o mem.o input.o lineedit.o complete.o zprompt.o zhistory.o \
	zparse.o expand.o var.o exec.o eval.o jobs.o builtins.o \
	zos_builtins.o $(SYSOBJ)

all: zash

linux: zash

zos:
	$(MAKE) CC='$(OXL_CC)' CFLAGS='$(OXL_CFLAGS)' \
	    CPPFLAGS='$(ZOS_CPPFLAGS)' LDFLAGS='$(OXL_LDFLAGS)' \
	    TERMLIB='$(TERMLIB_ZOS)' LIBEDIT_PLAT='$(LIBEDIT_ZOS)' \
	    SYSOBJ=sys_zos.o zash

xlc:
	$(MAKE) CC='$(XLC_CC)' CFLAGS='$(XLC_CFLAGS)' \
	    CPPFLAGS='$(ZOS_CPPFLAGS)' LDFLAGS='$(XLC_LDFLAGS)' \
	    TERMLIB='$(TERMLIB_ZOS)' LIBEDIT_PLAT='$(LIBEDIT_ZOS)' \
	    SYSOBJ=sys_zos.o zash

zash: $(OBJS) .flags $(EDIT_DEP)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

# libedit/ decides for itself whether anything needs rebuilding
$(LIBEDIT): FORCE
	cd libedit && $(MAKE) CC='$(CC)' CFLAGS='$(CFLAGS)' \
	    PLAT_CPPFLAGS='$(LIBEDIT_PLAT)'

# the two files that include <histedit.h> need it copied out first
lineedit.o complete.o: $(EDIT_DEP)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

$(OBJS): shell.h .flags

# Records the toolchain.  When it changes the old objects are deleted
# outright rather than left to timestamps: makes with one-second mtime
# resolution would otherwise keep objects from the previous compiler.
.flags: FORCE
	@echo '$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LIBS) $(SYSOBJ)' \
	    > .flags.new; \
	if cmp -s .flags.new .flags; then rm -f .flags.new; \
	else rm -f *.o zash; mv .flags.new .flags; fi

FORCE:

# compile_commands.json for clangd and clang-tidy: every source this
# build compiles, with its real flags.  Rerun after changing flags.
compdb: $(EDIT_DEP)
	@d=$$(pwd); cmd=$$(printf '%s' '$(CC) $(CFLAGS) $(CPPFLAGS)' | \
	    sed 's/\\/\\\\/g; s/"/\\"/g'); \
	{ echo '['; \
	  (cd libedit && $(MAKE) -s CC='$(CC)' CFLAGS='$(CFLAGS)' \
	      PLAT_CPPFLAGS='$(LIBEDIT_PLAT)' compdb-entries); \
	  for f in $(OBJS:.o=.c); do \
	    printf '{"directory":"%s","file":"%s","command":"%s -c %s"},\n' \
	        "$$d" "$$f" "$$cmd" "$$f"; done; \
	} | sed '$$s/,$$//' > compile_commands.json.new && echo ']' >> \
	    compile_commands.json.new && mv compile_commands.json.new \
	    compile_commands.json && \
	echo "compile_commands.json: $$(grep -c '"file"' compile_commands.json) entries"

names:
	@dup=; for f in *.c; do for d in libedit/netbsd/libedit \
	    libedit/netbsd/libc libedit/port; do test -f $$d/$$f && \
	    dup="$$dup $$f"; done; done; test -z "$$dup" || { echo \
	    "basename shared with libedit:$$dup (breaks the xlc link)" >&2; \
	    exit 1; }

check: names
	@test -x zash || { echo 'zash not built; run make first' >&2; exit 1; }
	./tests/run.sh ./zash
	@if test -x libedit/charset_test; then libedit/charset_test && \
	    echo 'libedit charset test passed'; fi

install:
	@test -x zash || { echo 'zash not built; run make first' >&2; exit 1; }
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp zash $(DESTDIR)$(PREFIX)/bin/zash

# Source tarball for z/OS (ustar, top directory zash/).  All files are
# ASCII; on z/OS extract as tagged EBCDIC with:
#   pax -rf zash.tar -ofrom=ISO8859-1,to=IBM-1047 -o setfiletag
DIST = LICENSE README.md DESIGN.md Makefile *.c *.h tests bench \
	.clang-format .clang-tidy .gitattributes .gitignore
DIST_LIBEDIT = libedit/Makefile libedit/LICENSE libedit/README.md \
	libedit/netbsd.sha256 libedit/netbsd libedit/patches libedit/port

dist:
	rm -rf .dist zash.tar && mkdir -p .dist/zash/libedit
	cp -R $(DIST) .dist/zash/ && cp -R $(DIST_LIBEDIT) .dist/zash/libedit/
	cd .dist && { tar --format=ustar -cf ../zash.tar zash 2>/dev/null || \
	    tar -cf ../zash.tar zash; }
	rm -rf .dist

clean:
	rm -f zash *.o .flags .flags.new zash.tar compile_commands.json.new
	rm -rf .dist
	cd libedit && $(MAKE) clean
