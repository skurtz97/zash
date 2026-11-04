echo hello world
x=5; y=$((x * 3 + 1)); echo "y=$y"
for i in a b c; do printf '%s-' "$i"; done; echo
if [ "$x" -gt 3 ]; then echo big; else echo small; fi
n=0; while [ $n -lt 3 ]; do n=$((n+1)); done; echo n=$n
f() { echo "f got $# args: $*"; return 3; }; f 1 "2 3"; echo rc=$?
echo $(echo sub $(echo nested))
echo `echo backq`
case foo.c in *.h) echo header;; *.c) echo csrc;; esac
echo a b c | tr '[:lower:]' '[:upper:]' | sed 's/ /-/g'
v=/usr/local/bin/zash; echo ${v##*/} ${v%/*} ${#v} ${unset:-dflt}
cat <<EOT
heredoc $x
EOT
cat <<'EOT'
literal $x
EOT
echo out >$T/o.txt; cat < $T/o.txt
ls /nonexistent 2>&1 | head -n 1 >/dev/null; echo "pipe rc=$?"
false || echo or-ok; true && echo and-ok
set -- one two three; shift; echo "$@" $#
for a in "$@"; do echo "[$a]"; done
echo ~ | grep -q / && echo tilde-ok
(cd /; pwd); pwd >/dev/null
(exit 3) & wait $!; echo bg-rc=$?
read a b <<EOT
first second third
EOT
echo "a=$a b=$b"
touch "$T/g1.c" "$T/g2.c" "$T/g3.h"; n=$(echo "$T"/*.c | wc -w); echo $((n)); echo "$T"/nomatch*
nosuchcmd 2>/dev/null; echo nf=$?
i=0; while true; do i=$((i+1)); [ $i -ge 5 ] && break; continue; done; echo i=$i
unset x; echo "x=${x-unset}"
type cd : >/dev/null && echo type-ok
