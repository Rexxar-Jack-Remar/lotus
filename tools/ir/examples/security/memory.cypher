# === Memory Safety Patterns ===
#
# Detects use-after-free, double-free, memory leaks, and heap misuse.
#
# Prerequisites:
#   --build-pdg (default on)
#   -g recommended for source location info
#
# The tool supports three analysis modes for memory safety:
#   A. Cypher API-scan for allocation/free callsites
#   B. --analysis resource-flow for leak and double-free detection
#   C. --analysis chop / shortest-path for UAF path exploration

# ---------------------------------------------------------------------------
# A.1 Find all heap-allocation and deallocation callsites
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["malloc", "calloc", "realloc", "reallocf", "valloc",
                    "aligned_alloc", "posix_memalign", "memalign",
                    "free", "cfree",
                    "mmap", "munmap", "mremap",
                    "new", "delete"]
RETURN c.callee AS callee, c.func AS function, c.src AS location, c.llvm AS ir
LIMIT 200

# ---------------------------------------------------------------------------
# A.2 Count allocation vs free per function (balance heuristic)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["malloc","calloc","realloc","valloc","aligned_alloc"]
RETURN c.func AS function, COUNT(*) AS alloc_count
ORDER BY alloc_count DESC
LIMIT 50

# ---------------------------------------------------------------------------
# B. Resource-leak detection (malloc without free)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "MATCH (c:INST_FUNCALL) WHERE c.callee IN ['malloc','calloc','realloc'] RETURN c" \
#     --resource-kind heap \
#     --format json
#
# Output fields:
#   acquire_sites:   locations where resources are acquired
#   release_sites:   locations where resources are released
#   orphaned_resources:  acquired but never released (LEAK candidates)
#   double_release_candidates:  released multiple times (DOUBLE-FREE candidates)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# B.2 Double-free candidate via resource-flow analysis
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "MATCH (c:INST_FUNCALL) WHERE c.callee IN ['malloc','calloc','realloc'] RETURN c" \
#     --resource-kind heap
#
# The double_release_candidates field lists call sites where the same
# allocation is freed more than once — a strong double-free signal.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# C.1 Use-After-Free candidate (shortest path from free to subsequent use)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis shortest-path \
#     --criteria-query "MATCH (c:INST_FUNCALL {callee:'free'}) RETURN c" \
#     --target-query "
#       MATCH (u:INST)
#       WHERE u.opcode IN ['load','store','getelementptr','call']
#       AND NOT u.func IN ['free']
#       RETURN u" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# Interpretation: If a path exists from free() to a subsequent load/store/GEP
# operating on the freed pointer, this is a UAF candidate.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# C.2 Chop from allocation to free (path coverage)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis chop \
#     --criteria-query "MATCH (c:INST_FUNCALL) WHERE c.callee IN ['malloc','calloc'] RETURN c" \
#     --target-query "MATCH (c:INST_FUNCALL {callee:'free'}) RETURN c" \
#     --edge-preset value-flow \
#     --context-sensitive \
#     --format json
#
# Result nodes are the intersection of forward-slice(from alloc) and
# backward-slice(to free). The witness paths show how the pointer flows.
# If no chop path exists, the allocation may escape analysis scope.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# C.3 Manual Cypher: data-flow neighborhood around free() argument
#
# This shows what data the free() argument depends on (backward through
# DATA_DEF_USE), helping identify what exactly is being freed.
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL {callee:"free"})<-[:DATA_DEF_USE*1..3]-(n)
RETURN n.label AS kind, n.func AS function, n.src AS location, n.llvm AS ir
LIMIT 100
