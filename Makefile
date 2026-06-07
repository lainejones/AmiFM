# AmiFM - dual-pane file manager for AmigaOS 3.2
#
# Build with the bebbo m68k-amigaos-gcc cross-compiler.
# Ensure the toolchain is on PATH, e.g.:  export PATH=/opt/amiga/bin:$PATH
#
#   make            build AmiFM (size-optimised, symbols stripped at link time)
#   make deploy     build, then copy AmiFM + AmiFM.info into the WinUAE Work: mount
#   make icon       regenerate the colour GlowIcon (AmiFM.info) from tools/mkglow.py
#   make clean      remove the built binary
#
# NOTE: do NOT post-process with the standalone m68k-amigaos-strip tool - it
# corrupts the Amiga hunk executable (illegal-instruction crash). The linker's
# own -s flag (in CFLAGS) strips safely.

# plain '=' (not '?=') - make predefines CC as the host 'cc', so '?=' would
# never assign and `make` would wrongly use the host compiler. Override with
# `make CC=...` if your cross-gcc has a different name.
CC      = m68k-amigaos-gcc
# -noixemul: self-contained binary (no ixemul.library dependency), ~half size.
# -Wno-pointer-sign: silence the harmless STRPTR vs char* string-literal warnings.
CFLAGS  ?= -Os -noixemul -fomit-frame-pointer -Wall -Wno-pointer-sign -s

# WinUAE "Work:" mount used by the headless test harness (deploy target).
WORK    ?= /mnt/c/Amiga/workspace

AmiFM: AmiFM.c
	$(CC) $(CFLAGS) -o $@ $<

deploy: AmiFM
	cp -f AmiFM AmiFM.info $(WORK)/

icon:
	cd tools && python3 mkglow.py

clean:
	rm -f AmiFM

.PHONY: deploy icon clean
