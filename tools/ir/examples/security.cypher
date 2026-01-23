# Security-flavored patterns (API-centric triage).

# Shell execution sinks (adjust the callee list as needed)
MATCH (c:INST_FUNCALL) WHERE c.callee = "system" OR c.callee = "popen" RETURN c.func AS func, c.src AS src, c.llvm AS ir LIMIT 200

# Common unsafe libc usage (examples)
MATCH (c:INST_FUNCALL) WHERE c.callee CONTAINS "strcpy" OR c.callee = "gets" RETURN c.func AS func, c.src AS src, c.llvm AS ir LIMIT 200

# malloc/free mismatches are not modeled, but callsite locations are useful for manual inspection
MATCH (c:INST_FUNCALL) WHERE c.callee = "malloc" OR c.callee = "free" RETURN c.callee AS callee, c.func AS func, c.src AS src LIMIT 200
