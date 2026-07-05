# === Taint Tracking & Data-Flow Integrity ===
#
# Traces data flow from untrusted input sources to security-sensitive
# sinks using PDG chop, backward slicing, and Cypher API scans.
#
# Prerequisites:
#   --build-pdg (default on; data-flow edges are essential)
#   -g recommended for src/src_line/src_file
#
# The tool supports four modes for taint analysis:
#   A. Cypher API scan — discover input sources and sensitive sinks
#   B. --analysis chop — source-to-sink path existence
#   C. --analysis slice-backward — trace individual sink argument origins
#   D. Manual Cypher — neighborhood exploration via DATA_DEF_USE chains
#
# Reference: CodeQL TaintTracking::Configuration uses source/sink/barrier
# predicates. Lotus chop() with edge-preset value-flow provides the
# equivalent source-sink reachability. For sanitizer/barrier support,
# pre-filter criteria or use compound queries (chop then filter by
# barrier predicates on witness_path nodes).

# ---------------------------------------------------------------------------
# A.1 Input source API scan — untrusted data entry points
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["fgets", "fgetc", "fread", "read", "pread",
                    "scanf", "fscanf", "sscanf", "vscanf", "vfscanf",
                    "getchar", "getc", "gets",
                    "getenv", "getenv_s",
                    "recv", "recvfrom", "recvmsg",
                    "readlink", "readlinkat",
                    "argv", "getopt", "getopt_long",
                    "stdin"]
RETURN c.callee AS source, c.func AS function, c.src AS location, c.llvm AS ir
ORDER BY c.callee
LIMIT 200

# ---------------------------------------------------------------------------
# A.2 Dangerous sink API scan — execution, write, format sinks
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["system", "popen", "execve", "execvp", "execle",
                    "execlp", "execl", "execv", "execvpe", "fexecve",
                    "dlopen", "LoadLibrary",
                    "strcpy", "strcat", "sprintf", "vsprintf",
                    "gets", "scanf", "fscanf", "sscanf",
                    "printf", "fprintf", "snprintf", "dprintf", "syslog",
                    "memcpy", "memmove", "memset",
                    "write", "fwrite", "send", "sendto",
                    "chmod", "chown", "unlink", "rename",
                    "mysql_query", "PQexec", "system"]
RETURN c.callee AS sink, c.func AS function, c.src AS location, c.llvm AS ir
ORDER BY c.callee
LIMIT 200

# ---------------------------------------------------------------------------
# A.3 Source count per function (hotspot identification)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["fgets","read","scanf","getenv","recv","gets"]
RETURN c.func AS function, COUNT(*) AS source_count
ORDER BY source_count DESC
LIMIT 50

# ---------------------------------------------------------------------------
# A.4 Sink count per function
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["system","popen","execve","strcpy","sprintf","gets"]
RETURN c.func AS function, COUNT(*) AS sink_count
ORDER BY sink_count DESC
LIMIT 50

# ---------------------------------------------------------------------------
# B.1 Taint tracking via chop: input source → dangerous sink
#
# The chop analysis computes forward-slice(source) ∩ backward-slice(sink)
# to find the exact instruction set connecting them. Witness paths show
# the chain.
#
#   lotus-ir-pdg-query input.bc \
#     --analysis chop \
#     --criteria-query "
#       MATCH (s:INST_FUNCALL)
#       WHERE s.callee IN ['fgets','read','scanf','getenv','recv','gets']
#       RETURN s" \
#     --target-query "
#       MATCH (t:INST_FUNCALL)
#       WHERE t.callee IN ['system','popen','execve','strcpy','sprintf']
#       RETURN t" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# Output witness_paths: instruction chain from source to sink.
# If no path exists, the sink is NOT reachable from the input sources
# in the PDG — either the data is sanitized (filtered through a check),
# or the connection is indirect (through memory/alias not captured by
# value-flow edges).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# B.2 Zero-hop taint: source directly assigned to sink (trivial taint)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis shortest-path \
#     --criteria-query "
#       MATCH (s:INST_FUNCALL)
#       WHERE s.callee IN ['getenv','getchar','gets','argv']
#       RETURN s" \
#     --target-query "
#       MATCH (t:INST_FUNCALL {callee:'system'})
#       RETURN t" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# C.1 Backward slice from a specific sink argument
#
# Trace where the argument to system()/strcpy() comes from:
#
#   lotus-ir-pdg-query input.bc \
#     --analysis slice-backward \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL {callee:'system'})-[:PARAMETER_IN]->(p:PARAM_ACTUALIN)
#       RETURN p" \
#     --edge-preset value-flow \
#     --max-unbounded-hops 10 \
#     --format json
#
# The backward slice shows all instructions that contribute to the
# system() argument. If the slice includes fgets/read/getenv at its
# leaves, the argument is attacker-controlled (true positive).
# If the slice terminates at constants or local string literals,
# the call is safe.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# C.2 Format string: backward slice from printf format argument
#
#   lotus-ir-pdg-query input.bc \
#     --analysis slice-backward \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)-[:PARAMETER_IN*2]->(p:PARAM_ACTUALIN)
#       WHERE c.callee IN ['printf','fprintf','sprintf','snprintf',
#                           'dprintf','syslog']
#       RETURN p" \
#     --edge-preset value-flow \
#     --max-unbounded-hops 8 \
#     --format json
#
# The first PARAMETER_IN goes from call to the actual-in parameter,
# the second goes from actual-in to the value node. If the leaf of
# the backward slice is a constant (GLOBAL_VAR with constant data or
# an ALLOCA with a fixed string), the format is safe. If it reaches
# a read() or parameter, the format is user-controlled.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# C.3 Backward slice from execve/sprintf buffer argument (arg index 0)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis slice-backward \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL {callee:'execve'})-[:PARAMETER_IN]->(p:PARAM_ACTUALIN)
#       RETURN p" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# D.1 Manual Cypher: DATA_DEF_USE chain from input sources
#
# Trace what an input source's return value reaches:
# ---------------------------------------------------------------------------
MATCH (s:INST_FUNCALL {callee:"getenv"})-[:DATA_DEF_USE*1..5]->(n:INST)
RETURN n.opcode AS opcode, n.func AS function,
       n.src AS location, n.llvm AS ir
LIMIT 200

# ---------------------------------------------------------------------------
# D.2 Manual Cypher: find PARAMETER_IN edges from calls to actual arguments
#     (shows argument plumbling for a specific sink)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL {callee:"system"})-[:PARAMETER_IN]->(p:PARAM_ACTUALIN)
RETURN p.label AS param_role, p.func AS function,
       p.src AS location, p.llvm AS ir
LIMIT 50

# ---------------------------------------------------------------------------
# D.3 Manual Cypher: find call sites in the same function as input + sink
#     (potential taint on same path)
# ---------------------------------------------------------------------------
MATCH (src:INST_FUNCALL)
WHERE src.callee IN ["fgets","read","scanf","getenv"]
MATCH (snk:INST_FUNCALL)
WHERE snk.callee IN ["system","popen","execve","strcpy","sprintf"]
  AND snk.func = src.func
RETURN src.func AS function,
       src.callee AS input_source, src.src AS input_location,
       snk.callee AS sink, snk.src AS sink_location
LIMIT 100
