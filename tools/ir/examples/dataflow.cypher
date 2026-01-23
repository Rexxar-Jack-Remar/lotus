# Dataflow / slicing-style queries (requires `--build-pdg`, enabled by default).

# Global-to-use slice (globals are defs; follow def-use edges to instructions)
MATCH (g:VAR)-[:DATA_DEP*1..2]->(u:INST) RETURN g.label AS gkind, u.func AS func, u.src AS src, u.llvm AS ir LIMIT 100

# Forward slice from a store (def-use) up to 3 hops
MATCH (s:INST {opcode:"store"})-[:DATA_DEP*1..3]->(u:INST) RETURN u.func AS func, u.src AS src, u.llvm AS ir LIMIT 200

# "Neighborhood" around a sink call using bidirectional data-deps (coarse backward+forward slice)
MATCH (sink:INST_FUNCALL {callee:"system"})<-[:DATA_DEP*1..4]->(n:INST) RETURN n.func AS func, n.src AS src, n.llvm AS ir LIMIT 300

# Read-after-write dependencies (memory deps) around loads
MATCH (l:INST {opcode:"load"})<-[:DATA_RAW*1..2]->(w:INST) RETURN w.func AS func, w.src AS src, w.llvm AS ir LIMIT 200
