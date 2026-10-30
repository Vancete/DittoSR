import sys, zlib, struct
# minimal PNG crop (8-bit RGB, filter-aware decode)
def read(p):
    d=open(p,'rb').read(); i=8; idat=b''; 
    while i<len(d):
        n=struct.unpack('>I',d[i:i+4])[0]; t=d[i+4:i+8]; c=d[i+8:i+8+n]; i+=12+n
        if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
        elif t==b'IDAT': idat+=c
    raw=zlib.decompress(idat); bpp=3; st=w*3; out=bytearray(); prev=bytearray(st); o=0
    for y in range(h):
        f=raw[o]; o+=1; line=bytearray(raw[o:o+st]); o+=st
        for x in range(st):
            a=line[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                pa=abs(b-c); pb=abs(a-c); pc=abs(a+b-2*c)
                pr=a if pa<=pb and pa<=pc else (b if pb<=pc else c); line[x]=(line[x]+pr)&255
        out+=line; prev=line
    return w,h,out
def write(p,w,h,px):
    raw=b''.join(b'\0'+bytes(px[y*w*3:(y+1)*w*3]) for y in range(h))
    ch=lambda t,c: struct.pack('>I',len(c))+t+c+struct.pack('>I',zlib.crc32(t+c)&0xffffffff)
    open(p,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
# crop x y w h scale from each input, stacked vertically: crop.py out x y w h s in1 in2 ...
if __name__ == "__main__":
  o,x0,y0,cw,chh,s=sys.argv[1],*map(int,sys.argv[2:7]); ims=sys.argv[7:]
  W=cw*s; H=chh*s*len(ims); px=bytearray(W*H*3)
  for k,p in enumerate(ims):
      w,h,d=read(p)
      for y in range(chh*s):
          for x in range(W):
              sx=x0+x//s; sy=y0+y//s; j=((k*chh*s+y)*W+x)*3; i=(sy*w+sx)*3; px[j:j+3]=d[i:i+3]
  write(o,W,H,px)
