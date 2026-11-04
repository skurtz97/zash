i=0; for w in a.c b.h c.txt d.sh e.c f.o; do :; done
while [ $i -lt 100000 ]; do
  for w in a.c b.h c.txt; do case $w in *.c) x=c;; *.h) x=h;; *) x=o;; esac; done
  i=$((i+1)); done
