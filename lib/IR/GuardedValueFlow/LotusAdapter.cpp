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

static void setAccessPathFromSegments(llvm::gvg::AccessPath &path,
                                      ArrayRef<std::pair<Value *, int64_t>> segments,
                                      bool is_from_return) {
  path.reset(nullptr);
  for (size_t idx = 0; idx < segments.size(); ++idx) {
    const auto &segment = segments[idx];
    path.addLevel(segment.first, segment.second,
                  is_from_return && idx + 1 == segments.size());
  }
}

static void setNodeAccessPathFromValue(GuardedValueFlowNode *node,
                                       IntraLotusAA &pta, Value *value) {
  if (!node || !value)
    return;
  std::vector<std::pair<Value *, int64_t>> segments;
  bool is_from_return = false;
  pta.getFullAccessPath(value, segments, &is_from_return);
  setAccessPathFromSegments(node->getAccessPath(), segments, is_from_return);
}

static void setNodeAccessPathFromOutputIndex(GuardedValueFlowNode *node,
                                             IntraLotusAA &pta,
                                             unsigned output_index) {
  if (!node)
    return;
  std::vector<std::pair<Value *, int64_t>> segments;
  bool is_from_return = false;
  pta.getFullOutputAccessPath(static_cast<int>(output_index), segments,
                              &is_from_return);
  setAccessPathFromSegments(node->getAccessPath(), segments, is_from_return);
}

static GuardedValueFlowReturnSite *
findOrCreateReturnSite(GuardedValueFlowGraph &graph, ReturnInst *ret) {
  if (!ret)
    return nullptr;
  if (auto *existing = graph.findReturnSite(ret))
    return existing;
  auto *site = graph.createSite<GuardedValueFlowReturnSite>(&graph, ret);
  graph.mapReturnSite(ret, site);
  return site;
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
    auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
        GuardedValueFlowNode::Kind::CallSiteReturnSummary, memory_type, &graph,
        bb, inst, nullptr, 0);
    summary_node->setDescription("summary.value");
    mem_node->addChild(summary_node);
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
  const auto &outputs = pta.getOutputs();
  for (size_t idx = 1; idx < outputs.size(); ++idx) {
    const auto *output = outputs[idx];
    auto *pseudo_return = graph.createNode<GuardedValueFlowReturnNode>(
        GuardedValueFlowNode::Kind::PseudoReturn, output->getType(), &graph,
        pta.getFunc()->empty() ? nullptr : &pta.getFunc()->getEntryBlock());
    pseudo_return->setDescription((Twine("pseudo.return.") + Twine(idx)).str());
    pseudo_return->setIndex(static_cast<unsigned>(idx - 1));
    setNodeAccessPathFromOutputIndex(pseudo_return, pta,
                                     static_cast<unsigned>(idx));
    graph.registerPseudoReturn(pseudo_return);

    for (const auto &ret_vals : output->getVal()) {
      auto *ret_inst = ret_vals.first;
      auto *site = findOrCreateReturnSite(graph, ret_inst);
      auto *load_mem = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::LoadMemory, output->getType(), &graph,
          ret_inst ? ret_inst->getParent() : pseudo_return->getParentBasicBlock(),
          nullptr, ret_inst);
      load_mem->setDescription(
          (Twine("return.mem.") + Twine(idx) + "." +
           Twine(ret_inst ? ret_inst->getParent()->getName() : StringRef("entry")))
              .str());
      pseudo_return->addChild(load_mem);
      pseudo_return->addReturnValueSitePair(load_mem, site);
      if (site)
        load_mem->addUseSite(site);
      populateLoadMemoryNode(graph, load_mem, ret_vals.second,
                             ret_inst ? ret_inst->getParent()
                                      : pseudo_return->getParentBasicBlock());
    }
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
        graph.registerSummaryArgumentNode(bucket, summary_node);
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
        graph.registerSummaryReturnNode(bucket, summary_node);
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
      SmallVector<std::pair<Value *, const mem_value_t *>, 8> pseudo_input_specs;
      bool complete_inputs = true;
      for (const auto &input_item : callee_graph->getInputs()) {
        const mem_value_t *binding_values = nullptr;
        if (call_bind_it != call_arg_bindings.end()) {
          auto callee_it = call_bind_it->second.find(callee);
          if (callee_it != call_bind_it->second.end()) {
            auto binding_it = callee_it->second.find(input_item.first);
            if (binding_it != callee_it->second.end())
              binding_values = &binding_it->second;
          }
        }
        if (!binding_values) {
          complete_inputs = false;
          break;
        }
        pseudo_input_specs.push_back({input_item.first, binding_values});
      }
      if (complete_inputs) {
        for (unsigned formal_input_idx = 0; formal_input_idx < pseudo_input_specs.size();
             ++formal_input_idx) {
          Value *formal_value = pseudo_input_specs[formal_input_idx].first;
          const mem_value_t *binding_values =
              pseudo_input_specs[formal_input_idx].second;
          auto *pseudo_input = graph.createNode<GuardedValueFlowCallOutputNode>(
              GuardedValueFlowNode::Kind::CallSitePseudoInput,
              formal_value->getType(), &graph, call->getParent(), formal_value,
              call, callee);
          pseudo_input->setDescription(
              (Twine("call.input.") + Twine(formal_input_idx)).str());
          pseudo_input->setIndex(formal_input_idx);
          setNodeAccessPathFromValue(pseudo_input, *callee_graph, formal_value);

          auto *load_mem = graph.createNode<GuardedValueFlowNode>(
              GuardedValueFlowNode::Kind::LoadMemory, formal_value->getType(),
              &graph, call->getParent(), nullptr, call);
          load_mem->setDescription(
              (Twine("call.input.mem.") + Twine(formal_input_idx)).str());
          pseudo_input->addChild(load_mem);
          populateLoadMemoryNode(graph, load_mem, *binding_values, call->getParent());
          site->addPseudoInput(callee, pseudo_input);
        }
      }

      auto ret_bind_it = call_ret_bindings.find(call);
      const std::vector<Value *> *pseudo_outputs = nullptr;
      if (ret_bind_it != call_ret_bindings.end()) {
        auto callee_it = ret_bind_it->second.find(callee);
        if (callee_it != ret_bind_it->second.end())
          pseudo_outputs = &callee_it->second;
      }

      const auto &outputs = callee_graph->getOutputs();
      SmallVector<Value *, 8> pseudo_output_values;
      bool complete_outputs = true;
      for (size_t idx = 1; idx < outputs.size(); ++idx) {
        Value *pseudo_value = nullptr;
        if (pseudo_outputs && idx < pseudo_outputs->size())
          pseudo_value = (*pseudo_outputs)[idx];
        if (!pseudo_value) {
          complete_outputs = false;
          break;
        }
        pseudo_output_values.push_back(pseudo_value);
      }
      if (complete_outputs) {
        for (size_t idx = 1; idx < outputs.size(); ++idx) {
          Value *pseudo_value = pseudo_output_values[idx - 1];
          auto *pseudo_output = graph.createNode<GuardedValueFlowCallOutputNode>(
              GuardedValueFlowNode::Kind::CallSitePseudoOutput,
              outputs[idx]->getType(), &graph, call->getParent(), pseudo_value,
              call, callee);
          pseudo_output->setDescription((Twine("call.output.") + Twine(idx)).str());
          pseudo_output->setIndex(static_cast<unsigned>(idx - 1));
          setNodeAccessPathFromOutputIndex(pseudo_output, *callee_graph,
                                           static_cast<unsigned>(idx));
          graph.mapValueNode(pseudo_value, pseudo_output);

          auto *store_mem = ensureStoreMemoryNode(graph, pseudo_value, call,
                                                  outputs[idx]->getType(),
                                                  call->getParent());
          pseudo_output->addChild(store_mem);
          site->addPseudoOutput(callee, pseudo_output);
        }
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
  unsigned pseudo_arg_index = 0;
  for (const auto &input_item : pta.getInputs()) {
    Value *value = input_item.first;
    auto *node = ensureValueNode(graph, value, value->getType(),
                                 pta.getFunc()->empty()
                                     ? nullptr
                                     : &pta.getFunc()->getEntryBlock(),
                                 GuardedValueFlowNode::Kind::PseudoArgument,
                                  "pseudo.input");
    node->setIndex(pseudo_arg_index++);
    setNodeAccessPathFromValue(node, pta, value);
    graph.registerPseudoArgument(node);
  }

  materializeStoreParity(graph, pta);
  materializeLoadParity(graph, pta);
  materializeFunctionOutputs(graph, pta);
  materializeCallTargetConditions(graph, pta);
  materializeCallsiteSummaryNodes(graph, pta);
  materializeCallsiteInterfaces(graph, pta);
}

ModulePass *llvm::gvg::createLotusGuardedValueFlowAdapterPass() {
  return new LotusGuardedValueFlowAdapterPass();
}
