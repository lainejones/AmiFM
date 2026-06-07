#!/usr/bin/env python3
# Decode amifm.info's colour images back out, to verify the encoder round-trips.
import struct
b = open('/mnt/c/Amiga/workspace/amifm.info','rb').read()
s = b.find(b'FORM'); assert b[s+8:s+12]==b'ICON', "no FORM ICON"
W=H=None; imags=[]
p=s+12
while p < len(b)-8:
    cid=b[p:p+4]; clen=struct.unpack('>I',b[p+4:p+8])[0]; doff=p+8
    if cid==b'FACE': W=b[doff]+1; H=b[doff+1]+1
    elif cid==b'IMAG': imags.append(doff)
    p+=8+clen+(clen&1)
print("FACE %dx%d, %d IMAG"%(W,H,len(imags)))
class BR:
    def __init__(s,d): s.d=d; s.p=0
    def bits(s,n):
        v=0
        for _ in range(n):
            v=(v<<1)|((s.d[s.p>>3]>>(7-(s.p&7)))&1); s.p+=1
        return v
def unpack(data,unit,count):
    br=BR(data); out=[]
    while len(out)<count:
        n=br.bits(8)
        if n<128:
            for _ in range(n+1): out.append(br.bits(unit))
        elif n>128:
            u=br.bits(unit)
            for _ in range(257-n): out.append(u)
    return out[:count]
for k,doff in enumerate(imags):
    transp=b[doff]; ncol=b[doff+1]+1; depth=b[doff+5]
    numimg=struct.unpack('>H',b[doff+6:doff+8])[0]+1
    numpal=struct.unpack('>H',b[doff+8:doff+10])[0]+1
    pp=doff+10; imgd=b[pp:pp+numimg]; pp+=numimg; pald=b[pp:pp+numpal]
    pix=unpack(imgd,depth,W*H); palu=unpack(pald,8,ncol*3)
    pal=[(palu[i*3],palu[i*3+1],palu[i*3+2]) for i in range(ncol)]
    with open('/mnt/c/Amiga/workspace/verify%d_grid.txt'%k,'w') as f:
        f.write("%d %d\n"%(W,H))
        for y in range(H):
            f.write(" ".join(("T" if pix[y*W+x]==transp else "%d,%d,%d"%pal[pix[y*W+x]]) for x in range(W))+"\n")
    print("image%d depth=%d ncol=%d -> verify%d_grid.txt"%(k,depth,ncol,k))
