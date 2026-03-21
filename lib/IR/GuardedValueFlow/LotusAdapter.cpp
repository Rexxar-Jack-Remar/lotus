#include "IR/GuardedValueFlow/LotusAdapter.h"

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GuardedValueFlow/ConditionRef.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Debug.h>

using namespace llvm;
using namespace llvm::gvg;

#define DEBUG_TYPE "gvg-lotus-adapter"

namespace {

static GuardedValueFlowOpcodeNode::OpcodeKind
chooseCastOpcode(Type *src_ty, Type *dst_ty) {
  if (!src_ty || !dst_ty)
    return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;

  if (auto *src_vec_ty = dyn_cast<VectorType>(src_ty)) {
    auto *dst_vec_ty = dyn_cast<VectorType>(dst_ty);
    if (!dst_vec_ty ||
        src_vec_ty->getElementCount() != dst_vec_ty->getElementCount())
      return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;
    src_ty = src_vec_ty->getElementType();
    dst_ty = dst_vec_ty->getElementType();
  } else if (dst_ty->isVectorTy()) {
    return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;
  }

  if (!src_ty->isSized() || !dst_ty->isSized())
    return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;

  const auto &src_bits = src_ty->getPrimitiveSizeInBits();
  const auto &dst_bits = dst_ty->getPrimitiveSizeInBits();

  if (dst_ty->isFloatingPointTy() && src_ty->isFloatingPointTy()) {
    if (dst_bits == src_bits)
      return GuardedValueFlowOpcodeNode::OpcodeKind::BitCast;
    return dst_bits > src_bits ? GuardedValueFlowOpcodeNode::OpcodeKind::FPExt
                               : GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc;
  }
  if (dst_ty->isFloatingPointTy() && src_ty->isIntegerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP;
  if (dst_ty->isIntegerTy() && src_ty->isFloatingPointTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI;
  if (dst_ty->isIntegerTy() && src_ty->isIntegerTy()) {
    if (dst_bits == src_bits)
      return GuardedValueFlowOpcodeNode::OpcodeKind::BitCast;
    return dst_bits > src_bits ? GuardedValueFlowOpcodeNode::OpcodeKind::SExt
                               : GuardedValueFlowOpcodeNode::OpcodeKind::Trunc;
  }
  if (dst_ty->isPointerTy() && src_ty->isIntegerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr;
  if (dst_ty->isIntegerTy() && src_ty->isPointerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt;
  if (dst_ty->isPointerTy() && src_ty->isPointerTy())
    return GuardedValueFlowOpcodeNode::OpcodeKind::BitCast;

  return GuardedValueFlowOpcodeNode::OpcodeKind::Invalid;
}

static BasicBlock *getEntryBlockOrNull(GuardedValueFlowGraph &graph) {
  Function *base = graph.getBaseFunction();
  return (base && !base->empty()) ? &base->getEntryBlock() : nullptr;
}

static BasicBlock *getFunctionEntryBlockOrNull(Function *func) {
  return (func && !func->empty()) ? &func->getEntryBlock() : nullptr;
}

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

static GuardedValueFlowNode *
createInterfaceArgumentNode(GuardedValueFlowGraph &graph, Value *value, Type *type,
                            BasicBlock *bb, StringRef desc) {
  if (!value)
    return nullptr;

  auto *node = graph.createNode<GuardedValueFlowArgumentNode>(
      GuardedValueFlowNode::Kind::PseudoArgument,
      type ? type : value->getType(), &graph, bb, value);
  node->setDescription(desc.str());
  return node;
}

static GuardedValueFlowNode *resolveOriginConditionNode(
    GuardedValueFlowGraphBuilderPass &builder, path_cond_t cond);

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

static Type *getStableSummaryNodeType(LLVMContext &ctx) {
  return PTGraph::DEFAULT_NON_POINTER_TYPE
             ? PTGraph::DEFAULT_NON_POINTER_TYPE
             : Type::getInt64Ty(ctx);
}

static mem_value_t makeFallbackValues(Value *value, Instruction *pos = nullptr,
                                      float confidence = 1.0f,
                                      path_cond_t cond = nullptr) {
  mem_value_t fallback;
  fallback.emplace_back(cond, pos, value, confidence);
  return fallback;
}

static Value *createSyntheticInterfaceValue(Type *type, StringRef name) {
  return new Argument(type, name);
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
  const bool is_anonymous_special =
      value == LocValue::UNDEF_VALUE || value == LocValue::SUMMARY_VALUE;
  if (!is_anonymous_special) {
    if (auto *existing = graph.findStoreMemoryNode(value, inst))
      return existing;
  }

  auto *mem_node = is_anonymous_special
                       ? graph.createAnonymousStoreMemoryNode(
                             memory_type, bb, inst, "store.mem.adapter")
                       : graph.findOrCreateStoreMemoryNode(
                             value, inst, memory_type, bb, "store.mem.adapter");

  if (value == LocValue::UNDEF_VALUE) {
    (void)LotusGuardedValueFlowAdapterPass::safeLink(
        graph, mem_node,
        createSpecialProducerNode(graph, GuardedValueFlowNode::Kind::UndefValue,
                                  memory_type, bb, inst, "undef.value"));
  } else if (value == LocValue::SUMMARY_VALUE) {
    auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
        GuardedValueFlowNode::Kind::CallSiteReturnSummary, memory_type, &graph,
        bb, inst, nullptr, 0);
    summary_node->setDescription("summary.value");
    (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, mem_node,
                                                     summary_node);
  } else if (value == LocValue::FREE_VARIABLE || value == LocValue::NO_VALUE) {
    (void)LotusGuardedValueFlowAdapterPass::safeLink(
        graph, mem_node,
        createSpecialProducerNode(graph, GuardedValueFlowNode::Kind::Unknown,
                                  memory_type, bb, inst, "unknown.value"));
  } else {
    auto *value_node =
        ensureValueNode(graph, value, value->getType(), bb,
                        GuardedValueFlowNode::Kind::SimpleOperand, "producer");
    (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, mem_node, value_node);
  }

  return mem_node;
}

static GuardedValueFlowRegionNode *resolveMatchingRegion(
    GuardedValueFlowGraphBuilderPass &builder, GuardedValueFlowGraph &graph,
    GuardedValueFlowNode *producer_mem, const mem_value_item_t &item) {
  if (item.cond)
    return graph.findOrCreateSemanticRegion(
        item.cond,
        producer_mem ? producer_mem->getParentBasicBlock() : nullptr,
        resolveOriginConditionNode(builder, item.cond));
  if (producer_mem && producer_mem->getRegion())
    return producer_mem->getRegion();
  return graph.getAlwaysTrueRegion();
}

static void linkMemoryValue(GuardedValueFlowGraph &graph,
                            GuardedValueFlowNode *load_mem_node,
                            const mem_value_item_t &item, BasicBlock *bb,
                            GuardedValueFlowGraphBuilderPass &builder) {
  Value *value = item.val;
  auto cond = item.cond ? ConditionRef::fromPathCond(item.cond)
                        : ConditionRef::none();
  Instruction *producer_inst = item.pos;
  auto *producer_mem = ensureStoreMemoryNode(graph, value, producer_inst,
                                             load_mem_node->getType(), bb);
  auto *linked_producer = LotusGuardedValueFlowAdapterPass::safeLink(
      graph, load_mem_node, producer_mem, item.confidence, cond);
  if (!linked_producer)
    return;
  load_mem_node->addMatchingRegion(
      linked_producer, resolveMatchingRegion(builder, graph, producer_mem, item),
      cond);
}

static void populateLoadMemoryNode(GuardedValueFlowGraph &graph,
                                   GuardedValueFlowNode *load_mem_node,
                                   const mem_value_t &values, BasicBlock *bb,
                                   GuardedValueFlowGraphBuilderPass &builder) {
  load_mem_node->clearChildren();
  for (const auto &item : values)
    linkMemoryValue(graph, load_mem_node, item, bb, builder);
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

static path_cond_t getImportedSource(path_cond_t cond) {
  if (!cond || cond->getKind() != PathCond::Kind::ImportedAtom)
    return nullptr;
  return cond->getImportedSource();
}

static GuardedValueFlowNode *resolveOriginConditionNode(
    GuardedValueFlowGraphBuilderPass &builder, path_cond_t cond) {
  path_cond_t imported_source = getImportedSource(cond);
  if (!imported_source)
    return nullptr;

  Function *origin_func = imported_source->getOwnerFunc();
  if (!origin_func || !builder.hasGraphFor(*origin_func))
    return nullptr;

  auto &origin_graph = builder.getGraph(*origin_func);
  auto *origin_region = origin_graph.findOrCreateSemanticRegion(
      imported_source, getFunctionEntryBlockOrNull(origin_func));
  return origin_region ? origin_region->getConditionNode() : nullptr;
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
                                  IntraLotusAA &pta,
                                  GuardedValueFlowGraphBuilderPass &builder) {
  for (Instruction &inst : instructions(*pta.getFunc())) {
    auto *load = dyn_cast<LoadInst>(&inst);
    if (!load)
      continue;

    auto *value_node = graph.findNode(load);
    auto *load_mem_node = graph.findLoadMemoryNode(load);
    if (!value_node || !load_mem_node)
      continue;

    value_node->clearChildren();
    value_node->addChild(load_mem_node);

    mem_value_t load_values;
    pta.getLoadValues(load->getPointerOperand(), load, load_values);
    if (load_values.empty())
      load_values = makeFallbackValues(LocValue::FREE_VARIABLE, load);
    populateLoadMemoryNode(graph, load_mem_node, load_values, load->getParent(),
                           builder);
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
    (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, mem_node, value_node);
  }
}

static void materializeFunctionOutputs(GuardedValueFlowGraph &graph,
                                       IntraLotusAA &pta,
                                       GuardedValueFlowGraphBuilderPass &builder) {
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

    const auto &ret_val_map = output->getVal();
    if (ret_val_map.empty()) {
      auto *load_mem = graph.createNode<GuardedValueFlowNode>(
          GuardedValueFlowNode::Kind::LoadMemory, output->getType(), &graph,
          pseudo_return->getParentBasicBlock(), nullptr, nullptr);
      load_mem->setDescription((Twine("return.mem.") + Twine(idx) + ".entry").str());
      (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, pseudo_return, load_mem);
      mem_value_t fallback = makeFallbackValues(LocValue::FREE_VARIABLE);
      populateLoadMemoryNode(graph, load_mem, fallback,
                             pseudo_return->getParentBasicBlock(), builder);
      continue;
    }

    for (const auto &ret_vals : ret_val_map) {
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
      auto *linked_ret = LotusGuardedValueFlowAdapterPass::safeLink(
          graph, pseudo_return, load_mem);
      if (linked_ret)
        pseudo_return->addReturnValueSitePair(linked_ret, site);
      if (site)
        load_mem->addUseSite(site);
      mem_value_t values = ret_vals.second;
      if (values.empty())
        values = makeFallbackValues(LocValue::FREE_VARIABLE, ret_inst);
      populateLoadMemoryNode(graph, load_mem, values,
                             ret_inst ? ret_inst->getParent()
                                      : pseudo_return->getParentBasicBlock(),
                             builder);
    }
  }
}

static void materializeCallsiteSummaryNodes(GuardedValueFlowGraph &graph,
                                            IntraLotusAA &pta,
                                            GuardedValueFlowGraphBuilderPass &builder) {
  const auto &call_arg_bindings = pta.getCallArgBindings();
  BasicBlock *entry_block = getEntryBlockOrNull(graph);

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
      int start_ap_depth = callee_graph->getInlineApDepth();
      const std::map<Value *, mem_value_t, llvm_cmp> *callee_input_bindings = nullptr;
      if (call_bind_it != call_arg_bindings.end()) {
        auto callee_it = call_bind_it->second.find(callee);
        if (callee_it != call_bind_it->second.end())
          callee_input_bindings = &callee_it->second;
      }

      const auto &summary_inputs = callee_graph->getSummaryInputs();
      for (unsigned bucket = 0; bucket < summary_inputs.size(); ++bucket) {
        const auto *summary_bucket = summary_inputs[bucket];
        if (!summary_bucket || summary_bucket->empty())
          continue;
        Type *summary_type = getStableSummaryNodeType(call->getContext());
        auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
            GuardedValueFlowNode::Kind::CallSiteArgumentSummary, summary_type,
            &graph, call->getParent(), call, callee, bucket);
        summary_node->setDescription(
            (Twine("call.input.summary.") + Twine(bucket)).str());

        mem_value_t aggregated;
        bool has_missing_binding = !callee_input_bindings;
        if (callee_input_bindings) {
          for (Value *summary_input : *summary_bucket) {
            auto binding_it = callee_input_bindings->find(summary_input);
            if (binding_it == callee_input_bindings->end()) {
              has_missing_binding = true;
              continue;
            }
            aggregated.insert(aggregated.end(), binding_it->second.begin(),
                              binding_it->second.end());
          }
        }
        if (aggregated.empty() || has_missing_binding)
          aggregated.emplace_back(nullptr, nullptr, LocValue::SUMMARY_VALUE, 1.0f);
        if (static_cast<int>(bucket) > start_ap_depth) {
          auto *load_mem = graph.createNode<GuardedValueFlowNode>(
              GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
              entry_block, nullptr, call);
          load_mem->setDescription(
              (Twine("call.input.summary.mem.") + Twine(bucket)).str());
          (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, summary_node,
                                                           load_mem);
          populateLoadMemoryNode(graph, load_mem, aggregated, entry_block,
                                 builder);
        }
        site->setInputSummaryNode(callee, bucket, summary_node);
        graph.registerSummaryArgumentNode(bucket, summary_node);
      }

      const auto &summary_outputs = callee_graph->getSummaryOutputs();
      for (unsigned bucket = 0; bucket < summary_outputs.size(); ++bucket) {
        const mem_value_t *summary_bucket = summary_outputs[bucket];
        if (!summary_bucket || summary_bucket->empty())
          continue;

        Type *summary_type = getStableSummaryNodeType(call->getContext());
        auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
            GuardedValueFlowNode::Kind::CallSiteReturnSummary, summary_type,
            &graph, call->getParent(), call, callee, bucket);
        summary_node->setDescription(
            (Twine("call.output.summary.") + Twine(bucket)).str());

        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
            entry_block, nullptr, call);
        load_mem->setDescription(
            (Twine("call.output.summary.mem.") + Twine(bucket)).str());
        (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, summary_node,
                                                         load_mem);

        mem_value_t imported =
            importSummaryValues(pta, call, callee, *summary_bucket);
        if (imported.empty())
          imported.emplace_back(nullptr, nullptr, LocValue::SUMMARY_VALUE, 1.0f);
        populateLoadMemoryNode(graph, load_mem, imported, entry_block,
                               builder);
        site->setOutputSummaryNode(callee, bucket, summary_node);
        graph.registerSummaryReturnNode(bucket, summary_node);
      }
    }
  }
}

static void materializeCallsiteInterfaces(GuardedValueFlowGraph &graph,
                                          IntraLotusAA &pta, LotusAA &lotus,
                                          GuardedValueFlowGraphBuilderPass &builder) {
  const auto &call_arg_bindings = pta.getCallArgBindings();
  const auto &call_ret_bindings = pta.getCallReturnBindings();
  BasicBlock *entry_block = getEntryBlockOrNull(graph);

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
      if (pta.getFunc() && callee && lotus.isBackEdge(pta.getFunc(), callee))
        continue;

      auto *callee_graph = dyn_cast_or_null<IntraLotusAA>(pta.getPtGraph(callee));
      if (!callee_graph)
        continue;

      auto call_bind_it = call_arg_bindings.find(call);
      const std::map<Value *, mem_value_t, llvm_cmp> *callee_input_bindings = nullptr;
      if (call_bind_it != call_arg_bindings.end()) {
        auto callee_it = call_bind_it->second.find(callee);
        if (callee_it != call_bind_it->second.end())
          callee_input_bindings = &callee_it->second;
      }

      using PseudoInputBinding = std::pair<Value *, const mem_value_t *>;
      std::vector<PseudoInputBinding> ordered_pseudo_inputs(
          callee_graph->getInputs().size(), {nullptr, nullptr});
      for (const auto &input_item : callee_graph->getInputs()) {
        Value *formal_value = input_item.first;
        int raw_pseudo_input_index = callee_graph->getPseudoInputIndex(formal_value);
        if (raw_pseudo_input_index < 0 ||
            static_cast<size_t>(raw_pseudo_input_index) >=
                ordered_pseudo_inputs.size() ||
            ordered_pseudo_inputs[raw_pseudo_input_index].first) {
          LLVM_DEBUG(dbgs() << "[gvg-adapter] Missing pseudo-input index for "
                            << *formal_value << " in callee "
                            << callee->getName() << "\n");
          break;
        }
        const mem_value_t *binding_values = nullptr;
        if (callee_input_bindings) {
          auto binding_it = callee_input_bindings->find(formal_value);
          if (binding_it != callee_input_bindings->end())
            binding_values = &binding_it->second;
        }
        ordered_pseudo_inputs[raw_pseudo_input_index] =
            {formal_value, binding_values};
      }

      for (size_t raw_index = 0; raw_index < ordered_pseudo_inputs.size();
           ++raw_index) {
        Value *formal_value = ordered_pseudo_inputs[raw_index].first;
        if (!formal_value)
          continue;
        const mem_value_t *binding_values = ordered_pseudo_inputs[raw_index].second;

        auto *pseudo_input = graph.createNode<GuardedValueFlowCallOutputNode>(
            GuardedValueFlowNode::Kind::CallSitePseudoInput,
            formal_value->getType(), &graph, call->getParent(), formal_value, call,
            callee);
        pseudo_input->setDescription((Twine("call.input.") + Twine(raw_index)).str());
        setNodeAccessPathFromValue(pseudo_input, *callee_graph, formal_value);

        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, formal_value->getType(), &graph,
            call->getParent(), nullptr, call);
        load_mem->setDescription(
            (Twine("call.input.mem.") + Twine(raw_index)).str());
        (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, pseudo_input,
                                                         load_mem);
        mem_value_t values = binding_values ? *binding_values
                                            : makeFallbackValues(LocValue::FREE_VARIABLE);
        if (values.empty())
          values = makeFallbackValues(LocValue::FREE_VARIABLE);
        populateLoadMemoryNode(graph, load_mem, values, call->getParent(), builder);
        site->addPseudoInput(callee, pseudo_input);
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
        Value *pseudo_value =
            (pseudo_outputs && idx < pseudo_outputs->size()) ? (*pseudo_outputs)[idx]
                                                             : nullptr;
        if (!pseudo_value)
          pseudo_value = createSyntheticInterfaceValue(
              outputs[idx]->getType(),
              (Twine("pseudo.output.") + Twine(idx - 1)).str());
        auto *pseudo_output = graph.createNode<GuardedValueFlowCallOutputNode>(
            GuardedValueFlowNode::Kind::CallSitePseudoOutput,
            outputs[idx]->getType(), &graph, entry_block, pseudo_value, call,
            callee);
        pseudo_output->setDescription(
            (Twine("call.output.") + Twine(idx - 1)).str());
        setNodeAccessPathFromOutputIndex(pseudo_output, *callee_graph,
                                         static_cast<unsigned>(idx));
        graph.mapValueNode(pseudo_value, pseudo_output);

        auto *store_mem = ensureStoreMemoryNode(graph, pseudo_value, call,
                                                outputs[idx]->getType(),
                                                call->getParent());
        store_mem->clearChildren();
        (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, store_mem,
                                                         pseudo_output);
        site->addPseudoOutput(callee, pseudo_output);
      }
    }
  }
}

static void materializeCallTargetConditions(GuardedValueFlowGraph &graph,
                                            IntraLotusAA &pta,
                                            GuardedValueFlowGraphBuilderPass &builder) {
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
            graph.findOrCreateSemanticRegion(
                target.second, call->getParent(),
                resolveOriginConditionNode(builder, target.second)));
    }
  }
}

static void materializeCallsiteBackEdges(GuardedValueFlowGraph &graph,
                                         LotusAA &lotus) {
  Function *caller = graph.getBaseFunction();
  if (!caller)
    return;

  for (const auto &site_ptr : graph.sites()) {
    auto *site = dynamic_cast<GuardedValueFlowCallSite *>(site_ptr.get());
    if (!site)
      continue;

    for (Function *callee : site->getCallees()) {
      if (lotus.isBackEdge(caller, callee))
        site->setBackEdge(callee);
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

GuardedValueFlowNode *LotusGuardedValueFlowAdapterPass::safeLink(
    GuardedValueFlowGraph &graph, GuardedValueFlowNode *parent,
    GuardedValueFlowNode *child, float confidence, ConditionRef condition) {
  if (!parent || !child)
    return nullptr;

  Type *src_ty = child->getType();
  Type *dst_ty = parent->getType();
  if (!src_ty || !dst_ty)
    return nullptr;

  if (src_ty == dst_ty) {
    parent->addChild(child, confidence, condition);
    return child;
  }

  auto opcode_kind = chooseCastOpcode(src_ty, dst_ty);
  if (opcode_kind == GuardedValueFlowOpcodeNode::OpcodeKind::Invalid) {
    LLVM_DEBUG(dbgs() << "[gvg-adapter] Unable to cast-link "
                      << parent->getDescription() << " <- "
                      << child->getDescription() << " in "
                      << graph.getBaseFunction()->getName() << "\n");
    return nullptr;
  }

  BasicBlock *cast_block =
      parent->getParentBasicBlock()
          ? parent->getParentBasicBlock()
          : (child->getParentBasicBlock() ? child->getParentBasicBlock()
                                          : getEntryBlockOrNull(graph));
  auto *cast_node = graph.createNode<GuardedValueFlowOpcodeNode>(
      GuardedValueFlowNode::Kind::CastOpcode, dst_ty, &graph, cast_block,
      opcode_kind);
  cast_node->setDescription("adapter.cast");
  cast_node->addChild(child);

  const DataLayout &DL = graph.getBaseFunction()->getParent()->getDataLayout();
  if (src_ty->isSized() && dst_ty->isSized())
    cast_node->setCastWidths(DL.getTypeSizeInBits(src_ty),
                             DL.getTypeSizeInBits(dst_ty));

  parent->addChild(cast_node, confidence, condition);
  return cast_node;
}

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
      adaptFunction(builder.getGraph(F), *pta, lotus, builder);
  }

  return false;
}

void LotusGuardedValueFlowAdapterPass::adaptFunction(
    GuardedValueFlowGraph &graph, IntraLotusAA &pta, LotusAA &lotus,
    GuardedValueFlowGraphBuilderPass &builder) {
  unsigned pseudo_arg_index = 0;
  for (const auto &input_item : pta.getInputs()) {
    Value *value = input_item.first;
    auto *node = createInterfaceArgumentNode(
        graph, value, value->getType(),
        pta.getFunc()->empty() ? nullptr : &pta.getFunc()->getEntryBlock(),
        "pseudo.input");
    node->setIndex(pseudo_arg_index++);
    setNodeAccessPathFromValue(node, pta, value);
    graph.registerPseudoArgument(node);
  }

  materializeStoreParity(graph, pta);
  materializeLoadParity(graph, pta, builder);
  materializeFunctionOutputs(graph, pta, builder);
  materializeCallTargetConditions(graph, pta, builder);
  materializeCallsiteSummaryNodes(graph, pta, builder);
  materializeCallsiteInterfaces(graph, pta, lotus, builder);
  materializeCallsiteBackEdges(graph, lotus);
}

ModulePass *llvm::gvg::createLotusGuardedValueFlowAdapterPass() {
  return new LotusGuardedValueFlowAdapterPass();
}
