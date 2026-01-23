# Debug-info-driven queries (works best when the bitcode includes DWARF/debug metadata).

# Show only instructions with a source location
MATCH (i:INST) WHERE i.src IS NOT NULL RETURN i.func AS func, i.src AS src, i.opcode AS op, i.llvm AS ir LIMIT 100

# Restrict to a source file suffix
MATCH (i:INST) WHERE i.src_file ENDS WITH ".c" RETURN i.func AS func, i.src AS src, i.llvm AS ir LIMIT 100

# Find instruction nodes with a particular debug type name fragment
MATCH (i:INST) WHERE i.di_type CONTAINS "struct" RETURN i.func AS func, i.src AS src, i.di_type AS type LIMIT 100
