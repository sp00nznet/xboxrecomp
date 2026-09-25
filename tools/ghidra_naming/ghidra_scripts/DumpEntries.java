/* Post-analysis: write every function entry point Ghidra knows, one hex
 * address per line, to the file named by the first script argument.
 *
 * Diff it against tools/disasm's functions.json. A Ghidra entry that falls
 * inside one of ours, right after a ret or ret N, is two functions the
 * detector merged; the second has no dispatch entry until it is seeded.
 *
 * @category XboxRecomp
 */
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DumpEntries extends GhidraScript {
    @Override
    public void run() throws Exception {
        try (PrintWriter w = new PrintWriter(getScriptArgs()[0])) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true))
                w.println(f.getEntryPoint());
        }
    }
}
