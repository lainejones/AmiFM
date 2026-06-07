#!/usr/bin/env python3
# amifm GlowIcon: glossy dual-pane window. Normal image = no glow; selected image
# = yellow-gold glow halo (authentic GlowIcons behaviour). Emits a real OS3.5
# colour-icon .info (classic DiskObject + planar fallback + FORM ICON with two
# RLE-compressed IMAG chunks). Also dumps grids for host preview.
import struct, math

W, H = 64, 44

def new_canvas(): return [[None]*W for _ in range(H)]
def lerp(a,b,t): return tuple(int(a[i]+(b[i]-a[i])*t) for i in range(3))

OX0,OY0,OX1,OY1 = 8,6,W-9,H-7
R = 4
def in_round(x,y,x0,y0,x1,y1,r):
    if x<x0 or x>x1 or y<y0 or y>y1: return False
    cx = x0+r if x<x0+r else (x1-r if x>x1-r else x)
    cy = y0+r if y<y0+r else (y1-r if y>y1-r else y)
    return (x-cx)**2+(y-cy)**2 <= r*r

FRAME=(26,34,56); TITLE_T=(150,196,250); TITLE_B=(58,104,186)
PANE_T=(252,252,255); PANE_B=(208,216,232); DIVIDER=(120,134,160)
FILE_LN=(70,86,116); SEL_T=(255,196,96); SEL_B=(236,150,40)
DOT_O=(240,140,40); DOT_G=(70,190,110); SHADOW=(40,46,66)
GLOW_NEAR=(255,238,150); GLOW_FAR=(238,176,42)

def draw_window():
    c=new_canvas()
    def put(x,y,rgb):
        if 0<=x<W and 0<=y<H: c[y][x]=rgb
    # drop shadow
    for y in range(OY0+3,OY1+4):
        for x in range(OX0+3,OX1+4):
            if in_round(x-3,y-3,OX0,OY0,OX1,OY1,R): put(x,y,SHADOW)
    # body + title gradient
    by=OY0+9
    for y in range(OY0,OY1+1):
        for x in range(OX0,OX1+1):
            if not in_round(x,y,OX0,OY0,OX1,OY1,R): continue
            if not in_round(x,y,OX0+1,OY0+1,OX1-1,OY1-1,R-1): put(x,y,FRAME); continue
            if y<=by: put(x,y,lerp(TITLE_T,TITLE_B,(y-(OY0+1))/max(1,(by-(OY0+1)))))
            else:     put(x,y,lerp(PANE_T,PANE_B,(y-(by+1))/max(1,(OY1-1-(by+1)))))
    for x in range(OX0+1,OX1): put(x,OY0+10,FRAME)
    for sx in (OX0+3,OX1-6):
        for yy in range(OY0+3,OY0+7):
            for xx in range(sx,sx+3): put(xx,yy,(245,250,255))
    midx=(OX0+OX1)//2
    for y in range(OY0+12,OY1-1): put(midx,y,FRAME); put(midx-1,y,DIVIDER); put(midx+1,y,DIVIDER)
    for i,ry in enumerate(range(OY0+13,OY1-2,3)):
        if i==0:
            for x in range(OX0+2,midx-1): put(x,ry-1,SEL_T); put(x,ry,SEL_B)
        else:
            for x in range(OX0+4,midx-3): put(x,ry,FILE_LN)
            put(OX0+2,ry,DOT_O)
        for x in range(midx+4,OX1-3): put(x,ry,FILE_LN)
        put(midx+2,ry,DOT_G)
    return c

BAYER4=[[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]]   # ordered-dither thresholds
def add_glow(src):
    # GlowIcons-style glow: solid bright gold hugging the silhouette, then a
    # gold stipple (ordered dither) fading outward to transparent.
    c=[row[:] for row in src]
    obj=[[src[y][x] is not None for x in range(W)] for y in range(H)]   # incl. drop shadow
    GR=5
    for y in range(H):
        for x in range(W):
            if c[y][x] is not None: continue
            best=99.0
            for dy in range(-GR,GR+1):
                for dx in range(-GR,GR+1):
                    yy,xx=y+dy,x+dx
                    if 0<=xx<W and 0<=yy<H and obj[yy][xx]:
                        d=math.hypot(dx,dy)
                        if d<best: best=d
            if best>GR: continue
            t=(best-1)/(GR-1)                       # 0 near .. 1 far
            density=1.0-t
            thr=(BAYER4[y&3][x&3]+0.5)/16.0
            if best<=1.5 or density>thr:            # inner ring solid, rest stippled
                c[y][x]=lerp(GLOW_NEAR,GLOW_FAR,min(1,max(0,t)))
    return c

base=draw_window()
glow=add_glow(base)

# ---- preview grids ----
for name,cv in (("base",base),("glow",glow)):
    with open('/mnt/c/Amiga/workspace/%s_grid.txt'%name,'w') as f:
        f.write("%d %d\n"%(W,H))
        for y in range(H):
            f.write(" ".join(("T" if cv[y][x] is None else "%d,%d,%d"%cv[y][x]) for x in range(W))+"\n")

# ---- colour-icon encoding ----
def build_image(cv):
    uniq=[]; seen=set()
    for y in range(H):
        for x in range(W):
            p=cv[y][x]
            if p is not None and p not in seen: seen.add(p); uniq.append(p)
    pal=[(0,0,0)]+uniq                          # 0 = transparent
    idx={c:i+1 for i,c in enumerate(uniq)}
    ncol=len(pal); depth=max(1,(ncol-1).bit_length())
    pix=[(0 if cv[y][x] is None else idx[cv[y][x]]) for y in range(H) for x in range(W)]
    palbytes=[ch for c in pal for ch in c]
    return ncol,depth,pix,palbytes

def rle(data,unit):
    pairs=[]; i=0; n=len(data)
    while i<n:
        runlen=1
        while i+runlen<n and data[i+runlen]==data[i] and runlen<128: runlen+=1
        if runlen>=2:
            pairs.append((257-runlen,[data[i]])); i+=runlen
        else:
            lit=[]
            while i<n and len(lit)<128:
                if i+1<n and data[i+1]==data[i]: break
                lit.append(data[i]); i+=1
            pairs.append((len(lit)-1,lit))
    bits=[]
    for ctrl,units in pairs:
        for b in range(7,-1,-1): bits.append((ctrl>>b)&1)
        for u in units:
            for b in range(unit-1,-1,-1): bits.append((u>>b)&1)
    while len(bits)%8: bits.append(0)
    out=bytearray()
    for j in range(0,len(bits),8):
        byte=0
        for b in range(8): byte=(byte<<1)|bits[j+b]
        out.append(byte)
    return bytes(out)

def imag_chunk(cv):
    ncol,depth,pix,palbytes=build_image(cv)
    img=rle(pix,depth); pal=rle(palbytes,8)
    hdr=struct.pack('>BBBBBB',0,ncol-1,0x03,1,1,depth)
    hdr+=struct.pack('>HH',len(img)-1,len(pal)-1)
    return hdr+img+pal, ncol*3

def chunk(cid,data):
    out=cid+struct.pack('>I',len(data))+data
    if len(data)&1: out+=b'\x00'
    return out

imag1,pb1=imag_chunk(base)        # normal (no glow)
imag2,pb2=imag_chunk(glow)        # selected (glow)
face=struct.pack('>BBBB',W-1,H-1,0,0x00)+struct.pack('>H',max(pb1,pb2))
form=b'ICON'+chunk(b'FACE',face)+chunk(b'IMAG',imag1)+chunk(b'IMAG',imag2)
formchunk=b'FORM'+struct.pack('>I',len(form))+form

# ---- planar fallback (from base, nearest of 8 WB pens) ----
PENS=[(170,170,170),(0,0,0),(255,255,255),(102,136,187),
      (187,187,187),(102,102,102),(150,180,225),(238,176,42)]
def npen(c): return 0 if c is None else min(range(8),key=lambda i:sum((c[j]-PENS[i][j])**2 for j in range(3)))
RW=(W+15)//16; planar=bytearray()
for pl in range(3):
    for y in range(H):
        for wd in range(RW):
            word=0
            for bit in range(16):
                x=wd*16+bit
                if x<W and (npen(base[y][x])>>pl)&1: word|=1<<(15-bit)
            planar+=struct.pack('>H',word)
img=struct.pack('>hhhhh',0,0,W,H,3)+struct.pack('>I',1)+struct.pack('>BB',0x07,0)+struct.pack('>I',0)+planar

NO_POS=0x80000000
DO=struct.pack('>HH',0xE310,1)
DO+=struct.pack('>I',0)+struct.pack('>hhhh',0,0,W,H)
DO+=struct.pack('>HHH',0x0004,0x0003,0x0001)
DO+=struct.pack('>III',1,0,0)+struct.pack('>iI',0,0)+struct.pack('>HI',0,0)
DO+=struct.pack('>BB',3,0)+struct.pack('>II',0,0)+struct.pack('>II',NO_POS,NO_POS)
DO+=struct.pack('>III',0,0,4096)

blob=DO+img+formchunk
open('/mnt/c/Amiga/workspace/amifm.info','wb').write(blob)
print("colour-icon .info %d bytes (img1 %dB, img2 %dB)"%(len(blob),len(imag1),len(imag2)))
