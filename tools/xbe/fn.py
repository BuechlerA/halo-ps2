import re,sys
t=open('halo_dec.c').read()
idx=[(int(m.group(1),16),m.start()) for m in re.finditer(r'^// FUNC ([0-9a-f]+)',t,re.M)]
def get(a):
    for i,(ad,st) in enumerate(idx):
        if ad==a: return t[st:(idx[i+1][1] if i+1<len(idx) else len(t))]
def callers(a):
    pat=re.compile(r'FUN_%08x\('%a); out=[]
    for i,(ad,st) in enumerate(idx):
        end=idx[i+1][1] if i+1<len(idx) else len(t)
        if pat.search(t,st,end): out.append(ad)
    return out
if __name__=='__main__':
    if sys.argv[1]=='c':
        print([hex(x) for x in callers(int(sys.argv[2],16))])
    else:
        for a in sys.argv[1:]: print(get(int(a,16)))
