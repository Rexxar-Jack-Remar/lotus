#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"

#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace bootstrap {
namespace {
const llvm::Function *directCallee(const llvm::CallBase &call) {
  const llvm::Value *value = call.getCalledOperand()->stripPointerCasts();
  std::set<const llvm::Value *> seen;
  while (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(value)) {
    if (!seen.insert(value).second || !alias->getAliasee())
      return nullptr;
    value = alias->getAliasee()->stripPointerCasts();
  }
  return llvm::dyn_cast<llvm::Function>(value);
}
bool knownReadOnly(const llvm::Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const llvm::StringRef name = function->getName();
  return name == "strlen" || name == "strcmp" || name == "strncmp" ||
         name == "memcmp" || name == "strchr" || name == "strrchr" ||
         name == "strstr" || name == "strpbrk" || name == "memchr" ||
         name == "rawmemchr";
}
bool writesMemory(const llvm::CallBase &call) {
  return !knownReadOnly(directCallee(call)) && !call.doesNotAccessMemory() &&
         !call.onlyReadsMemory();
}
enum class AllocationKind { None, Fresh, Reallocate };
struct AllocationModel {
  AllocationKind kind = AllocationKind::None;
  bool mayBeNull = false;
};
AllocationModel allocationModel(const llvm::CallBase &call) {
  const llvm::Function *function = directCallee(call);
  if (!function || !function->isDeclaration() || !call.getType()->isPointerTy())
    return {};
  const llvm::StringRef name = function->getName();
  auto integerArguments = [&](unsigned count) {
    if (call.arg_size() != count)
      return false;
    for (unsigned index = 0; index < count; ++index)
      if (!call.getArgOperand(index)->getType()->isIntegerTy())
        return false;
    return true;
  };
  if ((name == "malloc" || name == "valloc") && integerArguments(1))
    return {AllocationKind::Fresh, true};
  if ((name == "calloc" || name == "aligned_alloc" || name == "memalign") &&
      integerArguments(2))
    return {AllocationKind::Fresh, true};
  if (name == "realloc" && call.arg_size() == 2 &&
      call.getArgOperand(0)->getType()->isPointerTy() &&
      call.getArgOperand(1)->getType()->isIntegerTy())
    return {AllocationKind::Reallocate, true};
  if ((name == "_Znwm" || name == "_Znam" || name == "_Znwj" ||
       name == "_Znaj") &&
      integerArguments(1))
    return {AllocationKind::Fresh, false};
  return {};
}
bool deallocationCall(const llvm::CallBase &call) {
  const llvm::Function *function = directCallee(call);
  return function && function->isDeclaration() &&
         function->getName() == "free" && call.arg_size() == 1 &&
         call.getArgOperand(0)->getType()->isPointerTy() &&
         call.getType()->isVoidTy();
}
bool returnsInteriorPointer(const llvm::Function *function) {
  if (!knownReadOnly(function) || !function->getReturnType()->isPointerTy())
    return false;
  const llvm::StringRef name = function->getName();
  return name == "strchr" || name == "strrchr" || name == "strstr" ||
         name == "strpbrk" || name == "memchr" || name == "rawmemchr";
}
std::string nameOf(const llvm::Value &value) {
  if (value.hasName())
    return value.getName().str();
  std::string result;
  llvm::raw_string_ostream out(result);
  value.printAsOperand(out, false);
  return out.str();
}
} // namespace

struct BootstrapAA::Impl {
  const llvm::Module &module;
  Program program;
  std::unique_ptr<Analysis> analysis;
  std::unordered_map<const llvm::Value *, Id> values;
  std::unordered_map<const llvm::Value *, Id> objects;
  std::unordered_map<const llvm::Function *, Id> functions;
  std::unordered_map<const llvm::BasicBlock *, Id> blocks;
  std::unordered_map<const llvm::Instruction *, Id> sites;
  std::vector<const llvm::Value *> allocations{nullptr, nullptr};
  std::set<const llvm::Constant *> resolving;
  std::vector<bool> recursive;
  bool null_may_be_valid = false;
  Id null_value = INVALID;

  Impl(const llvm::Module &m, const llvm::Function *entry, Options options)
      : module(m) {
    std::string diagnostics;
    llvm::raw_string_ostream out(diagnostics);
    if (llvm::verifyModule(module, &out))
      throw std::invalid_argument("BootstrapAA: invalid LLVM module: " +
                                  out.str());
    if (!entry)
      entry = module.getFunction("main");
    if (!entry || entry->getParent() != &module || entry->isDeclaration())
      throw std::invalid_argument(
          "BootstrapAA: supply a defined entry function (default: main)");
    for (const llvm::Function &function : module) {
      null_may_be_valid |=
          function.hasFnAttribute(llvm::Attribute::NullPointerIsValid);
      Id id = program.addFunction(function.getName().str(),
                                  function.isDeclaration());
      functions.emplace(&function, id);
      program.functions[id].writes_memory = !knownReadOnly(&function) &&
                                            !function.doesNotAccessMemory() &&
                                            !function.onlyReadsMemory();
      Object object;
      object.name = nameOf(function);
      object.kind = ObjectKind::Function;
      object.function = id;
      addObject(function, std::move(object));
    }
    program.entry = functions.at(entry);
    computeRecursion();
    const llvm::DataLayout &layout = module.getDataLayout();
    for (const llvm::GlobalVariable &global : module.globals()) {
      Object object;
      object.name = nameOf(global);
      object.kind = ObjectKind::Global;
      object.singleton = global.getValueType()->isPointerTy();
      if (object.singleton)
        object.bytes = layout.getPointerTypeSize(global.getValueType());
      addObject(global, std::move(object));
    }
    // Pre-allocate objects and SSA IDs before resolving constants or PHIs.
    for (const llvm::Function &function : module) {
      Id f = functions.at(&function);
      for (const llvm::Argument &argument : function.args()) {
        program.functions[f].parameters.push_back(valueId(argument));
        if (argument.hasByValAttr()) {
          Object object;
          object.name = function.getName().str() + ":byval:" + nameOf(argument);
          object.kind = ObjectKind::Stack;
          // Implicit by-value copies are NOT aliases of the caller's cell.
          // Unknown contents conservatively replace a byte-wise aggregate copy.
          addObject(argument, std::move(object));
        }
      }
      for (const llvm::BasicBlock &block : function) {
        blocks.emplace(&block, program.addBlock(f));
        for (const llvm::Instruction &instruction : block) {
          if (instruction.getType()->isPointerTy())
            valueId(instruction);
          if (const auto *alloca =
                  llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
            Object object;
            object.name = function.getName().str() + ":" + nameOf(instruction);
            object.kind = ObjectKind::Stack;
            const auto *count =
                llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize());
            bool scalar = alloca->getAllocatedType()->isPointerTy() && count &&
                          count->isOne();
            // Entry-block static allocas execute once per activation. Acyclic
            // call-graph membership excludes simultaneously live same-site
            // frames.
            object.singleton =
                scalar && alloca->isStaticAlloca() && !recursive[f];
            if (scalar)
              object.bytes =
                  layout.getPointerTypeSize(alloca->getAllocatedType());
            addObject(instruction, std::move(object));
          } else if (const auto *call =
                         llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            if (allocationModel(*call).kind != AllocationKind::None) {
              Object object;
              object.name =
                  function.getName().str() + ":" + nameOf(instruction);
              object.kind = ObjectKind::Heap;
              // An allocation site can stand for arbitrarily many heap objects.
              // Do not assume that zero-filled bytes are pointer null values.
              addObject(instruction, std::move(object));
            }
          }
        }
      }
    }
    for (const llvm::GlobalVariable &global : module.globals()) {
      valueId(global);
      if (global.getValueType()->isPointerTy() && global.hasInitializer() &&
          !global.isExternallyInitialized() &&
          !module.getNamedGlobal("llvm.global_ctors"))
        program.objects[objects.at(&global)].initial =
            constantPoints(*global.getInitializer());
    }
    for (const llvm::GlobalAlias &alias : module.aliases())
      valueId(alias);
    for (const llvm::Function &function : module) {
      valueId(function);
      if (!function.isDeclaration()) {
        for (const llvm::Argument &argument : function.args()) {
          if (!argument.hasByValAttr())
            continue;
          Instruction copy;
          copy.opcode = Opcode::Allocate;
          copy.object = objects.at(&argument);
          copy.result = valueId(argument);
          program.append(functions.at(&function), 0, copy);
        }
      }
      for (const llvm::BasicBlock &block : function) {
        for (const llvm::Instruction &instruction : block) {
          for (const llvm::Use &operand : instruction.operands())
            if (operand->getType()->isPointerTy())
              valueId(*operand);
          lower(function, block, instruction);
        }
      }
    }
    for (const llvm::Function &function : module) {
      for (const llvm::BasicBlock &block : function) {
        for (const llvm::BasicBlock *successor : llvm::successors(&block)) {
          Edge edge;
          edge.target = blocks.at(successor);
          for (const llvm::Instruction &instruction : *successor) {
            const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
            if (!phi)
              break;
            if (phi->getType()->isPointerTy())
              edge.phi.emplace_back(
                  valueId(*phi),
                  valueId(*phi->getIncomingValueForBlock(&block)));
          }
          program.functions[functions.at(&function)]
              .blocks[blocks.at(&block)]
              .successors.push_back(std::move(edge));
        }
      }
    }
    analysis = std::make_unique<Analysis>(program, options);
  }
  void addObject(const llvm::Value &allocation, Object object) {
    Id id = program.addObject(std::move(object));
    objects.emplace(&allocation, id);
    allocations.push_back(&allocation);
  }
  Id nullValue() {
    if (null_value == INVALID)
      null_value =
          program.addConstant(PointsToSet(NULL_OBJECT), "<null-value>");
    return null_value;
  }
  void computeRecursion() {
    std::vector<std::set<Id>> graph(program.functions.size());
    for (const llvm::Function &function : module)
      for (const llvm::BasicBlock &block : function)
        for (const llvm::Instruction &instruction : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            if (const llvm::Function *callee = directCallee(*call)) {
              if (!callee->isDeclaration())
                graph[functions.at(&function)].insert(functions.at(callee));
            } else {
              // Deliberately conservative: unknown/bitcasted indirect calls may
              // re-enter any defined function. This only disables strong
              // updates.
              for (const llvm::Function &candidate : module)
                if (!candidate.isDeclaration())
                  graph[functions.at(&function)].insert(
                      functions.at(&candidate));
            }
          }
    recursive.assign(graph.size(), false);
    for (Id f = 0; f < graph.size(); ++f) {
      std::vector<Id> work(graph[f].begin(), graph[f].end());
      std::set<Id> seen;
      while (!work.empty()) {
        Id current = work.back();
        work.pop_back();
        if (current == f) {
          recursive[f] = true;
          break;
        }
        if (seen.insert(current).second)
          work.insert(work.end(), graph[current].begin(), graph[current].end());
      }
    }
  }
  PointsToSet constantPoints(const llvm::Constant &constant) {
    auto object = objects.find(&constant);
    if (object != objects.end())
      return PointsToSet(object->second);
    if (llvm::isa<llvm::ConstantPointerNull>(constant)) {
      if (null_may_be_valid ||
          constant.getType()->getPointerAddressSpace() != 0)
        return PointsToSet::top();
      return PointsToSet(NULL_OBJECT);
    }
    if (!resolving.insert(&constant).second)
      return PointsToSet::top();
    PointsToSet result = PointsToSet::top();
    if (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(&constant)) {
      if (alias->getAliasee())
        result = constantPoints(*alias->getAliasee());
    } else if (const auto *gep = llvm::dyn_cast<llvm::GEPOperator>(&constant)) {
      if (gep->isInBounds() || gep->hasAllZeroIndices())
        if (const auto *base =
                llvm::dyn_cast<llvm::Constant>(gep->getPointerOperand()))
          result = constantPoints(*base);
    } else if (const auto *expression =
                   llvm::dyn_cast<llvm::ConstantExpr>(&constant)) {
      if (expression->getOpcode() == llvm::Instruction::BitCast)
        result = constantPoints(
            *llvm::cast<llvm::Constant>(expression->getOperand(0)));
    }
    resolving.erase(&constant);
    return result;
  }
  Id valueId(const llvm::Value &value) {
    if (!value.getType()->isPointerTy())
      return INVALID;
    auto it = values.find(&value);
    if (it != values.end())
      return it->second;
    Value normalized;
    normalized.name = nameOf(value);
    if (const auto *constant = llvm::dyn_cast<llvm::Constant>(&value)) {
      normalized.constant = true;
      normalized.initial = constantPoints(*constant);
    } else if (const auto *instruction =
                   llvm::dyn_cast<llvm::Instruction>(&value)) {
      normalized.function = functions.at(instruction->getFunction());
    } else if (const auto *argument = llvm::dyn_cast<llvm::Argument>(&value)) {
      normalized.function = functions.at(argument->getParent());
    } else {
      normalized.constant = true;
      normalized.initial = PointsToSet::top();
    }
    Id id = program.addValue(std::move(normalized));
    values.emplace(&value, id);
    return id;
  }
  void lower(const llvm::Function &function, const llvm::BasicBlock &block,
             const llvm::Instruction &instruction) {
    Instruction i;
    i.result = valueId(instruction);
    i.opcode = i.result == INVALID ? Opcode::Nop : Opcode::Unknown;
    if (llvm::isa<llvm::AllocaInst>(instruction)) {
      i.opcode = Opcode::Allocate;
      i.object = objects.at(&instruction);
    } else if (const auto *load =
                   llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Load;
        i.operands = {valueId(*load->getPointerOperand())};
        i.read_bytes =
            module.getDataLayout().getPointerTypeSize(load->getType());
      }
    } else if (const auto *store =
                   llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      i.operands = {valueId(*store->getPointerOperand())};
      if (store->getValueOperand()->getType()->isPointerTy()) {
        i.opcode = Opcode::Store;
        i.operands.push_back(valueId(*store->getValueOperand()));
        i.write_bytes = module.getDataLayout().getPointerTypeSize(
            store->getValueOperand()->getType());
      } else {
        // Integer/aggregate stores may overwrite part of a pointer cell.
        // Ignoring them or treating integer zero as null is not conservative in
        // general.
        i.opcode = Opcode::Havoc;
      }
    } else if (const auto *bitcast =
                   llvm::dyn_cast<llvm::BitCastInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*bitcast->getOperand(0))};
      }
    } else if (const auto *gep =
                   llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
      if (gep->isInBounds() || gep->hasAllZeroIndices()) {
        i.opcode = Opcode::Copy;
        i.operands = {valueId(*gep->getPointerOperand())};
      }
    } else if (const auto *select =
                   llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
      if (i.result != INVALID) {
        i.opcode = Opcode::Join;
        i.operands = {valueId(*select->getTrueValue()),
                      valueId(*select->getFalseValue())};
      }
    } else if (llvm::isa<llvm::PHINode>(instruction)) {
      i.opcode =
          Opcode::Nop; // Simultaneous assignments are on incoming CFG edges.
    } else if (const auto *memory =
                   llvm::dyn_cast<llvm::MemIntrinsic>(&instruction)) {
      const auto *length =
          llvm::dyn_cast<llvm::ConstantInt>(memory->getLength());
      if (length && length->isZero()) {
        i.opcode = Opcode::Nop;
      } else {
        i.opcode = Opcode::Havoc;
        i.operands = {valueId(*memory->getRawDest())};
      }
    } else if (const auto *call =
                   llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      const AllocationModel allocation = allocationModel(*call);
      if (allocation.kind != AllocationKind::None) {
        if (null_may_be_valid ||
            call->getType()->getPointerAddressSpace() != 0) {
          i.opcode = Opcode::Unknown;
        } else {
          i.opcode = Opcode::Allocate;
          i.object = objects.at(&instruction);
          i.may_be_null = allocation.mayBeNull;
          if (allocation.kind == AllocationKind::Reallocate)
            i.operands = {valueId(*call->getArgOperand(0))};
        }
      } else if (deallocationCall(*call)) {
        i.opcode =
            Opcode::Nop; // free does not assign null to the caller's pointer.
      } else {
        const llvm::Function *callee = directCallee(*call);
        if (returnsInteriorPointer(callee) && call->arg_size() != 0 &&
            call->getArgOperand(0)->getType()->isPointerTy()) {
          i.opcode = Opcode::Join;
          i.operands = {valueId(*call->getArgOperand(0)), nullValue()};
          sites.emplace(&instruction, program.append(functions.at(&function),
                                                     blocks.at(&block), i));
          return;
        }
        llvm::StringRef name = callee ? callee->getName() : llvm::StringRef();
        if (name.startswith("llvm.dbg.") || name.startswith("llvm.lifetime.") ||
            name == "llvm.assume") {
          i.opcode = Opcode::Nop;
        } else {
          i.opcode = Opcode::Call;
          i.writes_memory = writesMemory(*call);
          if (callee)
            i.callee = functions.at(callee);
          else if (!call->isInlineAsm())
            i.indirect_target = valueId(*call->getCalledOperand());
          for (const llvm::Use &argument : call->args())
            i.operands.push_back(valueId(*argument));
          // Precise normal-return effects plus an opaque alternative. Do not
          // propagate only normal-exit memory down exceptional successor edges.
          i.opaque_alternative = llvm::isa<llvm::InvokeInst>(call) ||
                                 llvm::isa<llvm::CallBrInst>(call);
        }
      }
    } else if (const auto *atomic =
                   llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
      i.opcode = Opcode::Havoc;
      i.operands = {valueId(*atomic->getPointerOperand())};
    } else if (const auto *atomic =
                   llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction)) {
      i.opcode = Opcode::Havoc;
      i.operands = {valueId(*atomic->getPointerOperand())};
    } else if (const auto *ret =
                   llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
      i.opcode = Opcode::Return;
      if (ret->getReturnValue() &&
          ret->getReturnValue()->getType()->isPointerTy())
        i.operands = {valueId(*ret->getReturnValue())};
    } else if (llvm::isa<llvm::ResumeInst>(instruction) ||
               (llvm::isa<llvm::CleanupReturnInst>(instruction) &&
                llvm::cast<llvm::CleanupReturnInst>(instruction)
                    .unwindsToCaller())) {
      // Treat exceptional exits as possible summary exits; invoke callers also
      // get the opaque alternative above. This can add, but cannot drop,
      // effects.
      i.opcode = Opcode::Return;
    }
    // freeze, inttoptr, addrspacecast, va_arg, extractvalue/extractelement and
    // other unsupported pointer producers deliberately remain Unknown.
    sites.emplace(&instruction, program.append(functions.at(&function),
                                               blocks.at(&block), i));
  }
  Id queryValue(const llvm::Value &value) const {
    auto it = values.find(&value);
    if (it == values.end())
      throw std::invalid_argument(
          "BootstrapAA: query must name a scalar pointer in this module");
    return it->second;
  }
  Id querySite(const llvm::Instruction &instruction) const {
    auto it = sites.find(&instruction);
    if (it == sites.end())
      throw std::invalid_argument(
          "BootstrapAA: query site is in another module");
    return it->second;
  }
  Context context(const CallContext &path) const {
    Context result;
    for (const llvm::CallBase *call : path) {
      if (!call)
        throw std::invalid_argument("BootstrapAA: null call site in context");
      result.push_back(querySite(*call));
    }
    return result;
  }
};

BootstrapAA::BootstrapAA(const llvm::Module &module,
                         const llvm::Function *entry, Options options)
    : m_impl(std::make_unique<Impl>(module, entry, options)) {}
BootstrapAA::~BootstrapAA() = default;
BootstrapAA::BootstrapAA(BootstrapAA &&) noexcept = default;
BootstrapAA &BootstrapAA::operator=(BootstrapAA &&) noexcept = default;
QueryResult BootstrapAA::pointsTo(const llvm::Value &value,
                                  const llvm::Instruction &site,
                                  const CallContext &context, Point point) {
  return m_impl->analysis->pointsTo(m_impl->queryValue(value),
                                    m_impl->querySite(site),
                                    m_impl->context(context), point);
}
QueryResult BootstrapAA::pointsToAllContexts(const llvm::Value &value,
                                             const llvm::Instruction &site,
                                             Point point) {
  return m_impl->analysis->pointsToAllContexts(m_impl->queryValue(value),
                                               m_impl->querySite(site), point);
}
bool BootstrapAA::mayAlias(const llvm::Value &lhs, const llvm::Value &rhs,
                           const llvm::Instruction &site,
                           const CallContext &context) {
  return m_impl->analysis->mayAlias(
      m_impl->queryValue(lhs), m_impl->queryValue(rhs), m_impl->querySite(site),
      m_impl->context(context));
}
const llvm::Value *BootstrapAA::allocationSite(Id object) const {
  return m_impl->allocations.at(object);
}
Id BootstrapAA::objectId(const llvm::Value &allocation) const {
  auto it = m_impl->objects.find(&allocation);
  return it == m_impl->objects.end() ? INVALID : it->second;
}
const Object &BootstrapAA::objectInfo(Id object) const {
  return m_impl->program.objects.at(object);
}
const Statistics &BootstrapAA::statistics() const {
  return m_impl->analysis->statistics();
}
const SteensgaardHierarchy &BootstrapAA::hierarchy() const {
  return m_impl->analysis->hierarchy();
}

} // namespace bootstrap
} // namespace lotus
