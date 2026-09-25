
# Boolean-program frontend

`BooleanProgramParser.cpp` preserves the public `parseBooleanProgram` AST API
used by Lotus's NPA lowering and existing callers. `bp_parser.yy` and
`bp_scanner.ll` now contain the OOPSLA 23 Boolean-program grammar in place of
the old inactive skeleton. They build the `lotus-bool-parse` tool, whose flat
output is accepted by `lotus-bool-normalize`. Its entry point and the
normalizer source are under `tools/verifier/boolean-program/`. The normalizer
emits the `prep_output/BP` format documented
in `BooleanProgramFormat.txt`, plus PACE graph inputs.

Configure with `-DLOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS=ON` to build the two tools.
The corpus preparation workflow is in `scripts/prepare_demand_apa_boolean.py`.
The grammar and normalizer were imported from the OOPSLA-586 artifact's `step2`;
the token and CLI corrections are Lotus integration changes. All 54 bundled
Boolean programs were checked against the artifact's precomputed normalized
outputs byte for byte.

The BDD semantics and on-demand query engine live separately in
`include/Dataflow/DemandAPA/`, with its entry point under
`tools/dataflow/DemandAPA/`.
