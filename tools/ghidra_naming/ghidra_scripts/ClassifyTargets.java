/* Post-analysis triage: what is at each of these addresses?
 *
 * For each hex address in the script arguments, prints the memory block, the
 * Ghidra function containing it (marked (ENTRY) when the address is that
 * function's start), and whether an instruction starts exactly there, the
 * address lands mid-instruction, or Ghidra holds it as data. Built for the
 * addresses a run logs as "[ICALL] Failed to resolve VA": one headless pass
 * answers "missing function, merged function, or garbage pointer?" for
 * hundreds of them. Ghidra can be wrong too -- check the bytes when it says
 * DATA for an address a title calls.
 *
 *   CT 0022e280 block=.text func=- INSN:MOV EAX,[0x005bb930]
 *   CT 00194890 block=.text func=FUN_00194890@00194890(ENTRY) INSN:PUSH -0x1
 *
 * @category XboxRecomp
 */
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;

public class ClassifyTargets extends GhidraScript {
    @Override
    public void run() throws Exception {
        Listing l = currentProgram.getListing();
        for (String s : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(s.replace("0x", ""), 16));
            MemoryBlock b = currentProgram.getMemory().getBlock(a);
            Function f = getFunctionContaining(a);
            Instruction at = l.getInstructionAt(a);
            Instruction cov = l.getInstructionContaining(a);
            Data d = l.getDataContaining(a);
            String kind = at != null ? "INSN:" + at
                        : cov != null ? "MID_INSN(" + cov.getAddress() + ":" + cov + ")"
                        : d != null ? "DATA:" + d.getDataType().getName()
                        : "UNDEFINED";
            println("CT " + a + " block=" + (b == null ? "-" : b.getName())
                    + " func=" + (f == null ? "-" : f.getName() + "@" + f.getEntryPoint()
                                   + (f.getEntryPoint().equals(a) ? "(ENTRY)" : ""))
                    + " " + kind);
        }
    }
}
