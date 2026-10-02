# Object/subobject capacity and access widths, shared with BoundsQuery.
# Run: lotus-ir-pdg-query input.bc -f tools/ir/examples/security/codeql-memory-access.cypher
# Unknown sizes/offsets stay unknown. False means no proven bad access among
# known accesses; it does not prove an instruction's unknown accesses safe.
# Numeric access_* facts are available when exactly one access is known.
# Native --analysis rules --cwe 119,125,131,787 also handles symbolic allocation
# ends and missing terminator space, which these fixed-capacity predicates omit.

MATCH (n:INST) WHERE n.write_out_of_bounds = true RETURN n.opcode AS operation, n.access_bytes AS bytes, n.access_capacity_bytes AS subobject_bytes, n.access_offset_bytes AS offset, n.src AS location, n

MATCH (n:INST) WHERE n.read_out_of_bounds = true RETURN n.opcode AS operation, n.access_bytes AS bytes, n.access_capacity_bytes AS subobject_bytes, n.access_offset_bytes AS offset, n.src AS location, n

# Known negative offsets are retained, rather than wrapped to an unsigned size.
MATCH (n:INST) WHERE n.access_offset_bytes < 0 AND n.access_bytes > 0 RETURN n.access_offset_bytes AS offset, n.access_bytes AS bytes, n.src AS location, n
