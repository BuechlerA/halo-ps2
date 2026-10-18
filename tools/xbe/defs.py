import re,subprocess,struct,sys
t=open('halo_dec.c').read()
funcs=sorted(int(m.group(1),16) for m in re.finditer(r'^// FUNC ([0-9a-f]+)',t,re.M))
img=open('halo_flat.bin','rb').read()
CALLERS=[int(x,16) for x in eval(subprocess.run(['python3','-I','fn.py','c','0007e5d0'],capture_output=True,text=True).stdout)]
import bisect
def dis(a,b):
    chunk=img[a-0x10000:b-0x10000]
    open('/tmp/claude_chunk.bin','wb').write(chunk)
    out=subprocess.run(['ndisasm','-b','32','-o',hex(a),'/tmp/claude_chunk.bin'],capture_output=True,text=True).stdout
    return [p for p in (l.split(None,2) for l in out.splitlines()) if len(p)==3]
BASE=0x2fce60
def scan(fa):
    i=bisect.bisect_right(funcs,fa); fb=funcs[i] if i<len(funcs) else fa+0x4000
    ins=dis(fa,fb)
    mem={}  # dword offsets
    regs={}
    snaps=[]
    for ad,raw,txt in ins:
        txt=txt.strip()
        m=re.match(r'mov dword \[(0x[0-9a-f]+)\],(0x[0-9a-f]+)$',txt)
        if m:
            a=int(m.group(1),16)
            if BASE<=a<BASE+0xf0+0x20: mem[a]=int(m.group(2),16)
            continue
        m=re.match(r'mov (e[a-d]x|e[sd]i|ebp),(0x[0-9a-f]+)$',txt)
        if m: regs[m.group(1)]=int(m.group(2),16); continue
        m=re.match(r'xor (e..),\1$',txt)
        if m: regs[m.group(1)]=0; continue
        m=re.match(r'mov \[(0x[0-9a-f]+)\],(e..)$',txt)
        if m:
            a=int(m.group(1),16)
            if BASE<=a<BASE+0x110 and m.group(2) in regs: mem[a]=regs[m.group(2)]
            continue
        if txt.startswith('rep stosd') : mem={}  # zeroing
        if re.match(r'mov (e..),\[',txt) or re.match(r'(add|sub|shl|shr|and|or|imul) (e..)',txt):
            r=re.match(r'\w+ (e..)',txt)
            if r and r.group(1) in regs and not txt.startswith('mov e'): regs.pop(r.group(1),None)
            if txt.startswith('mov e'):
                r=re.match(r'mov (e..)',txt); regs.pop(r.group(1),None)
        if txt.startswith('call 0x7e5d0'):
            snaps.append((int(ad,16),dict(mem)))
    return snaps
if __name__=='__main__':
    for fa in CALLERS:
        if fa==0x7e5d0: continue
        for ad,m in scan(fa):
            print(hex(fa),hex(ad),len(m))
