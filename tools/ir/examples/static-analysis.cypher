# Basic static-analysis-style queries.

# Memory writes (store instructions), with location + IR
MATCH (i:INST) WHERE i.opcode = "store" RETURN i.func AS func, i.src AS src, i.llvm AS ir LIMIT 100

# Direct calls to a sink API (example: strcpy)
MATCH (c:INST_FUNCALL) WHERE c.callee = "strcpy" RETURN c.func AS func, c.src AS src, c.llvm AS ir LIMIT 100

# Indirect call sites (callee == "<indirect>") can be worth manual inspection
MATCH (c:INST_FUNCALL {callee:"<indirect>"}) RETURN c.func AS func, c.src AS src, c.llvm AS ir LIMIT 100

# Find all instructions at a specific source line in a specific function
MATCH (i:INST) WHERE i.func = "main" AND i.src_line = 42 RETURN i.src AS src, i.llvm AS ir LIMIT 50

