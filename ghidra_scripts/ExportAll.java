// Exports a grep-friendly snapshot of the program for offline/parallel analysis:
//   <out>/decomp/<block>_<addr>.c   decompiled C, one file per 64 KB of address space
//   <out>/callgraph.tsv              caller -> callee edges
//   <out>/strings.tsv                defined strings and the functions referencing them
//   <out>/functions.tsv              address, size, name, signature
// Initializer stubs (functions whose only content is a CRT static-init stub) are written
// to decomp/initstubs_*.c so they do not drown the real code.
// Headless: -postScript ExportAll.java <outdir> [<stub_start> <stub_end>]
// @category Futurama

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.TaskMonitor;

public class ExportAll extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        File decompDir = new File(out, "decomp");
        decompDir.mkdirs();
        long stubStart = args.length > 2 ? Long.decode(args[1]) : -1;
        long stubEnd = args.length > 2 ? Long.decode(args[2]) : -1;

        List<Function> funcs = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            funcs.add(it.next());
        }

        // functions.tsv + callgraph.tsv
        try (PrintWriter fw = new PrintWriter(new File(out, "functions.tsv"), "UTF-8");
             PrintWriter cw = new PrintWriter(new File(out, "callgraph.tsv"), "UTF-8")) {
            fw.println("address\tsize\tblock\tname\tsignature");
            cw.println("caller\tcaller_name\tcallee\tcallee_name");
            for (Function f : funcs) {
                MemoryBlock b = currentProgram.getMemory().getBlock(f.getEntryPoint());
                fw.printf("%s\t%d\t%s\t%s\t%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(),
                    b != null ? b.getName() : "?", f.getName(true), f.getPrototypeString(false, false));
                for (Function c : f.getCalledFunctions(monitor)) {
                    cw.printf("%s\t%s\t%s\t%s%n", f.getEntryPoint(), f.getName(true), c.getEntryPoint(), c.getName(true));
                }
            }
        }

        // strings.tsv
        try (PrintWriter sw = new PrintWriter(new File(out, "strings.tsv"), "UTF-8")) {
            sw.println("address\tfunctions\tstring");
            DataIterator di = currentProgram.getListing().getDefinedData(true);
            while (di.hasNext()) {
                Data d = di.next();
                if (!d.hasStringValue()) {
                    continue;
                }
                StringBuilder refs = new StringBuilder();
                for (Reference r : getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (refs.length() > 0) {
                        refs.append(',');
                    }
                    refs.append(f != null ? f.getEntryPoint().toString() : r.getFromAddress().toString() + "(data)");
                }
                String s = String.valueOf(d.getValue()).replace("\\", "\\\\").replace("\t", "\\t")
                    .replace("\n", "\\n").replace("\r", "\\r");
                sw.printf("%s\t%s\t%s%n", d.getAddress(), refs, s);
            }
        }

        // decompiled C, in parallel
        DecompileConfigurer cfg = new DecompileConfigurer() {
            @Override
            public void configure(DecompInterface d) {
                d.toggleCCode(true);
                d.toggleSyntaxTree(false);
                d.setSimplificationStyle("decompile");
                DecompileOptions o = new DecompileOptions();
                o.grabFromProgram(currentProgram);
                d.setOptions(o);
            }
        };
        DecompilerCallback<String[]> cb = new DecompilerCallback<String[]>(currentProgram, cfg) {
            @Override
            public String[] process(DecompileResults r, TaskMonitor m) throws Exception {
                Function f = r.getFunction();
                String body = r.decompileCompleted() ? r.getDecompiledFunction().getC()
                    : "// decompilation failed: " + r.getErrorMessage() + "\n";
                return new String[] { f.getEntryPoint().toString(), f.getName(true), body,
                    Long.toString(f.getBody().getNumAddresses()) };
            }
        };
        cb.setTimeout(120);
        List<String[]> results = ParallelDecompiler.decompileFunctions(cb, funcs, monitor);
        cb.dispose();

        Map<String, PrintWriter> files = new TreeMap<>();
        TreeMap<Long, String[]> sorted = new TreeMap<>();
        for (String[] r : results) {
            if (r != null) {
                sorted.put(Long.parseLong(r[0], 16), r);
            }
        }
        int n = 0;
        try {
            for (Map.Entry<Long, String[]> e : sorted.entrySet()) {
                long a = e.getKey();
                Address addr = toAddr(a);
                MemoryBlock b = currentProgram.getMemory().getBlock(addr);
                String block = b != null ? b.getName().replaceAll("[^A-Za-z0-9_]", "_") : "unknown";
                boolean stub = a >= stubStart && a < stubEnd;
                String key = (stub ? "initstubs" : block) + String.format("_%08x.c", a & ~0xFFFFL);
                PrintWriter w = files.get(key);
                if (w == null) {
                    w = new PrintWriter(new File(decompDir, key), "UTF-8");
                    files.put(key, w);
                }
                String[] r = e.getValue();
                w.printf("// ==== FUNC %s %s size=%s ====%n%s%n", r[0], r[1], r[3], r[2]);
                n++;
            }
        }
        finally {
            for (PrintWriter w : files.values()) {
                w.close();
            }
        }
        println(String.format("EXPORTED functions=%d decomp_files=%d", n, files.size()));
    }
}
