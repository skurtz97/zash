#!/usr/bin/env bash
# shellcheck shell=bash
# Usage: bench/run.sh [-v] [-n runs] shell...   (e.g. ./zash dash bash oksh)
# Prints the best-of-N wall time per workload; -v also prints each run.
set -u
verbose=0 runs=3
while getopts vn: opt; do
	case $opt in
	v) verbose=1 ;;
	n) runs=$OPTARG ;;
	*) echo "usage: $0 [-v] [-n runs] shell..." >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { echo "usage: $0 [-v] [-n runs] shell..." >&2; exit 2; }
dir=$(cd "$(dirname "$0")" && pwd)
# workloads run from a temp dir, so make relative shell paths absolute
shells=()
for s in "$@"; do
	case $s in
	*/*) s=$(cd "$(dirname "$s")" && pwd)/${s##*/} ;;
	esac
	shells+=("$s")
done
set -- "${shells[@]}"
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
seq 1 100000 | sed 's/$/ alpha beta gamma/' >"$tmp/data.txt"
cp "$dir"/w_*.sh "$tmp/"

printf '%-8s' work
for s in "$@"; do printf '%9s' "${s##*/}"; done
echo
for w in "$tmp"/w_*.sh; do
	name=${w##*/w_}
	name=${name%.sh}
	printf '%-8s' "$name"
	for s in "$@"; do
		best=
		for ((r = 0; r < runs; r++)); do
			st=$(date +%s%N)
			(cd "$tmp" && SH=$s "$s" "$w" >/dev/null 2>&1)
			ms=$((($(date +%s%N) - st) / 1000000))
			[ "$verbose" -eq 1 ] && echo "  $name ${s##*/} run$r ${ms}ms" >&2
			if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then best=$ms; fi
		done
		printf '%9s' "$((best / 1000)).$(printf '%02d' $((best % 1000 / 10)))s"
	done
	echo
done
