import sys
from defs import scan, BASE
REG={0:'0',1:'C0',2:'C1',3:'FOG',4:'V0',5:'V1',8:'T0',9:'T1',10:'T2',11:'T3',12:'R0',13:'R1',14:'V1R0',15:'EF'}
MAP=['%s','(1-%s)','expand(%s)','-expand(%s)','(%s-.5)','-(%s-.5)','signed(%s)','-signed(%s)']
TM=['none','2D','3Dp','cube','pass','clip','bump','bumplum','brdf','dotST','dotZW','dotRD','dotRS','dotSTR3D','dotSTRcube','depAR','depGB','dotRSc']
def inp(b,alpha_ctx):
    r=REG.get(b&15,'r%d'%(b&15)); ch=(b>>4)&1
    s=r+('.a' if ch else '.rgb' if not alpha_ctx else '.b')
    if alpha_ctx: s=r+('.a' if ch else '.b')
    return MAP[b>>5]%s
def icw(w,alpha):
    return [inp((w>>s)&255,alpha) for s in (24,16,8,0)]
OPS={0:'',1:'bias',2:'*2',3:'*2bias',4:'*4',6:'/2'}
def ocw(w):
    op=(w>>15)&7
    return dict(cd=REG.get(w&15,w&15),ab=REG.get((w>>4)&15,(w>>4)&15),sum=REG.get((w>>8)&15,(w>>8)&15),mux=(w>>12)&1,abdot=(w>>13)&1,cddot=(w>>14)&1,op=OPS.get(op,op))
def show(fa,ad,m):
    g=lambda off:m.get(BASE+off)
    n=(g(0xD4) or 0)&15
    print('=== func %x call %x  stages %s texmodes %s'%(fa,ad,n,None if g(0xD8) is None else [TM[(g(0xD8)>>(5*i))&31] if ((g(0xD8)>>(5*i))&31)<len(TM) else (g(0xD8)>>(5*i))&31 for i in range(4)]))
    for i in range(max(n,1)):
        ri=g(0x88+4*i); ro=g(0xB4+4*i); ai=g(4*i); ao=g(0x68+4*i)
        c0=g(0x28+4*i); c1=g(0x48+4*i)
        line='  s%d '%i
        if ri is not None:
            A,B,C,D=icw(ri,False); o=ocw(ro) if ro is not None else {}
            line+='RGB: A=%s B=%s C=%s D=%s'%(A,B,C,D)+(' out %s'%o if o else '')
        if ai is not None:
            A,B,C,D=icw(ai,True); o=ocw(ao) if ao is not None else {}
            line+=' | A: A=%s B=%s C=%s D=%s'%(A,B,C,D)+(' out %s'%o if o else '')
        if c0 is not None: line+=' C0=%08x'%c0
        if c1 is not None: line+=' C1=%08x'%c1
        print(line)
    if g(0x20) is not None:
        A,B,C,D=icw(g(0x20),False); E,F,G,_=icw(g(0x24)|0,False) if g(0x24) is not None else ('','','','')
        print('  final: A=%s B=%s C=%s D=%s | E=%s F=%s G=%s flags %x'%(A,B,C,D,E,F,G,(g(0x24) or 0)&255))
if __name__=='__main__':
    for a in sys.argv[1:]:
        fa=int(a,16)
        for ad,m in scan(fa): show(fa,ad,m)
