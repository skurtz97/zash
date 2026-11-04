.POSIX:
.SUFFIXES:

# NetBSD libedit for zash.  Normally built from ../Makefile, which
# passes the toolchain; standalone:  make CC=clang
#
# All sources compile in one compiler run: POSIX make cannot map
# netbsd/libedit/x.c to ./x.o without a rule per file, and vendored
# code changes rarely.  A toolchain change (.flags) rebuilds it.
#
# Internal macros are LE_-prefixed: macros given on a parent make's
# command line (CPPFLAGS, OBJS, ...) reach this make through MAKEFLAGS
# and would override same-named ones here.  Only histedit.h is exposed,
# copied to include/, so users never see upstream headers like
# search.h or tty.h that share names with system headers.

CC            = cc
CFLAGS        = -O2
AR            = ar
SH            = /bin/sh
# platform feature macros.  Linux: XSI (wcwidth) plus BSD types (u_char).
# z/OS: -D_ALL_SOURCE -D_UNIX03_THREADS -Iport/zos
PLAT_CPPFLAGS = -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE

LE   = netbsd/libedit
LC   = netbsd/libc
LE_CPPFLAGS = -Iport -Igen -I$(LE) -I$(LC) -DHAVE_NBTOOL_CONFIG_H=1 \
	$(PLAT_CPPFLAGS)

LE_SRCS = $(LE)/chared.c $(LE)/chartype.c $(LE)/common.c $(LE)/el.c \
	$(LE)/eln.c $(LE)/emacs.c $(LE)/filecomplete.c $(LE)/hist.c \
	$(LE)/history.c $(LE)/historyn.c $(LE)/keymacro.c $(LE)/literal.c \
	$(LE)/map.c $(LE)/parse.c $(LE)/prompt.c $(LE)/read.c \
	$(LE)/refresh.c $(LE)/search.c $(LE)/sig.c $(LE)/terminal.c \
	$(LE)/tokenizer.c $(LE)/tokenizern.c $(LE)/tty.c $(LE)/vi.c \
	$(LC)/vis.c $(LC)/unvis.c $(LC)/strlcpy.c $(LC)/strlcat.c \
	port/compat.c
LE_OBJS = chared.o chartype.o common.o el.o eln.o emacs.o filecomplete.o \
	hist.o history.o historyn.o keymacro.o literal.o map.o parse.o \
	prompt.o read.o refresh.o search.o sig.o terminal.o tokenizer.o \
	tokenizern.o tty.o vi.o vis.o unvis.o strlcpy.o strlcat.o compat.o

# no globs: POSIX make does not expand them in prerequisites
LE_HDRS = $(LE)/chared.h $(LE)/chartype.h $(LE)/el.h \
	$(LE)/filecomplete.h $(LE)/hist.h $(LE)/histedit.h $(LE)/history.h \
	$(LE)/keymacro.h $(LE)/literal.h $(LE)/map.h $(LE)/parse.h \
	$(LE)/prompt.h $(LE)/read.h $(LE)/refresh.h $(LE)/search.h \
	$(LE)/sig.h $(LE)/sys.h $(LE)/terminal.h $(LE)/tty.h $(LC)/vis.h \
	port/config.h port/port.h port/namespace.h port/nbtool_config.h

AHDR = gen/vi.h gen/emacs.h gen/common.h
GEN  = $(AHDR) gen/fcns.h gen/func.h gen/help.h
MKL  = $(SH) $(LE)/makelist

all: libedit.a include/histedit.h

libedit.a: $(LE_SRCS) $(LE_HDRS) $(GEN) .flags
	$(CC) $(CFLAGS) $(LE_CPPFLAGS) -c $(LE_SRCS)
	rm -f $@
	$(AR) -rc $@ $(LE_OBJS)
	rm -f $(LE_OBJS)

include/histedit.h: $(LE)/histedit.h
	@mkdir -p include
	cp $(LE)/histedit.h $@

# Generated exactly as NetBSD's Makefile does (makelist -h/-fh/-fc/-bh)
gen/vi.h: $(LE)/vi.c $(LE)/makelist
	@mkdir -p gen
	$(MKL) -h $(LE)/vi.c > $@.tmp && mv $@.tmp $@
gen/emacs.h: $(LE)/emacs.c $(LE)/makelist
	@mkdir -p gen
	$(MKL) -h $(LE)/emacs.c > $@.tmp && mv $@.tmp $@
gen/common.h: $(LE)/common.c $(LE)/makelist
	@mkdir -p gen
	$(MKL) -h $(LE)/common.c > $@.tmp && mv $@.tmp $@
gen/fcns.h: $(AHDR) $(LE)/makelist
	$(MKL) -fh $(AHDR) > $@.tmp && mv $@.tmp $@
gen/func.h: $(AHDR) $(LE)/makelist
	$(MKL) -fc $(AHDR) > $@.tmp && mv $@.tmp $@
gen/help.h: $(LE)/vi.c $(LE)/emacs.c $(LE)/common.c $(LE)/makelist
	@mkdir -p gen
	$(MKL) -bh $(LE)/vi.c $(LE)/emacs.c $(LE)/common.c > $@.tmp && \
	    mv $@.tmp $@

.flags: FORCE
	@echo '$(CC) $(CFLAGS) $(LE_CPPFLAGS)' > .flags.new; \
	if cmp -s .flags.new .flags; then rm -f .flags.new; \
	else rm -f libedit.a *.o; mv .flags.new .flags; fi

FORCE:

# Upstream files are kept byte-identical; this checks both their
# contents and that nothing has been added under netbsd/.  Needs
# sha256sum (a development-host check; z/OS has none by default).
verify:
	@cd netbsd && sha256sum -c --quiet ../netbsd.sha256 && \
	find libedit libc -type f | sort > ../.have && \
	awk '{ print $$2 }' ../netbsd.sha256 | sort > ../.want && \
	if cmp -s ../.want ../.have; then \
	    rm -f ../.want ../.have; echo 'netbsd/ matches upstream'; \
	else echo 'netbsd/ has extra or missing files:' >&2; \
	    diff ../.want ../.have >&2; rm -f ../.want ../.have; exit 1; fi

clean:
	rm -rf libedit.a *.o gen include .flags .flags.new
