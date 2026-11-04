#!/bin/sh
# shellcheck shell=sh
# Usage: tests/run.sh SHELL   -- run each tests/*.sh under SHELL and
# compare stdout+stderr with tests/*.out.  -v prints diffs.
verbose=0
if [ "$1" = -v ]; then verbose=1; shift; fi
sh_under_test=${1:?usage: run.sh [-v] shell}
case $sh_under_test in
*/*) sh_under_test=$(cd "$(dirname "$sh_under_test")" && pwd)/$(basename "$sh_under_test") ;;
esac
dir=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
SH=$sh_under_test
export T SH
fail=0
for t in "$dir"/*.sh; do
	case $t in */run.sh) continue ;; esac
	exp=${t%.sh}.out
	out=$T/$(basename "$t").got
	sed "s#\$T#$T#g" "$exp" > "$T/expected"
	(cd "$T" && "$sh_under_test" "$t") > "$out" 2>&1
	if cmp -s "$out" "$T/expected"; then
		[ "$verbose" -eq 1 ] && echo "ok   $t"
	else
		echo "FAIL $t" >&2
		[ "$verbose" -eq 1 ] && diff "$T/expected" "$out" >&2
		fail=1
	fi
done
exit "$fail"
