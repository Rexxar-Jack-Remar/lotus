# Semantic facts for CodeQL migrations. These inspect candidates;
# use --analysis rules for supported checks and explicit coverage.
# Unknown facts are empty. Argument indices start at zero.

MATCH (c:INST_FUNCALL) WHERE c.callee = 'gets' AND c.callee_param_count = 1 RETURN c.func AS function, c.src AS location

# Wide-character calls require target-ABI element sizes.
MATCH (c:INST_FUNCALL) WHERE c.callee IN ['strncpy', 'wcsncpy', 'strxfrm', 'wcsxfrm', 'strlcpy'] RETURN c.callee AS api, c.arg_count AS arguments, c.arg2_int AS count, c.arg0_object_bytes AS destination_bytes, c.src AS location

MATCH (c:INST_FUNCALL) WHERE c.callee = 'strncpy' AND c.arg0_object_bytes IS NULL RETURN c.func AS function, c.src AS location
