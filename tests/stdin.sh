# Script on stdin: a command that reads stdin gets the text after it.
printf 'read x\ndata-line\necho "read [$x]"\n' > "$T/s1"
"$SH" < "$T/s1"
printf 'read x\ndata-line\necho "read [$x]"\n' | "$SH"
# a child reading a regular file gets exactly the next line (a shell's
# read must not consume past it; head is not required to seek back)
printf 'read l; echo "child [$l]"\n' > "$T/child.sh"
printf '"$SH" "$T/child.sh"\nskipped line\necho after-child\n' > "$T/s2"
"$SH" < "$T/s2"
# multi-line compound command, then read
printf 'if true; then\n  echo in-if\nfi\nread y\nline-for-y\necho "y=[$y]"\n' | "$SH"
printf 'if true; then\n  echo in-if\nfi\nread y\nline-for-y\necho "y=[$y]"\n' > "$T/s3"
"$SH" < "$T/s3"
# two commands on one line: offset lands after the line, not mid-line
printf 'echo a; read z\nzline\necho "z=[$z]"\n' | "$SH"
printf 'echo a; read z\nzline\necho "z=[$z]"\n' > "$T/s4"
"$SH" < "$T/s4"
# in-process redirect of fd 0 must not seek the redirected file
# (long first line so a wrong lseek on it would succeed and land mid-line)
awk 'BEGIN { for (i = 0; i < 120; i++) printf "r"; print ""; print "r2" }' > "$T/other"
printf '{ read a; read b; } < "$T/other"; echo "${#a} $b"; read c\ncline\necho "c=[$c]"\n' > "$T/s5"
T=$T "$SH" < "$T/s5"
