// Creates Thumb functions at the given addresses (hex, odd = Thumb) and at
// every direct BL/BLX target inside existing code that has no function yet.
// @category Vita
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.ArmDisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class CreateFuncs extends GhidraScript {
	private int made = 0;

	private void make(long v) {
		boolean thumb = (v & 1) != 0;
		Address a = toAddr(v & ~1L);
		if (getFunctionAt(a) != null)
			return;
		if (getInstructionAt(a) == null) {
			ArmDisassembleCommand cmd = new ArmDisassembleCommand(a, null, thumb);
			cmd.applyTo(currentProgram, monitor);
		}
		if (createFunction(a, null) != null)
			made++;
	}

	@Override
	public void run() throws Exception {
		for (String s : getScriptArgs())
			make(Long.parseLong(s.replace("0x", ""), 16));
		for (int pass = 0; pass < 4; pass++) {
			int before = made;
			InstructionIterator it = currentProgram.getListing().getInstructions(true);
			java.util.List<Long> targets = new java.util.ArrayList<>();
			while (it.hasNext()) {
				Instruction ins = it.next();
				String m = ins.getMnemonicString().toLowerCase();
				if (!m.startsWith("bl"))
					continue;
				for (Reference r : ins.getReferencesFrom()) {
					if (!r.getReferenceType().isCall())
						continue;
					Address t = r.getToAddress();
					if (t.getAddressSpace().isMemorySpace() && getFunctionAt(t) == null
					    && currentProgram.getMemory().getBlock(t) != null
					    && currentProgram.getMemory().getBlock(t).isExecute())
						targets.add(t.getOffset() | 1); // SceExfatfs is all Thumb-2
				}
			}
			for (long t : targets)
				make(t);
			if (made == before)
				break;
		}
		println("created " + made + " functions");
	}
}
