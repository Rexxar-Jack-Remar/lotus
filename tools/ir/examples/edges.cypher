# Edge-centric queries (shows how to bind relationship variables and use `e.*` properties).

# Direct call edges: callsite -> callee entry
MATCH (c:INST_FUNCALL)-[e:CALL_INV]->(f:FUNC_ENTRY) RETURN e.label AS edge, c.callee AS callee, e.src_func AS caller, e.dst_func AS calleeFunc, e.src_src AS callsite LIMIT 100

# Indirect call edges (callsite -> candidate callee), if available
MATCH (c:INST_FUNCALL {callee:"<indirect>"})-[e:IND_CALL]->(f:FUNC_ENTRY) RETURN e.label AS edge, e.src_func AS caller, e.dst_func AS calleeFunc, e.src_src AS callsite LIMIT 200

# Control dependence edges out of branches
MATCH (b:INST_BR)-[e:CONTROL_DEP]->(n:INST) RETURN e.label AS edge, e.src_func AS func, e.src_src AS brLoc, e.dst_src AS depLoc, n.opcode AS depOpcode LIMIT 200
