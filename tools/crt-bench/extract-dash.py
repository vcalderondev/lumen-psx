"""extract-dash.py CRT_C OUT_PREFIX: pull the dashboard's VS_CRT / FS_CRT out of src/crt.c
(C string literals) into OUT_PREFIX.vs / OUT_PREFIX.fs for crt-bench's dash mode."""
import re, sys

src = open(sys.argv[1]).read()
for name, ext in (("VS_CRT", "vs"), ("FS_CRT", "fs")):
    body = src[src.index(f"*{name} =") :]
    body = body[: body.index(";\n")]
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    text = "".join(lits).encode().decode("unicode_escape")
    open(f"{sys.argv[2]}.{ext}", "w").write(text)
