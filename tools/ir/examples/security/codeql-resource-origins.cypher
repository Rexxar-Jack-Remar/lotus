# Model-aware allocation/release selection and PDG value-flow exploration.
# Edges require a full PDG. Traversals are candidate explanations; alias and
# data dependencies alone do not prove ownership, CFG order or feasibility.
# For semantic SSA allocation-family checks run:
# --analysis rules --rule cpp/new-free-mismatch
# --rule cpp/new-array-delete-mismatch --rule cpp/new-delete-array-mismatch

MATCH (a:INST_FUNCALL) WHERE a.allocation_kind IS NOT NULL RETURN a.allocation_kind AS family, a.func AS function, a.src AS location, a

MATCH (r:INST_FUNCALL) WHERE r.release_kind IS NOT NULL RETURN r.release_kind AS family, r.func AS function, r.src AS location, r

# Bounded value-flow candidates for allocation/cleanup inspection. Native
# context-sensitive chop supports call/return matching without this hop cap.
MATCH (a:INST_FUNCALL)-[:DATA_DEF_USE|DATA_RET|PARAMETER_IN|PARAMETER_OUT*1..8]->(r:INST_FUNCALL) WHERE a.allocation_kind IS NOT NULL AND r.release_kind IS NOT NULL RETURN a.allocation_kind AS allocation, r.release_kind AS release, a.src AS source, r.src AS sink, a, r

# Incompatible family candidates along the same dependence paths. Use native
# rules to require actual SSA allocation origins and valid release ordering.
MATCH (a:INST_FUNCALL)-[:DATA_DEF_USE|DATA_RET|PARAMETER_IN|PARAMETER_OUT*1..8]->(r:INST_FUNCALL) WHERE (a.allocation_kind = 'malloc' AND r.release_kind IN ['delete','delete[]']) OR (a.allocation_kind IN ['new','new[]'] AND r.release_kind = 'free') OR (a.allocation_kind = 'new[]' AND r.release_kind = 'delete') OR (a.allocation_kind = 'new' AND r.release_kind = 'delete[]') RETURN a.allocation_kind AS allocation, r.release_kind AS release, a.src AS source, r.src AS sink, a, r
