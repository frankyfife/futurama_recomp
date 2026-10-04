// Exports all functions (address, size, name, source) of the current program to a TSV file
// and prints a summary of how many are named vs. auto-named.
// Headless: -postScript ExportFunctions.java <out.tsv>
// @category Futurama

import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ExportFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "functions.tsv";
        Map<String, int[]> perBlock = new TreeMap<>();
        int total = 0, named = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("address\tsize\tblock\tsource\tname");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                MemoryBlock b = currentProgram.getMemory().getBlock(f.getEntryPoint());
                String block = b != null ? b.getName() : "?";
                boolean isNamed = f.getSymbol().getSource() != SourceType.DEFAULT;
                int[] c = perBlock.computeIfAbsent(block, k -> new int[2]);
                c[0]++;
                total++;
                if (isNamed) {
                    c[1]++;
                    named++;
                }
                w.printf("%s\t%d\t%s\t%s\t%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(),
                    block, f.getSymbol().getSource(), f.getName(true));
            }
        }
        println(String.format("FUNCTIONS total=%d named=%d", total, named));
        for (Map.Entry<String, int[]> e : perBlock.entrySet()) {
            println(String.format("  %-10s total=%6d named=%6d", e.getKey(), e.getValue()[0], e.getValue()[1]));
        }
    }
}
