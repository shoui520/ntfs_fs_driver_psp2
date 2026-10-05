// Applies struct definitions, global types, function names and signatures
// from a plain-text spec (see exfat-types.spec):
//   struct <name> <size>
//     <hexoff> <type> <field>          type: u8 u16 u32 u64 s32 ptr <struct>* <struct> <type>[n]
//   end
//   global <hexaddr> <type> <name>
//   func <hexaddr> <name> [<C prototype>]
// @category Vita
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.util.*;

public class ApplyTypes extends GhidraScript {
	private DataTypeManager dtm;
	private CategoryPath cat = new CategoryPath("/exfatfs");
	private Map<String, Structure> structs = new LinkedHashMap<>();

	private DataType base(String t) {
		switch (t) {
		case "u8": return new UnsignedCharDataType();
		case "s8": return new CharDataType();
		case "u16": return new UnsignedShortDataType();
		case "s16": return new ShortDataType();
		case "u32": return new UnsignedIntegerDataType();
		case "s32": return new IntegerDataType();
		case "u64": return new UnsignedLongLongDataType();
		case "s64": return new LongLongDataType();
		case "ptr": return new PointerDataType(VoidDataType.dataType);
		case "char": return new CharDataType();
		}
		DataType d = structs.get(t);
		if (d == null)
			d = dtm.getDataType(cat, t);
		if (d == null)
			throw new RuntimeException("unknown type " + t);
		return d;
	}

	private DataType type(String t) {
		if (t.endsWith("]")) {
			int i = t.indexOf('[');
			int n = Integer.decode(t.substring(i + 1, t.length() - 1));
			DataType e = type(t.substring(0, i));
			return new ArrayDataType(e, n, e.getLength());
		}
		if (t.endsWith("*"))
			return new PointerDataType(type(t.substring(0, t.length() - 1)));
		return base(t);
	}

	@Override
	public void run() throws Exception {
		dtm = currentProgram.getDataTypeManager();
		List<String> lines = new ArrayList<>();
		BufferedReader r = new BufferedReader(new FileReader(getScriptArgs()[0]));
		for (String l; (l = r.readLine()) != null; ) {
			int h = l.indexOf('#');
			if (h >= 0) l = l.substring(0, h);
			if (!l.trim().isEmpty()) lines.add(l.trim());
		}
		r.close();
		// pass 1: declare all structs (so they can reference each other)
		for (String l : lines) {
			String[] p = l.split("\\s+");
			if (p[0].equals("struct"))
				structs.put(p[1], new StructureDataType(cat, p[1], Integer.decode(p[2])));
		}
		for (String k : new ArrayList<>(structs.keySet()))
			structs.put(k, (Structure) dtm.addDataType(structs.get(k), DataTypeConflictHandler.REPLACE_HANDLER));
		// pass 2: fields
		Structure cur = null;
		for (String l : lines) {
			String[] p = l.split("\\s+", 3);
			switch (p[0]) {
			case "struct": cur = structs.get(p[1]); break;
			case "end": cur = null; break;
			case "global": case "func": break;
			default:
				if (cur == null) throw new RuntimeException("field outside struct: " + l);
				int off = Integer.decode(p[0]);
				DataType t = type(p[1]);
				cur.replaceAtOffset(off, t, t.getLength(), p[2], null);
			}
		}
		// pass 3: globals and functions
		Listing listing = currentProgram.getListing();
		for (String l : lines) {
			String[] p = l.split("\\s+", 4);
			if (p[0].equals("global")) {
				Address a = toAddr(Long.decode(p[1]));
				DataType t = type(p[2]);
				listing.clearCodeUnits(a, a.add(t.getLength() - 1), false);
				listing.createData(a, t);
				createLabel(a, p[3], true, SourceType.USER_DEFINED);
			} else if (p[0].equals("func")) {
				String[] q = l.split("\\s+", 4);
				Address a = toAddr(Long.decode(q[1]));
				Function f = getFunctionAt(a);
				if (f == null) f = createFunction(a, q[2]);
				if (f == null) { println("no function at " + a); continue; }
				f.setName(q[2], SourceType.USER_DEFINED);
				if (q.length > 3) {
					try {
						FunctionDefinitionDataType sig = (FunctionDefinitionDataType)
							CParserUtils.parseSignature((ghidra.app.services.DataTypeManagerService) null, currentProgram, q[3] + ";", true);
						if (sig != null)
							new ApplyFunctionSignatureCmd(a, sig, SourceType.USER_DEFINED).applyTo(currentProgram, monitor);
						else
							println("bad prototype for " + q[2] + ": " + q[3]);
					} catch (Exception e) {
						println("prototype error " + q[2] + ": " + e.getMessage());
					}
				}
			}
		}
	}
}
