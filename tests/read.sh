# read must not consume past the line, for files and pipes alike
printf 'one\ntwo\nthree\n' > "$T/r.txt"
{ read a; cat; } < "$T/r.txt"
printf 'p1\np2\np3\n' | { read a; echo "got=$a"; cat; }
{ read a; read b; echo "$a|$b"; head -n 1; } < "$T/r.txt"
# line longer than one chunk, and a continuation across lines
awk 'BEGIN { for (i = 0; i < 700; i++) printf "x"; printf " tail\nnext\n" }' > "$T/long.txt"
{ read a b; echo "${#a} $b"; cat; } < "$T/long.txt"
printf 'join\\\ned here\nafter\n' > "$T/cont.txt"
{ read a b; echo "$a|$b"; cat; } < "$T/cont.txt"
{ read -r a; echo "$a"; cat; } < "$T/cont.txt"
# no trailing newline: status 1 but value set
printf 'last' > "$T/nonl.txt"
read a < "$T/nonl.txt"; echo "rc=$? a=$a"
n=0; while read x; do n=$((n+1)); done < "$T/r.txt"; echo "lines=$n"
