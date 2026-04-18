# README

一些相对独立的数据预处理/工具脚本，用于为 GA 优化器准备训练数据。

## 前置配置（`config.yaml`）

所有脚本均依赖项目根目录下的 `config.yaml`，**运行任何脚本前请先确认以下字段**：

| 字段 | 说明 | 示例 |
|------|------|------|
| `slot_dir` | SLOT 二进制所在目录，需包含 `fastslot`、`slot`、`smt2llvm`、`llvm2smt` | `/path/to/build/` |
| `llvm_dir` | LLVM 二进制目录，需包含 `opt` 可执行文件 | `/path/to/llvm-project/build/bin` |
| `dataset_dir` | SMTLIB 数据集根目录（`.smt2` 文件所在目录） | `/data/QF_BV` |
| `smt_solver_path` | SMT 求解器可执行文件路径，支持 Z3 / CVC5 / Boolector | `/usr/bin/z3` |
| `output_csv_path` | RQ1 结果 CSV 输出路径 | `/path/to/RQ1.csv` |
| `smac_log_output_directory` | SMAC 优化日志输出目录 | `/path/to/SMAC` |

各脚本对配置的依赖：

- **`slot_validate.py`**：需要 `slot_dir`（使用 `fastslot`）、`dataset_dir`
- **`z3_validate.py`**：需要 `smt_solver_path`、`dataset_dir`（求解器决定写入哪个分类文件）
- **`find_under_1s.py`**：无需额外配置，依赖前两步生成的文件
- **`add_smt_status_to_ga_label.py`**：需要 `dataset_dir`
- **`refine_slot_data.py`**：需要 `slot_dir`（使用原始 `slot`）、`dataset_dir`

> `smt_solver_path` 决定了 `z3_validate.py` 中写入的分类文件。切换求解器时，同时需要修改脚本内的 `TODO` 注释，将输出路径改为对应求解器的分类路径（如 `CONFIG.cvc5_600s_smt_path`）。

## 数据预处理流程

整体流程分为以下几个阶段：

### 第一阶段：筛选可翻译的 SMT 约束

**`slot_validate.py`**

遍历数据集中所有 `.smt2` 文件，使用 SLOT 工具（`fastslot`）对每个文件进行翻译，筛选出在超时时间内（默认 20s）能完成翻译的约束，结果写入 `fast_smt_path`（`fastslot_under_20s_smt.txt`）。

```bash
python -m utils.slot_validate
```

### 第二阶段：按 SMT 求解时间分段

**`z3_validate.py`**

对第一阶段筛选出的 SMT 文件，使用指定的 SMT 求解器（Z3 / CVC5 / Boolector）运行，按求解时间分段（1s、30s、60s、120s、300s、600s）写入对应的分类文件。

> 注意：使用前需修改脚本中的 `TODO` 注释，指定读取文件和输出文件路径（对应 `config.py` 中的分类路径）。

```bash
python -m utils.z3_validate
```

**`find_under_1s.py`**

从 `fast_smt_path` 中减去 `over_1s` 列表，得到求解时间小于 1s 的 SMT 文件列表，写入 `under_1s` 路径。

```bash
python -m utils.find_under_1s
```


## 工具模块

| 文件 | 说明 |
|------|------|
| `config.py` | 全局配置，读取 `config.yaml`，统一管理所有路径和参数 |
| `file_utils.py` | 文件操作工具：列举 SMT 文件、读取 SMT 列表、初始化 CSV 等 |
| `logger.py` | 日志工具，输出到 `~/logs/RQ1/`，支持 TraceID（基于 `asgi_correlation_id`） |

## 分类路径说明（`config.py`）

求解器分类文件统一存放在 `resources/<smt_category>/<solver>_category/` 下，命名规则为：

```
<solver>_over_<low>s_under_<high>s_smt.txt
```

例如：`z3_over_1s_under_30s_smt.txt`，表示 Z3 求解时间在 1s～30s 之间的约束列表。