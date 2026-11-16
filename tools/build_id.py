# Genera include/build_id.h con l'identita' della build: commit git, marcatore di
# modifiche non salvate e data del commit. Serve a distinguere due binari con lo stesso
# numero di versione (la 3.0.0 ne aveva due diversi, indistinguibili).
# Niente data/ora di compilazione: renderebbe ogni build diversa e costringerebbe a
# ricompilare tutto a ogni giro. Il file si riscrive solo se il contenuto cambia.
import os
import subprocess

Import("env")  # noqa: F821

PROJ = env["PROJECT_DIR"]  # noqa: F821
OUT = os.path.join(PROJ, "include", "build_id.h")


def git(*args):
    try:
        return subprocess.check_output(["git", "-C", PROJ] + list(args),
                                       stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


rev = git("rev-parse", "--short=7", "HEAD") or "nogit"
date = git("log", "-1", "--format=%cs") or "?"
if rev != "nogit" and git("status", "--porcelain", "--untracked-files=no"):
    rev += "-mod"  # build da un albero con modifiche non salvate

content = (
    "// GENERATO da tools/build_id.py a ogni build: non modificare, non versionare.\n"
    "#pragma once\n"
    '#define AX_GIT_REV "%s"\n'
    '#define AX_GIT_DATE "%s"\n' % (rev, date)
)
old = open(OUT).read() if os.path.exists(OUT) else ""
if old != content:
    with open(OUT, "w") as f:
        f.write(content)
print("build_id: %s (%s)" % (rev, date))
