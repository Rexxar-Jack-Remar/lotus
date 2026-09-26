# CclyzerAA

Optional wrapper around [cclyzer++](https://github.com/GaloisInc/cclyzerpp) Datalog-based pointer analysis for LLVM IR. **Not** integrated into `AliasAnalysisWrapper`; use the `lotus::cclyzer::CclyzerAA` API directly when this backend is enabled.

## Enabling

1. Set **LOTUS_ENABLE_CCLYZER=ON** (defaults to using the in-tree `third-party/cclyzerpp`):
   ```bash
   cmake -DLOTUS_ENABLE_CCLYZER=ON ...
   ```
2. Install **Soufflé** (compiler and headers). cclyzer++ compiles its Datalog to C++ at build time and links it in.

## Usage

```cpp
#include "Alias/InclusionBased/CclyzerAA/CclyzerAA.h"

lotus::cclyzer::CclyzerAA aa;
if (aa.run(M)) {
  auto result = aa.alias(v1, v2);
  std::vector<const llvm::Value*> pts;
  aa.getPointsToSet(ptr, pts);
}
```

Analysis options (subset vs unification, context sensitivity) currently follow cclyzer++ defaults or process command-line flags; they are not yet configurable from this API.
