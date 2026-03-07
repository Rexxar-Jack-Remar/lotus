/*
 * Taint analysis based on the NPA framework
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralTaint.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Annotation/Taint/TaintConfigManager.h"
#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

namespace npa {

struct MemKey {
  const llvm::Value *base = nullptr;
  const llvm::Value *ptr = nullptr;
  int64_t offset = 0;
  bool unknown = false;
};

struct MemKeyHash {
  size_t operator()(const MemKey &k) const {
    size_t h = 0;
    h ^= std::hash<const void *>{}(k.base) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(k.offset) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<bool>{}(k.unknown) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

struct MemKeyEq {
  bool operator()(const MemKey &a, const MemKey &b) const {
    return a.base == b.base && a.offset == b.offset && a.unknown == b.unknown;
  }
};

class TaintInfo {
public:
  TaintInfo(llvm::Module &M, lotus::AliasAnalysisWrapper &aa)
      : module(M), aliasAnalysis(aa), dataLayout(M.getDataLayout()) {
    buildUniverse();
  }

  unsigned getBitWidth() const { return nextBit; }

  bool hasValueBit(const llvm::Value *v) const {
    return valueBits.find(v) != valueBits.end();
  }

  unsigned getValueBit(const llvm::Value *v) const {
    auto it = valueBits.find(v);
    if (it == valueBits.end())
      return invalidBit();
    return it->second;
  }

  unsigned getMemBitForPtr(const llvm::Value *ptr) const {
    MemKey key = buildMemKey(ptr);
    auto it = memBits.find(key);
    if (it == memBits.end())
      return invalidBit();
    return it->second;
  }

  std::vector<unsigned> getAliasMemBits(const llvm::Value *ptr) const {
    std::vector<unsigned> out;
    if (!ptr || !ptr->getType()->isPointerTy())
      return out;

    MemKey key = buildMemKey(ptr);
    for (const auto &entry : memBits) {
      const MemKey &cand = entry.first;
      if (mayAlias(key, cand))
        out.push_back(entry.second);
    }
    return out;
  }

  const llvm::DataLayout &getDataLayout() const { return dataLayout; }

  static unsigned invalidBit() { return static_cast<unsigned>(-1); }

private:
  llvm::Module &module;
  lotus::AliasAnalysisWrapper &aliasAnalysis;
  const llvm::DataLayout &dataLayout;
  unsigned nextBit = 0;
  std::unordered_map<const llvm::Value *, unsigned> valueBits;
  std::unordered_map<MemKey, unsigned, MemKeyHash, MemKeyEq> memBits;

  void addValueBit(const llvm::Value *v) {
    if (!v)
      return;
    if (valueBits.count(v))
      return;
    valueBits.emplace(v, nextBit++);
  }

  void addMemBit(const llvm::Value *ptr) {
    if (!ptr || !ptr->getType()->isPointerTy())
      return;
    MemKey key = buildMemKey(ptr);
    if (memBits.count(key))
      return;
    memBits.emplace(key, nextBit++);
  }

  MemKey buildMemKey(const llvm::Value *ptr) const {
    MemKey key;
    key.ptr = ptr;
    if (!ptr)
      return key;
    key.base = llvm::getUnderlyingObject(ptr);
    key.offset = 0;
    key.unknown = false;

    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(ptr)) {
      llvm::APInt offAP(64, 0, true);
      if (gep->accumulateConstantOffset(dataLayout, offAP)) {
        key.offset = offAP.getSExtValue();
      } else {
        key.unknown = true;
      }
    }

    return key;
  }

  bool mayAlias(const MemKey &a, const MemKey &b) const {
    if (!a.ptr || !b.ptr)
      return false;
    bool baseAlias = (a.base == b.base);
    if (!baseAlias)
      baseAlias = aliasAnalysis.mayAlias(a.ptr, b.ptr);
    if (!baseAlias)
      return false;
    if (a.unknown || b.unknown)
      return true;
    return a.offset == b.offset;
  }

  void buildUniverse() {
    for (auto &F : module) {
      for (auto &Arg : F.args()) {
        addValueBit(&Arg);
        if (Arg.getType()->isPointerTy())
          addMemBit(&Arg);
      }
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (!I.getType()->isVoidTy()) {
            addValueBit(&I);
            if (I.getType()->isPointerTy())
              addMemBit(&I);
          }
          if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            addMemBit(load->getPointerOperand());
          } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            addMemBit(store->getPointerOperand());
          } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            for (auto &arg : call->args()) {
              if (arg->getType()->isPointerTy())
                addMemBit(arg.get());
            }
            if (call->getType()->isPointerTy())
              addMemBit(call);
          } else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
            addMemBit(gep);
          }
        }
      }
    }
  }
};

class TaintAnalysis {
public:
  using FactType = llvm::APInt;

  TaintAnalysis(llvm::Module &M, lotus::AliasAnalysisWrapper &aa)
      : module(M), info(M, aa), aliasAnalysis(aa) {
    bitWidth = info.getBitWidth();
    if (bitWidth == 0)
      bitWidth = 1;
    TaintTransferDomain::setBitWidth(bitWidth);
    entryFacts = llvm::APInt(bitWidth, 0);
    initializeEntryFacts();
  }

  FactType getEntryValue() const { return entryFacts; }

  using D = TaintTransferDomain;
  using Exp = Exp0<D>;
  using E = E0<D>;

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I))
      return currentPath;

    D::value_type transfer = D::one();
    bool updated = false;

    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      const llvm::Value *value = store->getValueOperand();
      const llvm::Value *ptr = store->getPointerOperand();

      unsigned valueBit = info.getValueBit(value);
      if (valueBit != TaintInfo::invalidBit()) {
        for (unsigned memBit : info.getAliasMemBits(ptr)) {
          D::addEdge(transfer, valueBit, memBit);
          updated = true;
        }
      }

      if (valueBit != TaintInfo::invalidBit()) {
        for (unsigned memBit : info.getAliasMemBits(ptr)) {
          D::addEdge(transfer, memBit, valueBit);
          updated = true;
        }
      }
    } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      const llvm::Value *ptr = load->getPointerOperand();
      unsigned ptrBit = info.getValueBit(ptr);
      unsigned loadBit = info.getValueBit(load);

      if (ptrBit != TaintInfo::invalidBit() &&
          loadBit != TaintInfo::invalidBit()) {
        D::addEdge(transfer, ptrBit, loadBit);
        updated = true;
      }

      for (unsigned memBit : info.getAliasMemBits(ptr)) {
        D::addEdge(transfer, memBit, loadBit);
        updated = true;
      }
    } else if (auto *binop = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      unsigned lhsBit = info.getValueBit(binop->getOperand(0));
      unsigned rhsBit = info.getValueBit(binop->getOperand(1));
      unsigned outBit = info.getValueBit(binop);
      if (outBit != TaintInfo::invalidBit()) {
        if (lhsBit != TaintInfo::invalidBit()) {
          D::addEdge(transfer, lhsBit, outBit);
          updated = true;
        }
        if (rhsBit != TaintInfo::invalidBit()) {
          D::addEdge(transfer, rhsBit, outBit);
          updated = true;
        }
      }
    } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      unsigned inBit = info.getValueBit(cast->getOperand(0));
      unsigned outBit = info.getValueBit(cast);
      if (inBit != TaintInfo::invalidBit() &&
          outBit != TaintInfo::invalidBit()) {
        D::addEdge(transfer, inBit, outBit);
        updated = true;
      }
    } else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
      unsigned inBit = info.getValueBit(gep->getPointerOperand());
      unsigned outBit = info.getValueBit(gep);
      if (inBit != TaintInfo::invalidBit() &&
          outBit != TaintInfo::invalidBit()) {
        D::addEdge(transfer, inBit, outBit);
        updated = true;
      }
    } else if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      unsigned outBit = info.getValueBit(phi);
      if (outBit != TaintInfo::invalidBit()) {
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
          unsigned inBit = info.getValueBit(phi->getIncomingValue(i));
          if (inBit != TaintInfo::invalidBit()) {
            D::addEdge(transfer, inBit, outBit);
            updated = true;
          }
        }
      }
    }

    if (!updated)
      return currentPath;
    return Exp::seq(transfer, currentPath);
  }

  D::value_type getCallEntryTransfer(const llvm::CallBase &call,
                                     const llvm::Function &callee) {
    D::value_type transfer = D::one();
    unsigned numArgs = static_cast<unsigned>(
        std::min<size_t>(call.arg_size(), callee.arg_size()));
    const auto *paramIt = callee.arg_begin();
    for (unsigned i = 0; i < numArgs; ++i, ++paramIt) {
      const llvm::Value *arg = call.getArgOperand(i);
      unsigned argBit = info.getValueBit(arg);
      unsigned paramBit = info.getValueBit(&*paramIt);
      if (argBit != TaintInfo::invalidBit() &&
          paramBit != TaintInfo::invalidBit()) {
        D::addEdge(transfer, argBit, paramBit);
      }

      if (arg && arg->getType()->isPointerTy()) {
        unsigned paramMemBit = info.getMemBitForPtr(&*paramIt);
        if (paramMemBit != TaintInfo::invalidBit()) {
          for (unsigned memBit : info.getAliasMemBits(arg)) {
            D::addEdge(transfer, memBit, paramMemBit);
          }
        }
      }
    }
    return transfer;
  }

  D::value_type getCallReturnTransfer(const llvm::CallBase &call,
                                      const llvm::Function &callee) {
    D::value_type transfer = D::one();
    if (!call.getType()->isVoidTy()) {
      unsigned callBit = info.getValueBit(&call);
      if (callBit != TaintInfo::invalidBit()) {
        for (const auto &BB : callee) {
          if (auto *ret =
                  llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
            if (const llvm::Value *retVal = ret->getReturnValue()) {
              unsigned retBit = info.getValueBit(retVal);
              if (retBit != TaintInfo::invalidBit()) {
                D::addEdge(transfer, retBit, callBit);
              }
            }
          }
        }
      }
    }
    addSourceSpecs(call, transfer);
    addPipeSpecs(call, transfer);
    return transfer;
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &call) {
    D::value_type transfer = D::one();
    addSourceSpecs(call, transfer);
    addPipeSpecs(call, transfer);
    applySanitizers(call, transfer);
    return transfer;
  }

  FactType applySummary(const D::value_type &summary, const FactType &fact) {
    return D::apply(summary, fact);
  }

  FactType joinFacts(const FactType &a, const FactType &b) { return a | b; }

  bool factsEqual(const FactType &a, const FactType &b) { return a == b; }

private:
  llvm::Module &module;
  TaintInfo info;
  lotus::AliasAnalysisWrapper &aliasAnalysis;
  unsigned bitWidth = 1;
  FactType entryFacts;

  void initializeEntryFacts() {
    if (auto *Main = module.getFunction("main")) {
      for (auto &Arg : Main->args()) {
        if (Arg.getType()->isPointerTy()) {
          unsigned bit = info.getValueBit(&Arg);
          if (bit != TaintInfo::invalidBit())
            entryFacts.setBit(bit);
        }
      }
    }
  }

  void addSourceSpecs(const llvm::CallBase &call, D::value_type &transfer) {
    const llvm::Function *callee = call.getCalledFunction();
    if (!callee)
      return;
    std::string funcName =
        taint_config::normalize_name(callee->getName().str());
    const FunctionTaintConfig *cfg =
        taint_config::get_function_config(funcName);
    if (!cfg || !cfg->has_source_specs())
      return;

    for (const auto &spec : cfg->source_specs) {
      if (spec.location == TaintSpec::RET &&
          spec.access_mode == TaintSpec::VALUE) {
        unsigned bit = info.getValueBit(&call);
        if (bit != TaintInfo::invalidBit())
          D::addGen(transfer, bit);
      } else if (spec.location == TaintSpec::ARG &&
                 (spec.access_mode == TaintSpec::DIRECT_DEREF ||
                  spec.access_mode == TaintSpec::REACHABLE_DEREF)) {
        if (spec.arg_index >= 0 && spec.arg_index < (int)(call.arg_size())) {
          const llvm::Value *arg = call.getArgOperand(spec.arg_index);
          for (unsigned memBit : info.getAliasMemBits(arg)) {
            D::addGen(transfer, memBit);
          }
        }
      } else if (spec.location == TaintSpec::AFTER_ARG &&
                 (spec.access_mode == TaintSpec::DIRECT_DEREF ||
                  spec.access_mode == TaintSpec::REACHABLE_DEREF)) {
        int startIdx = spec.arg_index + 1;
        if (startIdx < 0)
          startIdx = 0;
        unsigned start = static_cast<unsigned>(startIdx);
        for (unsigned i = start; i < call.arg_size(); ++i) {
          const llvm::Value *arg = call.getArgOperand(i);
          for (unsigned memBit : info.getAliasMemBits(arg)) {
            D::addGen(transfer, memBit);
          }
        }
      }
    }
  }

  void addPipeSpecs(const llvm::CallBase &call, D::value_type &transfer) {
    const llvm::Function *callee = call.getCalledFunction();
    if (!callee)
      return;
    std::string funcName =
        taint_config::normalize_name(callee->getName().str());
    const FunctionTaintConfig *cfg =
        taint_config::get_function_config(funcName);
    if (!cfg || !cfg->has_pipe_specs())
      return;

    for (const auto &pipe : cfg->pipe_specs) {
      std::vector<unsigned> fromBits;
      if (pipe.from.location == TaintSpec::ARG) {
        int idx = pipe.from.arg_index;
        if (idx >= 0 && idx < (int)call.arg_size()) {
          const llvm::Value *arg = call.getArgOperand(idx);
          if (pipe.from.access_mode == TaintSpec::VALUE) {
            unsigned bit = info.getValueBit(arg);
            if (bit != TaintInfo::invalidBit())
              fromBits.push_back(bit);
          } else {
            auto memBits = info.getAliasMemBits(arg);
            fromBits.insert(fromBits.end(), memBits.begin(), memBits.end());
          }
        }
      }

      if (fromBits.empty())
        continue;

      if (pipe.to.location == TaintSpec::RET) {
        if (pipe.to.access_mode == TaintSpec::VALUE) {
          unsigned bit = info.getValueBit(&call);
          if (bit != TaintInfo::invalidBit()) {
            for (unsigned from : fromBits)
              D::addEdge(transfer, from, bit);
          }
        } else {
          for (unsigned memBit : info.getAliasMemBits(&call)) {
            for (unsigned from : fromBits)
              D::addEdge(transfer, from, memBit);
          }
        }
      } else if (pipe.to.location == TaintSpec::ARG) {
        int idx = pipe.to.arg_index;
        if (idx >= 0 && idx < (int)call.arg_size()) {
          const llvm::Value *arg = call.getArgOperand(idx);
          if (pipe.to.access_mode == TaintSpec::VALUE) {
            unsigned bit = info.getValueBit(arg);
            if (bit != TaintInfo::invalidBit()) {
              for (unsigned from : fromBits)
                D::addEdge(transfer, from, bit);
            }
          } else {
            for (unsigned memBit : info.getAliasMemBits(arg)) {
              for (unsigned from : fromBits)
                D::addEdge(transfer, from, memBit);
            }
          }
        }
      }
    }
  }

  void applySanitizers(const llvm::CallBase &call, D::value_type &transfer) {
    const llvm::Function *callee = call.getCalledFunction();
    if (!callee)
      return;
    static const std::unordered_set<std::string> sanitizers = {
        "strlen", "strcmp", "strncmp", "isdigit", "isalpha"};
    if (!sanitizers.count(callee->getName().str()))
      return;

    for (unsigned i = 0; i < call.arg_size(); ++i) {
      const llvm::Value *arg = call.getArgOperand(i);
      unsigned bit = info.getValueBit(arg);
      if (bit != TaintInfo::invalidBit()) {
        transfer.rel[bit].clearBit(bit);
      }
    }
  }
};

InterproceduralTaint::Result InterproceduralTaint::run(
    llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis, bool verbose) {
  if (!taint_config::load_default_config()) {
    llvm::errs() << "Error: Could not load taint configuration\n";
  }

  TaintAnalysis analysis(M, aliasAnalysis);
  auto engineResult =
      InterproceduralEngine<TaintTransferDomain, TaintAnalysis>::run(M,
                                                                     analysis,
                                                                     verbose);

  InterproceduralTaint::Result res;
  res.summaries.insert(engineResult.summaries.begin(),
                       engineResult.summaries.end());
  for (auto &kv : engineResult.blockEntryFacts)
    res.blockFacts[kv.first] = kv.second;
  return res;
}

} // namespace npa
