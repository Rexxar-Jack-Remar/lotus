# PDG: Program Dependence Graph

The **Program Dependence Graph (PDG)** is a fine-grained representation of data and control dependences. It is built on top of the ICFG and is used for slicing, security analyses, and other dependence-aware queries.

The PDG is field-sensitive, context-insensitive, and flow-insensitive, designed for practical inter-procedural program analysis.

## Key Features

- **Data Dependencies**: Def-use chains, read-after-write (RAW), and alias-based memory dependencies
- **Control Dependencies**: Execution order and branch condition dependencies
- **Interprocedural**: Tracks dependencies across function boundaries
- **Field-Sensitive**: Handles structure fields and array elements separately
- **Parameter Trees**: Tree structures for field-sensitive parameter analysis

## Components

- **`ProgramDependencyGraph.cpp`**: Main PDG pass that orchestrates construction of intraprocedural and interprocedural dependencies
- **`DataDependencyGraph.cpp`**: Builds def-use, RAW, and alias-based data dependence edges
- **`ControlDependencyGraph.cpp`**: Computes control dependences between basic blocks and instructions
- **`Graph.cpp`**: Core graph representation with reachability queries
- **`PDGNode.cpp`**: Node representation for program elements (instructions, values, parameters)
- **`Slicing.cpp`**: Program slicing algorithms
- **`ContextSensitiveSlicing.cpp`**: Context-sensitive slicing support

## Usage

```cpp
#include "IR/PDG/Core/ProgramDependencyGraph.h"

// Run PDG passes
legacy::PassManager PM;
PM.add(new DataDependencyGraph());
PM.add(new ControlDependencyGraph());
auto *pdgPass = new ProgramDependencyGraph();
PM.add(pdgPass);
PM.run(module);

// Query the PDG
ProgramGraph *G = pdgPass->getPDG();

Value* src;
Value* dst;
pdg::Node* src_node = G->getNode(*src);
pdg::Node* dst_node = G->getNode(*dst);

if (G->canReach(src_node, dst_node)) {
  // Nodes are connected
}
```

## Alias Analysis Selection

PDG data-dependence construction supports alias analysis selection:

- `-pdg-aa=<type>`: Selects over-approximate AA (default: `andersen`)
- `-pdg-aa-under=<type>`: Optionally enables under-approximate AA (default: `underapprox`)

Supported AA types: `andersen`, `andersen-1cfa`, `andersen-2cfa`, `dyck`, `cfl-anders`, `cfl-steens`, `combined`, `underapprox`

## Available Passes

- `-pdg`: Generate the program dependence graph (inter-procedural)
- `-cdg`: Generate the control dependence graph (intra-procedural)
- `-ddg`: Generate the data dependence graph (intra-procedural)
- `-dot-*`: Visualization passes (dot format)

## See Also

- Headers: `include/IR/PDG/`
- Documentation: `docs/source/ir/pdg.rst`
- Existing README: `lib/IR/PDG/README.md` (detailed usage guide)
