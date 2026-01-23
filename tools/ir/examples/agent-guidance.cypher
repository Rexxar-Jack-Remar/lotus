# Queries that can guide a coding agent toward relevant code, dataflow, and context.

# "Where are the sensitive calls?" (example: system)
MATCH (s:INST_FUNCALL {callee:"system"}) RETURN s.func AS func, s.src AS src, s.llvm AS ir LIMIT 50

# Immediate def-use neighbors of a sensitive call (1-hop data-dependence out-edges)
MATCH (s:INST_FUNCALL {callee:"system"})-[:DATA_DEP]->(n) RETURN n.label AS kind, n.func AS func, n.src AS src, n.llvm AS ir LIMIT 200

# Immediate control-dependence neighborhood of a branch (often highlights guards)
MATCH (b:INST_BR)-[:CONTROL_DEP]->(n) RETURN n.label AS kind, n.func AS func, n.src AS src, n.llvm AS ir LIMIT 200

# Parameter-flow neighborhood from a call site (useful when debugging argument/return plumbing)
MATCH (c:INST_FUNCALL)-[:PARAM_OUT]->(p:PARAM) RETURN p.label AS kind, p.func AS func LIMIT 200

