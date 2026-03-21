#include "IR/GuardedValueFlow/LotusAdapter.h"

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GuardedValueFlow/ConditionRef.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
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
  if (auto *existing = graph.findInterfaceNode(value))
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

static GuardedValueFlowRegionNode *translatePathCondToRegion(
    GuardedValueFlowGraphBuilderPass &builder, GuardedValueFlowGraph &graph,
    path_cond_t cond, BasicBlock *fallback_block);

static GuardedValueFlowNode *resolveOriginConditionNode(
    GuardedValueFlowGraphBuilderPass &builder, path_cond_t cond);
static GuardedValueFlowNode *resolveProducerValueNode(
    GuardedValueFlowGraph &graph, Value *value, Type *type, BasicBlock *bb);

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

enum class SummaryDirection {
  Input,
  Output,
};

struct SummarySentinelProvenance {
  Instruction *callsite{nullptr};
  Function *callee{nullptr};
  unsigned bucket{0};
  SummaryDirection direction{SummaryDirection::Output};
};

static bool instructionReachesInstruction(const Instruction *src,
                                          const Instruction *dst) {
  if (!src || !dst)
    return false;

  const BasicBlock *src_bb = src->getParent();
  const BasicBlock *dst_bb = dst->getParent();
  if (!src_bb || !dst_bb)
    return false;

  if (src_bb == dst_bb)
    return src != dst && src->comesBefore(dst);

  SmallVector<const BasicBlock *, 16> worklist;
  SmallPtrSet<const BasicBlock *, 16> visited;
  for (const BasicBlock *succ : successors(src_bb)) {
    if (visited.insert(succ).second)
      worklist.push_back(succ);
  }

  while (!worklist.empty()) {
    const BasicBlock *bb = worklist.pop_back_val();
    if (bb == dst_bb)
      return true;
    for (const BasicBlock *succ : successors(bb)) {
      if (visited.insert(succ).second)
        worklist.push_back(succ);
    }
  }

  return false;
}

static void collectExactPointerStoreFallbacks(Function &func, LoadInst *load,
                                              mem_value_t &values) {
  if (!load)
    return;

  Value *load_ptr = load->getPointerOperand();
  if (!load_ptr)
    return;

  for (Instruction &inst : instructions(func)) {
    auto *store = dyn_cast<StoreInst>(&inst);
    if (!store || store->getPointerOperand() != load_ptr)
      continue;
    if (!instructionReachesInstruction(store, load))
      continue;
    values.emplace_back(nullptr, store, store->getValueOperand(), 1.0f);
  }
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

static GuardedValueFlowNode *createSummaryProducerNode(
    GuardedValueFlowGraph &graph, Type *type, BasicBlock *bb,
    const SummarySentinelProvenance &provenance) {
  auto *summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary, type, &graph, bb,
      provenance.callsite, provenance.callee, provenance.bucket);
  summary_node->setDescription(
      provenance.direction == SummaryDirection::Input ? "summary.input.value"
                                                      : "summary.output.value");
  return summary_node;
}

static BasicBlock *getValueBlockOrEntry(GuardedValueFlowGraph &graph, Value *value) {
  if (auto *inst = dyn_cast_or_null<Instruction>(value))
    return inst->getParent();
  return getEntryBlockOrNull(graph);
}

static GuardedValueFlowNode *
findOrCreateConditionNode(GuardedValueFlowGraph &graph, Value *value) {
  if (!value)
    return nullptr;
  if (auto *existing = graph.findNode(value))
    return existing;
  if (auto *existing = graph.findInterfaceNode(value))
    return existing;
  return ensureValueNode(graph, value, value->getType(),
                         getValueBlockOrEntry(graph, value),
                         GuardedValueFlowNode::Kind::SimpleOperand,
                         "path.cond");
}

static GuardedValueFlowRegionNode *
translateLocalPathCondToRegion(GuardedValueFlowGraph &graph, path_cond_t cond,
                               BasicBlock *fallback_block) {
  if (!cond)
    return graph.getAlwaysTrueRegion();

  Function *owner = cond->getOwnerFunc();
  if (owner && owner != graph.getBaseFunction())
    return nullptr;

  switch (cond->getKind()) {
  case PathCond::Kind::True:
    return graph.getAlwaysTrueRegion();
  case PathCond::Kind::False:
    return graph.getAlwaysFalseRegion();
  case PathCond::Kind::ValueAtom: {
    auto *condition_node = findOrCreateConditionNode(graph, cond->getValue());
    if (!condition_node)
      return nullptr;
    BasicBlock *block =
        condition_node->getParentBasicBlock()
            ? condition_node->getParentBasicBlock()
            : getValueBlockOrEntry(graph, cond->getValue());
    return graph.findOrCreateUnitRegion(condition_node, cond->getSense(), block,
                                        ConditionRef::fromPathCond(cond));
  }
  case PathCond::Kind::BranchAtom: {
    auto *condition_node = findOrCreateConditionNode(graph, cond->getValue());
    if (!condition_node)
      return nullptr;
    BasicBlock *block = cond->getSuccessor();
    if (!block)
      block = cond->getBlock() ? cond->getBlock() : fallback_block;
    return graph.findOrCreateUnitRegion(condition_node, cond->getSense(), block,
                                        ConditionRef::fromPathCond(cond));
  }
  case PathCond::Kind::BlockAtom:
    if (cond->getBlock() == nullptr)
      return graph.getAlwaysTrueRegion();
    if (auto *region = graph.findRegion(cond->getBlock()))
      return region;
    if (graph.getBaseFunction() && !graph.getBaseFunction()->empty() &&
        cond->getBlock() == &graph.getBaseFunction()->getEntryBlock()) {
      return graph.getAlwaysTrueRegion();
    }
    return nullptr;
  case PathCond::Kind::Not: {
    auto *input = translateLocalPathCondToRegion(graph, cond->getLhs(),
                                                 fallback_block);
    return input ? graph.findOrCreateNotRegion(input, fallback_block) : nullptr;
  }
  case PathCond::Kind::And: {
    auto *lhs =
        translateLocalPathCondToRegion(graph, cond->getLhs(), fallback_block);
    auto *rhs =
        translateLocalPathCondToRegion(graph, cond->getRhs(), fallback_block);
    if (!lhs || !rhs)
      return nullptr;
    return graph.findOrCreateAndRegion(lhs, rhs, fallback_block);
  }
  case PathCond::Kind::Or: {
    auto *lhs =
        translateLocalPathCondToRegion(graph, cond->getLhs(), fallback_block);
    auto *rhs =
        translateLocalPathCondToRegion(graph, cond->getRhs(), fallback_block);
    if (!lhs || !rhs)
      return nullptr;
    return graph.findOrCreateOrRegion(lhs, rhs, fallback_block);
  }
  case PathCond::Kind::ImportedAtom:
  case PathCond::Kind::CallTargetAtom:
  case PathCond::Kind::SwitchCaseAtom:
  case PathCond::Kind::SwitchDefaultAtom:
  case PathCond::Kind::InvokeNormalAtom:
  case PathCond::Kind::InvokeUnwindAtom:
    return nullptr;
  }
  return nullptr;
}

static GuardedValueFlowRegionNode *translatePathCondToRegion(
    GuardedValueFlowGraphBuilderPass &builder, GuardedValueFlowGraph &graph,
    path_cond_t cond, BasicBlock *fallback_block) {
  if (!cond)
    return graph.getAlwaysTrueRegion();
  if (auto *structural =
          translateLocalPathCondToRegion(graph, cond, fallback_block)) {
    return structural;
  }
  return graph.findOrCreateSemanticRegion(
      cond, fallback_block, resolveOriginConditionNode(builder, cond));
}

static GuardedValueFlowNode *
ensureStoreMemoryNode(GuardedValueFlowGraph &graph, Value *value,
                      Instruction *inst, Type *memory_type, BasicBlock *bb,
                      const SummarySentinelProvenance *summary_provenance =
                          nullptr) {
  if (value == LocValue::FREE_VARIABLE || value == LocValue::NO_VALUE)
    return nullptr;

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
    SummarySentinelProvenance fallback_provenance;
    fallback_provenance.callsite = inst;
    const SummarySentinelProvenance &provenance =
        summary_provenance ? *summary_provenance : fallback_provenance;
    GuardedValueFlowNode *summary_node =
        createSummaryProducerNode(graph, memory_type, bb, provenance);
    (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, mem_node,
                                                     summary_node);
  } else {
    auto *value_node = resolveProducerValueNode(graph, value, memory_type, bb);
    (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, mem_node, value_node);
  }

  return mem_node;
}

static GuardedValueFlowRegionNode *resolveMatchingRegion(
    GuardedValueFlowGraphBuilderPass &builder, GuardedValueFlowGraph &graph,
    GuardedValueFlowNode *producer_mem, const mem_value_item_t &item) {
  if (item.cond)
    return translatePathCondToRegion(
        builder, graph, item.cond,
        producer_mem ? producer_mem->getParentBasicBlock() : nullptr);
  if (producer_mem && producer_mem->getRegion())
    return producer_mem->getRegion();
  return graph.getAlwaysTrueRegion();
}

static void linkMemoryValue(GuardedValueFlowGraph &graph,
                            GuardedValueFlowNode *load_mem_node,
                            const mem_value_item_t &item, BasicBlock *bb,
                            GuardedValueFlowGraphBuilderPass &builder,
                            const SummarySentinelProvenance *summary_provenance =
                                nullptr) {
  Value *value = item.val;
  auto cond = item.cond ? ConditionRef::fromPathCond(item.cond)
                        : ConditionRef::none();
  Instruction *producer_inst = item.pos;
  auto *producer_mem = ensureStoreMemoryNode(graph, value, producer_inst,
                                             load_mem_node->getType(), bb,
                                             summary_provenance);
  if (!producer_mem)
    return;
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
                                   GuardedValueFlowGraphBuilderPass &builder,
                                   const SummarySentinelProvenance *summary_provenance =
                                       nullptr) {
  load_mem_node->clearChildren();
  load_mem_node->clearMatchingRegions();
  for (const auto &item : values)
    linkMemoryValue(graph, load_mem_node, item, bb, builder,
                    item.val == LocValue::SUMMARY_VALUE ? summary_provenance
                                                        : nullptr);
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
  auto *origin_region = translatePathCondToRegion(
      builder, origin_graph, imported_source, getFunctionEntryBlockOrNull(origin_func));
  if (origin_region && origin_region->getConditionNode())
    return origin_region->getConditionNode();

  origin_region = origin_graph.findOrCreateSemanticRegion(
      imported_source, getFunctionEntryBlockOrNull(origin_func));
  return origin_region ? origin_region->getConditionNode() : nullptr;
}

static GuardedValueFlowNode *
resolveProducerValueNode(GuardedValueFlowGraph &graph, Value *value, Type *type,
                         BasicBlock *bb) {
  if (!value)
    return nullptr;
  if (auto *pseudo_arg = graph.findPseudoArgumentBySource(value))
    return pseudo_arg;
  if (auto *existing = graph.findInterfaceNode(value))
    return existing;
  return ensureValueNode(graph, value, type ? type : value->getType(), bb,
                         GuardedValueFlowNode::Kind::SimpleOperand, "producer");
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
  SmallPtrSet<LoadInst *, 16> populated_representatives;
  for (Instruction &inst : instructions(*pta.getFunc())) {
    auto *load = dyn_cast<LoadInst>(&inst);
    if (!load)
      continue;

    auto *load_value_node = graph.findNode(load);
    const auto &equivalent_loads = pta.getAllLoadWithSameValue(load);
    LoadInst *representative =
        equivalent_loads.empty() ? load : *equivalent_loads.begin();
    auto *load_mem_node = graph.findLoadMemoryNode(representative);
    if (!load_mem_node)
      load_mem_node = graph.findLoadMemoryNode(load);
    if (!load_value_node || !load_mem_node)
      continue;

    if (equivalent_loads.empty()) {
      load_value_node->clearChildren();
      load_value_node->addChild(load_mem_node);
      graph.mapLoadMemoryNode(load, load_mem_node);
    } else {
      for (LoadInst *equivalent_load : equivalent_loads) {
        if (auto *equivalent_value_node = graph.findNode(equivalent_load)) {
          equivalent_value_node->clearChildren();
          equivalent_value_node->addChild(load_mem_node);
        }
        graph.mapLoadMemoryNode(equivalent_load, load_mem_node);
      }
    }

    if (!populated_representatives.insert(representative).second)
      continue;

    mem_value_t load_values;
    pta.getLoadValues(representative->getPointerOperand(), representative,
                      load_values);
    if (load_values.empty())
      collectExactPointerStoreFallbacks(*pta.getFunc(), representative,
                                        load_values);
    populateLoadMemoryNode(graph, load_mem_node, load_values,
                           representative->getParent(), builder);
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
    auto *value_node = resolveProducerValueNode(
        graph, store->getValueOperand(), store->getValueOperand()->getType(),
        store->getParent());
    (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, mem_node, value_node);
  }
}

static void materializeFunctionOutputs(GuardedValueFlowGraph &graph,
                                       IntraLotusAA &pta,
                                       GuardedValueFlowGraphBuilderPass &builder) {
  const auto &outputs = pta.getOutputs();
  for (size_t idx = 1; idx < outputs.size(); ++idx) {
    const auto *output = outputs[idx];
    auto *pseudo_return = graph.getPseudoReturn(static_cast<unsigned>(idx - 1));
    if (!pseudo_return) {
      Value *pseudo_value = graph.createSyntheticInterfaceValue(
          output->getType(),
          (Twine("pseudo.return.value.") + Twine(idx - 1)).str());
      pseudo_return = graph.createNode<GuardedValueFlowReturnNode>(
          GuardedValueFlowNode::Kind::PseudoReturn, output->getType(), &graph,
          pta.getFunc()->empty() ? nullptr : &pta.getFunc()->getEntryBlock(),
          pseudo_value);
      pseudo_return->setDescription((Twine("pseudo.return.") + Twine(idx)).str());
      pseudo_return->setIndex(static_cast<unsigned>(idx - 1));
      graph.mapInterfaceNode(pseudo_value, pseudo_return);
      graph.registerPseudoReturn(pseudo_return);
    } else if (pseudo_return->getLLVMValue()) {
      graph.mapInterfaceNode(pseudo_return->getLLVMValue(), pseudo_return);
    }
    pseudo_return->clearChildren();
    setNodeAccessPathFromOutputIndex(pseudo_return, pta,
                                     static_cast<unsigned>(idx));

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
        auto *summary_node = dyn_cast_or_null<GuardedValueFlowCallSummaryNode>(
            site->getInputSummaryNode(callee, bucket));
        if (!summary_node) {
          summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
              GuardedValueFlowNode::Kind::CallSiteArgumentSummary, summary_type,
              &graph, call->getParent(), call, callee, bucket);
          summary_node->setDescription(
              (Twine("call.input.summary.") + Twine(bucket)).str());
          site->setInputSummaryNode(callee, bucket, summary_node);
        } else {
          summary_node->clearChildren();
        }

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
        SummarySentinelProvenance summary_provenance{
            call, callee, bucket, SummaryDirection::Input};
        auto *load_mem = graph.createNode<GuardedValueFlowNode>(
            GuardedValueFlowNode::Kind::LoadMemory, summary_type, &graph,
            entry_block, nullptr, call);
        load_mem->setDescription(
            (Twine("call.input.summary.mem.") + Twine(bucket)).str());
        (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, summary_node,
                                                         load_mem);
        populateLoadMemoryNode(graph, load_mem, aggregated, entry_block,
                               builder, &summary_provenance);
        graph.registerSummaryArgumentNode(bucket, summary_node);
      }

      const auto &summary_outputs = callee_graph->getSummaryOutputs();
      for (unsigned bucket = 0; bucket < summary_outputs.size(); ++bucket) {
        const mem_value_t *summary_bucket = summary_outputs[bucket];
        if (!summary_bucket || summary_bucket->empty())
          continue;

        Type *summary_type = getStableSummaryNodeType(call->getContext());
        auto *summary_node = dyn_cast_or_null<GuardedValueFlowCallSummaryNode>(
            site->getOutputSummaryNode(callee, bucket));
        if (!summary_node) {
          summary_node = graph.createNode<GuardedValueFlowCallSummaryNode>(
              GuardedValueFlowNode::Kind::CallSiteReturnSummary, summary_type,
              &graph, call->getParent(), call, callee, bucket);
          summary_node->setDescription(
              (Twine("call.output.summary.") + Twine(bucket)).str());
          site->setOutputSummaryNode(callee, bucket, summary_node);
        } else {
          summary_node->clearChildren();
        }

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
        SummarySentinelProvenance summary_provenance{
            call, callee, bucket, SummaryDirection::Output};
        populateLoadMemoryNode(graph, load_mem, imported, entry_block,
                               builder, &summary_provenance);
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

        auto *pseudo_input = dyn_cast_or_null<GuardedValueFlowCallOutputNode>(
            site->getPseudoInput(callee, static_cast<unsigned>(raw_index)));
        if (!pseudo_input) {
          Value *pseudo_value = graph.createSyntheticInterfaceValue(
              formal_value->getType(),
              (Twine("pseudo.input.") + callee->getName() + "." +
               Twine(raw_index))
                  .str());
          pseudo_input = graph.createNode<GuardedValueFlowCallOutputNode>(
              GuardedValueFlowNode::Kind::CallSitePseudoInput,
              formal_value->getType(), &graph, call->getParent(), pseudo_value,
              call, callee);
          pseudo_input->setDescription(
              (Twine("call.input.") + Twine(raw_index)).str());
          graph.mapInterfaceNode(pseudo_value, pseudo_input);
          site->addPseudoInput(callee, pseudo_input);
        } else if (pseudo_input->getLLVMValue()) {
          graph.mapInterfaceNode(pseudo_input->getLLVMValue(), pseudo_input);
        }
        pseudo_input->clearChildren();
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
        auto *pseudo_output = dyn_cast_or_null<GuardedValueFlowCallOutputNode>(
            site->getPseudoOutput(callee, static_cast<unsigned>(idx - 1)));
        Value *pseudo_value = pseudo_output ? pseudo_output->getLLVMValue() : nullptr;
        if (!pseudo_value) {
          pseudo_value =
              (pseudo_outputs && idx < pseudo_outputs->size()) ? (*pseudo_outputs)[idx]
                                                               : nullptr;
        }
        if (!pseudo_value) {
          pseudo_value = graph.createSyntheticInterfaceValue(
              outputs[idx]->getType(),
              (Twine("pseudo.output.") + Twine(idx - 1)).str());
        }
        if (!pseudo_output) {
          pseudo_output = graph.createNode<GuardedValueFlowCallOutputNode>(
              GuardedValueFlowNode::Kind::CallSitePseudoOutput,
              outputs[idx]->getType(), &graph, entry_block, pseudo_value, call,
              callee);
          pseudo_output->setDescription(
              (Twine("call.output.") + Twine(idx - 1)).str());
          site->addPseudoOutput(callee, pseudo_output);
        }
        graph.mapInterfaceNode(pseudo_value, pseudo_output);
        setNodeAccessPathFromOutputIndex(pseudo_output, *callee_graph,
                                         static_cast<unsigned>(idx));

        auto *store_mem = ensureStoreMemoryNode(graph, pseudo_value, call,
                                                outputs[idx]->getType(),
                                                call->getParent());
        store_mem->clearChildren();
        (void)LotusGuardedValueFlowAdapterPass::safeLink(graph, store_mem,
                                                         pseudo_output);
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
            translatePathCondToRegion(builder, graph, target.second,
                                      call->getParent()));
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
    BasicBlock *bridge_block =
        parent->getParentBasicBlock()
            ? parent->getParentBasicBlock()
            : (child->getParentBasicBlock() ? child->getParentBasicBlock()
                                            : getEntryBlockOrNull(graph));
    auto *bridge_node = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::Unknown, dst_ty, &graph, bridge_block,
        nullptr, nullptr);
    bridge_node->setDescription("adapter.bridge");
    bridge_node->addChild(child);
    parent->addChild(bridge_node, confidence, condition);
    return bridge_node;
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
    auto *node = graph.getPseudoArgument(pseudo_arg_index);
    if (!node) {
      node = createInterfaceArgumentNode(
          graph, value, value->getType(),
          pta.getFunc()->empty() ? nullptr : &pta.getFunc()->getEntryBlock(),
          "pseudo.input");
      node->setIndex(pseudo_arg_index);
      graph.registerPseudoArgument(node);
    }
    graph.mapPseudoArgumentSource(value, node);
    setNodeAccessPathFromValue(node, pta, value);
    ++pseudo_arg_index;
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
