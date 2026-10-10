// Post-analysis script: write functions and a per-word listing of the code range as JSON.
//
// Arguments: <output json path> <text start hex> <text end hex (exclusive)>
//
// All address ranges in the output are end-exclusive.
//
// @category PS3

import java.io.BufferedWriter;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.framework.Application;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class ExportAnalysis extends GhidraScript {
    private BufferedWriter out;
    private boolean firstInArray;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 3) {
            printerr("usage: ExportAnalysis.java <output json> <text start hex> <text end hex>");
            throw new IllegalArgumentException("expected 3 arguments");
        }
        File outFile = new File(args[0]);
        long textStart = Long.decode(args[1]);
        long textEnd = Long.decode(args[2]);
        if (textStart % 4 != 0 || textEnd % 4 != 0 || textEnd <= textStart) {
            throw new IllegalArgumentException("invalid text range");
        }

        File parent = outFile.getAbsoluteFile().getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        try (BufferedWriter writer = Files.newBufferedWriter(outFile.toPath(),
                StandardCharsets.UTF_8)) {
            out = writer;
            out.write("{\n");
            out.write("\"schema\": 1,\n");
            out.write("\"generator\": \"ExportAnalysis.java\",\n");
            out.write("\"ghidra_version\": " + str(Application.getApplicationVersion()) + ",\n");
            out.write("\"language\": "
                    + str(currentProgram.getLanguageID().getIdAsString()) + ",\n");
            out.write("\"compiler_spec\": "
                    + str(currentProgram.getCompilerSpec().getCompilerSpecID().getIdAsString())
                    + ",\n");
            out.write("\"executable_sha256\": "
                    + str(currentProgram.getExecutableSHA256()) + ",\n");
            writeMemoryBlocks();
            out.write("\"text\": {\"start\": " + str(hex(textStart)) + ", \"end\": "
                    + str(hex(textEnd)) + "},\n");
            writeFunctions();
            AddressSet textSet = new AddressSet(toAddr(textStart), toAddr(textEnd - 1));
            writeWordsAndJumpTables(textStart, textEnd);
            writeDataRefs(textSet);
            out.write("}\n");
        }
        println("exported to " + outFile);
    }

    private void writeMemoryBlocks() throws IOException {
        beginArray("memory_blocks");
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            element("{\"name\": " + str(block.getName())
                    + ", \"start\": " + str(hex(block.getStart().getOffset()))
                    + ", \"end\": " + str(hex(block.getEnd().getOffset() + 1))
                    + ", \"initialized\": " + block.isInitialized()
                    + ", \"read\": " + block.isRead()
                    + ", \"write\": " + block.isWrite()
                    + ", \"execute\": " + block.isExecute() + "}");
        }
        endArray(false);
    }

    private void writeFunctions() throws Exception {
        beginArray("functions");
        int count = 0;
        for (Function function : currentProgram.getFunctionManager().getFunctions(true)) {
            monitor.checkCancelled();
            if (function.isExternal()) {
                continue;
            }
            StringBuilder body = new StringBuilder();
            for (AddressRange range : function.getBody().getAddressRanges()) {
                if (body.length() > 0) {
                    body.append(", ");
                }
                body.append("[").append(str(hex(range.getMinAddress().getOffset()))).append(", ")
                        .append(str(hex(range.getMaxAddress().getOffset() + 1))).append("]");
            }
            element("{\"entry\": " + str(hex(function.getEntryPoint().getOffset()))
                    + ", \"name\": " + str(function.getName())
                    + ", \"thunk\": " + function.isThunk()
                    + ", \"body\": [" + body + "]}");
            count++;
        }
        endArray(false);
        println("functions: " + count);
    }

    private void writeWordsAndJumpTables(long textStart, long textEnd) throws Exception {
        Listing listing = currentProgram.getListing();
        Memory memory = currentProgram.getMemory();
        PseudoDisassembler pseudo = new PseudoDisassembler(currentProgram);
        List<String> jumpTables = new ArrayList<>();

        beginArray("words");
        for (long offset = textStart; offset < textEnd; offset += 4) {
            if ((offset & 0xfff) == 0) {
                monitor.checkCancelled();
            }
            Address addr = toAddr(offset);
            long word = memory.getInt(addr) & 0xffffffffL;
            Instruction code = listing.getInstructionAt(addr);
            String kind;
            Instruction decoded;
            if (code != null) {
                if (code.getLength() != 4) {
                    throw new IllegalStateException(
                            "instruction at " + addr + " is " + code.getLength() + " bytes");
                }
                kind = "code";
                decoded = code;
                String jump = jumpTableEntry(code);
                if (jump != null) {
                    jumpTables.add(jump);
                }
            } else {
                kind = listing.getDefinedDataContaining(addr) != null ? "data" : "undefined";
                decoded = pseudoDecode(pseudo, addr);
            }

            String mnemonic = "null";
            String operands = "null";
            if (decoded != null) {
                String name = decoded.getMnemonicString();
                String text = decoded.toString();
                if (text.startsWith(name)) {
                    text = text.substring(name.length());
                }
                mnemonic = str(name);
                operands = str(text.trim());
            }
            element("{\"addr\": " + str(hex(offset)) + ", \"word\": " + str(hex(word))
                    + ", \"kind\": " + str(kind) + ", \"mnemonic\": " + mnemonic
                    + ", \"operands\": " + operands + "}");
        }
        endArray(false);

        beginArray("jump_tables");
        for (String entry : jumpTables) {
            element(entry);
        }
        endArray(false);
    }

    private Instruction pseudoDecode(PseudoDisassembler pseudo, Address addr) {
        try {
            return pseudo.disassemble(addr);
        } catch (Exception e) {
            return null;
        }
    }

    private String jumpTableEntry(Instruction instruction) {
        FlowType flow = instruction.getFlowType();
        if (!flow.isJump() || !flow.isComputed()) {
            return null;
        }
        TreeSet<Long> targets = new TreeSet<>();
        for (Reference ref : instruction.getReferencesFrom()) {
            if (ref.getReferenceType().isFlow() && ref.getToAddress().isMemoryAddress()) {
                targets.add(ref.getToAddress().getOffset());
            }
        }
        if (targets.isEmpty()) {
            return null;
        }
        StringBuilder list = new StringBuilder();
        for (long target : targets) {
            if (list.length() > 0) {
                list.append(", ");
            }
            list.append(str(hex(target)));
        }
        return "{\"branch\": " + str(hex(instruction.getAddress().getOffset()))
                + ", \"targets\": [" + list + "]}";
    }

    private void writeDataRefs(AddressSet textSet) throws Exception {
        ReferenceManager refs = currentProgram.getReferenceManager();
        Memory memory = currentProgram.getMemory();
        List<Reference> selected = new ArrayList<>();
        AddressIterator destinations = refs.getReferenceDestinationIterator(textSet, true);
        while (destinations.hasNext()) {
            monitor.checkCancelled();
            for (Reference ref : refs.getReferencesTo(destinations.next())) {
                if (!ref.getFromAddress().isMemoryAddress()) {
                    continue;
                }
                if (ref.getReferenceType().isData() || !isExecutable(memory, ref.getFromAddress())) {
                    selected.add(ref);
                }
            }
        }
        selected.sort(Comparator.<Reference>comparingLong(r -> r.getFromAddress().getOffset())
                .thenComparingLong(r -> r.getToAddress().getOffset()));

        beginArray("data_refs");
        for (Reference ref : selected) {
            element("{\"from\": " + str(hex(ref.getFromAddress().getOffset()))
                    + ", \"to\": " + str(hex(ref.getToAddress().getOffset()))
                    + ", \"type\": " + str(ref.getReferenceType().getName())
                    + ", \"from_exec\": " + isExecutable(memory, ref.getFromAddress()) + "}");
        }
        endArray(true);
    }

    private static boolean isExecutable(Memory memory, Address addr) {
        MemoryBlock block = memory.getBlock(addr);
        return block != null && block.isExecute();
    }

    private void beginArray(String key) throws IOException {
        out.write("\"" + key + "\": [\n");
        firstInArray = true;
    }

    private void element(String json) throws IOException {
        if (!firstInArray) {
            out.write(",\n");
        }
        firstInArray = false;
        out.write(json);
    }

    private void endArray(boolean last) throws IOException {
        out.write(firstInArray ? "" : "\n");
        out.write(last ? "]\n" : "],\n");
    }

    private static String hex(long value) {
        return String.format("0x%08x", value);
    }

    private static String str(String value) {
        if (value == null) {
            return "null";
        }
        StringBuilder sb = new StringBuilder(value.length() + 2);
        sb.append('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            switch (c) {
                case '"':
                    sb.append("\\\"");
                    break;
                case '\\':
                    sb.append("\\\\");
                    break;
                case '\n':
                    sb.append("\\n");
                    break;
                case '\r':
                    sb.append("\\r");
                    break;
                case '\t':
                    sb.append("\\t");
                    break;
                default:
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
            }
        }
        return sb.append('"').toString();
    }
}
