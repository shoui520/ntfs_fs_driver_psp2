// Dumps every function: entry, size, callees, callers, referenced strings.
// @category Vita
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;
import java.util.*;

public class DumpCallGraph extends GhidraScript {
	@Override
	public void run() throws Exception {
		String out = getScriptArgs()[0];
		PrintWriter w = new PrintWriter(new FileWriter(out));
		FunctionManager fm = currentProgram.getFunctionManager();
		ReferenceManager rm = currentProgram.getReferenceManager();
		Listing listing = currentProgram.getListing();
		for (Function f : fm.getFunctions(true)) {
			if (f.isExternal())
				continue;
			StringBuilder sb = new StringBuilder();
			sb.append(f.getEntryPoint()).append('\t').append(f.getName()).append('\t')
			  .append(f.getBody().getNumAddresses()).append('\t');
			TreeSet<String> callees = new TreeSet<>();
			for (Function c : f.getCalledFunctions(monitor))
				callees.add(c.getName());
			sb.append(String.join(",", callees)).append('\t');
			TreeSet<String> callers = new TreeSet<>();
			for (Function c : f.getCallingFunctions(monitor))
				callers.add(c.getName());
			sb.append(String.join(",", callers)).append('\t');
			TreeSet<String> strs = new TreeSet<>();
			AddressIterator it = f.getBody().getAddresses(true);
			while (it.hasNext()) {
				Address a = it.next();
				for (Reference r : rm.getReferencesFrom(a)) {
					Data d = listing.getDataAt(r.getToAddress());
					if (d != null && d.hasStringValue())
						strs.add(d.getDefaultValueRepresentation());
				}
			}
			sb.append(String.join(" ", strs));
			w.println(sb);
		}
		w.close();
	}
}
