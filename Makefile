.POSIX:
.SUFFIXES:
.SUFFIXES: .c .o

# zash build.  Three toolchains:
#
#   make            Linux/BSD, clang (same as `make linux`)
#   make zos        z/OS, IBM Open XL C/C++ (ibm-clang, 64-bit)
#   make xlc        z/OS, IBM z/OS XL C (xlc, 64-bit)
#
# z/OS builds are ASCII mode (zopen convention).  For EBCDIC:
#   make zos OXL_ASCII=        make xlc XLC_ASCII=
# Without line editing (any build):
#   make EDIT_CPPFLAGS= EDIT_LIBS=
# libedit outside the default search path:
#   make zos EDIT_LIBS='-L/path/to/lib -ledit' EDIT_CPPFLAGS='-DWITH_LIBEDIT -I/path/to/include'
#
# Override the Linux build with CC, CFLAGS, LDFLAGS as usual (e.g.
# `make CC=gcc` if clang is not installed).  Override the z/OS builds
# with the OXL_* and XLC_* macros below.
#
# Changing compiler or flags rebuilds everything (see .flags), so one
# tree can switch between `make zos` and `make xlc`.  check and install
# use the zash already built and never rebuild it.

PREFIX   = /usr/local

EDIT_CPPFLAGS = -DWITH_LIBEDIT
EDIT_LIBS     = -ledit

# Linux/BSD: clang
CC       = clang
CFLAGS   = -std=c11 -O2 -Wall -Wextra
CPPFLAGS = -D_XOPEN_SOURCE=700 $(EDIT_CPPFLAGS)
LDFLAGS  =
LIBS     = $(EDIT_LIBS)
SYSOBJ   = sys_posix.o

# z/OS feature macros, shared by both z/OS compilers
ZOS_CPPFLAGS = -D_XOPEN_SOURCE=600 -D_OPEN_SYS_FILE_EXT=1 $(EDIT_CPPFLAGS)

# IBM Open XL C/C++.  -m64 is required: ibm-clang defaults to 31-bit
# non-XPLINK (-m32n), which rejects -fzos-le-char-mode=ascii.
OXL_CC      = ibm-clang
OXL_ASCII   = -fzos-le-char-mode=ascii
OXL_CFLAGS  = -m64 -std=c11 -O2 -Wall -Wextra $(OXL_ASCII)
OXL_LDFLAGS = -m64

# IBM z/OS XL C.  -qlanglvl=extc1x is its C11 level; -q64 is LP64,
# which needs XPLINK.  xlc has no -Wall/-Wextra; add -qinfo=... to taste.
# VERIFY on z/OS: -qascii and _ENHANCED_ASCII_EXT for ASCII mode.
XLC_CC      = xlc
XLC_ASCII   = -qascii -D_ENHANCED_ASCII_EXT=0xFFFFFFFF
XLC_CFLAGS  = -q64 -qxplink -qlanglvl=extc1x -O2 $(XLC_ASCII)
XLC_LDFLAGS = -q64 -qxplink

OBJS = main.o mem.o input.o lineedit.o complete.o prompt.o history.o \
	parse.o expand.o var.o exec.o eval.o jobs.o builtins.o \
	zos_builtins.o $(SYSOBJ)

all: zash

linux: zash

zos:
	$(MAKE) CC='$(OXL_CC)' CFLAGS='$(OXL_CFLAGS)' \
	    CPPFLAGS='$(ZOS_CPPFLAGS)' LDFLAGS='$(OXL_LDFLAGS)' \
	    LIBS='$(EDIT_LIBS)' SYSOBJ=sys_zos.o zash

xlc:
	$(MAKE) CC='$(XLC_CC)' CFLAGS='$(XLC_CFLAGS)' \
	    CPPFLAGS='$(ZOS_CPPFLAGS)' LDFLAGS='$(XLC_LDFLAGS)' \
	    LIBS='$(EDIT_LIBS)' SYSOBJ=sys_zos.o zash

zash: $(OBJS) .flags
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

$(OBJS): shell.h .flags

# Records the toolchain; rewritten only when it changes, so a new
# compiler or new flags make every object out of date.
.flags: FORCE
	@echo '$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LIBS) $(SYSOBJ)' \
	    > .flags.new; \
	if cmp -s .flags.new .flags; then rm -f .flags.new; \
	else mv .flags.new .flags; fi

FORCE:

check:
	@test -x zash || { echo 'zash not built; run make first' >&2; exit 1; }
	./tests/run.sh ./zash

install:
	@test -x zash || { echo 'zash not built; run make first' >&2; exit 1; }
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp zash $(DESTDIR)$(PREFIX)/bin/zash

clean:
	rm -f zash *.o .flags .flags.new