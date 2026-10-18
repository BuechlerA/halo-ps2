import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;
public class Dump extends GhidraScript {
  public void run() throws Exception {
    Memory mem = currentProgram.getMemory();
    long base = 0x11000, len = 0x1d6620 - 0x11000;
    byte[] b = new byte[(int)len];
    mem.getBytes(toAddr(base), b);
    TreeSet<Long> targets = new TreeSet<>();
    for (int i = 0; i + 5 <= b.length; i++) {
      if ((b[i] & 0xff) == 0xE8) {
        int rel = (b[i+1]&0xff)|((b[i+2]&0xff)<<8)|((b[i+3]&0xff)<<16)|((b[i+4]&0xff)<<24);
        long t = base + i + 5 + rel;
        if (t >= base && t < base + len) targets.add(t);
      }
    }
    // builtin function table pointers: scan .data/.rdata words that point into text and are 16-aligned
    byte[] d = new byte[0x1b4d28 + 0x1ac30 + 0x100];
    long dbase = 0x1d6620; long dlen = 0x1f1260 + 0x6e41c - dbase;
    byte[] dd = new byte[(int)dlen];
    mem.getBytes(toAddr(dbase), dd);
    for (int i = 0; i + 4 <= dd.length; i += 4) {
      long v = (dd[i]&0xffL)|((dd[i+1]&0xffL)<<8)|((dd[i+2]&0xffL)<<16)|((dd[i+3]&0xffL)<<24);
      if (v >= base && v < base + len && (v & 0xF) == 0) targets.add(v);
    }
    println("targets " + targets.size());
    int made = 0;
    for (long t : targets) {
      Address a = toAddr(t);
      if (currentProgram.getFunctionManager().getFunctionAt(a) != null) continue;
      new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
      if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) made++;
    }
    println("functions made " + made);
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    PrintWriter w = new PrintWriter(new FileWriter("halo_dec.c"));
    FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
    int n=0;
    while (it.hasNext()) {
      Function f = it.next();
      DecompileResults r = di.decompileFunction(f, 30, monitor);
      if (r.decompileCompleted()) { w.println("// FUNC " + f.getEntryPoint()); w.println(r.getDecompiledFunction().getC()); }
      n++;
    }
    w.close();
    println("decompiled "+n);
  }
}
