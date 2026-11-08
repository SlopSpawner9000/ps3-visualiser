import sys, zlib, struct, os
def u32(d,o): return struct.unpack_from('>I',d,o)[0]
def load(p):
    d=open(p,'rb').read()
    if d[:4]==b'QRCC': d=zlib.decompress(d[8:])
    assert d[:4]==b'QRCF'
    return d
def cstr(d,o): return d[o:d.index(b'\0',o)].decode()
def extract(p,out):
    d=load(p)
    toff,tsz,ioff,isz,noff,nsz,_,_,_,_,doff,dsz=[u32(d,8+4*i) for i in range(12)]
    files=[]
    def walk(e):
        while e!=0xffffffff:
            b=toff+e
            tag,nattr,par,prev,nxt,fc,lc=[u32(d,b+4*i) for i in range(7)]
            attrs={}
            for i in range(nattr):
                an,at,v1,v2=[u32(d,b+28+16*i+4*j) for j in range(4)]
                attrs[cstr(d,noff+an)]=(at,v1,v2)
            if 'src' in attrs and 'id' in attrs:
                _,o,s=attrs['src']; name=cstr(d,ioff+attrs['id'][1]+4)
                files.append((name,o,s))
            if fc!=0xffffffff: walk(fc)
            e=nxt
    walk(0)
    for name,o,s in files:
        fp=os.path.join(out,name.lstrip('/')); os.makedirs(os.path.dirname(fp),exist_ok=True)
        open(fp,'wb').write(d[doff+o:doff+o+s])
    return files
if __name__=='__main__':
    for n,o,s in extract(sys.argv[1],sys.argv[2]): print(f'{s:9d} {n}')
