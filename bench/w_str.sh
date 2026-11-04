p=/usr/local/lib/libfoo.so.1.2.3; i=0
while [ $i -lt 100000 ]; do b=${p##*/}; d=${p%/*}; e=${b#*.}; n=${#p}; i=$((i+1)); done
