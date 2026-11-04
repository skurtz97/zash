# history and fc in an interactive shell fed from a here-doc (no tty)
export HOME="$T" HISTFILE="$T/h" HISTCONTROL=ignoreboth
# an fc editor that works everywhere (sed -i is GNU-only)
printf 'sed s/one/ONE/ "$1" > "$1.new" && mv "$1.new" "$1"\n' > "$T/ed.sh"
"$SH" -i 2>/dev/null <<'EOT'
echo one
echo two
echo two
 echo hidden
history
fc -l -3 -2
fc -s two=TWO echo
fc -ln 1 2
FCEDIT="sh $T/ed.sh" fc 1
EOT
echo "--- file:"; cat "$T/h"
ls -l "$T/h" | cut -c1-10
# a second session loads the file; HISTFILESIZE trims it
HISTFILESIZE=3 "$SH" -i 2>/dev/null <<'EOT'
history 2
EOT
echo "--- trimmed:"; cat "$T/h"
