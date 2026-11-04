#!/bin/sh
# shellcheck shell=sh
#
# Run on z/OS:  sh libedit/port/probe/probe.sh [-v]
# Builds the probe in EBCDIC and ASCII mode with each available compiler
# and prints what the EBCDIC libedit design needs to know.  -v shows
# compiler output.  Set CURSES_LIB (e.g. '-L$ZOPEN_ROOT/lib -lncurses')
# to test a different curses; the default is system -lcurses.

verbose=0
[ "${1:-}" = -v ] && verbose=1
dir=$(cd "$(dirname "$0")" && pwd)
tmp=${TMPDIR:-/tmp}/zash-probe.$$
mkdir -p "$tmp" || exit 1
trap 'rm -rf "$tmp"' EXIT
curses=${CURSES_LIB:--lcurses}

run() {
	if [ "$verbose" -eq 1 ]; then "$@"; else "$@" >/dev/null 2>&1; fi
}

# build NAME CC MAIN_FLAGS ASCII_FLAGS LINK_FLAGS
build() {
	name=$1 cc=$2 mflags=$3 aflags=$4 lflags=$5
	command -v "$cc" >/dev/null 2>&1 || { echo "== $name: $cc not found"; return; }
	echo "== $name"
	cdef=-DPROBE_CURSES
	# shellcheck disable=SC2086
	if ! run "$cc" $aflags $cdef -c -o "$tmp/a.o" "$dir/probe_a.c"; then
		cdef=
		# shellcheck disable=SC2086
		run "$cc" $aflags -c -o "$tmp/a.o" "$dir/probe_a.c" ||
		    { echo "   ASCII unit failed to compile (rerun with -v)"; return; }
		echo "   (curses headers unusable; curses check skipped)"
	fi
	# shellcheck disable=SC2086
	run "$cc" $mflags -c -o "$tmp/m.o" "$dir/probe.c" ||
	    { echo "   main failed to compile (rerun with -v)"; return; }
	lib=; [ -n "$cdef" ] && lib=$curses
	# shellcheck disable=SC2086
	if ! run "$cc" $lflags -o "$tmp/p" "$tmp/m.o" "$tmp/a.o" $lib; then
		echo "   link failed (rerun with -v)"; return
	fi
	"$tmp/p" | sed 's/^/   /'
}

build "Open XL, EBCDIC main" ibm-clang "-m64" \
    "-m64 -fzos-le-char-mode=ascii" "-m64"
build "Open XL, ASCII main" ibm-clang "-m64 -fzos-le-char-mode=ascii" \
    "-m64 -fzos-le-char-mode=ascii" "-m64"
build "XL C, EBCDIC main" xlc "-q64 -qlanglvl=extc1x" \
    "-q64 -qlanglvl=extc1x -qascii -D_ENHANCED_ASCII_EXT=0xFFFFFFFF" "-q64"
build "XL C, ASCII main" xlc \
    "-q64 -qlanglvl=extc1x -qascii -D_ENHANCED_ASCII_EXT=0xFFFFFFFF" \
    "-q64 -qlanglvl=extc1x -qascii -D_ENHANCED_ASCII_EXT=0xFFFFFFFF" "-q64"
