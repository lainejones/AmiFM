# amifm - dual-pane file manager for AmigaOS 3.2
#
# Build with the bebbo m68k-amigaos-gcc cross-compiler.
# Ensure the toolchain is on PATH, e.g.:  export PATH=/opt/amiga/bin:$PATH
#
#   make            build amifm (size-optimised, symbols stripped at link time)
#   make deploy     build, then copy amifm + amifm.info into the WinUAE Work: mount
#   make icon       regenerate the colour GlowIcon (amifm.info) from tools/mkglow.py
#   make clean      remove the built binary
#
# NOTE: do NOT post-process with the standalone m68k-amigaos-strip tool - it
# corrupts the Amiga hunk executable (illegal-instruction crash). The linker's
# own -s flag (in CFLAGS) strips safely.

CC      ?= m68k-amigaos-gcc
CFLAGS  ?= -Os -Wall -s

# WinUAE "Work:" mount used by the headless test harness (deploy target).
WORK    ?= /mnt/c/Amiga/workspace

amifm: amifm.c
	$(CC) $(CFLAGS) -o $@ $<

deploy: amifm
	cp -f amifm amifm.info $(WORK)/

icon:
	cd tools && python3 mkglow.py

clean:
	rm -f amifm

.PHONY: deploy icon clean
