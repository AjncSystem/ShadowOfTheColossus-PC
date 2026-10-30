// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: Ghidra headless post-script for the SotC eboot (tools/re/README.md).
// 1. Names the PLT stubs and known functions from a "0xADDR name" file (script argument 1).
// 2. Decompiles every function with N threads (argument 3) and writes one C file per function
//    under the output directory (argument 2): <out>/<addr >> 16>/<addr>.c, each starting with a
//    header line "// <name> @ <addr> size <bytes>, called by: <callers>".
//@category SotC

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path names = Paths.get(args[0]);
        Path out = Paths.get(args[1]);
        int threads = args.length > 2 ? Integer.parseInt(args[2]) : 8;

        // 1. names
        int named = 0;
        if (Files.exists(names)) {
            for (String line : Files.readAllLines(names, StandardCharsets.UTF_8)) {
                String[] p = line.trim().split("\\s+", 2);
                if (p.length != 2 || !p[0].startsWith("0x")) {
                    continue;
                }
                Address a = toAddr(Long.parseLong(p[0].substring(2), 16));
                String n = p[1].replaceAll("[^A-Za-z0-9_:.]", "_");
                Function f = getFunctionAt(a);
                if (f == null) {
                    f = createFunction(a, n);
                }
                if (f != null) {
                    f.setName(n, SourceType.USER_DEFINED);
                    named++;
                }
            }
        }
        println("named " + named + " functions");

        // 2. decompile
        List<Function> all = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            all.add(it.next());
        }
        println("decompiling " + all.size() + " functions with " + threads + " threads");
        Files.createDirectories(out);
        AtomicInteger next = new AtomicInteger(0);
        AtomicInteger done = new AtomicInteger(0);
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        for (int t = 0; t < threads; t++) {
            pool.submit(() -> {
                DecompInterface di = new DecompInterface();
                di.setOptions(new DecompileOptions());
                di.toggleCCode(true);
                di.openProgram(currentProgram);
                ConsoleTaskMonitor mon = new ConsoleTaskMonitor();
                int i;
                while ((i = next.getAndIncrement()) < all.size()) {
                    Function f = all.get(i);
                    long addr = f.getEntryPoint().getOffset();
                    Path dir = out.resolve(String.format("%04x", addr >> 16));
                    Path file = dir.resolve(String.format("%08x.c", addr));
                    if (Files.exists(file)) {
                        done.incrementAndGet();
                        continue;
                    }
                    StringBuilder sb = new StringBuilder();
                    sb.append("// ").append(f.getName()).append(" @ 0x").append(Long.toHexString(addr))
                      .append(" size ").append(f.getBody().getNumAddresses()).append(", called by:");
                    int c = 0;
                    for (Function caller : f.getCallingFunctions(mon)) {
                        if (c++ == 12) {
                            sb.append(" ...");
                            break;
                        }
                        sb.append(' ').append(caller.getName());
                    }
                    sb.append('\n');
                    try {
                        DecompileResults r = di.decompileFunction(f, 60, mon);
                        if (r != null && r.decompileCompleted()) {
                            sb.append(r.getDecompiledFunction().getC());
                        } else {
                            sb.append("// decompile failed: ")
                              .append(r == null ? "null" : r.getErrorMessage()).append('\n');
                        }
                        Files.createDirectories(dir);
                        try (BufferedWriter w = Files.newBufferedWriter(file, StandardCharsets.UTF_8)) {
                            w.write(sb.toString());
                        }
                    } catch (Exception e) {
                        // keep going: one bad function must not stop the export
                    }
                    int d = done.incrementAndGet();
                    if (d % 2000 == 0) {
                        println("decompiled " + d + "/" + all.size());
                    }
                }
                di.dispose();
            });
        }
        pool.shutdown();
        pool.awaitTermination(7, TimeUnit.DAYS);
        println("export done: " + done.get() + " functions in " + out);
    }
}
