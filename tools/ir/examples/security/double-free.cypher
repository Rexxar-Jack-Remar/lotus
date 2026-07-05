# === Double-Free & Use-After-Free ===
#
# Detects double-free (same pointer freed twice) and use-after-free
# (pointer used after deallocation) using PDG chop and shortest-path.
#
# Two complementary approaches:
#   A. --analysis resource-flow — structural double-free candidates
#   B. --analysis chop/shortest-path — data-flow path from free to use/second-free
#   C. Cypher API scan — manual exploration of free/use neighborhoods
#
# Prerequisites:
#   --build-pdg (default on; data-flow edges needed for chopping)
#   -g recommended for src/src_line/src_file
#
# Reference: CodeQL FlowAfterFree library uses DataFlow::GlobalWithState
# with the freed expression as the flow state. Lotus chop() provides
# equivalent source-sink path existence but without flow-state labels.
# For flow-state precision, compound chop: first chop from alloc→free,
# then chop from that free's post-dominating region→use.

# ---------------------------------------------------------------------------
# A.1 Resource-flow analysis: list all free/delete callsites
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["free", "cfree", "delete", "_ZdlPv", "_ZdaPv",
                    "munmap", "munmap64", "pclose"]
RETURN c.callee AS callee, c.func AS function, c.src AS location, c.llvm AS ir
ORDER BY c.callee
LIMIT 200

# ---------------------------------------------------------------------------
# A.2 Resource-flow analysis: double-free candidate detection
#
# The built-in resource-flow analysis pairs each acquire (malloc/calloc/realloc)
# with its releases (free/cfree). When multiple release candidates exist for the
# same acquire, it's a double-free signal — the same allocation may flow to more
# than one free() call.
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['malloc','calloc','realloc']
#       RETURN c" \
#     --resource-kind heap \
#     --format json
#
# Output: double_release_candidates shows acquire→release pairs where
# multiple free() calls claim to release the same allocation.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# A.3 Resource-flow analysis: lock double-release (similar pattern)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['pthread_mutex_lock','pthread_mutex_trylock']
#       RETURN c" \
#     --resource-kind lock \
#     --format json
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# B.1 Double-free via chop: alloc → free → free path
#
# Chop finds the intersection of forward-slice(alloc) and backward-slice(free).
# To detect double-free, we need two passes:
#
#   Pass 1: find where alloc→free paths exist (shortest path)
#   Pass 2: from that same alloc, find if another free also reachable
#
# One-shot: shortest path from first free() to second free() on same value
#
#   lotus-ir-pdg-query input.bc \
#     --analysis shortest-path \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL {callee:'free'})
#       RETURN c" \
#     --target-query "
#       MATCH (c:INST_FUNCALL {callee:'free'})
#       WHERE c.func IN (MATCH (c2:INST_FUNCALL {callee:'free'})
#                        RETURN c2.func)
#       RETURN c" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# The witness_paths show how a freed pointer reaches another free() call.
# If the path goes through a DATA_DEF_USE chain, the same underlying
# allocation is freed twice — a double-free violation.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# B.2 Use-After-Free via shortest-path: free → use
#
#   lotus-ir-pdg-query input.bc \
#     --analysis shortest-path \
#     --criteria-query "
#       MATCH (f:INST_FUNCALL {callee:'free'})
#       RETURN f" \
#     --target-query "
#       MATCH (u:INST)
#       WHERE u.opcode IN ['load','store','getelementptr','call']
#       AND u.func <> 'free'
#       RETURN u" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# If a path exists from a free() to a subsequent load/store/GEP, the
# program may be dereferencing freed memory. The witness_path shows
# the instruction chain connecting the free to the use.
#
# Note: This overapproximates (any load/store after any free in the
# same value-flow chain). To improve precision, restrict by:
#   - Dominance: only flag uses that are post-dominated by the free
#     (i.e., free dominates the use on all paths)
#   - Alias filtering: only paths where DATA_ALIAS edges connect the
#     freed value to the use operand
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# B.3 Chop: allocation → free path coverage (check if alloc escapes)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis chop \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['malloc','calloc','realloc']
#       RETURN c" \
#     --target-query "
#       MATCH (c:INST_FUNCALL {callee:'free'})
#       RETURN c" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# If no chop path exists from alloc to free, the allocation either
# escapes the function scope (returned/stored in global) or may be
# freed via an indirect path (function pointer, alias set). This is
# a leak candidate in combination with --analysis resource-flow.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# C.1 Manual Cypher: free() argument backward slice (find what gets freed)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL {callee:"free"})<-[:DATA_DEF_USE*1..5]-(n)
RETURN n.label AS kind, n.opcode AS opcode,
       n.func AS function, n.src AS location, n.llvm AS ir
LIMIT 100

# ---------------------------------------------------------------------------
# C.2 Manual Cypher: find all load/store instructions that use freed pointer
#     (first-hop neighbors of free() via DATA_DEF_USE in the forward direction)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL {callee:"free"})-[:DATA_DEF_USE]->(n:INST)
WHERE n.opcode IN ["load", "store", "getelementptr"]
RETURN c.func AS function, c.src AS free_location,
       n.opcode AS use_opcode, n.src AS use_location, n.llvm AS ir
LIMIT 100

# ---------------------------------------------------------------------------
# C.3 Manual Cypher: same-function free + use sites (coarse pairing)
# ---------------------------------------------------------------------------
MATCH (f:INST_FUNCALL {callee:"free"})
MATCH (u:INST)
WHERE u.func = f.func
  AND u.opcode IN ["load","store","getelementptr"]
  AND u.src_line >= f.src_line
RETURN f.func AS function,
       f.src AS free_location,
       u.src AS use_location,
       u.opcode AS use_type
LIMIT 100

# ---------------------------------------------------------------------------
# C.4 Manual Cypher: new/delete pairing within same function
# ---------------------------------------------------------------------------
MATCH (n:INST_FUNCALL)
WHERE n.callee IN ["_Znwm", "_Znam", "new"]
MATCH (d:INST_FUNCALL)
WHERE d.callee IN ["_ZdlPv", "_ZdaPv", "delete"]
  AND d.func = n.func
RETURN n.func AS function,
       n.callee AS alloc_type, n.src AS alloc_location,
       d.callee AS free_type, d.src AS free_location
LIMIT 100
