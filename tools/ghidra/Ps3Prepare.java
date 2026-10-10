// Pre-analysis script: set the TOC register and seed known function entries.
//
// Arguments: <toc hex | none> [<function address hex> | @<address file> ...]
//
// An argument starting with '@' names a text file with one hex address per line (blank lines
// and '#' comments ignored). Long seed lists go through a file because analyzeHeadless.bat
// runs under cmd.exe, which rejects command lines over about 8191 characters.
//
// The ELF sections are unnamed, so Ghidra cannot locate the TOC base (r2 in the PPC64 ELFv1
// ABI) by itself. Without it, TOC-relative addressing is not resolved during analysis.
//
// @category PS3

import java.io.IOException;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.MemoryBlock;

public class Ps3Prepare extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: Ps3Prepare.java <toc hex | none> [<function address hex> | @<address file> ...]");
            throw new IllegalArgumentException("missing arguments");
        }

        if (!args[0].equalsIgnoreCase("none")) {
            setToc(Long.decode(args[0]));
        } else {
            println("TOC not given; r2 left unset");
        }

        List<Long> offsets = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            if (args[i].startsWith("@")) {
                List<Long> fromFile = readAddressFile(args[i].substring(1));
                println("read " + fromFile.size() + " addresses from " + args[i].substring(1));
                offsets.addAll(fromFile);
            } else {
                offsets.add(Long.decode(args[i]));
            }
        }

        int created = 0;
        for (long offset : offsets) {
            if (seedFunction(offset)) {
                created++;
            }
        }
        println("seeded " + created + " of " + offsets.size() + " functions");
    }

    private List<Long> readAddressFile(String path) throws IOException {
        List<Long> result = new ArrayList<>();
        for (String line : Files.readAllLines(Paths.get(path), StandardCharsets.UTF_8)) {
            int hash = line.indexOf('#');
            String text = (hash >= 0 ? line.substring(0, hash) : line).trim();
            if (!text.isEmpty()) {
                result.add(Long.decode(text));
            }
        }
        return result;
    }

    private void setToc(long toc) throws Exception {
        Register r2 = currentProgram.getLanguage().getRegister("r2");
        if (r2 == null) {
            printerr("warning: language has no register r2; TOC not set");
            return;
        }
        ProgramContext context = currentProgram.getProgramContext();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isExecute()) {
                continue;
            }
            context.setValue(r2, block.getStart(), block.getEnd(), BigInteger.valueOf(toc));
            println(String.format("r2 = 0x%08x over %s [%s - %s]", toc, block.getName(),
                    block.getStart(), block.getEnd()));
        }
    }

    private boolean seedFunction(long offset) {
        Address addr = toAddr(offset);
        try {
            if (!disassemble(addr)) {
                printerr(String.format("warning: cannot disassemble seed 0x%08x", offset));
                return false;
            }
            if (createFunction(addr, null) == null) {
                printerr(String.format("warning: cannot create function at 0x%08x", offset));
                return false;
            }
            println(String.format("function at 0x%08x", offset));
            return true;
        } catch (Exception e) {
            printerr(String.format("warning: seed 0x%08x failed: %s", offset, e));
            return false;
        }
    }
}
