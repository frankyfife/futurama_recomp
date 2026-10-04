// Applies a symbol TSV (address<TAB>kind<TAB>name) to the current program.
//   kind = func            create/rename function; "Class::method" goes into class namespace,
//                          methods other than CreateObject become __thiscall
//   kind = label           plain label
//   kind = vtable:<n>      label + array of n pointers
//   kind = class:<size>:<parent>   (address ignored) class namespace + struct of <size> bytes
//                          whose first member is the parent struct (or a vfptr for roots)
// Headless: -postScript ApplySymbols.java <symbols.tsv>
// @category Futurama

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.listing.VariableUtilities;

public class ApplySymbols extends GhidraScript {
    private SymbolTable st;
    private DataTypeManager dtm;

    private GhidraClass getClass(String name) throws Exception {
        Namespace ns = NamespaceUtils.createNamespaceHierarchy(name, null, currentProgram, SourceType.USER_DEFINED);
        if (ns instanceof GhidraClass) {
            return (GhidraClass) ns;
        }
        return NamespaceUtils.convertNamespaceToClass(ns);
    }

    @Override
    protected void run() throws Exception {
        st = currentProgram.getSymbolTable();
        dtm = currentProgram.getDataTypeManager();
        List<String[]> rows = new ArrayList<>();
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            if (!line.isBlank() && !line.startsWith("#")) {
                rows.add(line.split("\t"));
            }
        }
        int funcs = 0, labels = 0, vtables = 0, classes = 0, failed = 0;

        // Pass 1: classes and their structs (parents first, so repeat until stable).
        List<String[]> pending = new ArrayList<>();
        for (String[] r : rows) {
            if (r[1].startsWith("class:")) {
                pending.add(r);
            }
        }
        int guard = 0;
        while (!pending.isEmpty() && guard++ < 32) {
            List<String[]> next = new ArrayList<>();
            for (String[] r : pending) {
                String[] k = r[1].split(":", -1);
                int size = Integer.parseInt(k[1]);
                String parent = k.length > 2 ? k[2] : "";
                Structure parentStruct = null;
                if (!parent.isEmpty()) {
                    parentStruct = VariableUtilities.findExistingClassStruct(getClass(parent), dtm);
                    if (parentStruct == null || parentStruct.isZeroLength()) {
                        if (pending.stream().anyMatch(p -> p[2].equals(parent))) {
                            next.add(r);
                            continue;
                        }
                        parentStruct = null;
                    }
                }
                GhidraClass cls = getClass(r[2]);
                Structure s = VariableUtilities.findOrCreateClassStruct(cls, dtm);
                if (s.getLength() <= 1 || s.isZeroLength()) {
                    s.deleteAll();
                    if (parentStruct != null) {
                        s.add(parentStruct, "base", null);
                    }
                    else {
                        s.add(new PointerDataType(new PointerDataType(dtm), dtm), "vfptr", null);
                    }
                    if (s.getLength() < size) {
                        s.growStructure(size - s.getLength());
                    }
                }
                classes++;
            }
            pending = next;
        }

        // Pass 2: addresses.
        for (String[] r : rows) {
            if (r[1].startsWith("class:")) {
                continue;
            }
            try {
                Address a = toAddr(Long.parseLong(r[0], 16));
                String full = r[2];
                Namespace ns = currentProgram.getGlobalNamespace();
                String name = full;
                int sep = full.lastIndexOf("::");
                if (sep > 0) {
                    ns = getClass(full.substring(0, sep));
                    name = full.substring(sep + 2);
                }
                if (r[1].equals("func")) {
                    Function f = getFunctionAt(a);
                    if (f == null) {
                        if (getInstructionAt(a) == null) {
                            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                        }
                        f = createFunction(a, null);
                    }
                    if (f == null) {
                        failed++;
                        continue;
                    }
                    f.setParentNamespace(ns);
                    f.setName(name, SourceType.USER_DEFINED);
                    if (ns instanceof GhidraClass && !name.equals("CreateObject")) {
                        f.setCallingConvention("__thiscall");
                    }
                    funcs++;
                }
                else {
                    Symbol s = st.createLabel(a, name, ns, SourceType.USER_DEFINED);
                    s.setPrimary();
                    if (r[1].startsWith("vtable:")) {
                        int n = Integer.parseInt(r[1].substring(7));
                        if (n > 0) {
                            DataType arr = new ArrayDataType(new PointerDataType(dtm), n, 4, dtm);
                            clearListing(a, a.add(4L * n - 1));
                            createData(a, arr);
                        }
                        vtables++;
                    }
                    else {
                        labels++;
                    }
                }
            }
            catch (Exception e) {
                printerr("failed " + String.join(" ", r) + ": " + e.getMessage());
                failed++;
            }
        }
        println(String.format("APPLIED classes=%d funcs=%d labels=%d vtables=%d failed=%d",
            classes, funcs, labels, vtables, failed));
    }
}
