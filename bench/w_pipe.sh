i=0; while [ $i -lt 1000 ]; do /bin/echo a | /bin/cat | /bin/cat >/dev/null; i=$((i+1)); done
