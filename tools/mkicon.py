#!/usr/bin/env python3
# Generate amifm.info  - a 4-colour Amiga icon (dual-pane file-manager look).
# Pens (standard Workbench palette): 0=grey(bg) 1=black 2=white 3=blue
import struct

W, H, D = 48, 26, 3
# pen indices (standard-ish WB 8-colour palette)
PEN_GREY, PEN_BLK, PEN_WHT, PEN_BLU = 0, 1, 2, 3
PEN_LGRY, PEN_DGRY, PEN_LBLU, PEN_ORNG = 4, 5, 6, 7
px = [[PEN_GREY]*W for _ in range(H)]

def rect(x0,y0,x1,y1,p):
    for y in range(y0,y1+1):
        for x in range(x0,x1+1):
            if 0<=x<W and 0<=y<H: px[y][x]=p
def frame(x0,y0,x1,y1,p):
    for x in range(x0,x1+1): px[y0][x]=p; px[y1][x]=p
    for y in range(y0,y1+1): px[y][x0]=p; px[y][x1]=p
def hline(x0,x1,y,p):
    for x in range(x0,x1+1):
        if 0<=x<W and 0<=y<H: px[y][x]=p

# ---- 3D window body ----
rect(1,1,W-2,H-2,PEN_WHT)              # interior white
frame(0,0,W-1,H-1,PEN_BLK)            # black outer frame
hline(1,W-2,1,PEN_WHT)                 # top inner highlight
for y in range(1,H-1): px[y][1]=PEN_WHT
hline(1,W-2,H-2,PEN_DGRY)             # bottom inner shadow
for y in range(1,H-1): px[y][W-2]=PEN_DGRY
# ---- title bar (blue, with lighter top line = gradient) ----
rect(2,2,W-3,6,PEN_BLU)
hline(2,W-3,2,PEN_LBLU)
hline(2,W-3,7,PEN_BLK)                 # underline
rect(4,3,6,5,PEN_WHT); rect(W-7,3,W-5,5,PEN_WHT)   # gadget squares
# ---- two panes ----
rect(2,8,W//2-2,H-3,PEN_WHT)           # left pane
rect(W//2+1,8,W-3,H-3,PEN_WHT)         # right pane
rect(W//2-1,8,W//2,H-3,PEN_LGRY)       # divider (light grey)
px_div = W//2-1
for y in range(8,H-2): px[y][px_div]=PEN_BLK
# file rows (black lines) + a couple of colour-coded file dots
for i,y in enumerate(range(11,H-4,3)):
    hline(5, W//2-4, y, PEN_BLK)
    hline(W//2+4, W-6, y, PEN_BLK)
    px[y][3]=PEN_ORNG; px[y][W//2+2]=PEN_BLU      # tiny file markers
# selected row (orange bar) in the left pane
rect(3,9,W//2-3,10,PEN_ORNG); hline(5,W//2-5,9,PEN_BLK)

# ---- pack to Amiga planar bitmap: plane0 then plane1, rows word-padded ----
RW = (W + 15)//16          # words per row
planes = bytearray()
for plane in range(D):
    for y in range(H):
        for wd in range(RW):
            word = 0
            for bit in range(16):
                x = wd*16 + bit
                if x < W and (px[y][x] >> plane) & 1:
                    word |= (1 << (15-bit))
            planes += struct.pack('>H', word)

NO_POS = 0x80000000
out = bytearray()
# --- DiskObject ---
out += struct.pack('>H', 0xE310)        # do_Magic
out += struct.pack('>H', 1)             # do_Version
# do_Gadget (44 bytes)
out += struct.pack('>I', 0)             # NextGadget
out += struct.pack('>hh', 0, 0)         # LeftEdge, TopEdge
out += struct.pack('>hh', W, H)         # Width, Height
out += struct.pack('>H', 0x0004)        # Flags = GFLG_GADGIMAGE
out += struct.pack('>H', 0x0003)        # Activation = RELVERIFY|IMMEDIATE
out += struct.pack('>H', 0x0001)        # GadgetType = BOOLGADGET
out += struct.pack('>I', 1)             # GadgetRender (non-zero -> image follows)
out += struct.pack('>I', 0)             # SelectRender
out += struct.pack('>I', 0)             # GadgetText
out += struct.pack('>i', 0)             # MutualExclude
out += struct.pack('>I', 0)             # SpecialInfo
out += struct.pack('>H', 0)             # GadgetID
out += struct.pack('>I', 0)             # UserData
out += struct.pack('>B', 3)             # do_Type = WBTOOL
out += struct.pack('>B', 0)             # pad
out += struct.pack('>I', 0)             # do_DefaultTool
out += struct.pack('>I', 0)             # do_ToolTypes
out += struct.pack('>I', NO_POS)        # do_CurrentX
out += struct.pack('>I', NO_POS)        # do_CurrentY
out += struct.pack('>I', 0)             # do_DrawerData
out += struct.pack('>I', 0)             # do_ToolWindow
out += struct.pack('>I', 4096)          # do_StackSize
# --- Image (since GadgetRender != 0) ---
out += struct.pack('>hh', 0, 0)         # LeftEdge, TopEdge
out += struct.pack('>hh', W, H)         # Width, Height
out += struct.pack('>h', D)             # Depth
out += struct.pack('>I', 1)             # ImageData (non-zero placeholder)
out += struct.pack('>B', (1<<D)-1)      # PlanePick = 3
out += struct.pack('>B', 0)             # PlaneOnOff
out += struct.pack('>I', 0)             # NextImage
out += planes                           # bitplane data

with open('/mnt/c/Amiga/workspace/amifm.info','wb') as f:
    f.write(out)
# dump the pixel grid for a host-side preview
with open('/mnt/c/Amiga/workspace/icon_grid.txt','w') as f:
    f.write("%d %d\n" % (W,H))
    for y in range(H):
        f.write(" ".join(str(px[y][x]) for x in range(W)) + "\n")
print("wrote amifm.info", len(out), "bytes; image", W, "x", H, "x", D)
