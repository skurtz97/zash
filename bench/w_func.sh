f() { r=$(( $1 + $2 )); }
i=0; while [ $i -lt 100000 ]; do f $i 1; i=$((i+1)); done
