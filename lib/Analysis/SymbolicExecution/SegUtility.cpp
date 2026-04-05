#include "Analysis/SymbolicExecution/SegUtility.h"

#include "Analysis/SymbolicExecution/MemoryAPI.h"
#include "Analysis/SymbolicExecution/ProgramVar.h"
#include "Analysis/SymbolicExecution/TaintModel.h"

#include <array>
#include <sstream>

using namespace llvm;
using namespace SymbolicExecution;

namespace seg_utility {

namespace {

struct AnalysisInterface {
  const DataLayout *dl{nullptr};
  GuardedValueFlowBuilderPass *builder{nullptr};
  ExternalMemorySpecCompat memspec;
} Analysis;

std::vector<Function *> FuncSeq;
std::unordered_map<Function *, unsigned> FunDepthMap;
std::unordered_map<Function *, std::unordered_set<Function *>> CallInfoTopDown;
std::unordered_map<Function *, std::unordered_set<Function *>> CallInfoBottomUp;

Function *getDirectCallee(const CallBase *CB) {
  if (!CB)
    return nullptr;

  if (Function *Callee = CB->getCalledFunction())
    return Callee;

  Value *CalledOperand = CB->getCalledOperand();
  return dyn_cast_or_null<Function>(
      CalledOperand ? CalledOperand->stripPointerCasts() : nullptr);
}

} // namespace

std::vector<int>
ExternalMemorySpecCompat::getHeapAllocSize(const CallBase *CB) const {
  const auto *API = AllocatorAPI::get(getDirectCallee(CB));
  return API ? API->getSizeArgs() : std::vector<int>{};
}

bool ExternalMemorySpecCompat::isPureLib(const Function *F) const {
  return F && (F->doesNotAccessMemory() || F->onlyReadsMemory());
}

void initAnalysisInterface(Module *M, const DataLayout *DL,
                           GuardedValueFlowBuilderPass *builder) {
  (void)M;
  Analysis.dl = DL;
  Analysis.builder = builder;
}

TaintModel *getTaintSpec() {
  static TaintModel model;
  return &model;
}

GuardedValueFlowGraph *getGraph(Function *func) {
  if (!Analysis.builder || !func || !Analysis.builder->hasGraphFor(*func))
    return nullptr;
  return &Analysis.builder->getGraph(*func);
}

std::vector<std::pair<GuardedValueFlowNode *, GuardedValueFlowRegionNode *>>
getIncomingValuesForLoad(const GuardedValueFlowNode *node) {
  std::vector<std::pair<GuardedValueFlowNode *, GuardedValueFlowRegionNode *>>
      result;
  if (!node)
    return result;

  for (const auto &match : node->getMatchingRegions()) {
    auto *producer = dyn_cast_or_null<GuardedValueFlowNode>(match.producer);
    auto *region = match.region;
    if (producer && region)
      result.emplace_back(producer, region);
  }
  return result;
}

Function *getEnclosingFunc(const Var &v) {
  ProgramValuePtr value = v.getValue();
  if (value.isVacuous())
    return nullptr;
  if (Value *llvm_value = value.getLLVMVal()) {
    if (auto *inst = dyn_cast<Instruction>(llvm_value))
      return inst->getFunction();
    if (auto *arg = dyn_cast<Argument>(llvm_value))
      return arg->getParent();
  }
  if (value.isa<GuardedValueFlowNodeValue>()) {
    auto *node = value.getAs<GuardedValueFlowNodeValue>()->getNode();
    if (node && node->getParentBasicBlock())
      return node->getParentBasicBlock()->getParent();
  }
  return nullptr;
}

Function *getCallee(Instruction *I) {
  auto *CB = dyn_cast_or_null<CallBase>(I);
  return getDirectCallee(CB);
}

bool isDefiniteCall(Instruction *I) {
  Function *Callee = getCallee(I);
  return Callee && !Callee->isDeclaration();
}

bool isMatchLib(CallInst *CallI, const std::string &callee_name,
                const std::string &lib_name) {
  if (!CallI)
    return false;
  Function *callee = CallI->getCalledFunction();
  if (!callee)
    return false;
  std::string actual = callee->getName().str();
  return actual.find(callee_name) != std::string::npos &&
         actual.find(lib_name) != std::string::npos;
}

bool isKnownLib(const std::string &lib_name) {
  static const std::unordered_set<std::string> known = {"memset",
                                                        "snprintf",
                                                        "memcpy",
                                                        "memmove",
                                                        "fread",
                                                        "fwrite",
                                                        "memchr",
                                                        "strchr",
                                                        "strcpy",
                                                        "puts",
                                                        "strncpy",
                                                        "strncmp",
                                                        "strstr",
                                                        "strcasestr",
                                                        "strcmp",
                                                        "strtok",
                                                        "strlen",
                                                        "recv",
                                                        "recvfrom",
                                                        "read",
                                                        "readlink",
                                                        "pathconf",
                                                        "strnlen",
                                                        "strncat",
                                                        "strcat",
                                                        "free",
                                                        "_ZdlPv",
                                                        "_ZdaPv",
                                                        "malloc",
                                                        "calloc",
                                                        "realloc",
                                                        "memalign",
                                                        "aligned_alloc",
                                                        "pvalloc",
                                                        "valloc",
                                                        "strndup",
                                                        "_Znwj",
                                                        "_ZnwjRKSt9nothrow_t",
                                                        "_Znwm",
                                                        "_ZnwmRKSt9nothrow_t",
                                                        "_Znaj",
                                                        "_ZnajRKSt9nothrow_t",
                                                        "_Znam",
                                                        "_ZnamRKSt9nothrow_t",
                                                        "kmalloc",
                                                        "kzalloc",
                                                        "devm_kmalloc",
                                                        "devm_kzalloc",
                                                        "kmalloc_array",
                                                        "kcalloc",
                                                        "krealloc",
                                                        "krealloc_array",
                                                        "kmalloc_node",
                                                        "__kmalloc_node",
                                                        "kmalloc_array_node",
                                                        "kcalloc_node",
                                                        "kzalloc_node",
                                                        "kvmalloc",
                                                        "kvmalloc_node",
                                                        "kvzalloc_node",
                                                        "kvzalloc",
                                                        "kvmalloc_array",
                                                        "kvcalloc",
                                                        "kvrealloc"};
  static const std::array<std::string, 3> known_prefixes = {
      "llvm.memset", "llvm.memcpy", "llvm.memmove"};

  if (known.count(lib_name) != 0)
    return true;

  return llvm::any_of(known_prefixes, [&](const std::string &prefix) {
    return lib_name.rfind(prefix, 0) == 0;
  });
}

std::set<Var> getTaintedVars(Instruction *I, TaintModel *TaintSpec) {
  std::set<Var> Result;
  if (!I || !TaintSpec) {
    return Result;
  }

  auto *CB = dyn_cast<CallBase>(I);
  if (!CB) {
    return Result;
  }

  Function *Callee = CB->getCalledFunction();
  if (!Callee) {
    return Result;
  }

  GuardedValueFlowGraph *Graph = getGraph(I->getFunction());
  if (!Graph) {
    return Result;
  }

  auto AddValue = [&](Value *V) {
    if (!V) {
      return;
    }

    V = V->stripPointerCasts();
    if (auto *Node = Graph->findNode(V)) {
      Result.insert(Var(Node));
    }
  };

  if (!CB->getType()->isVoidTy() && TaintSpec->isFunctionRetAsSource(Callee)) {
    AddValue(CB);
  }

  const std::vector<int> *SourceArgs =
      TaintSpec->getTaintSourceArguments(Callee);
  if (!SourceArgs) {
    return Result;
  }

  for (int ArgIdx : *SourceArgs) {
    if (ArgIdx >= 0) {
      if (static_cast<unsigned>(ArgIdx) < CB->arg_size()) {
        AddValue(CB->getArgOperand(static_cast<unsigned>(ArgIdx)));
      }
      continue;
    }

    auto FirstVariadicIdx = static_cast<unsigned>(-ArgIdx);
    for (unsigned I = FirstVariadicIdx; I < CB->arg_size(); ++I) {
      AddValue(CB->getArgOperand(I));
    }
  }

  return Result;
}

void getTopoOrder(Module &M) {
  FuncSeq.clear();
  FunDepthMap.clear();
  CallInfoTopDown.clear();
  CallInfoBottomUp.clear();

  for (Function &F : M) {
    CallInfoTopDown[&F];
    CallInfoBottomUp[&F];
  }

  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        Function *Callee = CB ? CB->getCalledFunction() : nullptr;
        if (!Callee)
          continue;
        CallInfoTopDown[&F].insert(Callee);
        CallInfoBottomUp[Callee].insert(&F);
      }
    }
  }

  std::unordered_set<Function *> visited;
  std::unordered_set<Function *> in_stack;

  std::function<void(Function *)> dfs = [&](Function *F) {
    visited.insert(F);
    in_stack.insert(F);
    for (Function *Next : CallInfoTopDown[F]) {
      if (!visited.count(Next))
        dfs(Next);
    }
    in_stack.erase(F);
    FuncSeq.push_back(F);
  };

  for (Function &F : M) {
    if (!visited.count(&F))
      dfs(&F);
  }

  std::reverse(FuncSeq.begin(), FuncSeq.end());

  for (auto it = FuncSeq.rbegin(); it != FuncSeq.rend(); ++it) {
    Function *F = *it;
    unsigned max_depth = 0;
    for (Function *Callee : CallInfoTopDown[F]) {
      auto depth_it = FunDepthMap.find(Callee);
      if (depth_it != FunDepthMap.end())
        max_depth = std::max(max_depth, depth_it->second + 1);
    }
    FunDepthMap[F] = max_depth;
  }
}

const std::vector<Function *> &getFuncSeq() { return FuncSeq; }

unsigned getFunctionDepth(Function *func) {
  auto it = FunDepthMap.find(func);
  return it == FunDepthMap.end() ? 0u : it->second;
}

bool isFunctionTopLevel(Function *func) {
  auto it = CallInfoBottomUp.find(func);
  return it == CallInfoBottomUp.end() || it->second.empty();
}

ExternalMemorySpecCompat *getMemSpec() { return &Analysis.memspec; }

size_t hashHelper(const std::vector<size_t> &hash_vals) {
  size_t seed = 0;
  for (size_t value : hash_vals) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  }
  return seed;
}

std::string ptrToString(const void *ptr) {
  std::ostringstream os;
  os << ptr;
  return os.str();
}

const DataLayout *getDL() { return Analysis.dl; }

uint64_t getTypeSizeInBits(Type *Ty) {
  if (!Analysis.dl || !Ty || !Ty->isSized())
    return 0;
  return Analysis.dl->getTypeSizeInBits(Ty);
}

uint64_t getTypeStoreSize(Type *Ty) {
  if (!Analysis.dl || !Ty || !Ty->isSized())
    return 0;
  return Analysis.dl->getTypeStoreSize(Ty);
}

uint64_t getTypeStoreSizeInBits(Type *Ty) { return getTypeStoreSize(Ty) * 8; }

uint64_t getElementOffset(StructType *St, unsigned Idx) {
  if (!Analysis.dl || !St || Idx >= St->getNumElements())
    return 0;
  return Analysis.dl->getStructLayout(St)->getElementOffset(Idx);
}

} // namespace seg_utility
