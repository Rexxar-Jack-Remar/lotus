//===-- PRUE.cpp --------------------------------------------------------===//
//
// Optimizer-friendly event quantification based on delayed delta-counter
// updates and a late PRUE rewrite.
//
// ASPLOS 2026: Optimizer-Friendly Instrumentation for Event Quantification with PRUE Algorithm
//===----------------------------------------------------------------------===//

#include "Transform/Nisse/Nisse.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <deque>
#include <set>
#include <vector>

using namespace llvm;

namespace nisse {

namespace {

constexpr const char *kPrueCounterArrayName = "prue-counter-array";
constexpr const char *kPrueIndexArrayName = "prue-index-array";
constexpr const char *kPrueUpdateMetadata = "nisse.prue.update";

struct UpdateTask {
  StoreInst *store = nullptr;
  GlobalVariable *counter_array = nullptr;
  uint64_t counter_index = 0;
};

struct ParsedUpdate {
  StoreInst *store = nullptr;
  LoadInst *old_load = nullptr;
  BinaryOperator *add = nullptr;
  Instruction *ptr_inst = nullptr;
  Instruction *synthetic_delta = nullptr;
  Value *operand = nullptr;
  GlobalVariable *counter_array = nullptr;
  uint64_t counter_index = 0;
};

struct FunctionPRUEContext {
  Function &function;
  DominatorTree dt;
  PostDominatorTree pdt;
  LoopInfo li;

  explicit FunctionPRUEContext(Function &function) : function(function) {
    recalculate();
  }

  void recalculate() {
    dt.recalculate(function);
    pdt.recalculate(function);
    li = LoopInfo(dt);
  }
};

struct LoopClosure {
  SmallPtrSet<Value *, 16> members;
  SmallVector<PHINode *, 8> phis;
  SmallVector<SelectInst *, 8> selects;
  SmallVector<Instruction *, 8> wrappers;
  SmallVector<BinaryOperator *, 8> adds;
};

static bool hasMustTailReturn(const Function &F) {
  for (const BasicBlock &BB : F) {
    auto *ret = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!ret) {
      continue;
    }

    if (const Instruction *prev = BB.getTerminator()->getPrevNonDebugInstruction()) {
      if (const auto *call = dyn_cast<CallInst>(prev)) {
        if (call->isMustTailCall()) {
          return true;
        }
      }
    }
  }

  return false;
}

static bool unifyReturnBlocks(Function &F) {
  std::vector<BasicBlock *> returning_blocks;

  for (BasicBlock &BB : F) {
    if (isa<ReturnInst>(BB.getTerminator())) {
      returning_blocks.push_back(&BB);
    }
  }

  if (returning_blocks.size() <= 1) {
    return false;
  }

  BasicBlock *new_ret_block =
      BasicBlock::Create(F.getContext(), "UnifiedReturnBlock", &F);

  PHINode *phi = nullptr;
  if (F.getReturnType()->isVoidTy()) {
    ReturnInst::Create(F.getContext(), nullptr, new_ret_block);
  } else {
    phi = PHINode::Create(F.getReturnType(), returning_blocks.size(),
                          "UnifiedRetVal");
    new_ret_block->getInstList().push_back(phi);
    ReturnInst::Create(F.getContext(), phi, new_ret_block);
  }

  for (BasicBlock *BB : returning_blocks) {
    if (phi) {
      phi->addIncoming(BB->getTerminator()->getOperand(0), BB);
    }
    BB->getInstList().pop_back();
    BranchInst::Create(new_ret_block, BB);
  }

  return true;
}

static std::vector<Edge> collectAllEdges(Function &F) {
  std::vector<Edge> edges;
  int index = 0;
  for (BasicBlock &BB : F) {
    for (BasicBlock *succ : successors(&BB)) {
      edges.emplace_back(&BB, succ, index++);
    }
  }
  return edges;
}

static MDNode *makePrueMetadata(LLVMContext &ctx, uint64_t counter_index) {
  Metadata *ops[] = {ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt64Ty(ctx), counter_index))};
  return MDNode::get(ctx, ops);
}

static bool extractCounterIndex(const StoreInst *store, uint64_t &counter_index) {
  auto *node = store->getMetadata(kPrueUpdateMetadata);
  if (!node || node->getNumOperands() != 1) {
    return false;
  }
  auto *meta = dyn_cast<ConstantAsMetadata>(node->getOperand(0).get());
  if (!meta) {
    return false;
  }
  auto *constant = dyn_cast<ConstantInt>(meta->getValue());
  if (!constant) {
    return false;
  }
  counter_index = constant->getZExtValue();
  return true;
}

static Value *castToInt64(Value *value, IRBuilder<> &builder) {
  Type *int64Ty = builder.getInt64Ty();
  if (value->getType() == int64Ty) {
    return value;
  }
  if (value->getType()->isIntegerTy()) {
    return builder.CreateIntCast(value, int64Ty, true);
  }
  if (value->getType()->isPointerTy()) {
    return builder.CreatePtrToInt(value, int64Ty);
  }
  return value;
}

static bool isTransparentPrueWrapperOpcode(unsigned opcode) {
  switch (opcode) {
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::BitCast:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::Freeze:
    return true;
  default:
    return false;
  }
}

static bool isTransparentPrueWrapper(const Value *value) {
  auto *op = dyn_cast<Operator>(value);
  return op && op->getNumOperands() == 1 &&
         isTransparentPrueWrapperOpcode(op->getOpcode());
}

static Value *getTransparentPrueOperand(Value *value) {
  if (!isTransparentPrueWrapper(value)) {
    return nullptr;
  }
  return cast<Operator>(value)->getOperand(0);
}

static Value *stripTransparentPrueWrappers(Value *value) {
  while (Value *wrapped = getTransparentPrueOperand(value)) {
    value = wrapped;
  }
  return value;
}

static Instruction *getPointerInstruction(Value *value) {
  value = value->stripPointerCasts();
  return dyn_cast<Instruction>(value);
}

static BasicBlock *getDefBlock(Function &function, Value *value) {
  value = stripTransparentPrueWrappers(value);
  if (auto *inst = dyn_cast<Instruction>(value)) {
    return inst->getParent();
  }
  if (isa<Argument>(value)) {
    return &function.getEntryBlock();
  }
  return nullptr;
}

static bool sameLoop(BasicBlock *lhs, BasicBlock *rhs, LoopInfo &li) {
  return li.getLoopFor(lhs) == li.getLoopFor(rhs);
}

static bool extractCounterLocation(StoreInst *store, GlobalVariable *&counter_array,
                                   uint64_t &counter_index) {
  auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
  if (!gep || gep->getNumOperands() < 2) {
    return false;
  }

  counter_array = dyn_cast<GlobalVariable>(
      gep->getPointerOperand()->stripPointerCasts());
  if (!counter_array) {
    return false;
  }

  auto *index = dyn_cast<ConstantInt>(gep->getOperand(gep->getNumOperands() - 1));
  if (!index) {
    return false;
  }
  counter_index = index->getZExtValue();
  return true;
}

static bool parseUpdateStore(StoreInst *store, GlobalVariable *counter_array,
                             uint64_t counter_index, ParsedUpdate &parsed) {
  auto *root = store->getValueOperand();
  auto *root_add = dyn_cast<BinaryOperator>(root);
  if (!root_add || root_add->getOpcode() != Instruction::Add) {
    return false;
  }

  Value *store_ptr = store->getPointerOperand()->stripPointerCasts();
  LoadInst *old_load = nullptr;
  SmallVector<Value *, 8> delta_terms;
  std::function<bool(Value *)> peel_old_load = [&](Value *current) -> bool {
    auto *candidate = dyn_cast<LoadInst>(current);
    if (candidate &&
        candidate->getPointerOperand()->stripPointerCasts() == store_ptr) {
      if (old_load) {
        return false;
      }
      old_load = candidate;
      return true;
    }

    auto *current_add = dyn_cast<BinaryOperator>(current);
    if (!current_add || current_add->getOpcode() != Instruction::Add) {
      return false;
    }

    const bool lhs_has_old = peel_old_load(current_add->getOperand(0));
    if (lhs_has_old && old_load == nullptr) {
      return false;
    }

    LoadInst *saved_old = old_load;
    SmallVector<Value *, 8> saved_terms(delta_terms.begin(), delta_terms.end());
    if (lhs_has_old) {
      if (peel_old_load(current_add->getOperand(1))) {
        old_load = saved_old;
        delta_terms.assign(saved_terms.begin(), saved_terms.end());
        return false;
      }
      delta_terms.push_back(current_add->getOperand(1));
      return true;
    }

    if (peel_old_load(current_add->getOperand(1))) {
      delta_terms.push_back(current_add->getOperand(0));
      return true;
    }

    old_load = saved_old;
    delta_terms.assign(saved_terms.begin(), saved_terms.end());
    return false;
  };

  if (!peel_old_load(root)) {
    return false;
  }

  if (!old_load) {
    return false;
  }

  Value *operand = nullptr;
  Instruction *synthetic_delta = nullptr;
  if (delta_terms.empty()) {
    operand = ConstantInt::get(Type::getInt64Ty(store->getContext()), 0);
  } else if (delta_terms.size() == 1) {
    operand = delta_terms.front();
  } else {
    IRBuilder<> builder(store);
    Value *acc = castToInt64(delta_terms.front(), builder);
    for (unsigned i = 1; i < delta_terms.size(); ++i) {
      Value *rhs = castToInt64(delta_terms[i], builder);
      acc = builder.CreateAdd(acc, rhs, i + 1 == delta_terms.size()
                                            ? "prue.delta.expr"
                                            : "prue.delta.part");
    }
    operand = acc;
    synthetic_delta = dyn_cast<Instruction>(acc);
  }

  parsed.store = store;
  parsed.old_load = old_load;
  parsed.add = root_add;
  parsed.ptr_inst = getPointerInstruction(store_ptr);
  parsed.synthetic_delta = synthetic_delta;
  parsed.operand = operand;
  parsed.counter_array = counter_array;
  parsed.counter_index = counter_index;
  return true;
}

static void eraseIfDead(Instruction *inst) {
  if (inst && inst->use_empty()) {
    inst->eraseFromParent();
  }
}

static void eraseUpdate(const ParsedUpdate &update) {
  StoreInst *store = update.store;
  BinaryOperator *add = update.add;
  LoadInst *old_load = update.old_load;
  Instruction *ptr_inst = update.ptr_inst;
  Instruction *synthetic_delta = update.synthetic_delta;

  store->eraseFromParent();
  eraseIfDead(synthetic_delta);
  eraseIfDead(add);
  eraseIfDead(old_load);
  eraseIfDead(ptr_inst);
}

static StoreInst *createUpdate(BasicBlock *block, GlobalVariable *counter_array,
                               uint64_t counter_index, Value *operand,
                               bool add_metadata) {
  IRBuilder<> builder(block->getTerminator());
  Type *int64Ty = builder.getInt64Ty();
  Value *index_list[] = {builder.getInt32(0), builder.getInt32(counter_index)};
  auto *ptr =
      builder.CreateInBoundsGEP(counter_array->getValueType(), counter_array,
                                index_list);
  auto *old_value = builder.CreateLoad(int64Ty, ptr);
  auto *delta_value = castToInt64(operand, builder);
  auto *new_value = builder.CreateAdd(old_value, delta_value, "prue.update");
  auto *store = builder.CreateStore(new_value, ptr);
  if (add_metadata) {
    store->setMetadata(kPrueUpdateMetadata,
                       makePrueMetadata(builder.getContext(), counter_index));
  }
  return store;
}

static bool isZeroValue(Value *value) {
  value = stripTransparentPrueWrappers(value);
  auto *constant = dyn_cast<ConstantInt>(value);
  return constant && constant->isZero();
}

static BasicBlock *findRelocationTarget(const ParsedUpdate &update,
                                        FunctionPRUEContext &ctx) {
  BasicBlock *use_block = update.store->getParent();
  BasicBlock *def_block = getDefBlock(ctx.function, update.operand);
  if (!def_block || def_block == use_block) {
    return nullptr;
  }

  BasicBlock *candidate = nullptr;
  DomTreeNode *node = ctx.dt.getNode(use_block);
  while (node && node->getIDom()) {
    node = node->getIDom();
    BasicBlock *current = node->getBlock();
    if (!sameLoop(use_block, current, ctx.li)) {
      break;
    }
    if (!ctx.dt.dominates(def_block, current)) {
      break;
    }
    if (!ctx.pdt.dominates(use_block, current)) {
      continue;
    }
    candidate = current;
  }

  return candidate;
}

static bool canSplitPhi(const ParsedUpdate &update) {
  auto *phi = dyn_cast<PHINode>(update.operand);
  if (!phi || phi->getParent() != update.store->getParent()) {
    return false;
  }

  for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
    Value *incoming = phi->getIncomingValue(i);
    if (incoming == phi) {
      return false;
    }
  }
  return true;
}

static bool applySplit(const ParsedUpdate &update, std::deque<UpdateTask> &worklist,
                       FunctionPRUEContext &ctx) {
  auto *phi = dyn_cast<PHINode>(update.operand);
  if (!phi || phi->getParent() != update.store->getParent()) {
    return false;
  }
  if (!canSplitPhi(update)) {
    return false;
  }

  SmallVector<BasicBlock *, 4> incoming_blocks;
  SmallVector<Value *, 4> incoming_values;
  incoming_blocks.reserve(phi->getNumIncomingValues());
  incoming_values.reserve(phi->getNumIncomingValues());
  for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
    incoming_blocks.push_back(phi->getIncomingBlock(i));
    incoming_values.push_back(phi->getIncomingValue(i));
  }

  eraseUpdate(update);

  for (unsigned i = 0; i < incoming_blocks.size(); ++i) {
    BasicBlock *edge_block =
        SplitEdge(incoming_blocks[i], phi->getParent(), &ctx.dt, &ctx.li);
    StoreInst *store =
        createUpdate(edge_block, update.counter_array, update.counter_index,
                     incoming_values[i], false);
    UpdateTask subtask;
    subtask.store = store;
    subtask.counter_array = update.counter_array;
    subtask.counter_index = update.counter_index;
    worklist.push_back(subtask);
  }

  ctx.recalculate();
  return true;
}

static bool isLoopExitBlock(BasicBlock *block, LoopInfo &li) {
  for (BasicBlock *pred : predecessors(block)) {
    Loop *loop = li.getLoopFor(pred);
    while (loop) {
      if (!loop->contains(block)) {
        return true;
      }
      loop = loop->getParentLoop();
    }
  }
  return false;
}

static std::vector<Loop *> getExitLoops(BasicBlock *block, LoopInfo &li) {
  std::vector<Loop *> loops;
  SmallPtrSet<Loop *, 8> seen;
  for (BasicBlock *pred : predecessors(block)) {
    for (Loop *loop = li.getLoopFor(pred); loop; loop = loop->getParentLoop()) {
      if (loop->contains(block) || !seen.insert(loop).second) {
        continue;
      }
      loops.push_back(loop);
    }
  }
  std::sort(loops.begin(), loops.end(),
            [](Loop *lhs, Loop *rhs) { return lhs->getLoopDepth() < rhs->getLoopDepth(); });
  return loops;
}

static void enqueueLoopClosureValue(Value *value, LoopClosure &closure,
                                    std::deque<Value *> &worklist) {
  if (!closure.members.insert(value).second) {
    return;
  }

  if (auto *phi = dyn_cast<PHINode>(value)) {
    closure.phis.push_back(phi);
  } else if (auto *select = dyn_cast<SelectInst>(value)) {
    closure.selects.push_back(select);
  } else if (auto *add = dyn_cast<BinaryOperator>(value)) {
    if (add->getOpcode() == Instruction::Add) {
      closure.adds.push_back(add);
    }
  } else if (auto *inst = dyn_cast<Instruction>(value)) {
    if (isTransparentPrueWrapper(inst)) {
      closure.wrappers.push_back(inst);
    }
  }

  worklist.push_back(value);
}

static bool isClosureValueOrZero(Value *value, const LoopClosure &closure) {
  return closure.members.count(value) || isZeroValue(value);
}

static bool isIgnorableClosureUser(const User *user) {
  auto *inst = dyn_cast<Instruction>(user);
  return isa_and_nonnull<DbgInfoIntrinsic>(inst);
}

static bool hasOnlyPrueClosureUsers(const ParsedUpdate &update,
                                    const LoopClosure &closure) {
  for (Value *member : closure.members) {
    for (User *user : member->users()) {
      if (user == update.add || closure.members.count(user) ||
          isIgnorableClosureUser(user)) {
        continue;
      }
      return false;
    }
  }

  return true;
}

static bool collectLoopClosure(const ParsedUpdate &update,
                               FunctionPRUEContext &ctx, LoopClosure &closure) {
  std::deque<Value *> worklist;
  enqueueLoopClosureValue(update.operand, closure, worklist);

  while (!worklist.empty()) {
    Value *current = worklist.front();
    worklist.pop_front();

    for (User *user : current->users()) {
      if (user == update.add) {
        continue;
      }

      auto *inst = dyn_cast<Instruction>(user);
      if (!inst) {
        continue;
      }

      if (auto *phi = dyn_cast<PHINode>(user)) {
        if (!ctx.pdt.dominates(update.store->getParent(), phi->getParent())) {
          return false;
        }
        enqueueLoopClosureValue(phi, closure, worklist);
        continue;
      }

      if (auto *select = dyn_cast<SelectInst>(user)) {
        if (select->getTrueValue() != current && select->getFalseValue() != current) {
          continue;
        }
        if (!ctx.pdt.dominates(update.store->getParent(), select->getParent())) {
          return false;
        }
        enqueueLoopClosureValue(select, closure, worklist);
        continue;
      }

      if (isTransparentPrueWrapper(inst)) {
        if (inst->getOperand(0) != current) {
          continue;
        }
        if (!ctx.pdt.dominates(update.store->getParent(), inst->getParent())) {
          return false;
        }
        enqueueLoopClosureValue(inst, closure, worklist);
        continue;
      }

      auto *bin = dyn_cast<BinaryOperator>(user);
      if (!bin || bin->getOpcode() != Instruction::Add) {
        continue;
      }
      if (!ctx.pdt.dominates(update.store->getParent(), bin->getParent())) {
        return false;
      }
      enqueueLoopClosureValue(bin, closure, worklist);
    }
  }

  for (PHINode *phi : closure.phis) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      Value *incoming = phi->getIncomingValue(i);
      if (isClosureValueOrZero(incoming, closure)) {
        continue;
      }
      return false;
    }
  }

  for (SelectInst *select : closure.selects) {
    if (!isClosureValueOrZero(select->getTrueValue(), closure) ||
        !isClosureValueOrZero(select->getFalseValue(), closure)) {
      return false;
    }
  }

  for (BinaryOperator *add : closure.adds) {
    const bool lhs_in = closure.members.count(add->getOperand(0));
    const bool rhs_in = closure.members.count(add->getOperand(1));
    if (lhs_in == rhs_in) {
      return false;
    }
  }

  return true;
}

static bool getSingleExitingBlock(Loop *loop, BasicBlock *&exiting_block,
                                  SmallVectorImpl<BasicBlock *> &outside_succs) {
  exiting_block = nullptr;
  outside_succs.clear();
  SmallPtrSet<BasicBlock *, 4> seen_succs;

  for (BasicBlock *BB : loop->blocks()) {
    for (BasicBlock *succ : successors(BB)) {
      if (loop->contains(succ)) {
        continue;
      }
      if (exiting_block && exiting_block != BB) {
        return false;
      }
      exiting_block = BB;
      if (seen_succs.insert(succ).second) {
        outside_succs.push_back(succ);
      }
    }
  }

  return exiting_block && !outside_succs.empty();
}

static bool isDefinedOutsideLoop(Value *value, Loop *loop, Function &function) {
  if (isZeroValue(value)) {
    return false;
  }

  BasicBlock *def_block = getDefBlock(function, value);
  return !def_block || !loop->contains(def_block);
}

static Value *getExternalOperand(BinaryOperator *add, const LoopClosure &closure) {
  if (closure.members.count(add->getOperand(0))) {
    return add->getOperand(1);
  }
  return add->getOperand(0);
}

static bool applyOffload(const ParsedUpdate &update,
                         std::deque<UpdateTask> &worklist,
                         FunctionPRUEContext &ctx) {
  if (!isLoopExitBlock(update.store->getParent(), ctx.li)) {
    return false;
  }

  LoopClosure closure;
  if (!collectLoopClosure(update, ctx, closure)) {
    return false;
  }
  if (!hasOnlyPrueClosureUsers(update, closure)) {
    return false;
  }
  if (closure.adds.size() != 1) {
    return false;
  }

  BinaryOperator *x_plus = closure.adds.front();
  std::vector<Loop *> loops = getExitLoops(update.store->getParent(), ctx.li);

  Loop *selected_loop = nullptr;
  BasicBlock *selected_exiting_block = nullptr;
  SmallVector<BasicBlock *, 4> selected_outside_succs;

  for (Loop *loop : loops) {
    BasicBlock *def_block = x_plus->getParent();
    if (!loop->contains(def_block)) {
      continue;
    }
    if (!ctx.pdt.dominates(def_block, loop->getHeader())) {
      continue;
    }

    BasicBlock *exiting_block = nullptr;
    SmallVector<BasicBlock *, 4> outside_succs;
    if (!getSingleExitingBlock(loop, exiting_block, outside_succs)) {
      continue;
    }

    selected_loop = loop;
    selected_exiting_block = exiting_block;
    selected_outside_succs = outside_succs;
    break;
  }

  if (!selected_loop) {
    return false;
  }

  for (PHINode *phi : closure.phis) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      Value *incoming = phi->getIncomingValue(i);
      if (!isDefinedOutsideLoop(incoming, selected_loop, ctx.function)) {
        continue;
      }
      phi->setIncomingValue(i, ConstantInt::get(phi->getType(), 0));
    }
  }

  for (SelectInst *select : closure.selects) {
    if (isDefinedOutsideLoop(select->getTrueValue(), selected_loop, ctx.function)) {
      select->setTrueValue(ConstantInt::get(select->getType(), 0));
    }
    if (isDefinedOutsideLoop(select->getFalseValue(), selected_loop, ctx.function)) {
      select->setFalseValue(ConstantInt::get(select->getType(), 0));
    }
  }

  SmallVector<UpdateTask, 4> new_tasks;
  for (BasicBlock *outside_succ : selected_outside_succs) {
    BasicBlock *dedicated_exit =
        SplitEdge(selected_exiting_block, outside_succ, &ctx.dt, &ctx.li);
    StoreInst *store =
        createUpdate(dedicated_exit, update.counter_array, update.counter_index,
                     x_plus, false);
    new_tasks.push_back(
        UpdateTask{store, update.counter_array, update.counter_index});
  }

  eraseUpdate(update);
  for (const UpdateTask &subtask : new_tasks) {
    worklist.push_back(subtask);
  }
  ctx.recalculate();
  return true;
}

static bool applyUnpack(const ParsedUpdate &update,
                        std::deque<UpdateTask> &worklist,
                        FunctionPRUEContext &ctx) {
  if (!isLoopExitBlock(update.store->getParent(), ctx.li)) {
    return false;
  }

  LoopClosure closure;
  if (!collectLoopClosure(update, ctx, closure)) {
    return false;
  }
  if (closure.adds.empty()) {
    return false;
  }

  SmallVector<UpdateTask, 4> new_tasks;
  for (BinaryOperator *add : closure.adds) {
    StoreInst *store =
        createUpdate(add->getParent(), update.counter_array, update.counter_index,
                     getExternalOperand(add, closure), false);
    new_tasks.push_back(
        UpdateTask{store, update.counter_array, update.counter_index});
  }

  eraseUpdate(update);
  for (const UpdateTask &subtask : new_tasks) {
    worklist.push_back(subtask);
  }
  return true;
}

} // namespace

PreservedAnalyses DeltaCounterPass::run(Module &M, ModuleAnalysisManager &MAM) {
  LLVMContext &ctx = M.getContext();
  SmallPtrSet<Function *, 8> musttail_functions;

  bool prepared_cfg = false;
  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    if (hasMustTailReturn(F)) {
      musttail_functions.insert(&F);
      continue;
    }
    prepared_cfg |= unifyReturnBlocks(F);
    prepared_cfg |= SplitAllCriticalEdges(F) > 0;
  }
  (void)prepared_cfg;

  outfile.open("info.prof");
  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    if (musttail_functions.count(&F)) {
      continue;
    }
    std::vector<Edge> edges = collectAllEdges(F);
    int size = static_cast<int>(edges.size());

    if (size == 0) {
      continue;
    }

    NumEdges += size;
    outfile << F.getName().str() << " " << size << "\n";
  }
  outfile.close();

  ArrayType *counterArrayType = ArrayType::get(Type::getInt64Ty(ctx), NumEdges);
  CounterArray = new GlobalVariable(
      M, counterArrayType, false, GlobalValue::ExternalLinkage,
      Constant::getNullValue(counterArrayType), kPrueCounterArrayName);

  ArrayType *indexArrayType = ArrayType::get(Type::getInt32Ty(ctx), NumEdges);
  IndexArray = new GlobalVariable(
      M, indexArrayType, false, GlobalValue::ExternalLinkage,
      Constant::getNullValue(indexArrayType), kPrueIndexArrayName);

  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    if (musttail_functions.count(&F)) {
      // Preserve the paper's single-root PRUE model by skipping functions
      // whose musttail returns cannot be merged into a unified exit block.
      continue;
    }
    std::vector<Edge> edges = collectAllEdges(F);
    int size = static_cast<int>(edges.size());

    if (size == 0) {
      continue;
    }

    IRBuilder<> entry_builder(&F.getEntryBlock(), F.getEntryBlock().begin());
    Type *int64Ty = entry_builder.getInt64Ty();
    Type *int32Ty = entry_builder.getInt32Ty();

    std::vector<AllocaInst *> local_counters;
    local_counters.reserve(edges.size());

    int index = Offset;
    for (std::vector<Edge>::const_iterator it = edges.begin(), end = edges.end();
         it != end; ++it, ++index) {
      AllocaInst *slot = entry_builder.CreateAlloca(
          int64Ty, nullptr, "prue.delta." + Twine(index));
      entry_builder.CreateStore(entry_builder.getInt64(0), slot);
      local_counters.push_back(slot);

      auto *counter_index = entry_builder.getInt32(it->getIndex());
      Value *index_list[] = {entry_builder.getInt32(0),
                             entry_builder.getInt32(index)};
      auto *global_index = entry_builder.CreateInBoundsGEP(
          IndexArray->getValueType(), IndexArray, index_list);
      entry_builder.CreateStore(counter_index, global_index);
    }

    unsigned local_index = 0;
    for (std::vector<Edge>::const_iterator it = edges.begin(), end = edges.end();
         it != end; ++it, ++local_index) {
      it->insertDeltaIncrFn(local_counters[local_index]);
    }

    for (BasicBlock &BB : F) {
      auto *ret = dyn_cast<ReturnInst>(BB.getTerminator());
      if (!ret) {
        continue;
      }

      IRBuilder<> builder(ret);
      for (unsigned i = 0; i < local_counters.size(); ++i) {
        uint64_t global_index = static_cast<uint64_t>(Offset + i);
        Value *counter_index[] = {builder.getInt32(0),
                                  builder.getInt32(global_index)};
        auto *global_ptr = builder.CreateInBoundsGEP(
            CounterArray->getValueType(), CounterArray, counter_index);
        auto *old_value = builder.CreateLoad(int64Ty, global_ptr);
        auto *delta_value = builder.CreateLoad(int64Ty, local_counters[i]);
        auto *new_value =
            builder.CreateAdd(old_value, delta_value, "prue.delayed.update");
        auto *store = builder.CreateStore(new_value, global_ptr);
        store->setMetadata(kPrueUpdateMetadata,
                           makePrueMetadata(builder.getContext(), global_index));
      }
    }

    if (F.getName() == "main") {
      insertExitFn(M, F, CounterArray, IndexArray, NumEdges);
    }

    Offset += size;
  }

  return PreservedAnalyses::none();
}

PreservedAnalyses PruePass::run(Module &M, ModuleAnalysisManager &) {
  bool changed = false;

  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    std::vector<UpdateTask> roots;
    for (Instruction &inst : instructions(F)) {
      auto *store = dyn_cast<StoreInst>(&inst);
      if (!store) {
        continue;
      }

      uint64_t counter_index = 0;
      if (!extractCounterIndex(store, counter_index)) {
        continue;
      }

      GlobalVariable *counter_array = nullptr;
      uint64_t ptr_index = 0;
      if (!extractCounterLocation(store, counter_array, ptr_index)) {
        continue;
      }
      if (ptr_index != counter_index) {
        continue;
      }

      UpdateTask task;
      task.store = store;
      task.counter_array = counter_array;
      task.counter_index = counter_index;
      roots.push_back(task);
    }

    FunctionPRUEContext ctx(F);
    for (std::vector<UpdateTask>::const_iterator it = roots.begin(),
                                                end = roots.end();
         it != end; ++it) {
      std::deque<UpdateTask> worklist;
      worklist.push_back(*it);

      while (!worklist.empty()) {
        UpdateTask task = worklist.front();
        worklist.pop_front();
        if (!task.store || !task.store->getParent()) {
          continue;
        }

        ParsedUpdate update;
        if (!parseUpdateStore(task.store, task.counter_array, task.counter_index,
                              update)) {
          continue;
        }

        if (isZeroValue(update.operand)) {
          eraseUpdate(update);
          changed = true;
          continue;
        }

        BasicBlock *relocate_target = findRelocationTarget(update, ctx);
        if (relocate_target) {
          StoreInst *store = createUpdate(relocate_target, update.counter_array,
                                          update.counter_index, update.operand,
                                          false);
          eraseUpdate(update);
          UpdateTask subtask;
          subtask.store = store;
          subtask.counter_array = update.counter_array;
          subtask.counter_index = update.counter_index;
          worklist.push_back(subtask);
          changed = true;
          continue;
        }

        if (applySplit(update, worklist, ctx)) {
          changed = true;
          continue;
        }

        if (applyOffload(update, worklist, ctx)) {
          changed = true;
          continue;
        }

        if (applyUnpack(update, worklist, ctx)) {
          changed = true;
          continue;
        }
      }
    }
  }

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace nisse
