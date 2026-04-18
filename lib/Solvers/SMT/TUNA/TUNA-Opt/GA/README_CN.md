# GA 优化器

使用遗传算法（GA）为 SMT 约束求解自动搜索最优 LLVM 编译 pass 组合，以降低 SMT 求解时间。

## 核心思路

对每个 SMT 约束文件，执行以下完整优化链路，并以总耗时作为 GA 的适应度函数：

```
SMT → smt2llvm → LLVM IR → opt (pass 组合) → LLVM IR → llvm2smt → SMT → 求解器
```

GA 在 LLVM pass 的开关组合空间中搜索，找到使总耗时最小的 pass 集合，并将结果写入 CSV 供后续分析。

## 目录结构

```
GA/
├── config.yaml              # 全局路径与参数配置
├── ga_optimizer.py          # 主入口：GA 优化器
├── phrase_time_counter.py   # 各阶段耗时统计基类
├── delta_debugger.py        # GA 结果后处理：剪枝冗余 pass
├── mlopt/
│   ├── ga_opt.py            # GA 算法核心（种群管理、选择、交叉）
│   └── params.py            # 参数编码（Params 类、变异、交叉）
└── utils/                   # 数据预处理脚本（见 utils/README.md）
```

## 前置配置（`config.yaml`）

运行任何脚本前，先确认以下字段：

| 字段 | 说明 | 示例 |
|------|------|------|
| `slot_dir` | SLOT 二进制目录，需含 `fastslot`、`slot`、`smt2llvm`、`llvm2smt` | `/path/to/build/` |
| `llvm_dir` | LLVM 二进制目录，需含 `opt` | `/path/to/llvm-project/build/bin` |
| `dataset_dir` | SMTLIB 数据集根目录 | `/data/QF_BV` |
| `smt_solver_path` | SMT 求解器路径，支持 Z3 / CVC5 / Boolector | `/usr/bin/z3` |
| `output_csv_path` | GA 结果 CSV 输出路径 | `/path/to/RQ1.csv` |
| `smac_log_output_directory` | SMAC 日志目录 | `/path/to/SMAC` |

## 完整运行流程

### 第一步：数据预处理

详见 [utils/README.md](utils/README.md)，产出各求解器按求解时间分段的 SMT 文件列表。

### 第二步：运行 GA 优化

```bash
python -m ga_optimizer
```

`GAOptimizer` 继承自 `PhraseTimeCounter`，对 `fast_smt_path` 中的每个 SMT 文件：

1. 将 SMT 转换为 LLVM IR（`smt2llvm`）
2. 以 SLOT 默认 pass 组合作为基准耗时
3. 运行 GA（32 次迭代，种群大小 32）搜索最优 pass 组合
4. 将原始耗时、SLOT 耗时、GA 找到的所有优于原始的结果写入 `output_csv_path`

输出 CSV 格式：每个 SMT 文件对应多行，列为 `file_relative_path, cost_time, <pass1>, <pass2>, ..., status`。

默认使用 40 个进程并行处理。

### 第三步：剪枝冗余 pass（可选）

```bash
python -m delta_debugger
```

读取 GA 产出的 label CSV，对每个文件取 GA 找到的最优 pass 组合，通过迭代删除法找到产生相同 LLVM IR 输出的最小 pass 子集，结果写入 `Refine_<原文件名>.csv`。

运行前需修改 `delta_debugger.py` 中的 `base_name` 和路径变量。

### 第四步：统计各阶段耗时（可选）

```bash
python -m phrase_time_counter
```

读取 GA label CSV，对每个文件统计并对比以下四种策略的总耗时：

| 策略 | 说明 |
|------|------|
| `default_solve_time` | 直接用求解器求解原始 SMT |
| `slot_solve_time` | SLOT 默认 pass 优化后求解 |
| `ga_best_solve_time` | GA 找到的最优 pass 组合 |
| `ga_worst_solve_time` | GA 找到的最差 pass 组合 |
| `o3_solve_time` | `-O3` 优化后求解 |

结果写入 `config.py` 中对应求解器的 `*_ga_cost_time_path`。

## 模块说明

### `mlopt/ga_opt.py` — GA 算法

| 参数 | 值 | 说明 |
|------|----|------|
| `_population_size` | 32 | 种群大小 |
| `_retain_percentage` | 0.25 | 精英保留比例 |

每轮迭代：`evaluate`（计算适应度）→ `repopulate`（选择 + 交叉 + 变异）。适应度为优化链路总耗时，`4294967295`（`OPT_INF`）表示超时或失败。

### `mlopt/params.py` — 参数编码

每个 LLVM pass 编码为一个布尔参数（`true`/`false`），`Params` 类支持：
- `load(optlist)`：从字符串列表初始化参数
- `mutate()`：以 0.5 概率随机翻转每个参数
- `crossover(p1, p2)`：单点交叉
- `to_cmd_args()`：将值为 `true` 的 pass 转为命令行参数列表

### `phrase_time_counter.py` — 耗时统计基类

`PhraseTimeCounter` 封装了完整优化链路的各阶段调用：

| 方法 | 说明 |
|------|------|
| `smt_to_llvm()` | SMT → LLVM IR，返回转换耗时 |
| `try_optimize(path, passes)` | 用指定 pass 列表运行 `opt`，返回耗时 |
| `try_optimize_with_o3(path)` | 用 `-O3` 运行 `opt`，返回耗时 |
| `llvm_to_smt(llvm, smt)` | LLVM IR → SMT，返回转换耗时 |
| `smt_solve_time(smt)` | 运行求解器，返回求解耗时 |
| `get_all_phrase_time(use_o3, passes)` | 返回完整链路总耗时 |

所有操作的中间文件写入 `/tmp/rq1/`，超时统一返回 `OPT_INF`。

## 日志

运行时日志写入 `~/logs/RQ1/`：

- `rq1.info`：正常流程日志，含各阶段耗时
- `rq1.error`：超时、转换失败、求解结果不一致等异常

日志按天轮转，保留 7 天。
