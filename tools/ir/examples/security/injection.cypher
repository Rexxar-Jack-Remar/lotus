# === Command Injection Detection ===
#
# Detects shell-execution sinks and traces data flow from input-like sources.
#
# Prerequisites:
#   --build-pdg (default on; required for data-flow edges)
#   Input bitcode should be compiled with -g for src/src_line/src_file
#
# Two approaches:
#   1. Cypher-only: discover shell-execution callsites
#   2. C++ analysis: chop from input-source to shell-exec sink

# ---------------------------------------------------------------------------
# 1. Find all shell execution callsites (quick API scan)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["system", "popen", "pclose", "execve", "execvp",
                    "execle", "execlp", "execl", "exec", "execv",
                    "execvpe", "fexecve"]
RETURN c.func AS function, c.src AS location, c.callee AS callee, c.llvm AS ir
LIMIT 100

# ---------------------------------------------------------------------------
# 2. Count injection sinks per function (prioritization)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["system", "popen", "execve", "execvp", "execle"]
RETURN c.func AS function, COUNT(DISTINCT c.callee) AS sink_count, COLLECT(c.callee) AS sinks
ORDER BY sink_count DESC
LIMIT 50

# ---------------------------------------------------------------------------
# 3. Data flow from input sources to shell execution (chop analysis)
#
# Use via --analysis chop with edge-preset value-flow:
#
#   lotus-ir-pdg-query input.bc \
#     --analysis chop \
#     --criteria-query "MATCH (s:INST_FUNCALL) WHERE s.callee IN ['fgets','read','scanf','getenv','getchar','gets','recv','readlink'] RETURN s" \
#     --target-query "MATCH (t:INST_FUNCALL) WHERE t.callee IN ['system','popen','execve','execvp'] RETURN t" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# Output: witness paths showing data flow from source to sink.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 4. Backward slice from shell-execution argument (neighborhood exploration)
#
# Trace where the argument to system()/exec() comes from:
#
#   lotus-ir-pdg-query input.bc \
#     --analysis slice-backward \
#     --criteria-query "MATCH (c:INST_FUNCALL {callee:'system'})-[:PARAMETER_IN]->(p:PARAM_ACTUALIN) RETURN p" \
#     --edge-preset value-flow \
#     --max-unbounded-hops 10 \
#     --format json
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 5. Find all indirect calls (potential injection through function pointers)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL {callee:"<indirect>"})
RETURN c.func AS function, c.src AS location, c.llvm AS ir
LIMIT 200
