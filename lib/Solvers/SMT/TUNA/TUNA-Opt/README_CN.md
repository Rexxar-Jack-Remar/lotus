# TUNA-Opt

TUNA-Opt 是一个面向 SMT 约束求解的自动优化框架，主要包含两个模块：

- **SMT2LLVM**：SMT-LIB 2 与 LLVM IR 之间的双向转换工具套件
- **GA**：基于遗传算法的 LLVM Pass 组合自动搜索优化器

核心思路：将 SMT 公式转换为 LLVM IR，利用 LLVM 优化 Pass 化简，再转换回 SMT-LIB 2，从而降低 SMT 求解时间。

## 目录结构

```
TUNA-Opt/
├── SMT2LLVM/                # SMT ↔ LLVM IR 转换工具套件
│   ├── include/
│   ├── src/
│   │   └── tools/           # slot / fastslot / smt2llvm / llvm2smt / llvm2feat
│   ├── passes-run.txt        # 41 个有意义 Pass
│   ├── passes-filter.txt     # 25 个优化子集
│   └── FunctionPasses.md
└── GA/                      # 遗传算法优化器
    ├── config.yaml
    ├── ga_optimizer.py
    ├── delta_debugger.py
    ├── phrase_time_counter.py
    ├── mlopt/
    │   ├── ga_opt.py
    │   └── params.py
    └── utils/               # 数据预处理脚本
```

---

## SMT2LLVM

基于 [SLOT](https://github.com/TUNA-SMT/SLOT) 扩展开发的双向转换工具套件，编译后生成五个可执行文件：

| 可执行文件  | 功能简述                          |
|-------------|-----------------------------------|
| `slot`      | 完整流程：SMT → LLVM 优化 → SMT   |
| `fastslot`  | 快速流程：支持从 Pass 文件配置优化  |
| `smt2llvm`  | 单步转换：SMT → LLVM IR           |
| `llvm2smt`  | 单步转换：LLVM IR → SMT           |
| `llvm2feat` | 特征提取：从 LLVM IR 提取程序特征  |

### 安装

**系统依赖**

```bash
sudo apt install -y git gcc g++ cmake ninja-build python3 \
                    zlib1g-dev libtinfo-dev libxml2-dev
```

**编译 LLVM 16.0.0**

```bash
git clone git@github.com:llvm/llvm-project.git
cd llvm-project
git checkout llvmorg-16.0.0
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -j 80 -C build
```

**编译 Z3 4.12.1**

```bash
git clone git@github.com:Z3Prover/z3.git
cd z3
git checkout z3-4.12.1
python3 scripts/mk_make.py
cd build
make -j 80 && sudo make install
```

**构建本项目**

1. 修改 `SMT2LLVM/CMakeLists.txt` 中的依赖路径：

   ```cmake
   set(LLVM_PATH "/path/to/llvm-project")
   set(Z3_PATH   "/path/to/z3")
   ```

2. 编译：

   ```bash
   cd SMT2LLVM
   mkdir build && cd build
   cmake ..
   make -j 32
   ```

### 工具使用

**`slot` — 完整 SMT 优化流程**

```
用法: slot [选项]

必需参数:
  -s <file>          输入 SMT-LIB 2 文件

可选参数:
  -o <file>          输出优化后的 SMT-LIB 2 文件（默认 stdout）
  -lu <file>         输出优化前的 LLVM IR
  -lo <file>         输出优化后的 LLVM IR
  -t <file>          输出计时统计（CSV 格式）
  -m                 将常量右移操作转换为乘法
  -p <file>          从文件读取 Pass 列表
  -h                 显示帮助信息
```

```bash
./slot -m \
  -s problem.smt2 \
  -lu before.ll -lo after.ll \
  -o result.smt2 \
  -p ../passes-run.txt
```

**`fastslot` — 快速 SLOT 流程**

与 `slot` 功能类似，只支持通过 `-p` Pass 文件指定优化管道，适合脚本批量处理。

```bash
./fastslot -m -s input.smt2 -o result.smt2 -p ../passes-run.txt
```

**`smt2llvm` / `llvm2smt` — 单步转换**

```bash
./smt2llvm -s input.smt2 -lu output.ll
./llvm2smt -lo optimized.ll -o result.smt2
```

**`llvm2feat` — 特征提取**

从 LLVM IR 中提取 58 维程序特征（指令计数、控制流、常量统计等）。

```bash
./smt2llvm -s input.smt2 -lu input.ll
./llvm2feat -lo input.ll -f ./features/
```

### 典型使用场景

**端到端 SMT 化简**

```bash
./slot -m -s problem.smt2 -o simplified.smt2 -p ../passes-run.txt
```

**分步调试，查看中间 IR**

```bash
./smt2llvm -s problem.smt2 -lu before.ll
opt -passes="instcombine,gvn" -S before.ll -o after.ll
./llvm2smt -lo after.ll -o simplified.smt2
```

**批量提取特征**

```bash
for f in corpus/*.smt2; do
  base=$(basename "$f" .smt2)
  ./smt2llvm -s "$f" -lu /tmp/${base}.ll
  ./llvm2feat -lo /tmp/${base}.ll -f features/${base}/
done
```

### Pass 配置文件

| 文件                   | Pass 数量 | 说明                                    |
|------------------------|-----------|-----------------------------------------|
| `passes-slot-old.txt`  | 9         | 旧版 SLOT 使用的核心 Pass               |
| `passes-run.txt`       | 41        | 当前支持的全部有意义 Pass               |
| `passes-filter.txt`    | 25        | 对优化有明确意义的 Pass 子集            |
| `passes-useful.txt`    | 33        | 优化相关 Pass（含循环/向量化等）         |
| `passes-16.txt`        | 80        | LLVM 16 可用的基础 Pass 列表            |
| `passes-all-llvm16.txt`| 353       | LLVM 16 全量 Pass 列表（含实验性 Pass） |

---

## GA 优化器

使用遗传算法为 SMT 约束自动搜索最优 LLVM Pass 组合，以降低求解时间。

完整优化链路：

```
SMT → smt2llvm → LLVM IR → opt (pass 组合) → LLVM IR → llvm2smt → SMT → 求解器
```

GA 在 Pass 开关组合空间中搜索，以总耗时作为适应度函数，找到使求解时间最小的 Pass 集合。

### 前置配置（`GA/config.yaml`）

| 字段 | 说明 | 示例 |
|------|------|------|
| `slot_dir` | SLOT 二进制目录，需含 `fastslot`、`slot`、`smt2llvm`、`llvm2smt` | `/path/to/build/` |
| `llvm_dir` | LLVM 二进制目录，需含 `opt` | `/path/to/llvm-project/build/bin` |
| `dataset_dir` | SMTLIB 数据集根目录（`.smt2` 文件所在目录） | `/data/QF_BV` |
| `smt_solver_path` | SMT 求解器路径，支持 Z3 / CVC5 / Boolector | `/usr/bin/z3` |
| `output_csv_path` | GA 结果 CSV 输出路径 | `/path/to/RQ1.csv` |
| `smac_log_output_directory` | SMAC 日志目录 | `/path/to/SMAC` |

### 运行流程

**第一步：数据预处理**

详见 [GA/utils/README.md](GA/utils/README.md)，产出各求解器按求解时间分段的 SMT 文件列表。

主要步骤：

1. `slot_validate.py`：筛选可翻译的 SMT 约束（`fastslot` 20s 内完成翻译）
2. `z3_validate.py`：按求解时间分段（1s / 30s / 60s / 120s / 300s / 600s）
3. `find_under_1s.py`：提取求解时间小于 1s 的文件列表

```bash
python -m utils.slot_validate
python -m utils.z3_validate
python -m utils.find_under_1s
```

**第二步：运行 GA 优化**

```bash
cd GA
python -m ga_optimizer
```

对 `fast_smt_path` 中的每个 SMT 文件，运行 GA（32 次迭代，种群大小 32，40 进程并行），将优于原始求解时间的 Pass 组合写入 `output_csv_path`。

输出 CSV 格式：`file_relative_path, cost_time, <pass1>, <pass2>, ..., status`

**第三步：剪枝冗余 Pass（可选）**

```bash
python -m delta_debugger
```

对 GA 找到的最优 Pass 组合，通过迭代删除法找到产生相同 LLVM IR 输出的最小 Pass 子集，结果写入 `Refine_<原文件名>.csv`。

**第四步：统计各阶段耗时（可选）**

```bash
python -m phrase_time_counter
```

对比以下四种策略的总耗时：

| 策略 | 说明 |
|------|------|
| `default_solve_time` | 直接用求解器求解原始 SMT |
| `slot_solve_time` | SLOT 默认 Pass 优化后求解 |
| `ga_best_solve_time` | GA 找到的最优 Pass 组合 |
| `ga_worst_solve_time` | GA 找到的最差 Pass 组合 |
| `o3_solve_time` | `-O3` 优化后求解 |

### 模块说明

**`mlopt/ga_opt.py`**：GA 算法核心，种群大小 32，精英保留比例 25%。每轮迭代执行 `evaluate`（计算适应度）→ `repopulate`（选择 + 交叉 + 变异）。

**`mlopt/params.py`**：每个 LLVM Pass 编码为布尔参数，支持 `mutate()`（0.5 概率随机翻转）和 `crossover()`（单点交叉）。

**`phrase_time_counter.py`**：封装完整优化链路各阶段调用（`smt_to_llvm`、`try_optimize`、`llvm_to_smt`、`smt_solve_time`），中间文件写入 `/tmp/rq1/`，超时返回 `OPT_INF`。

### 日志

运行时日志写入 `~/logs/RQ1/`，按天轮转，保留 7 天：

- `rq1.info`：正常流程日志，含各阶段耗时
- `rq1.error`：超时、转换失败、求解结果不一致等异常
