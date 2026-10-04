// Creates functions at every non-null pointer of one or more pointer tables
// (e.g. the CRT __xc_a..__xc_z static-initializer table).
// Headless: -postScript CreateFunctionsFromTable.java <start> <end> [<start> <end> ...]
// @category Futurama

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class CreateFunctionsFromTable extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        int created = 0, existing = 0, failed = 0;
        for (int i = 0; i + 1 < args.length; i += 2) {
            Address start = toAddr(args[i]);
            Address end = toAddr(args[i + 1]);
            for (Address a = start; a.compareTo(end) < 0; a = a.add(4)) {
                long p = getInt(a) & 0xffffffffL;
                if (p == 0 || p == 0xffffffffL) {
                    continue;
                }
                Address target = toAddr(p);
                if (getFunctionAt(target) != null) {
                    existing++;
                    continue;
                }
                if (getInstructionAt(target) == null) {
                    new DisassembleCommand(target, null, true).applyTo(currentProgram, monitor);
                }
                Function f = createFunction(target, null);
                if (f != null) {
                    created++;
                } else {
                    failed++;
                }
            }
        }
        println(String.format("TABLE FUNCTIONS created=%d existing=%d failed=%d", created, existing, failed));
    }
}
