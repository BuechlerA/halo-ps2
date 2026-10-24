"""Run the real Xbox observer (default.xbe, 0x11dd30) under unicorn (pip install unicorn): the ground truth for camera motion (docs/36, ADR-083).

    python3 observer_oracle.py golden > engine/tests/cine_observer_golden.h     (needs halo_flat.bin in the working directory)

The director's job (scripted camera, 0x120a90 case 0 + 0x120160) is replayed here: it builds the 0x68 byte record and the remaining time; the observer
update runs the XBE's own code (0x11c760 .. 0x11d2d0), stopping where the collision and BSP part of 0x11d900 begins. A script is the camscript format of
engine/tests/test_cine_observer.c: `cut yaw pitch roll x y z`, `move yaw pitch roll x y z ticks`, `run n` (Halo euler angles)."""
import struct, math, sys
from unicorn import *
from unicorn.x86_const import *

BASE = 0x10000
FLAT = "halo_flat.bin"                    # xbe2flat.py output, the working directory of every tools/xbe script
STACK = 0x7ff00000
SENT = 0x0F000000

class Oracle:
    def __init__(self):
        self.mu = Uc(UC_ARCH_X86, UC_MODE_32)
        img = open(FLAT, "rb").read()
        size = (len(img) + 0xfff) & ~0xfff
        self.mu.mem_map(BASE, size + 0x100000)
        self.mu.mem_write(BASE, img)
        self.mu.mem_map(STACK - 0x100000, 0x200000)
        self.mu.mem_map(SENT, 0x1000)
        self.mu.mem_map(0x05000000, 0x10000)
        self.mu.mem_write(0x05000000 + 4, struct.pack("<i", 0))          # player 0 exists (entry != -1)
        self.w32(0x2fad20, 0x05000000)
        self.mu.hook_add(UC_HOOK_CODE, self._hook, begin=0x11d900, end=0x11d900)
        self.mu.hook_add(UC_HOOK_CODE, self._hook, begin=SENT, end=SENT)
    def _hook(self, uc, addr, size, data): uc.emu_stop()
    def w32(self, a, v): self.mu.mem_write(a, struct.pack("<I", v & 0xffffffff))
    def wf(self, a, *v): self.mu.mem_write(a, struct.pack("<%df" % len(v), *v))
    def rf(self, a, n): return struct.unpack("<%df" % n, bytes(self.mu.mem_read(a, 4 * n)))
    def call(self, addr, args=(), regs=None):
        esp = STACK
        for a in reversed(args): esp -= 4; self.w32(esp, a)
        esp -= 4; self.w32(esp, SENT)
        self.mu.reg_write(UC_X86_REG_ESP, esp)
        for r in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP): self.mu.reg_write(r, 0)
        for r, v in (regs or {}).items(): self.mu.reg_write(r, v)
        self.mu.emu_start(addr, SENT, count=2000000)
    def fbits(self, f): return struct.unpack("<I", struct.pack("<f", f))[0]
    # ---- the observer -------------------------------------------------------------------------------------------
    REC = 0x271150            # the director's record of player 0 (0x68 bytes)
    def set_target(self, pos, fwd, up, fov, remaining, first):
        rec = [0.0] * 26
        self.mu.mem_write(self.REC, b"\0" * 0x68)
        self.mu.mem_write(self.REC, struct.pack("<I", 8 | 1))
        self.wf(self.REC + 4, *pos)
        self.wf(self.REC + 0x20, fov)
        self.wf(self.REC + 0x24, *fwd); self.wf(self.REC + 0x30, *up)
        self.wf(self.REC + 0x48, remaining)
        self.w32(0x2714e0, self.REC)
        self.mu.mem_write(0x27154c, b"\x01")
        if first:
            self.mu.mem_write(0x27154d, b"\x01")
    def update(self, dt):
        self.call(0x11dd30, (self.fbits(dt),))
    def state(self):
        s = self.rf(0x27158c, 14)
        return dict(pos=s[0:3], dist=s[6], fov=s[7], fwd=s[8:11], up=s[11:14])

def halo_axes(y, p, r):
    c1, s1, c2, s2, c3, s3 = math.cos(y), math.sin(y), math.cos(p), math.sin(p), math.cos(r), math.sin(r)
    return ([c1*c2, s1*c3 - s2*s3*c1, s1*s3 + s2*c3*c1], [-s2, -c2*s3, c2*c3])


def run_move(o, a, b, ticks, total, dt=1 / 30.0, first=True):
    """Cut to pose a, then camera_set b over `ticks` ticks; one record per game tick. poses are (pos, (yaw, pitch, roll), fov)."""
    fa, ua = halo_axes(*a[1]); fb, ub = halo_axes(*b[1])
    o.set_target(a[0], fa, ua, a[2], 0.0, first); o.update(1e-4)
    rem = int(ticks / 30) * 1.0                      # the director's seconds (integer division, 0x120f78)
    out = []
    o.set_target(b[0], fb, ub, b[2], rem, False); o.update(1e-4)       # camera_set: the immediate pass
    for t in range(total):
        o.set_target(b[0], fb, ub, b[2], rem, False)
        o.update(dt)
        rem = max(0.0, rem - dt)
        s = o.state(); out.append((t + 1, s))
    return out

def run_script(o, lines):
    """Same script format as camscript.c; returns [(tick, pos, fwd, up)]."""
    out = []; tk = 0; rem = 0.0; tgt = None; first = True
    for ln in lines:
        w = ln.split()
        if w[0] in ("cut", "move"):
            v = [float(x) for x in w[1:7]]
            ticks = int(w[7]) if w[0] == "move" else 0
            f, u = halo_axes(v[0], v[1], v[2])
            tgt = (v[3:6], f, u); rem = ticks / 30.0
            o.set_target(tgt[0], f, u, 1.222, rem, first); first = False
            o.update(1e-4)                                    # the camera_set pass
        elif w[0] == "run":
            for _ in range(int(w[1])):
                o.set_target(tgt[0], tgt[1], tgt[2], 1.222, rem, False)
                o.update(1 / 30.0); rem = max(0.0, rem - 1 / 30.0); tk += 1
                s = o.state(); out.append((tk, s["pos"], s["fwd"], s["up"]))
    return out


GOLDEN_SCRIPTS = [
    ["cut 2.381 0.797 -0.598 -51.48 0.57 1.98", "move 2.310 1.115 -0.688 -51.51 0.60 2.02 120", "run 130"],
    ["cut 0.0 0.0 0.0 0 0 1", "move 1.2 -0.3 0.4 10 5 2 90", "run 40", "move -2.5 0.5 -0.6 -4 8 3 60", "run 30", "move 3.0 0.0 0.0 1 1 1 30", "run 45"],
    ["cut 3.1 -0.2 0.0 -50.6 0.05 1.94", "move -3.11 0.56 0.01 -50.6 0.05 2.68 200", "run 100", "move -2.17 -0.15 -0.21 -54.7 1.1 2.0 250", "run 60"],
    ["cut -1.25 -0.55 1.0 -26.1 9.9 5.1", "move -1.15 -0.42 0.73 -26.3 10.6 3.6 200", "run 80", "move 0.6 0.1 0.1 5 5 2 0", "run 5"],
]

def _demo():
    o = Oracle()
    for t, pos, f, u in run_script(o, GOLDEN_SCRIPTS[0])[::10]: print(t, [round(x, 4) for x in pos], [round(x, 4) for x in f], [round(x, 4) for x in u])

def golden():
    print("/* Generated by tools/xbe/observer_oracle.py golden from the Xbox executable's observer (XBE 0x11dd30): camscript, then per script the samples every 5th tick:\n"
          " * tick, position xyz, forward xyz, up xyz. Behaviour only, no code. */")
    print("static const char *const OBSERVER_SCRIPTS[] = {")
    for sc in GOLDEN_SCRIPTS: print('\t"' + "\\n".join(sc) + '\\n",')
    print("};")
    print("static const float OBSERVER_GOLDEN[][10] = {")
    for si, sc in enumerate(GOLDEN_SCRIPTS):
        res = run_script(Oracle(), sc)
        print("\t/* script %d */" % si)
        for t, pos, f, u in res:
            if t % 5 == 0: print("\t{ %d, %s }," % (t, ", ".join("%.6ff" % x for x in (*pos, *f, *u))))
        print("\t{ -1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },")
    print("};")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "golden": golden()
    else: _demo()
