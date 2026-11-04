.POSIX:
.SUFFIXES:
.SUFFIXES: .c .o

# Defaults: Linux/BSD with libedit.  Override on the command line.
#
# z/OS, IBM Open XL C/C++, ASCII mode (64-bit is required: ascii mode
# is rejected with ibm-clang's default -m32n):
#   make CC=ibm-clang64 SYSOBJ=sys_zos.o \
#     CFLAGS='-std=c11 -O2 -Wall -Wextra -fzos-le-char-mode=ascii' \
#     CPPFLAGS='-D_XOPEN_SOURCE=600 -D_OPEN_SYS_FILE_EXT=1 -DWITH_LIBEDIT'
#
# z/OS, EBCDIC mode: same, without -fzos-le-char-mode=ascii.
#
# No line editing:
#   make CPPFLAGS=-D_XOPEN_SOURCE=700 LIBS=

CC       = cc
CFLAGS   = -std=c11 -O2 -Wall -Wextra
CPPFLAGS = -D_XOPEN_SOURCE=700 -DWITH_LIBEDIT
LDFLAGS  =
LIBS     = -ledit
SYSOBJ   = sys_posix.o
PREFIX   = /usr/local

OBJS = main.o mem.o input.o lineedit.o complete.o prompt.o history.o parse.o \
	expand.o var.o \
	exec.o eval.o jobs.o builtins.o zos_builtins.o $(SYSOBJ)

all: zash

zash: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

$(OBJS): shell.h

check: zash
	./tests/run.sh ./zash

install: zash
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp zash $(DESTDIR)$(PREFIX)/bin/zash

clean:
	rm -f zash *.o
