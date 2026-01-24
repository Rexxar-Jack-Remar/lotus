# New/extended PDG Cypher primitives
# Run with: ./build/bin/pdg-query -f tools/ir/examples/primitives.cypher <input.bc>

# 1) Directional traversal
# Outgoing-only def-use slice (forward)
MATCH (s:INST {opcode:"store"})-[:DATA_DEP*1..3]->(u:INST) RETURN u.func AS func, u.src AS src, u.llvm AS ir LIMIT 50

# Incoming-only def-use slice (backward)
MATCH (sink:INST_FUNCALL {callee:"system"})<-[:DATA_DEP*1..4]-(n:INST) RETURN n.func AS func, n.src AS src, n.llvm AS ir LIMIT 50

# Undirected/bidirectional neighborhood (both directions)
MATCH (sink:INST_FUNCALL {callee:"system"})<-[:DATA_DEP*1..2]->(n:INST) RETURN n.func AS func, n.src AS src, n.llvm AS ir LIMIT 50

# 2) Multi-edge-type traversal via "TYPE1|TYPE2"
MATCH (i:INST_FUNCALL)-[:DATA_DEP|CONTROL_DEP*1..2]->(n:INST) RETURN n.func AS func, n.src AS src, n.llvm AS ir LIMIT 50

# 3) List membership in WHERE
MATCH (i:INST) WHERE i.opcode IN ["load", "store"] RETURN i.func AS func, i.opcode AS opcode, i.src AS src LIMIT 50

# 4) Aggregations (COUNT)
MATCH (i:INST) RETURN COUNT(i) AS inst_count
MATCH (c:INST_FUNCALL) RETURN COUNT(DISTINCT c.callee) AS distinct_callees

# 5) Query parameters (pass via --param callee=malloc)
MATCH (c:INST_FUNCALL {callee:$callee}) RETURN c.func AS func, c.src AS src, c.llvm AS ir LIMIT 20
