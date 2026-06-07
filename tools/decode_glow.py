#!/usr/bin/env python3
# Decode a real GlowIcon's first colour image (to confirm the RLE bitstream format).
import struct, sys
fn = '/mnt/c/Amiga/test32/Tools/Calculator.info'
b = open(fn,'rb').read()

# find FORM....ICON
s = b.find(b'FORM')
assert b[s+8:s+12]==b'ICON'
o = s+12
def chunks():
    p=o
    while p < len(b)-8:
        cid=b[p:p+4]; clen=struct.unpack('>I',b[p+4:p+8])[0]
        yield cid, p+8, clen
        p += 8+clen+(clen&1)
W=H=None; imags=[]
for cid,doff,clen in chunks():
    if cid==b'FACE':
        W=b[doff]+1; H=b[doff+1]+1
    elif cid==b'IMAG':
        imags.append((doff,clen))
print("FACE %dx%d, %d IMAG chunks"%(W,H,len(imags)))

class BitReader:
    def __init__(s,data): s.d=data; s.pos=0
    def bits(s,n):
        v=0
        for _ in range(n):
            byte=s.d[s.pos>>3]; bit=(byte>>(7-(s.pos&7)))&1
            v=(v<<1)|bit; s.pos+=1
        return v

def decode_imag(doff):
    transp=b[doff]; ncol=b[doff+1]+1; flags=b[doff+2]
    imgfmt=b[doff+3]; palfmt=b[doff+4]; depth=b[doff+5]
    numimg=struct.unpack('>H',b[doff+6:doff+8])[0]+1
    numpal=struct.unpack('>H',b[doff+8:doff+10])[0]+1
    p=doff+10
    imgdata=b[p:p+numimg]; p+=numimg
    paldata=b[p:p+numpal]
    print("  IMAG transp=%d ncol=%d depth=%d imgfmt=%d palfmt=%d numimg=%d numpal=%d"%(
          transp,ncol,depth,imgfmt,palfmt,numimg,numpal))
    # --- decode image: bit-level PackBits, units = depth bits ---
    def unpack(data, unit, count):
        br=BitReader(data); out=[]
        while len(out)<count:
            n=br.bits(8)
            if n<128:               # literal: n+1 units
                for _ in range(n+1): out.append(br.bits(unit))
            elif n>128:             # repeat: 257-n copies of 1 unit
                u=br.bits(unit)
                for _ in range(257-n): out.append(u)
            # n==128 -> nop
        return out[:count]
    pix = unpack(imgdata, depth, W*H) if imgfmt==1 else None
    palu = unpack(paldata, 8, ncol*3) if palfmt==1 else list(paldata)
    pal=[(palu[i*3],palu[i*3+1],palu[i*3+2]) for i in range(ncol)]
    return transp,depth,pix,pal

transp,depth,pix,pal = decode_imag(imags[0][0])
# dump grid (transparent shown as 'T')
with open('/mnt/c/Amiga/workspace/decode_grid.txt','w') as f:
    f.write("%d %d\n"%(W,H))
    for y in range(H):
        row=[]
        for x in range(W):
            v=pix[y*W+x]
            row.append("T" if v==transp else "%d,%d,%d"%pal[v])
        f.write(" ".join(row)+"\n")
print("decoded image1 -> decode_grid.txt")
