#!/usr/bin/env python3
"""
Deploy / launch helper for AmiFM on the WinUAE clean-030 OS 3.2 box.

DH0: is the host directory /mnt/c/Amiga/Clean 030 323, readable+writable from
the host. This script (run under WSL):

  setupgui    - copy AmiFM + AmiFM.info into DH0:AmiFM/ and auto-run AmiFM on
                the next boot (so the GUI opens for testing). Backs up the
                Startup-Sequence first.
  restoregui  - put the original Startup-Sequence back.
  deploy      - just copy the binary + icon (no boot changes).

Usage:
  python3 tools/uae_test.py setupgui
  python3 tools/uae_test.py restoregui
"""
import os, sys, shutil

CLEAN = "/mnt/c/Amiga/Clean 030 323"
SS    = os.path.join(CLEAN, "S", "Startup-Sequence")
DST   = os.path.join(CLEAN, "AmiFM")
ROOT  = os.path.join(os.path.dirname(__file__), "..")
MARK  = ";AMIFMGUI"

def read(p):  return open(p, "r", newline="").read()
def write(p, s):
    with open(p, "w", newline="") as f: f.write(s.replace("\r\n", "\n"))

def deploy():
    os.makedirs(DST, exist_ok=True)
    for name in ("AmiFM", "AmiFM.info"):
        src = os.path.join(ROOT, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(DST, name)); print("deployed", name)
        else:
            sys.exit("missing: " + src + " (run make)")
    print("-> DH0:AmiFM/ (clean 030 box)")

def setupgui():
    deploy()
    if not os.path.exists(SS + ".amfmbak"):
        shutil.copyfile(SS, SS + ".amfmbak")
    lines = [l for l in read(SS).split("\n") if MARK not in l]
    block = [MARK + " begin", "Wait 2", "Run DH0:AmiFM/AmiFM", MARK + " end"]
    out, done = [], False
    for ln in lines:
        out.append(ln)
        if (not done) and ln.strip().lower().startswith("loadwb"):
            out.extend(block); done = True
    if not done:
        out.extend(block)
    write(SS, "\n".join(out))
    print("setupgui: AmiFM will open on next boot of the clean 030 box")

def restoregui():
    if os.path.exists(SS + ".amfmbak"):
        shutil.copyfile(SS + ".amfmbak", SS); os.remove(SS + ".amfmbak")
        print("restored clean-box Startup-Sequence")
    else:
        print("(no backup to restore)")

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if   cmd == "deploy":     deploy()
    elif cmd == "setupgui":   setupgui()
    elif cmd == "restoregui": restoregui()
    else: sys.exit(__doc__)
