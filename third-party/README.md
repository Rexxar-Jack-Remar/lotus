# Third-party libraries

Vendored or in-tree copies of external dependencies used by this project.

| Library | Directory | Description | Upstream |
|--------|-----------|-------------|----------|
| **CUDD** | `CUDD/` | CU Decision Diagram package — BDD/ADD/ZDD manipulation | [ivmai/cudd](https://github.com/ivmai/cudd) (mirror); original by Fabio Somenzi, University of Colorado |
| **WPDS** | `WPDS/` | Weighted pushdown system library (WALi-style) for interprocedural dataflow | Wisconsin/GrammaTech WALi lineage; see e.g. [WALi-OpenNWA](https://github.com/WaliDev/WALi-OpenNWA) |
| **WALi/OpenNWA** | `WALi-OpenNWA/` | Full WALi weighted automata library and OpenNWA nested-word automata implementation | [WaliDev/WALi-OpenNWA](https://github.com/WaliDev/WALi-OpenNWA) |
| **spdlog** | `spdlog/` | Fast C++ logging library (header-only) | [gabime/spdlog](https://github.com/gabime/spdlog) |
| **CRAB** | `crab/` | Abstract interpretation library used by `lib/Verification/clam` | [seahorn/crab](https://github.com/seahorn/crab) |

## Usage

- **Include path**: The project adds `third-party/` to the global include path. Use `#include <spdlog/spdlog.h>`, `#include "CUDD/cudd.h"`, and `#include "WPDS/..."` as in the rest of the codebase.
- **CMake**: CUDD and WPDS are built via `third-party/CMakeLists.txt`; link targets `CanaryCUDD`, `wpds`, `wpds++` (and interfaces `ewpds`, `wpdsplusplus_util`) as needed. WALi/OpenNWA is opt-in with `-DENABLE_WALI_OPENNWA=ON` and exposes `WALi::wali`. spdlog is header-only; optional target `spdlog::spdlog` exposes include directories. CRAB is configured from `cmake/ConfigureClamCrab.cmake` and is discovered from `third-party/crab/` by default.

## Updating

When updating a vendored copy, preserve the layout and include paths above so existing `#include` directives and CMake targets continue to work.
