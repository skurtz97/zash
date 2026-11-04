# z/OS builtins must exist; off z/OS they fail cleanly with status 2
type tso chtag extattr
chtag -p /dev/null 2>/dev/null; echo "chtag rc=$?"
