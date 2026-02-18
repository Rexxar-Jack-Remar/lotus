/// @file IntraProceduralAnalysis.cpp
/// @brief Main driver for intra-procedural pointer analysis in LotusAA
///
/// This file contains the **analysis orchestration** logic that coordinates all
/// transfer functions to perform flow-sensitive, field-sensitive pointer
/// analysis within a single function.
///
/// **Architecture:**
/// ```
/// IntraLotusAA (per-function analysis)
///   ├── computePTA() - Main analysis driver
///   │   ├── Process instructions in topological order
///   │   ├── Dispatch to transfer functions by opcode
///   │   └── Collect function interface (summary)
///   ├── computeCG() - Call graph resolution
///   └── Analysis utilities (show, clearMemory, etc.)
/// ```
///
/// **Transfer Function Organization** (in TransferFunctions/ subdirectory):
/// - `PointerInstructions.cpp`: Load, Store, PHI, Select, GEP, Casts,
/// processBasePointer
/// - `BasicOps.cpp`: Alloca, Arguments, Globals, Constants
/// - `CallHandling.cpp`: Function calls and summary application
/// - `CallGraphSolver.cpp`: Indirect call resolution
/// - `SummaryBuilder.cpp`: Function summary collection
///
/// **Analysis Phases:**
/// 1. **Initialization**: Topological BB ordering, sequence numbering
/// 2. **Instruction Processing**: Dispatch by opcode to transfer functions
/// 3. **Summary Generation**: Extract inputs/outputs/escaped objects
/// 4. **Call Graph Construction**: Resolve indirect calls (if enabled)
///
/// **Configuration Options:**
/// - `lotus_restrict_inline_depth`: Max inter-procedural inlining depth
/// (default: 2)
/// - `lotus_restrict_cg_size`: Max indirect call targets (default: 5)
/// - `lotus_restrict_inline_size`: Max summary size (default: 100)
/// - `lotus_restrict_ap_level`: Max access path depth (default: 2)
///
/// @see IntraProceduralAnalysis.h for class declaration and data structures
/// @see TransferFunctions/ subdirectory for individual transfer function
/// implementations

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>


using namespace llvm;
using namespace std;

// Configuration
int IntraLotusAAConfig::lotus_restrict_inline_depth = 2;
double IntraLotusAAConfig::lotus_timeout = 10.0;
int IntraLotusAAConfig::lotus_restrict_cg_size = 5;
bool IntraLotusAAConfig::lotus_test_correctness = false;
int IntraLotusAAConfig::lotus_restrict_inline_size = 100;
int IntraLotusAAConfig::lotus_restrict_ap_level = 2;

static cl::opt<int> lotus_restrict_inline_depth_cl(
    "lotus-restrict-inline-depth",
    cl::desc("Maximum inlining depth for inter-procedural analysis"),
    cl::init(2), cl::Hidden);

static cl::opt<int> lotus_restrict_cg_size_cl(
    "lotus-restrict-cg-size",
    cl::desc("Maximum indirect call targets to process"), cl::init(5),
    cl::Hidden);

void IntraLotusAAConfig::setParam() {
  if (lotus_restrict_inline_depth_cl.getNumOccurrences() > 0)
    lotus_restrict_inline_depth = lotus_restrict_inline_depth_cl;
  if (lotus_restrict_cg_size_cl.getNumOccurrences() > 0)
    lotus_restrict_cg_size = lotus_restrict_cg_size_cl;
}

// IntraLotusAA implementation
const int IntraLotusAA::PTR_TO_ESC_OBJ = -1;

IntraLotusAA::IntraLotusAA(Function *F, LotusAA *lotus_aa)
    : PTGraph(F, lotus_aa), func_obj(nullptr), func_new(nullptr),
      is_PTA_computed(false), is_CG_computed(false),
      is_considered_as_library(false), is_timeout_found(false),
      inline_ap_depth(0) {

  getReturnInst();

  // Build a topological (reverse post-order) traversal of the CFG so that
  // instructions are processed before their uses wherever possible.
  // The previous code simply iterated over the function's basic block list,
  // which is NOT guaranteed to be in topological order and causes
  // processBasePointer() to fall back to processUnknown() for values that
  // haven't been processed yet.
  //
  // ReversePostOrderTraversal gives a correct RPO ordering for reducible CFGs
  // and a reasonable approximation for irreducible ones.
  ReversePostOrderTraversal<Function *> RPOT(F);
  for (BasicBlock *BB : RPOT) {
    topBBs.push_back(BB);
  }

}

IntraLotusAA::~IntraLotusAA() {
  for (OutputItem *item : outputs) {
    delete item;
  }

  // Delete pseudo-argument Argument objects created in createPseudoOutputNodes
  // and createEscapedObjects.  These are synthetic LLVM Argument objects that
  // are NOT owned by any LLVM Function, so they must be freed here.
  // Previously they were only stored in func_pseudo_ret_cache (as keys in
  // pt_results and as values in func_ret) but never deleted, causing a leak
  // proportional to the number of call sites × callees.
  for (auto &kv : func_pseudo_ret_cache) {
    // kv.first is the synthetic Argument* we allocated with `new Argument(...)`.
    // It is safe to delete because it has no parent Function.
    kv.first->deleteValue();
  }
  func_pseudo_ret_cache.clear();
}


void IntraLotusAA::computePTA() {
  if (is_considered_as_library || is_PTA_computed)
    return;

  // Cache instruction sequence
  int seq_num = 0;
  for (BasicBlock *bb : topBBs) {
    for (Instruction &inst : *bb) {
      value_seq[&inst] = seq_num++;
    }
  }

  cacheFunctionCallInfo();

  // Process instructions
  for (BasicBlock *bb : topBBs) {
    for (Instruction &inst : *bb) {
      switch (inst.getOpcode()) {
      case Instruction::Store:
        processStore(cast<StoreInst>(&inst));
        break;

      case Instruction::Load: {
        LoadInst *load = cast<LoadInst>(&inst);
        if (load->getType()->isPointerTy())
          processLoad(load);
        else {
          mem_value_t tmp;
          processBasePointer(load->getPointerOperand());
          loadPtrAt(load->getPointerOperand(), load, tmp, true);
        }
        break;
      }

      case Instruction::PHI:
        if (inst.getType()->isPointerTy())
          processPhi(cast<PHINode>(&inst));
        break;

      case Instruction::Alloca:
        processAlloca(cast<AllocaInst>(&inst));
        break;

      case Instruction::Call:
      case Instruction::Invoke:
        if (!isa<DbgInfoIntrinsic>(&inst)) {
          processCall(cast<CallBase>(&inst));
        }
        break;

      case Instruction::Select:
        if (inst.getType()->isPointerTy())
          processSelect(cast<SelectInst>(&inst));
        break;

      case Instruction::BitCast:
      case Instruction::GetElementPtr:
        processBasePointer(&inst);
        break;
      }
    }
  }

  // Collect interface for interprocedural analysis
  if (IntraLotusAAConfig::lotus_restrict_inline_depth != 0) {
    collectOutputs();
    collectInputs();
    finalizeInterface();
  }

  is_PTA_computed = true;
}

void IntraLotusAA::show() {
  outs() << "\n========== LotusAA Results: " << analyzed_func->getName()
         << " ==========\n";

  // Show points-to sets
  for (auto &it : pt_results) {
    Value *ptr = it.first;
    if (!ptr)
      continue;

    PTResult *res = it.second;
    PTResultIterator iter(res, this);

    outs() << "Pointer: ";
    if (ptr->hasName())
      outs() << ptr->getName();
    else
      ptr->print(outs());
    outs() << " -> " << iter.size() << " locations\n";
    outs() << iter << "\n";
  }

  outs() << "==============================================\n\n";
}
