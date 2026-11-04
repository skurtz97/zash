# z/OS builtins must exist.  On z/OS chtag -p works; elsewhere the
# builtins fail cleanly with status 2.
type tso chtag extattr
if [ "$(uname)" = OS/390 ]; then
	chtag -p /dev/null >/dev/null && echo chtag-ok
else
	chtag -p /dev/null 2>/dev/null; [ $? -eq 2 ] && echo chtag-ok
fi
