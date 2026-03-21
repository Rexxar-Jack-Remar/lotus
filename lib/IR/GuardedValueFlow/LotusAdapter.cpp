#include "IR/GuardedValueFlow/LotusAdapter.h"

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GuardedValueFlow/ConditionRef.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace llvm::gvg;

namespace {

static GuardedValueFlowNode *ensureValueNode(GuardedValueFlowGraph &graph,
                                             Value *value, Type *type,
                                             BasicBlock *bb,
                                             GuardedValueFlowNode::Kind kind,
                                             StringRef desc) {
  if (!value)
    return nullptr;

  if (auto *existing = graph.findNode(value))
    return existing;

  GuardedValueFlowNode *node = nullptr;
  if (kind == GuardedValueFlowNode::Kind::PseudoArgument ||
      kind == GuardedValueFlowNode::Kind::CommonArgument ||
      kind == GuardedValueFlowNode::Kind::VariableArgument) {
    node = graph.createNode<GuardedValueFlowArgumentNode>(
        kind, type ? type : value->getType(), &graph, bb, value);
  } else {
    node = graph.createNode<GuardedValueFlowNode>(
        kind, type ? type : value->getType(), &graph, bb, value,
        dyn_cast<Instruction>(value));
  }
  node->setDescription(desc.str());
  graph.mapValueNode(value, node);
  return node;
}

static GuardedValueFlowReturnNode *
findCommonReturnNode(GuardedValueFlowGraph &graph) {
  for (const auto &node_ptr : graph.nodes()) {
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::CommonReturn)
      return dyn_cast<GuardedValueFlowReturnNode>(node_ptr.get());
  }
  return nullptr;
}

static GuardedValueFlowNode *
createSpecialProducerNode(GuardedValueFlowGraph &graph,
                          GuardedValueFlowNode::Kind kind, Type *type,
                          BasicBlock *bb, Instruction *inst,
                          StringRef description) {
  auto *node =
      graph.createNode<GuardedValueFlowNode>(kind, type, &graph, bb, nullptr, inst);
  node->setDescription(description.str());
  return node;
}

static GuardedValueFlowNode *
createStandaloneStoreMemoryNode(GuardedValueFlowGraph &graph, Type *memory_type,
                                BasicBlock *bb, Instruction *inst,
                                StringRef description,
                                GuardedValueFlowNode::Kind producer_kind,
                                StringRef producer_desc) {
  auto *mem_node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::StoreMemory, memory_type, &graph, bb, nullptr,
      inst);
  mem_node->setDescription(description.str());
  mem_node->addChild(createSpecialProducerNode(graph, producer_kind, memory_type,
                                               bb, inst, producer_desc));
  return mem_node;
}

static GuardedValueFlowNode *
ensureStoreMemoryNode(GuardedValueFlowGraph &graph, Value *value,
                      Instruction *inst, Type *memory_type, BasicBlock *bb) {
  if (auto *existing = graph.findStoreMemoryNode(value, inst))
    return existing;

  auto *mem_node = graph.findOrCreateStoreMemoryNode(
      value, inst, memory_type, bb, "store.mem.adapter");

  if (value == LocValue::UNDEF_VALUE) {
    mem_node->addChild(createSpecialProducerNode(
        graph, GuardedValueFlowNode::Kind::UndefValue, memory_type, bb, inst,
        "undef.value"));
  } else if (value == LocValue::SUMMARY_VALUE) {
    mem_node->addChild(createSpecialProducerNode(
        graph, GuardedValueFlowNode::Kind::CallSiteReturnSummary, memory_type,
        bb, inst, "summary.value"));
  } else if (value == LocValue::FREE_VARIABLE || value == LocValue::NO_VALUE) {
    mem_node->addChild(createSpecialProducerNode(
        graph, GuardedValueFlowNode::Kind::Unknown, memory_type, bb, inst,
        "unknown.value"));
  } else {
    auto *value_node =
        ensureValueNode(graph, value, value->getType(), bb,
                        GuardedValueFlowNode::Kind::SimpleOperand, "producer");
    mem_node->addChild(value_node);
  }

  return mem_node;
}

static GuardedValueFlowRegionNode *resolveMatchingRegion(
    GuardedValueFlowGraph &graph, GuardedValueFlowNode *producer_mem,
    const mem_value_item_t &item) {
  if (item.cond)
    return graph.findOrCreateSemanticRegion(item.cond,
                                            producer_mem
                                                ? producer_mem->getParentBasicBlock()
                                                : nullptr);
  if (producer_mem && producer_mem->getRegion())
    return producer_mem->getRegion();
  return graph.getAlwaysTrueRegion();
}

static void linkMemoryValue(GuardedValueFlowGraph &graph,
                            GuardedValueFlowNode *load_mem_node,
                            const mem_value_item_t &item, BasicBlock *bb) {
  Value *value = item.val;
  if (value == LocValue::FREE_VARIABLE || value == LocValue::NO_VALUE)
    return;

  auto cond = item.cond ? ConditionRef::fromPathCond(item.cond)
                        : ConditionRef::none();
  Instruction *producer_inst = item.pos;
  auto *producer_mem = ensureStoreMemoryNode(graph, value, producer_inst,
                                             load_mem_node->getType(), bb);
  load_mem_node->addChild(producer_mem, item.confidence, cond);
  load_mem_node->addMatchingRegion(
      producer_mem, resolveMatchingRegion(graph, producer_mem, item), cond);
}

static void populateLoadMemoryNode(GuardedValueFlowGraph &graph,
                                   GuardedValueFlowNode *load_mem_node,
                                   const mem_value_t &values, BasicBlock *bb) {
  load_mem_node->clearChildren();
  for (const auto &item : values)
    linkMemoryValue(graph, load_mem_node, item, bb);
}

static Type *chooseSummaryNodeType(LLVMContext &ctx,
                                   const std::set<Value *, llvm_cmp> *values) {
  if (!values || values->empty())
    return Type::getVoidTy(ctx);
  Type *chosen = nullptr;
  for (Value *value : *values) {
    if (!value)
      continue;
    if (!chosen)
      chosen = value->getType();
    else if (chosen != value->getType())
      return Type::getVoidTy(ctx);
  }
  return chosen ? chosen : Type::getVoidTy(ctx);
}

static Type *chooseSummaryNodeType(LLVMContext &ctx, const mem_value_t *values) {
  if (!values || values->empty())
    return Type::getVoidTy(ctx);
  Type *chosen = nullptr;
  for (const auto &item : *values) {
    if (!item.val)
      continue;
    Type *curr = item.val->getType();
    if (!chosen)
      chosen = curr;
    else if (chosen != curr)
      return Type::getVoidTy(ctx);
  }
  return chosen ? chosen : Type::getVoidTy(ctx);
}

static mem_value_t importSummaryValues(IntraLotusAA &pta, Instruction *callsite,
                                       Function *callee,
                                       const mem_value_t &summary_values) {
  mem_value_t imported;
  imported.reserve(summary_values.size());
  for (const auto &item : summary_values) {
    path_cond_t imported_cond =
        item.cond ? pta.importSummaryCond(item.cond, callsite, callee) : nullptr;
    imported.emplace_back(imported_cond, item.pos, item.val, item.confidence);
  }
  return imported;
}

static SmallVector<Function *, 4>
collectCallees(const GuardedValueFlowCallSite &site, CallBase &call,
               const IntraLotusAA &pta) {
  SmallVector<Function *, 4> result;
  for (Function *callee : site.getCallees())
    result.push_back(callee);

  auto resolved_it = pta.getResolvedCallTargets().find(&call);
  if (resolved_it != pta.getResolvedCallTargets().end()) {
    for (const auto &target : resolved_it->second) {
      if (llvm::find(result, target.first) == result.end())
        result.push_back(target.first);
    }
  }

  if (result.empty()) {
    if (Function *direct_callee = call.getCalledFunction())
      result.push_back(direct_callee);
  }

  return result;
}

static void materializeLoadParity(GuardedValueFlowGraph &graph,
                                  IntraLotusAA &pta) {
  DenseMap<LoadInst *, mem_value_t> rep_values;

  for (Instruction &inst : instructions(*pta.getFunc())) {
    auto *load = dyn_cast<LoadInst>(&inst);
    if (!load)
      continue;

    auto *value_node = graph.findNode(load);
    auto *load_mem_node = graph.findLoadMemoryNode(load);
    if (!value_node || !load_mem_node)
      continue;

    const auto &equivalent_loads = pta.getAllLoadWithSameValue(load);
    LoadInst *rep = equivalent_loads.empty() ? load : *equivalent_loads.begin();

    value_node->clearChildren();
    value_node->addChild(load_mem_node);

    auto rep_it = rep_values.find(rep);
    if (rep_it == rep_values.end()) {
      mem_value_t load_values;
      pta.getLoadValues(rep->getPointerOperand(), rep, load_values);
      rep_it = rep_values.try_emplace(rep, std::move(load_values)).first;
    }

    populateLoadMemoryNode(graph, load_mem_node, rep_it->second, load->getParent());
  }
}

static void materializeStoreParity(GuardedValueFlowGraph &graph,
                                   IntraLotusAA &pta) {
  for (Instruction &inst : instructions(*pta.getFunc())) {
    auto *store = dyn_cast<StoreInst>(&inst);
    if (!store)
      continue;

    auto *mem_node =
        graph.findStoreMemoryNode(store->getValueOperand(), store);
    if (!mem_node)
      continue;

    mem_node->clearChildren();
    auto *value_node = ensureValueNode(
        graph, store->getValueOperand(), store->getValueOperand()->getType(),
        store->getParent(), GuardedValueFlowNode::Kind::SimpleOperand,
        "store.value");
    mem_node->addChild(value_node);
  }
}

static void materializeFunctionOutputs(GuardedValueFlowGraph &graph,
                                       IntraLotusAA &pta) {
  GuardedValueFlowReturnNode *common_return = findCommonReturnNode(graph);
  const auto &outputs = pta.getOutputs();
  for (size_t idx = 0; idx < outputs.size(); ++idx) {
    const auto *output = outputs[idx];
    GuardedValueFlowNode *owner = nullptr;

    if (idx == 0) {
      owner = common_return;
      if (!owner)
        continue;
    } else {
      auto *pseudo_return = graph.createNode<GuardedValueFlowReturnNode>(
          GuardedValueFlowNode::Kind::PseudoReturn, output->getType(), &graph,
          pta.getFunc()->empty() ? nullptr : &pta.getFunc()->getEntryBlock());
      pseudo_return->setDescription((Twine("pseudo.return.") + Twine(idx)).str());
      pseudo_return->setIndex(static_cast<unsigned>(idx - 1));
      pseudo_return->setAccessPath(AccessPath(
          output->getSymbolicInfo().getParentPtr(),
          output->getSymbolicInfo().getOffset()));
      owner = pseudo_return;
    }

    auto *load_mem = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::LoadMemory, output->getType(), &graph,
        owner->getParentBasicBlock(), nullptr, owner->getDebugInstruction());
    load_mem->setDescription((Twine("return.mem.") + Twine(idx)).str());
    owner->addChild(load_mem);

    mem_value_t aggregated;
    for (const auto &ret_vals : output->getVal())
      aggregated.insert(aggregated.end(), ret_vals.second.begin(),
                        ret_vals.second.end());
    populateLoadMemoryNode(graph, load_mem, aggregated,
                           owner->getParentBasicBlock());
  }
}

static void materializeCallsiteSummaryNodes(GuardedValueFlowGraph &graph,
                                            IntraLotusAA &pta) {
  const auto &call_arg_bindings = pta.getCallArgBindings();

  for (const auto &site_ptr : graph.sites()) {
    auto *site = dynamic_cast<GuardedValueFlowCallSite *>(site_ptr.get());
    if (!site)
      continue;

    auto *call = dyn_cast_or_null<CallBase>(site->getInstruction());
    if (!call)
      continue;

    SmallVector<Function *, 4> callees = collectCallees(*site, *call, pta);
    for (Function *callee : callees) {
      auto *callee_graph = dyn_cast_or_null<IntraLotusAA>(pta.getPtGraph(callee));
      if (!callee_graph)
        continue;

      auto call_bind_it = call_arg_bindings.find(call);
      const auto &summary_inputs = callee_graph->getSummaryInputs();
      for (unsigned bucket = 0; bucket < summary_inputs.size(); ++bucket) {
        const auto *summary_bucket = summary_inputs[bucket];
        if (!summary_bucket || summary_bucket->empty())
          continue;

        Type *summary_type =
            chooseSummaryNodeType(call->getContext(), summary_bucket);
        auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
            GuardedValueFlowNode::Kind::CallSiteArgumentSummary, summary_type,
            &graph, call->getParent(), call, callee, bucket);
        summary_node->setDescription(
            (Twine("call.input.summary.") + Twine(bucket)).str());

        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
            call->getParent(), nullptr, call);
        load_mem->setDescription(
            (Twine("call.input.summary.mem.") + Twine(bucket)).str());
        summary_node->addChild(load_mem);

        mem_value_t aggregated;
        if (call_bind_it != call_arg_bindings.end()) {
          auto callee_it = call_bind_it->second.find(callee);
          if (callee_it != call_bind_it->second.end()) {
            for (Value *summary_input : *summary_bucket) {
              auto binding_it = callee_it->second.find(summary_input);
              if (binding_it != callee_it->second.end()) {
                aggregated.insert(aggregated.end(), binding_it->second.begin(),
                                  binding_it->second.end());
              }
            }
          }
        }
        populateLoadMemoryNode(graph, load_mem, aggregated, call->getParent());
        site->setInputSummaryNode(bucket, summary_node);
      }

      const auto &summary_outputs = callee_graph->getSummaryOutputs();
      for (unsigned bucket = 0; bucket < summary_outputs.size(); ++bucket) {
        const mem_value_t *summary_bucket = summary_outputs[bucket];
        if (!summary_bucket || summary_bucket->empty())
          continue;

        Type *summary_type =
            chooseSummaryNodeType(call->getContext(), summary_bucket);
        auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
            GuardedValueFlowNode::Kind::CallSiteReturnSummary, summary_type,
            &graph, call->getParent(), call, callee, bucket);
        summary_node->setDescription(
            (Twine("call.output.summary.") + Twine(bucket)).str());

        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
            call->getParent(), nullptr, call);
        load_mem->setDescription(
            (Twine("call.output.summary.mem.") + Twine(bucket)).str());
        summary_node->addChild(load_mem);

        mem_value_t imported =
            importSummaryValues(pta, call, callee, *summary_bucket);
        populateLoadMemoryNode(graph, load_mem, imported, call->getParent());
        site->setOutputSummaryNode(bucket, summary_node);
      }
    }
  }
}

static void materializeCallsiteInterfaces(GuardedValueFlowGraph &graph,
                                          IntraLotusAA &pta) {
  const auto &call_arg_bindings = pta.getCallArgBindings();
  const auto &call_ret_bindings = pta.getCallReturnBindings();

  for (const auto &site_ptr : graph.sites()) {
    auto *site = dynamic_cast<GuardedValueFlowCallSite *>(site_ptr.get());
    if (!site)
      continue;

    auto *call = dyn_cast_or_null<CallBase>(site->getInstruction());
    if (!call)
      continue;

    SmallVector<Function *, 4> callees = collectCallees(*site, *call, pta);
    for (Function *callee : callees) {
      site->addCallee(callee);
      auto *callee_graph = dyn_cast_or_null<IntraLotusAA>(pta.getPtGraph(callee));
      if (!callee_graph)
        continue;

      auto call_bind_it = call_arg_bindings.find(call);
      unsigned input_idx = 0;
      for (const auto &input_item : callee_graph->getInputs()) {
        auto *pseudo_input = graph.createNode<GuardedValueFlowCallOutputNode>(
            GuardedValueFlowNode::Kind::CallSitePseudoInput,
            input_item.first->getType(), &graph, call->getParent(),
            input_item.first, call, callee);
        pseudo_input->setDescription((Twine("call.input.") + Twine(input_idx)).str());
        pseudo_input->setIndex(input_idx);
        pseudo_input->setAccessPath(
            AccessPath(input_item.second.getParentPtr(), input_item.second.getOffset()));

        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, input_item.first->getType(),
            &graph, call->getParent(), nullptr, call);
        load_mem->setDescription((Twine("call.input.mem.") + Twine(input_idx)).str());
        pseudo_input->addChild(load_mem);

        if (call_bind_it != call_arg_bindings.end()) {
          auto callee_it = call_bind_it->second.find(callee);
          if (callee_it != call_bind_it->second.end()) {
            auto binding_it = callee_it->second.find(input_item.first);
            if (binding_it != callee_it->second.end())
              populateLoadMemoryNode(graph, load_mem, binding_it->second,
                                     call->getParent());
          }
        }

        site->addPseudoInput(callee, pseudo_input);
        ++input_idx;
      }

      auto ret_bind_it = call_ret_bindings.find(call);
      const std::vector<Value *> *pseudo_outputs = nullptr;
      if (ret_bind_it != call_ret_bindings.end()) {
        auto callee_it = ret_bind_it->second.find(callee);
        if (callee_it != ret_bind_it->second.end())
          pseudo_outputs = &callee_it->second;
      }

      const auto &outputs = callee_graph->getOutputs();
      for (size_t idx = 1; idx < outputs.size(); ++idx) {
        Value *pseudo_value = nullptr;
        if (pseudo_outputs && idx < pseudo_outputs->size())
          pseudo_value = (*pseudo_outputs)[idx];

        auto *pseudo_output = graph.createNode<GuardedValueFlowCallOutputNode>(
            GuardedValueFlowNode::Kind::CallSitePseudoOutput,
            outputs[idx]->getType(), &graph, call->getParent(), pseudo_value,
            call, callee);
        pseudo_output->setDescription((Twine("call.output.") + Twine(idx)).str());
        pseudo_output->setIndex(static_cast<unsigned>(idx - 1));
        pseudo_output->setAccessPath(
            AccessPath(outputs[idx]->getSymbolicInfo().getParentPtr(),
                       outputs[idx]->getSymbolicInfo().getOffset()));
        if (pseudo_value)
          graph.mapValueNode(pseudo_value, pseudo_output);

        GuardedValueFlowNode *store_mem = nullptr;
        if (pseudo_value) {
          store_mem = ensureStoreMemoryNode(graph, pseudo_value, call,
                                            outputs[idx]->getType(),
                                            call->getParent());
        } else {
          store_mem = createStandaloneStoreMemoryNode(
              graph, outputs[idx]->getType(), call->getParent(), call,
              (Twine("call.output.mem.") + Twine(idx)).str(),
              GuardedValueFlowNode::Kind::CallSiteReturnSummary,
              "summary.value");
        }
        pseudo_output->addChild(store_mem);
        site->addPseudoOutput(callee, pseudo_output);
      }
    }
  }
}

static void materializeDirectCallFallbackInterfaces(GuardedValueFlowGraph &graph,
                                                    IntraLotusAA &pta) {
  for (Instruction &inst : instructions(*pta.getFunc())) {
    auto *call = dyn_cast<CallBase>(&inst);
    if (!call)
      continue;

    Function *callee = call->getCalledFunction();
    if (!callee)
      continue;

    auto *site = graph.findCallSite(call);
    auto *callee_graph = dyn_cast_or_null<IntraLotusAA>(pta.getPtGraph(callee));
    if (!site || !callee_graph)
      continue;

    if (site->getNumPseudoInputs(callee) == 0) {
      unsigned idx = 0;
      for (const auto &input_item : callee_graph->getInputs()) {
        auto *pseudo_input = graph.createNode<GuardedValueFlowCallOutputNode>(
            GuardedValueFlowNode::Kind::CallSitePseudoInput,
            input_item.first->getType(), &graph, call->getParent(),
            input_item.first, call, callee);
        pseudo_input->setDescription((Twine("call.input.direct.") + Twine(idx)).str());
        pseudo_input->setIndex(idx);
        pseudo_input->setAccessPath(
            AccessPath(input_item.second.getParentPtr(), input_item.second.getOffset()));
        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, input_item.first->getType(),
            &graph, call->getParent(), nullptr, call);
        load_mem->setDescription((Twine("call.input.direct.mem.") + Twine(idx)).str());
        pseudo_input->addChild(load_mem);
        site->addPseudoInput(callee, pseudo_input);
        ++idx;
      }
    }

    if (site->getNumPseudoOutputs(callee) == 0) {
      const auto &outputs = callee_graph->getOutputs();
      for (size_t idx = 1; idx < outputs.size(); ++idx) {
        auto *pseudo_output = graph.createNode<GuardedValueFlowCallOutputNode>(
            GuardedValueFlowNode::Kind::CallSitePseudoOutput,
            outputs[idx]->getType(), &graph, call->getParent(), nullptr, call,
            callee);
        pseudo_output->setDescription((Twine("call.output.direct.") + Twine(idx)).str());
        pseudo_output->setIndex(static_cast<unsigned>(idx - 1));
        pseudo_output->setAccessPath(
            AccessPath(outputs[idx]->getSymbolicInfo().getParentPtr(),
                       outputs[idx]->getSymbolicInfo().getOffset()));
        auto *store_mem = createStandaloneStoreMemoryNode(
            graph, outputs[idx]->getType(), call->getParent(), call,
            (Twine("call.output.direct.mem.") + Twine(idx)).str(),
            GuardedValueFlowNode::Kind::CallSiteReturnSummary, "summary.value");
        pseudo_output->addChild(store_mem);
        site->addPseudoOutput(callee, pseudo_output);
      }
    }
  }
}

static void materializeCallTargetConditions(GuardedValueFlowGraph &graph,
                                            IntraLotusAA &pta) {
  for (const auto &cg_item : pta.getResolvedCallTargets()) {
    auto *call = dyn_cast<CallBase>(cg_item.first);
    auto *site = call ? graph.findCallSite(call) : nullptr;
    if (!site)
      continue;

    for (const auto &target : cg_item.second) {
      site->addCallee(target.first);
      if (target.second)
        site->setCalleeCondition(
            target.first, ConditionRef::fromPathCond(target.second),
            graph.findOrCreateSemanticRegion(target.second, call->getParent()));
    }
  }
}

} // namespace

char LotusGuardedValueFlowAdapterPass::ID = 0;
static RegisterPass<LotusGuardedValueFlowAdapterPass>
    Y("gvg-lotus-adapter", "LotusAA to GuardedValueFlowGraph adapter", false,
      true);

LotusGuardedValueFlowAdapterPass::LotusGuardedValueFlowAdapterPass()
    : ModulePass(ID) {}

void LotusGuardedValueFlowAdapterPass::getAnalysisUsage(
    AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<LotusAA>();
  AU.addRequired<GuardedValueFlowGraphBuilderPass>();
}

bool LotusGuardedValueFlowAdapterPass::runOnModule(Module &M) {
  auto &lotus = getAnalysis<LotusAA>();
  auto &builder = getAnalysis<GuardedValueFlowGraphBuilderPass>();

  for (Function &F : M) {
    if (F.isDeclaration() || !builder.hasGraphFor(F))
      continue;
    if (IntraLotusAA *pta = lotus.getPtGraph(&F))
      adaptFunction(builder.getGraph(F), *pta);
  }

  return false;
}

void LotusGuardedValueFlowAdapterPass::adaptFunction(
    GuardedValueFlowGraph &graph, IntraLotusAA &pta) {
  for (const auto &input_item : pta.getInputs()) {
    Value *value = input_item.first;
    auto *node = ensureValueNode(graph, value, value->getType(),
                                 pta.getFunc()->empty()
                                     ? nullptr
                                     : &pta.getFunc()->getEntryBlock(),
                                 GuardedValueFlowNode::Kind::PseudoArgument,
                                 "pseudo.input");
    node->setAccessPath(
        AccessPath(input_item.second.getParentPtr(), input_item.second.getOffset()));
  }

  materializeStoreParity(graph, pta);
  materializeLoadParity(graph, pta);
  materializeFunctionOutputs(graph, pta);
  materializeCallTargetConditions(graph, pta);
  materializeCallsiteSummaryNodes(graph, pta);
  materializeCallsiteInterfaces(graph, pta);
  materializeDirectCallFallbackInterfaces(graph, pta);
}

ModulePass *llvm::gvg::createLotusGuardedValueFlowAdapterPass() {
  return new LotusGuardedValueFlowAdapterPass();
}
