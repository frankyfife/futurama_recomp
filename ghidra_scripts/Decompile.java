// Decompiles the functions containing the given addresses and writes the C output to a file.
// Headless: -postScript Decompile.java <out.c> <addr> [<addr> ...]
// @category Futurama

import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionContaining(a);
                if (f == null) {
                    w.printf("// %s: no function%n%n", args[i]);
                    continue;
                }
                DecompileResults r = ifc.decompileFunction(f, 60, monitor);
                w.printf("// ---- %s @ %s ----%n", f.getName(true), f.getEntryPoint());
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
        ifc.dispose();
    }
}
