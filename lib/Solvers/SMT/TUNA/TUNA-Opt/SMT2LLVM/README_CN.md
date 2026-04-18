# SMT2LLVM

TUNA-Opt 依赖的 SMT-LIB 2 与 LLVM IR 之间的双向转换工具套件，基于 [SLOT](https://github.com/TUNA-SMT/SLOT) 扩展开发。

核心功能：将 SMT-LIB 2 公式转换为 LLVM IR，利用 LLVM 优化 Pass 化简，再将结果转换回 SMT-LIB 2，从而实现对 SMT 公式的结构化简。

## 目录结构

```
SMT2LLVM/
├── include/                  # 头文件
├── src/
│   ├── tools/                # 各可执行工具的 main 入口
│   │   ├── main.cpp          # slot
│   │   ├── SMT2LLVM.cpp      # smt2llvm
│   │   ├── LLVM2SMT.cpp      # llvm2smt
│   │   ├── LLVM2FEAT.cpp     # llvm2feat
│   │   └── fastslot.cpp      # fastslot
│   └── ...                   # 转换逻辑实现
├── resources/                # Bug 复现示例
├── passes-slot-old.txt       # 旧版 SLOT 使用的 9 个 Pass
├── passes-run.txt            # 扩展后支持的全部 41 个有意义 Pass
├── passes-filter.txt         # 对优化有意义的 25 个 Pass 子集
├── passes-useful.txt         # 优化相关的 33 个 Pass
├── passes-16.txt             # LLVM 16 可用 Pass 基础列表（80 个）
├── passes-all-llvm16.txt     # LLVM 16 全量 Pass 列表（353 个）
└── FunctionPasses.md         # 142 个 LLVM FunctionPass 说明文档
```

## 安装

### 系统依赖

```bash
sudo apt install -y git gcc g++ cmake ninja-build python3 \
                    zlib1g-dev libtinfo-dev libxml2-dev
```

### 编译 LLVM 16.0.0

```bash
git clone git@github.com:llvm/llvm-project.git
cd llvm-project
git checkout llvmorg-16.0.0
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -j 80 -C build
```

### 编译 Z3 4.12.1

```bash
git clone git@github.com:Z3Prover/z3.git
cd z3
git checkout z3-4.12.1
python3 scripts/mk_make.py
cd build
make -j 80 && sudo make install
```

### 构建本项目

1. 修改 `CMakeLists.txt` 中的依赖路径：

   ```cmake
   set(LLVM_PATH "/path/to/llvm-project")   # LLVM 源码及构建目录
   set(Z3_PATH   "/path/to/z3")             # Z3 源码目录
   ```

2. 编译：

   ```bash
   mkdir build && cd build
   cmake ..
   make -j 32
   ```

编译完成后，`build/` 目录下会生成以下五个可执行文件：

| 可执行文件  | 功能简述                          |
|-------------|-----------------------------------|
| `slot`      | 完整流程：SMT → LLVM 优化 → SMT   |
| `fastslot`  | 快速流程：支持从 Pass 文件配置优化  |
| `smt2llvm`  | 单步转换：SMT → LLVM IR           |
| `llvm2smt`  | 单步转换：LLVM IR → SMT           |
| `llvm2feat` | 特征提取：从 LLVM IR 提取程序特征  |

---

## 工具使用说明

### 不是AI写的命令
```
./smt2llvm -s ../../../SLOT-FSE23/samples/multiplyOverflow.smt2 -lu ./multiplyOverflow.ll
./llvm2feat -lo ./multiplyOverflow.ll -f std
./llvm2smt -lo ./multiplyOverflow.ll -o ./multiplyOverflow-opt.smt2
./fastslot -m -s ../../../SLOT-FSE23/samples/multiplyOverflow.smt2  -o ./multiplyOverflow-opt.smt2 -p ../passes-run.txt
./slot -m -s ../../../SLOT-FSE23/samples/multiplyOverflow.smt2 -lu ./multiplyOverflow.ll -lo ./multiplyOverflow-opt.ll -o ./multiplyOverflow-opt.smt2 -p ../passes-run.txt
```

### `slot` — 完整 SMT 优化流程

读取 SMT-LIB 2 文件，翻译为 LLVM IR，应用选定的 LLVM 优化 Pass，再将优化后的 IR 转换回 SMT-LIB 2。

```
用法: slot [选项]

必需参数:
  -s <file>          输入 SMT-LIB 2 文件

可选参数:
  -o <file>          输出优化后的 SMT-LIB 2 文件（默认 stdout）
  -lu <file>         输出优化前的 LLVM IR（中间结果）
  -lo <file>         输出优化后的 LLVM IR（中间结果）
  -t <file>          输出计时统计（CSV 格式，默认 stdout）
  -m                 将常量右移操作转换为乘法（有助于暴露更多优化机会）
  -p <file>          从文件读取 Pass 列表（与下方 Pass 标志互斥）

Pass 标志（逐个启用）:
  -instcombine       指令合并
  -ainstcombine      激进指令合并
  -reassociate       表达式重新关联
  -sccp              稀疏条件常量传播
  -dce               死代码消除
  -adce              激进死代码消除
  -instsimplify      指令简化
  -gvn               全局值编号
  -pall              启用所有内置 Pass

  -h                 显示帮助信息
```

**统计输出格式（CSV）**：
```
文件名,是否启用移位转乘,Pass标志,前端时间(ms),优化时间(ms),后端时间(ms),实际使用的Pass
```

**示例**：

```bash
# 使用 Pass 文件运行完整流程，保留所有中间文件
./slot -m \
  -s ../samples/multiplyOverflow.smt2 \
  -lu ./before-opt.ll \
  -lo ./after-opt.ll \
  -o  ./result.smt2 \
  -p  ../passes-run.txt

# 只用 instcombine + gvn，将统计写入文件
./slot -s input.smt2 -o output.smt2 -instcombine -gvn -t stats.csv
```

---

### `fastslot` — 快速 SLOT 流程

与 `slot` 功能类似，但**只支持通过 `-p` Pass 文件指定优化管道**，不支持逐个 Pass 标志。适合脚本批量处理。

```
用法: fastslot [选项]

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

**Pass 文件格式**：每行一个 Pass 名称，对应 LLVM 新 Pass Manager 的名称。

```
# passes-run.txt 示例片段
instcombine
aggressive-instcombine
reassociate
sccp
dce
gvn
```

**示例**：

```bash
./fastslot -m \
  -s ../samples/multiplyOverflow.smt2 \
  -o ./result.smt2 \
  -p ../passes-run.txt
```

---

### `smt2llvm` — SMT 转 LLVM IR

将 SMT-LIB 2 文件翻译为 LLVM IR，输出 `.ll` 文件。可作为 `slot` 流程的前端单独使用。

```
用法: smt2llvm [选项]

必需参数:
  -s <file>          输入 SMT-LIB 2 文件

可选参数:
  -lu <file>         输出 LLVM IR 文件（默认 stdout）
```

翻译完成后会在 stdout 打印一行计时统计：`文件名,翻译时间(ms)`。

**支持的 SMT-LIB 2 类型**：

| SMT 类型               | 对应 LLVM 类型    |
|------------------------|-------------------|
| `Bool`                 | `i1`              |
| `(_ BitVec N)`         | `iN`              |
| `(_ FloatingPoint 5 11)` / `Float16`  | `half`   |
| `(_ FloatingPoint 8 24)` / `Float32`  | `float`  |
| `(_ FloatingPoint 11 53)` / `Float64` | `double` |
| `(_ FloatingPoint 15 113)` / `Float128` | `fp128` |

**示例**：

```bash
./smt2llvm -s input.smt2 -lu output.ll
```

---

### `llvm2smt` — LLVM IR 转 SMT

将 LLVM IR 文件翻译为 SMT-LIB 2 约束，输出 `.smt2` 文件。可作为 `slot` 流程的后端单独使用。

```
用法: llvm2smt [选项]

必需参数:
  -lo <file>         输入 LLVM IR 文件

可选参数:
  -o <file>          输出 SMT-LIB 2 文件（默认 stdout）
```

注意：`llvm2smt` 默认启用移位转乘法（等价于 `slot -m`）。

翻译完成后会在 stdout 打印一行计时统计：`文件名,转换时间(ms)`。

**示例**：

```bash
./llvm2smt -lo optimized.ll -o result.smt2
```

---

### `llvm2feat` — LLVM IR 特征提取

从 LLVM IR 中提取 58 维程序特征，供机器学习模型或分析工具使用。

```
用法: llvm2feat [选项]

必需参数:
  -lo <file>         输入 LLVM IR 文件
  -f <dir>           特征输出目录（写入该目录下的特征文件）
```

**提取的特征维度（共 58 个）**涵盖：
- 基本块数量、指令总数
- 各类指令计数（Add、Sub、Mul、Load、Store、ICmp、FCmp 等）
- Phi 节点及其参数数量
- 控制流特性（分支数、边数）
- 常量统计（32/64 位常数、零值、一值）
- 内存操作统计

**示例**：

```bash
./smt2llvm -s input.smt2 -lu input.ll
./llvm2feat -lo input.ll -f ./features/
```

---

## 典型使用场景

### 场景一：端到端 SMT 化简

```bash
./slot -m \
  -s problem.smt2 \
  -o simplified.smt2 \
  -p ../passes-run.txt
```

### 场景二：分步调试，查看中间 IR

```bash
# Step 1: SMT → LLVM IR
./smt2llvm -s problem.smt2 -lu before.ll

# Step 2: 手动用 opt 优化（可选，便于调试）
opt -passes="instcombine,gvn" -S before.ll -o after.ll

# Step 3: LLVM IR → SMT
./llvm2smt -lo after.ll -o simplified.smt2
```

### 场景三：批量提取特征

```bash
for f in corpus/*.smt2; do
  base=$(basename "$f" .smt2)
  ./smt2llvm -s "$f" -lu /tmp/${base}.ll
  ./llvm2feat -lo /tmp/${base}.ll -f features/${base}/
done
```

---

## Pass 配置文件说明

| 文件                   | Pass 数量 | 说明                                    |
|------------------------|-----------|-----------------------------------------|
| `passes-slot-old.txt`  | 9         | 旧版 SLOT 使用的核心 Pass               |
| `passes-run.txt`       | 41        | 当前支持的全部有意义 Pass               |
| `passes-filter.txt`    | 25        | 对优化有明确意义的 Pass 子集            |
| `passes-useful.txt`    | 33        | 优化相关 Pass（含循环/向量化等）         |
| `passes-16.txt`        | 80        | LLVM 16 可用的基础 Pass 列表            |
| `passes-all-llvm16.txt`| 353       | LLVM 16 全量 Pass 列表（含实验性 Pass） |

Pass 名称对应 LLVM 新 Pass Manager（`-passes=` 语法），详细说明见 `FunctionPasses.md`。
