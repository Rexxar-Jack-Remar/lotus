# === Unsafe C Library Usage Patterns ===
#
# Detects calls to known-unsafe C library functions and traces data flow
# through them for buffer-overflow and format-string analysis.
#
# Prerequisites:
#   --build-pdg (default on; data-flow edges needed for source tracing)
#   -g recommended for src/src_line/src_file

# ---------------------------------------------------------------------------
# 1. Find all unsafe string copy calls (buffer overflow sinks)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["strcpy", "strcat", "sprintf", "vsprintf", "gets",
                    "scanf", "sscanf", "vfscanf", "vscanf", "vsscanf",
                    "wcscpy", "wcscat", "swprintf",
                    "realpath", "getwd"]
RETURN c.callee AS callee, c.func AS function, c.src AS location, c.llvm AS ir
ORDER BY c.callee
LIMIT 200

# ---------------------------------------------------------------------------
# 2. Count dangerous calls per function (hotspot profiling)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["strcpy","strcat","sprintf","vsprintf","gets","scanf"]
RETURN c.func AS function, COUNT(*) AS danger_count
ORDER BY danger_count DESC
LIMIT 50

# ---------------------------------------------------------------------------
# 3. strcpy/sprintf backward slice — trace where the destination buffer
#    or source string comes from.
#
#   lotus-ir-pdg-query input.bc \
#     --analysis slice-backward \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL {callee:'strcpy'})-[:PARAMETER_IN]->(p:PARAM_ACTUALIN)
#       RETURN p" \
#     --edge-preset value-flow \
#     --max-unbounded-hops 8 \
#     --format json
#
# The output shows what values flow into strcpy's arguments. If the
# destination is a stack buffer and the source comes from user input,
# this is a buffer overflow candidate.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 4. Chop from source to strcpy/gets sink (full taint-like path)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis chop \
#     --criteria-query "
#       MATCH (s:INST_FUNCALL)
#       WHERE s.callee IN ['fgets','read','scanf','getenv','getchar','gets','recv']
#       RETURN s" \
#     --target-query "
#       MATCH (t:INST_FUNCALL)
#       WHERE t.callee IN ['strcpy','strcat','sprintf','gets']
#       RETURN t" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# Witness paths show the exact instruction chain from input to unsafe sink.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 5. Find bounded vs unbounded string ops (safer alternatives exist)
#    — shows strncpy/strncat/snprintf usage alongside unbounded ones
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["strncpy","strncat","snprintf","vsnprintf","strlcpy","strlcat"]
RETURN c.callee AS callee, c.func AS function, c.src AS location, c.llvm AS ir
ORDER BY c.callee
LIMIT 200

# ---------------------------------------------------------------------------
# 6. Format string vulnerability — printf with non-constant format argument
#
#    A non-constant format string to printf/sprintf/fprintf is dangerous
#    (can read/write arbitrary memory). This query finds the call sites;
#    use slice-backward to check if the format arg is a constant.
#
#   lotus-ir-pdg-query input.bc \
#     --analysis slice-backward \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)-[:PARAMETER_IN]->(p:PARAM_ACTUALIN)
#       WHERE c.callee IN ['printf','fprintf','sprintf','snprintf','dprintf','syslog']
#       RETURN p" \
#     --edge-preset value-flow
#
# If the backward slice terminates at a constant (GLOBAL_VAR with constant
# initializer or an INST with opcode that doesn't read input), the format
# is likely safe. If it reaches a function parameter or read() call, it's
# attacker-controlled.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 7. memcpy/memset with potentially oversized arguments
#    — look for memcpy where the size argument is dynamic
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["memcpy", "memmove", "memset", "bcopy"]
RETURN c.callee AS callee, c.func AS function, c.src AS location, c.llvm AS ir
ORDER BY c.callee
LIMIT 200
