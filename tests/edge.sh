set -- "a b" "" c
printf '<%s>' "$@"; echo
printf '<%s>' $@; echo
printf '<%s>' "$*"; echo
set --
printf '<%s>' "$@" x; echo
e=; printf '<%s>' $e "" "$e"; echo
IFS=: ; v="a:b::c"; printf '<%s>' $v; echo; IFS=' 	
'
echo 'single "dq" \n' "double 'sq' \$HOME \\ \"" \x
echo "nested $(echo "in $(echo 'deep')")"
{ echo to-stderr >&2; } 2>&1 | cat
( echo err >&2 ) 2>/dev/null; echo swallowed
sh -c 'echo out; echo err >&2' 2>&1 >/dev/null | sed 's/^/got:/'
exec 3>$T/fd3.txt; echo via3 >&3; exec 3>&-; cat $T/fd3.txt
eval 'q=evald; echo $q'
echo 'dotvar=fromdot' > $T/dot.sh; . $T/dot.sh; echo $dotvar
fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }; fact 10
echo builtin | read x; echo "subshell read: x=${x-unset}"
for i in 1 2 3; do for j in a b; do [ $j = b ] && continue 2; echo $i$j; done; done
x=1 sh -c 'echo child x=$x'; echo "parent x=${x-unset}"
: ${def:=assigned}; echo $def
echo ${novar:+alt}[] ${def:+alt}
cat <<-EOT
	tabs stripped $def
	EOT
echo "$(printf 'trail\n\n\n')|"
ls -d / /tmp | while read d; do echo "dir:$d"; done
! false && echo negated
echo "exit test"; (exit 7); echo rc=$?
set -e; false || true; echo still-here
sh -c 'set -e; false; echo NOT'; echo "set-e rc=$?"
