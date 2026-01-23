# Code metrics-style queries (use the "Result(N nodes)" count as the metric).
# For full counts, run `pdg-query` with `--limit 0`.

# Total instruction nodes in the whole module
MATCH (i:INST) RETURN i

# Call sites (total count), with a preview of callee + location
MATCH (c:INST_FUNCALL) RETURN c.callee AS callee, c.func AS func, c.src AS src

# Branches (total count), preview locations
MATCH (b:INST_BR) RETURN b.func AS func, b.src AS src, b.llvm AS ir

# Returns (total count), preview locations
MATCH (r:INST_RET) RETURN r.func AS func, r.src AS src, r.llvm AS ir

# Calls to a particular callee (example: malloc)
MATCH (c:INST_FUNCALL {callee:"malloc"}) RETURN c.func AS func, c.src AS src, c.llvm AS ir

# Instructions inside a specific function (example: main)
MATCH (i:INST) WHERE i.func = "main" RETURN i.opcode AS opcode, i.src AS src
