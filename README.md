<p align="center">
  <img src="docs/source/_static/logo.svg" alt="Lotus — LLVM Static Analysis Framework Logo" width="80"/>
</p>

# Lotus: LLVM-based Static Analysis Framework

Lotus is an LLVM-based program analysis & verification framework. It provides a comprehensive set of toolkits for alias analysis, bug detection, dataflow analysis, concurrency analysis, abstract interpretation, and formal verification — usable individually or in combination.

## Features

- **Alias Analysis** — Pointer analysis with flow-sensitive, context-sensitive, and insensitive variants
- **Dataflow Analysis** — IFDS/IDE frameworks, Newtonian program analysis, weighted pushdown systems (WPDS), etc.
- **Intermediate Representations** — ICFG, PDG, SVFG, SSA variants, and MemorySSA
- **Bug Detection** — Null-pointer, memory safety, concurrency bugs, type errors, and more
- **Formal Verification** — CLAM (abstract interpretation), SeaHorn, SMACK, and more
- **Program Optimization** — Interprocedural optimization, partial evaluation, pass ordering, scalar and prefetch optimization

## Publications

If you use Lotus in your research or work, please cite the following:

```bibtex
@misc{lotus2025,
  title = {Lotus: A Versatile and Industrial-Scale Program Analysis Framework},
  author = {ZJU Programming Languages and Automated Reasoning Group},
  year = {2025},
  url = {https://github.com/ZJU-PL/lotus},
  note = {Program analysis framework built on LLVM}
}
```

Papers that use Lotus:

- **SPLASH/ISSTA 2026 Demo**: Phoenix: A Modular and Versatile Framework for C/C++ Pointer Analysis. Peisen Yao, Zinan Gu, and Qingkai Shi. ([lib/Alias](https://github.com/ZJU-PL/lotus/tree/main/lib/Alias))
- **ASE 2026**: SIMD-Accelerated Sparse Bit-Vectors for Pointer Analysis. Zhaoyang Tan, Peisen Yao, and Kui Ren. ([lib/Alias](https://github.com/ZJU-PL/lotus/tree/main/lib/Alias))
- **FM 2026**: EUF-based Solving Dyck-Reachability with Applications to Static Analysis. Yide Du, Zhenbang Chen, Kunlin Liu, Guofeng Zhang, Xudong Wang, Ke Ma, Wei Dong and Ji Wang.
- **CAV 2026**: Sound and Precise Symbolic Automata Model for Stateful Software Systems. Xinlong Wu, Ruiyu Zhou, Peisen Yao, and Qingkai Shi. ([third-party/seal](https://github.com/ZJU-PL/lotus/tree/main/third-party/seal))
- **TOSEM 2026**: Compiler Optimizations-Based SMT Simplifications: An In-Depth Study. Hanyun Jiang, Peisen Yao*, Jiachen Lu, Yongwang Zhao, and Kui Ren.
- **ISSTA 2025**: Program Analysis Combining Generalized Bit-Level and Word-Level Abstractions. Guangsheng Fan, Liqian Chen, Banghu Yin, Wenyu Zhang, Peisen Yao, and Ji Wang. ([third-party/crab](https://github.com/ZJU-PL/lotus/tree/main/third-party/crab))
- **S&P 2024**: Titan: Efficient Multi-target Directed Greybox Fuzzing. Heqing Huang, Peisen Yao, Hung-Chun Chiu, Yiyuan Guo, and Charles Zhang.
- **USENIX Security 2024**: Unleashing the Power of Type-Based Call Graph Construction by Using Regional Pointer Information. Yuandao Cai, Yibo Jin, and Charles Zhang. ([lib/Alias/Specialized/FPA](https://github.com/ZJU-PL/lotus/tree/main/lib/Alias/Specialized/FPA))
- **TSE 2024**: Fast and Precise Static Null Exception Analysis with Synergistic Preprocessing. Yi Sun, Chengpeng Wang, Gang Fan, Qingkai Shi, and Xiangyu Zhang. ([lib/Analysis/NullPointer](https://github.com/ZJU-PL/lotus/tree/main/lib/Analysis/NullPointer))
- **OOPSLA 2022**: Indexing the Extended Dyck-CFL Reachability for Context-Sensitive Program Analysis. Qingkai Shi, Yongchao Wang, Peisen Yao, and Charles Zhang. ([lib/CFL/CSIndex](https://github.com/ZJU-PL/lotus/tree/main/lib/CFL/CSIndex))

## Quick Start

```bash
git clone https://github.com/ZJU-PL/lotus
cd lotus
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

> See [INSTALL.md](INSTALL.md) for detailed build options (CMake toggles, Boost dependencies, custom LLVM path, etc.)

## Documentation

Full documentation: [zju-pl.github.io/lotus](https://zju-pl.github.io/lotus)

- [Major Components Overview](https://zju-pl.github.io/lotus/user_guide/major_components.html) — subsystems, libraries, and tools
- [Tools Reference](https://zju-pl.github.io/lotus/tools/index.html) — command-line tool usage

## Contributors

- rainoftime / cutelimination
- qingkaishi
- rhuab
- Zahrinas
- Rexxar-Jack-Remar
- x6eull
- hiruwaKS
