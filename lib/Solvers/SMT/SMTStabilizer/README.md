# SMTStabilizer

SMTStabilizer normalizes SMT-LIB2 inputs to reduce runtime variance caused by
syntactic mutations such as assertion reordering, symbol renaming, and
commutative operand reordering.

This copy is integrated into Lotus and has been adapted to compile as C++17.
The original project is available at
<https://github.com/shaowei-cai-group/SMTStabilizer>.

## Lotus integration

The component is disabled by default because it requires GMP, GMPXX, and MPFR.
Enable it when configuring Lotus:

```bash
cmake -S . -B build -DLOTUS_ENABLE_SMT_STABILIZER=ON
cmake --build build --target LotusSMTStabilizer lotus-smt-stabilizer
```

If the dependencies are installed in non-standard prefixes, set `GMP_ROOT`
and `MPFR_ROOT` in the environment or CMake cache.

The integration provides:

- `LotusSMTStabilizer`, a static library;
- `Lotus::SMTStabilizer`, a CMake alias target;
- `lotus-smt-stabilizer`, a command-line frontend;
- optional API tests in the Lotus `solver_tests` target.

Public API headers are under
`include/Solvers/SMT/SMTStabilizer/api/`. Implementation files live under
`lib/Solvers/SMT/SMTStabilizer/`.

## C++ API

```cpp
#include "Solvers/SMT/SMTStabilizer/api/stabilizer_api.h"

stabilizer::api::SMTStabilizerOptions options;
options.set_rewrite(true);
options.set_context_propagation(true);
options.set_subgraph_pruning(true);

stabilizer::api::SMTStabilizer stabilizer(options);
std::string normalized = stabilizer.apply_file("input.smt2");
```

## C API

Include `Solvers/SMT/SMTStabilizer/api/stabilizer_c_api.h`. Strings returned
by `stabilizer_apply_file` and `stabilizer_apply_text` must be released with
`stabilizer_free_string`.

## License and attribution

SMTStabilizer is distributed under the MIT License; see [LICENSE](LICENSE).
The parser is based on the SOMTParser project and was modified by the
SMTStabilizer authors.
