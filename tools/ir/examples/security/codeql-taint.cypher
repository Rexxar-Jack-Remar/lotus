# Model-aware roles for the PDG-native CodeQL taint rules. Each query is one
# line for -f. These facts select sources and sinks; they do not prove a flow.
# Run the semantic checks using --analysis rules, for example:
# --rule cpp/uncontrolled-process-operation --rule cpp/command-line-injection
# --rule cpp/sql-injection --rule cpp/path-injection
# --rule cpp/tainted-format-string --rule cpp/non-constant-format
# --rule cpp/uncontrolled-allocation-size
# The JSON taint_origins contain source, sink argument and concatenation site.
# PDG uses CFG ordering, matched calls/returns and modeled content transfers.
# Command injection additionally requires dangerous concatenation, while SQL
# escaping is a barrier only for SQL. Indirect/C++ object flows remain partial.

MATCH (s:INST_FUNCALL) WHERE s.taint_source_kind IS NOT NULL RETURN s.callee AS api, s.taint_source_outputs AS outputs, s.src AS location, s

MATCH (c:INST_FUNCALL) WHERE c.taint_process_args IS NOT NULL RETURN c.callee AS api, c.taint_process_args AS arguments, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.taint_command_args IS NOT NULL RETURN c.callee AS api, c.taint_command_args AS arguments, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.taint_sql_args IS NOT NULL RETURN c.callee AS api, c.taint_sql_args AS arguments, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.taint_path_args IS NOT NULL RETURN c.callee AS api, c.taint_path_args AS arguments, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.taint_format_args IS NOT NULL RETURN c.callee AS api, c.taint_format_args AS arguments, c.src AS location, c

# Allocation-size roles are scalar inputs, kept distinct from string contents.
# Native rules apply dominating guards and range-reducing expression checks.
MATCH (c:INST_FUNCALL) WHERE c.taint_allocation_args IS NOT NULL RETURN c.callee AS api, c.taint_allocation_args AS arguments, c.src AS location, c

# Restricting --criteria-query with these selections narrows reporting sites.
# PDG still analyzes the whole module so interprocedural sources are retained.
