#include "IR/PDG/Analysis/TaintQuery.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Analysis/FunctionFacts.h"
#include "IR/PDG/Analysis/ValueFacts.h"
#include "IR/PDG/Core/CallWrapper.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <tuple>

using namespace llvm;

namespace pdg {
namespace taint_detail {

enum class Kind { Zero, Value, Memory };
struct Fact {
  Kind kind = Kind::Zero;
  const Value *root = nullptr;
  int64_t offset = 0;
  uint64_t width = 0;
  bool unknown_offset = false;
  bool pointer_slot = false;
  const Value *source = nullptr;
  const Instruction *concat = nullptr;
  bool nonconstant = false;
  bool numeric = false;
  bool sql_safe = false;
  bool allocation_safe = false;

  auto key() const {
    return std::make_tuple(kind, reinterpret_cast<uintptr_t>(root), offset,
                           width, unknown_offset, pointer_slot,
                           reinterpret_cast<uintptr_t>(source),
                           reinterpret_cast<uintptr_t>(concat), nonconstant,
                           numeric, sql_safe, allocation_safe);
  }
  bool operator<(const Fact &other) const { return key() < other.key(); }
  bool operator==(const Fact &other) const { return key() == other.key(); }
  bool operator!=(const Fact &other) const { return !(*this == other); }
};
} // namespace taint_detail
} // namespace pdg

namespace pdg {
namespace taint_detail {

struct Location {
  const Value *root = nullptr;
  int64_t offset = 0;
  bool unknown = false;
};

class TransferFunctions {
public:
  using FactSet = std::set<Fact>;
  TransferFunctions(const Module &module, const TaintPolicy &policy)
      : module_(module), policy_(policy) {}
  Fact zero_fact() const { return {}; }

  Location location(const Value *pointer) const {
    if (!pointer || !pointer->getType()->isPointerTy())
      return {};
    int64_t offset = 0;
    const Value *base = GetPointerBaseWithConstantOffset(
        pointer, offset, module_.getDataLayout());
    const Value *root = getUnderlyingObject(base);
    return {root, base == root ? offset : 0, base != root};
  }
  uint64_t bytes(Type *type) const {
    if (!type->isSized())
      return 0;
    auto size = module_.getDataLayout().getTypeStoreSize(type);
    return size.isScalable() ? 0 : size.getFixedValue();
  }
  Fact memory(const Value *pointer, const Fact &origin, uint64_t width = 0,
              bool slot = false) const {
    Fact result = origin;
    auto address = location(pointer);
    result.kind = Kind::Memory;
    result.root = address.root;
    result.offset = address.offset;
    result.unknown_offset = address.unknown;
    result.width = width;
    result.pointer_slot = slot;
    return result;
  }
  Fact value(const Value *v, const Fact &origin) const {
    Fact result = origin;
    result.kind = Kind::Value;
    result.root = v;
    result.offset = 0;
    result.width = 0;
    result.unknown_offset = false;
    result.pointer_slot = false;
    if (const auto *inst = dyn_cast<Instruction>(v)) {
      auto &facts = function_facts_[inst->getFunction()];
      if (!facts)
        facts = std::make_unique<FunctionFacts>(
            *const_cast<Function *>(inst->getFunction()));
      result.allocation_safe |= facts->boundedAt(*v, *inst);
    }
    return result;
  }
  bool overlaps(const Fact &fact, const Value *pointer,
                uint64_t width = 0) const {
    if (fact.kind != Kind::Memory)
      return false;
    auto address = location(pointer);
    if (!address.root || fact.root != address.root)
      return false;
    if (fact.unknown_offset || address.unknown)
      return true;
    if (fact.offset == address.offset)
      return true;
    // Unknown-size byte buffers cover pointer offsets, but an unknown extent
    // of a structure field does not automatically taint other fields.
    if (!fact.width) {
      Type *type = fact.root->getType();
      return !fact.pointer_slot && fact.offset == 0 && type->isPointerTy() &&
             type->getPointerElementType()->isIntegerTy();
    }
    APInt from(128, static_cast<uint64_t>(fact.offset), true);
    APInt to(128, static_cast<uint64_t>(address.offset), true);
    return from.sle(to) ? (to - from).ult(fact.width)
                        : width && (from - to).ult(width);
  }
  bool matches(const Fact &fact, const Value *argument,
               TaintChannel channel) const {
    return channel == TaintChannel::Value
               ? fact.kind == Kind::Value && fact.root == argument
               : overlaps(fact, argument);
  }
  const Value *endpoint(const CallBase &call, TaintEndpoint point) const {
    return point.argument < 0
               ? static_cast<const Value *>(&call)
               : ValueFacts::argument(call,
                                      static_cast<unsigned>(point.argument));
  }
  Fact output(const Value *target, TaintEndpoint point,
              const Fact &origin) const {
    Fact result;
    if (point.channel == TaintChannel::Value)
      result = value(target, origin);
    else {
      Type *element = target->getType()->getPointerElementType();
      bool slot = element->isPointerTy();
      auto capacity = ValueFacts::objectBytes(*target);
      result = memory(target, origin,
                      slot ? bytes(element) : (capacity ? *capacity : 0), slot);
    }
    result.numeric |= point.numeric;
    return result;
  }
  bool valid(const Value *target, TaintEndpoint point) const {
    return target && !target->getType()->isVoidTy() &&
           (point.channel != TaintChannel::Memory ||
            !isa<ConstantPointerNull>(target)) &&
           (point.channel == TaintChannel::Value ||
            target->getType()->isPointerTy());
  }
  CallTaintModel model(const CallBase &call) const {
    return policy_.call_model ? policy_.call_model(call)
                              : LibraryModels::taint(call);
  }

  FactSet initial_facts(const Function *function) {
    FactSet result{zero_fact()};
    if (function->getName() == "main" && function->arg_size() > 1) {
      const Argument *argv = function->getArg(1);
      if (argv->getType()->isPointerTy()) {
        Fact source;
        source.source = argv;
        Fact fact = memory(argv, source, 0, true);
        fact.unknown_offset = true; // All argv slots, not the scalar argc.
        result.insert(fact);
      }
    }
    if (policy_.include_nonconstant_sources && function->use_empty() &&
        function->getName() != "main")
      for (const Argument &arg : function->args())
        if (arg.getType()->isPointerTy() &&
            !arg.getType()->getPointerElementType()->isFunctionTy()) {
          Fact origin;
          origin.source = &arg;
          origin.nonconstant = true;
          result.insert(memory(&arg, origin));
        }
    return result;
  }

  void phi_edge(const Instruction *from, const Instruction *to,
                FactSet &facts) const {
    if (!to || from->getParent() == to->getParent())
      return;
    FactSet additions;
    for (const Instruction &inst : *to->getParent()) {
      const auto *phi = dyn_cast<PHINode>(&inst);
      if (!phi)
        break;
      int incoming = phi->getBasicBlockIndex(from->getParent());
      if (incoming < 0)
        continue;
      const Value *operand = phi->getIncomingValue(incoming);
      for (const Fact &fact : facts) {
        if (fact.kind == Kind::Value && fact.root == operand)
          additions.insert(value(phi, fact));
        if (phi->getType()->isPointerTy() && overlaps(fact, operand)) {
          Fact mapped = memory(phi, fact, fact.width, fact.pointer_slot);
          additions.insert(mapped);
        }
      }
    }
    facts.insert(additions.begin(), additions.end());
  }

  FactSet normal_flow(const Instruction *inst, const Instruction *next,
                      const Fact &fact) {
    FactSet result;
    if (const auto *branch = dyn_cast<BranchInst>(inst))
      if (branch->isConditional())
        if (const auto *condition =
                dyn_cast<ConstantInt>(branch->getCondition()))
          if (next && next->getParent() !=
                          branch->getSuccessor(condition->isZero() ? 1 : 0))
            return result;
    bool keep = true;
    if (fact.kind == Kind::Zero)
      result.insert(fact);
    else if (const auto *store = dyn_cast<StoreInst>(inst)) {
      const Value *stored = store->getValueOperand(),
                  *pointer = store->getPointerOperand();
      auto address = location(pointer);
      uint64_t size = bytes(stored->getType());
      if (fact.kind == Kind::Memory && !fact.unknown_offset &&
          !address.unknown && fact.root == address.root &&
          fact.offset == address.offset) {
        if (fact.width && size >= fact.width)
          keep = false;
        if (!fact.pointer_slot && isa<ConstantInt>(stored) &&
            cast<ConstantInt>(stored)->isZero())
          keep = false;
      }
      if (fact.kind == Kind::Value && fact.root == stored)
        result.insert(
            memory(pointer, fact, size, stored->getType()->isPointerTy()));
      if (stored->getType()->isPointerTy() && overlaps(fact, stored))
        result.insert(memory(pointer, fact, size, true));
    } else if (const auto *load = dyn_cast<LoadInst>(inst)) {
      if (overlaps(fact, load->getPointerOperand(), bytes(load->getType()))) {
        if (load->getType()->isPointerTy()) {
          if (fact.pointer_slot)
            result.insert(memory(load, fact));
        } else
          result.insert(value(load, fact));
      }
    } else if (const auto *select = dyn_cast<SelectInst>(inst)) {
      for (unsigned i = 1; i < 3; ++i) {
        if (fact.kind == Kind::Value && fact.root == select->getOperand(i))
          result.insert(value(select, fact));
        if (select->getType()->isPointerTy() &&
            overlaps(fact, select->getOperand(i)))
          result.insert(memory(select, fact, fact.width, fact.pointer_slot));
      }
    } else if (!isa<PHINode>(inst) && !isa<CmpInst>(inst) &&
               !isa<GetElementPtrInst>(inst) &&
               (isa<BinaryOperator>(inst) || isa<CastInst>(inst) ||
                isa<UnaryOperator>(inst))) {
      if (fact.kind == Kind::Value)
        for (const Use &operand : inst->operands())
          if (operand.get() == fact.root)
            result.insert(value(inst, fact));
    }
    if (keep && fact.kind != Kind::Zero)
      result.insert(fact);
    phi_edge(inst, next, result);
    return result;
  }

  FactSet call_flow(const CallBase *call, const Function *callee,
                    const Fact &fact) {
    FactSet result;
    if (!callee || callee->isDeclaration() || model(*call).known)
      return result;
    if (fact.kind == Kind::Zero)
      return {fact};
    bool mapped = false;
    unsigned count = std::min<unsigned>(callee->arg_size(), call->arg_size());
    for (unsigned i = 0; i < count; ++i) {
      const Value *actual = call->getArgOperand(i);
      const Argument *formal = callee->getArg(i);
      if (fact.kind == Kind::Value && fact.root == actual) {
        result.insert(value(formal, fact));
        mapped = true;
      }
      if (actual->getType()->isPointerTy() &&
          formal->getType()->isPointerTy()) {
        auto address = location(actual);
        if (fact.kind == Kind::Memory && address.root == fact.root) {
          Fact output = fact;
          output.root = formal;
          if (__builtin_sub_overflow(fact.offset, address.offset,
                                     &output.offset)) {
            output.offset = 0;
            output.unknown_offset = true;
          }
          output.unknown_offset |= address.unknown;
          // Normalize unknown byte regions and shrink known regions as a
          // pointer advances. This keeps recursive input summaries finite
          // without merging separate fixed fields of an aggregate.
          if (output.unknown_offset || (!output.width && output.offset <= 0)) {
            output.offset = 0;
            output.unknown_offset = true;
          } else if (output.offset < 0) {
            uint64_t skipped =
                uint64_t(0) - static_cast<uint64_t>(output.offset);
            if (output.width && skipped >= output.width)
              continue;
            if (output.width)
              output.width -= skipped;
            output.offset = 0;
          }
          result.insert(output);
          mapped = true;
        }
      }
    }
    if (!mapped && fact.kind == Kind::Memory && isa<GlobalVariable>(fact.root))
      result.insert(fact);
    return result;
  }

  FactSet return_flow(const CallBase *call, const Instruction *exit,
                      const Instruction *next, const Function *callee,
                      const Fact &fact, const Fact &) {
    FactSet result;
    if (fact.kind == Kind::Zero)
      return {fact};
    if (fact.kind == Kind::Memory) {
      if (const auto *formal = dyn_cast<Argument>(fact.root))
        if (formal->getParent() == callee &&
            formal->getArgNo() < call->arg_size()) {
          const Value *actual = call->getArgOperand(formal->getArgNo());
          if (actual->getType()->isPointerTy()) {
            auto address = location(actual);
            Fact output = fact;
            output.root = address.root;
            if (__builtin_add_overflow(fact.offset, address.offset,
                                       &output.offset)) {
              output.offset = 0;
              output.unknown_offset = true;
            }
            output.unknown_offset |= address.unknown;
            if (output.unknown_offset)
              output.offset = 0;
            result.insert(output);
          }
        }
      if (isa<GlobalVariable>(fact.root))
        result.insert(fact);
    }
    if (const auto *ret = dyn_cast<ReturnInst>(exit))
      if (const Value *returned = ret->getReturnValue()) {
        if (fact.kind == Kind::Value && fact.root == returned)
          result.insert(value(call, fact));
        if (returned->getType()->isPointerTy() && overlaps(fact, returned))
          result.insert(memory(call, fact, fact.width, fact.pointer_slot));
      }
    phi_edge(call, next, result);
    return result;
  }

  FactSet call_to_return_flow(const CallBase *call, const Instruction *next,
                              ArrayRef<const Function *>, const Fact &fact) {
    FactSet result;
    CallTaintModel effects = model(*call);
    bool keep = true;
    const Function *target = ValueFacts::callee(*call);
    if (fact.kind != Kind::Zero) {
      if (target && !target->isDeclaration() && !effects.known &&
          fact.kind == Kind::Memory) {
        if (isa<GlobalVariable>(fact.root))
          keep = false;
        for (const Value *actual : call->args())
          if (actual->getType()->isPointerTy() &&
              location(actual).root == fact.root)
            keep = false;
      }
      for (TaintEndpoint point : effects.overwrites) {
        const Value *destination = endpoint(*call, point);
        auto address = location(destination);
        if (fact.kind == Kind::Memory && !fact.unknown_offset &&
            !address.unknown && fact.root == address.root &&
            fact.offset == address.offset)
          keep = false;
      }
      for (const TaintTransfer &transfer : effects.transfers) {
        const Value *input = endpoint(*call, transfer.input),
                    *target = endpoint(*call, transfer.output);
        if (!input || !valid(target, transfer.output) ||
            !matches(fact, input, transfer.input.channel))
          continue;
        if (fact.numeric && transfer.output.channel == TaintChannel::Memory &&
            !transfer.numeric)
          continue;
        Fact propagated = output(target, transfer.output, fact);
        propagated.numeric |= transfer.numeric;
        propagated.sql_safe |= transfer.sql_barrier;
        if (transfer.concatenates)
          propagated.concat = call;
        result.insert(propagated);
      }
    } else {
      for (TaintEndpoint point : effects.sources) {
        const Value *target = endpoint(*call, point);
        if (!valid(target, point))
          continue;
        Fact source;
        source.source = call;
        result.insert(output(target, point, source));
      }
      if (policy_.include_nonconstant_sources && !effects.known &&
          (!target || target->isDeclaration())) {
        Fact source;
        source.source = call;
        source.nonconstant = true;
        if (call->getType()->isPointerTy())
          result.insert(output(call, {-1, TaintChannel::Memory}, source));
        if (!call->onlyReadsMemory())
          for (unsigned i = 0; i < call->arg_size(); ++i)
            if (call->getArgOperand(i)->getType()->isPointerTy() &&
                !call->paramHasAttr(i, Attribute::ReadOnly)) {
              const auto *root = dyn_cast<GlobalVariable>(
                  location(call->getArgOperand(i)).root);
              if ((root && root->isConstant()) ||
                  isa<ConstantPointerNull>(call->getArgOperand(i)))
                continue;
              result.insert(output(call->getArgOperand(i),
                                   {static_cast<int>(i), TaintChannel::Memory},
                                   source));
            }
      }
    }
    if (keep)
      result.insert(fact);
    phi_edge(call, next, result);
    return result;
  }

private:
  const Module &module_;
  const TaintPolicy &policy_;
  mutable std::map<const Function *, std::unique_ptr<FunctionFacts>>
      function_facts_;
};
/// A PDG-owned context worklist. Each callee summary is keyed by its incoming
/// content/value state; return effects are instantiated at the originating
/// callsite. Recursive summaries grow to a fixed point and notify dependents.
/// This reuses PDG call wrappers and LLVM CFG attached to PDG program points.
class ContextSolver {
public:
  using FactSet = TransferFunctions::FactSet;
  ContextSolver(ProgramGraph &graph, TransferFunctions &transfer, size_t budget)
      : graph_(graph), transfer_(transfer), budget_(budget) {}

  void solve(const Module &module) {
    for (const Function &function : module)
      if (!function.isDeclaration())
        contextFor(function, transfer_.initial_facts(&function));
    while (!queue_.empty()) {
      if (budget_ && steps_ >= budget_) {
        limited_ = true;
        break;
      }
      auto item = queue_.front();
      queue_.pop_front();
      queued_.erase(item);
      ++steps_;
      Context &context = *contexts_[item.first];
      const Instruction *inst = item.second;
      FactSet state = context.states[inst];
      if (state.empty())
        continue;
      if (isa<ReturnInst>(inst) || isa<ResumeInst>(inst)) {
        FactSet &summary = context.exits[inst];
        size_t before = summary.size();
        summary.insert(state.begin(), state.end());
        if (summary.size() != before)
          for (auto dependent : context.dependents)
            enqueue(dependent.first, dependent.second);
        continue;
      }
      auto successors = nextInstructions(*inst);
      if (const auto *call = dyn_cast<CallBase>(inst)) {
        const Function *target = callee(*call);
        auto effects = transfer_.model(*call);
        Context *child = nullptr;
        if (target && !target->isDeclaration() && !effects.known) {
          FactSet inputs;
          for (const Fact &fact : state) {
            auto mapped = transfer_.call_flow(call, target, fact);
            inputs.insert(mapped.begin(), mapped.end());
          }
          child = &contextFor(*target, inputs);
          child->dependents.insert({item.first, inst});
          // A defined function with no reachable exit does not return. Do not
          // invent bypass execution before a return summary exists.
          if (child->exits.empty())
            continue;
        }
        for (const Instruction *next : successors) {
          FactSet output;
          for (const Fact &fact : state) {
            auto mapped = transfer_.call_to_return_flow(call, next, {}, fact);
            output.insert(mapped.begin(), mapped.end());
          }
          if (child)
            for (const auto &exit : child->exits)
              for (const Fact &fact : exit.second) {
                auto returned = transfer_.return_flow(call, exit.first, next,
                                                      target, fact, {});
                output.insert(returned.begin(), returned.end());
              }
          merge(item.first, next, output);
        }
      } else
        for (const Instruction *next : successors) {
          FactSet output;
          for (const Fact &fact : state) {
            auto mapped = transfer_.normal_flow(inst, next, fact);
            output.insert(mapped.begin(), mapped.end());
          }
          merge(item.first, next, output);
        }
    }
  }
  FactSet factsAt(const Instruction &inst) const {
    FactSet result;
    for (const auto &context : contexts_) {
      auto it = context->states.find(&inst);
      if (it != context->states.end())
        result.insert(it->second.begin(), it->second.end());
    }
    return result;
  }
  size_t steps() const { return steps_; }
  bool limited() const { return limited_; }
  size_t summaryHits() const { return hits_; }
  size_t summaryMisses() const { return misses_; }

private:
  struct ContextKey {
    const Function *function = nullptr;
    FactSet inputs;
    bool operator<(const ContextKey &other) const {
      if (function != other.function)
        return std::less<const Function *>{}(function, other.function);
      return inputs < other.inputs;
    }
  };
  struct Context {
    std::map<const Instruction *, FactSet> states;
    std::map<const Instruction *, FactSet> exits;
    std::set<std::pair<size_t, const Instruction *>> dependents;
  };
  Context &contextFor(const Function &function, const FactSet &input) {
    ContextKey key{&function, input};
    auto known = context_ids_.find(key);
    if (known != context_ids_.end()) {
      ++hits_;
      return *contexts_[known->second];
    }
    ++misses_;
    size_t id = contexts_.size();
    contexts_.push_back(std::make_unique<Context>());
    context_ids_.emplace(std::move(key), id);
    merge(id, &function.getEntryBlock().front(), input);
    return *contexts_[id];
  }
  const Function *callee(const CallBase &call) const {
    auto it = graph_.getCallWrapperMap().find(const_cast<CallBase *>(&call));
    if (it != graph_.getCallWrapperMap().end() && it->second)
      if (const Function *target = it->second->getCalledFunc())
        return target;
    return ValueFacts::callee(call);
  }
  static std::vector<const Instruction *>
  nextInstructions(const Instruction &inst) {
    std::vector<const Instruction *> result;
    if (const auto *call = dyn_cast<CallBase>(&inst))
      if (call->doesNotReturn())
        return result;
    if (const Instruction *next = inst.getNextNode())
      return {next};
    if (inst.isTerminator())
      for (const BasicBlock *block : llvm::successors(inst.getParent()))
        result.push_back(&block->front());
    return result;
  }
  void enqueue(size_t id, const Instruction *inst) {
    auto key = std::make_pair(id, inst);
    if (queued_.insert(key).second)
      queue_.push_back(key);
  }
  void merge(size_t id, const Instruction *inst, const FactSet &input) {
    FactSet &state = contexts_[id]->states[inst];
    size_t before = state.size();
    state.insert(input.begin(), input.end());
    if (state.size() != before)
      enqueue(id, inst);
  }
  ProgramGraph &graph_;
  TransferFunctions &transfer_;
  size_t budget_, steps_ = 0, hits_ = 0, misses_ = 0;
  bool limited_ = false;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::map<ContextKey, size_t> context_ids_;
  std::deque<std::pair<size_t, const Instruction *>> queue_;
  std::set<std::pair<size_t, const Instruction *>> queued_;
};
} // namespace taint_detail

std::vector<TaintOrigin> TaintFlowResult::origins(const CallBase &call,
                                                  unsigned argument,
                                                  TaintChannel channel) const {
  const auto &arguments =
      channel == TaintChannel::Memory ? string_arguments : value_arguments;
  auto it = arguments.find({&call, argument});
  return it == arguments.end() ? std::vector<TaintOrigin>() : it->second;
}

std::vector<unsigned>
TaintFlowResult::sinkArguments(const CallBase &call, TaintDomain domain,
                               bool include_wrappers) const {
  auto result = LibraryModels::taintSinks(call, domain);
  if (include_wrappers)
    if (const Function *target = ValueFacts::callee(call)) {
      auto it = wrapper_arguments.find({target, domain});
      if (it != wrapper_arguments.end())
        result.insert(result.end(), it->second.begin(), it->second.end());
    }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool TaintFlowResult::forwardedParameter(const CallBase &call, unsigned index,
                                         TaintDomain domain) const {
  if (index >= call.arg_size())
    return false;
  const auto *formal =
      dyn_cast<Argument>(call.getArgOperand(index)->stripPointerCasts());
  if (!formal || formal->getParent() != call.getFunction())
    return false;
  auto it = wrapper_arguments.find({call.getFunction(), domain});
  return it != wrapper_arguments.end() && it->second.count(formal->getArgNo());
}

TaintFlowResult TaintQuery::analyze(const Module &module,
                                    const TaintPolicy &policy) const {
  TaintFlowResult result;
  taint_detail::TransferFunctions problem(module, policy);
  taint_detail::ContextSolver solver(graph_, problem, policy.max_steps);
  solver.solve(module);
  result.diagnostics.explored_states = solver.steps();
  result.diagnostics.state_limit_hit = solver.limited();
  result.diagnostics.summary_cache_hits = solver.summaryHits();
  result.diagnostics.summary_cache_misses = solver.summaryMisses();
  // Infer unchanged-parameter wrapper roles to a finite fixed point, shared
  // by the string-flow rules rather than hard-coding individual wrappers.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const Function &function : module)
      for (const BasicBlock &block : function)
        for (const Instruction &inst : block)
          if (const auto *call = dyn_cast<CallBase>(&inst))
            for (auto domain : {TaintDomain::Process, TaintDomain::Command,
                                TaintDomain::Sql, TaintDomain::Path,
                                TaintDomain::Format, TaintDomain::Allocation})
              for (unsigned index : result.sinkArguments(*call, domain)) {
                if (index >= call->arg_size())
                  continue;
                const auto *formal = dyn_cast<Argument>(
                    call->getArgOperand(index)->stripPointerCasts());
                if (formal && formal->getParent() == &function)
                  changed |= result.wrapper_arguments[{&function, domain}]
                                 .insert(formal->getArgNo())
                                 .second;
              }
  }
  for (const Function &function : module)
    for (const BasicBlock &block : function)
      for (const Instruction &inst : block)
        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          if (!graph_.hasNode(const_cast<CallBase &>(*call)))
            continue;
          auto facts = solver.factsAt(*call);
          for (unsigned i = 0; i < call->arg_size(); ++i) {
            const Value *argument = call->getArgOperand(i);
            for (const auto &fact : facts)
              if (fact.kind == taint_detail::Kind::Value &&
                  fact.root == argument)
                result.value_arguments[{call, i}].push_back(
                    {fact.source, fact.concat, fact.nonconstant, fact.sql_safe,
                     fact.allocation_safe});
            if (!argument->getType()->isPointerTy())
              continue;
            std::set<std::tuple<uintptr_t, uintptr_t, bool, bool>> seen;
            for (const auto &fact : facts) {
              if (fact.numeric || fact.pointer_slot ||
                  !problem.overlaps(fact, argument))
                continue;
              auto key =
                  std::make_tuple(reinterpret_cast<uintptr_t>(fact.source),
                                  reinterpret_cast<uintptr_t>(fact.concat),
                                  fact.nonconstant, fact.sql_safe);
              if (seen.insert(key).second)
                result.string_arguments[{call, i}].push_back(
                    {fact.source, fact.concat, fact.nonconstant,
                     fact.sql_safe});
            }
          }
        }
  result.diagnostics.notes.push_back(
      "PDG context-summary call/return matching with separate value/content "
      "facts; evidence origins are not path-feasibility proofs. Fixed-offset "
      "objects only; unknown aliases/indirect calls/C++ string objects remain "
      "outside coverage.");
  if (solver.limited())
    result.diagnostics.notes.push_back(
        "Taint step limit reached; results are incomplete.");
  return result;
}
} // namespace pdg
