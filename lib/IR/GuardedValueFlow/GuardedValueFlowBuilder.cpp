#include "IR/GuardedValueFlow/GuardedValueFlowBuilder.h"

#include "IR/GSA/GSA.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace llvm::gvg;

namespace {

static GuardedValueFlowNode::Kind classifyValueNode(Value *V) {
  if (isa<Argument>(V))
    return GuardedValueFlowNode::Kind::CommonArgument;
  if (isa<PHINode>(V))
    return GuardedValueFlowNode::Kind::Phi;
  if (isa<UndefValue>(V))
    return GuardedValueFlowNode::Kind::UndefValue;
  return GuardedValueFlowNode::Kind::SimpleOperand;
}

static GuardedValueFlowNode *
findOrCreateValueNode(GuardedValueFlowGraph &graph, Value *V) {
  if (!V)
    return nullptr;
  auto *existing = graph.findNode(V);
  if (existing)
    return existing;

  auto *block = dyn_cast<Instruction>(V) ? cast<Instruction>(V)->getParent()
                                         : nullptr;
  auto *node = graph.createNode<GuardedValueFlowNode>(classifyValueNode(V),
                                                      V->getType(), &graph,
                                                      block, V,
                                                      dyn_cast<Instruction>(V));
  if (V->hasName())
    node->setDescription(V->getName().str());
  graph.mapValueNode(V, node);
  return node;
}

static void buildRegions(GuardedValueFlowGraph &graph, Function &F,
                         gsa::ControlDependenceAnalysis &cda) {
  for (BasicBlock &BB : F) {
    ConditionRef cond = ConditionRef::none();
    if (cda.isTracked(BB)) {
      auto deps = cda.getCDBlocks(&BB);
      if (!deps.empty()) {
        BasicBlock *dep = deps.front();
        Instruction *term = dep->getTerminator();
        if (auto *br = dyn_cast<BranchInst>(term)) {
          if (br->isConditional()) {
            bool sense = br->getSuccessor(0) == &BB;
            cond = ConditionRef::fromGuard(
                sense ? gsa::GuardKind::BranchTrue
                      : gsa::GuardKind::BranchFalse,
                dep, &BB, br->getCondition());
          }
        }
      }
    }

    auto *region =
        graph.createNode<GuardedValueFlowRegionNode>(Type::getVoidTy(F.getContext()),
                                                     &graph, &BB, cond);
    region->setDescription((Twine("region.") + BB.getName()).str());
    graph.mapRegion(&BB, region);
  }
}

static void buildInstruction(GuardedValueFlowGraph &graph, Instruction &I) {
  auto *block = I.getParent();
  auto *value_node = I.getType()->isVoidTy() ? nullptr : findOrCreateValueNode(graph, &I);

  switch (I.getOpcode()) {
  case Instruction::Alloca: {
    auto *site = graph.createSite<GuardedValueFlowSite>(
        GuardedValueFlowSite::Kind::Alloc, &graph, &I);
    if (value_node)
      value_node->addUseSite(site);
    break;
  }
  case Instruction::Load: {
    auto *mem_node = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::LoadMemory, I.getType(), &graph, block,
        nullptr, &I);
    mem_node->setDescription("load.mem");
    graph.mapLoadMemoryNode(&I, mem_node);
    if (value_node)
      value_node->addChild(mem_node);
    auto *site = graph.createSite<GuardedValueFlowDereferenceSite>(&graph, &I);
    auto *ptr_node = findOrCreateValueNode(graph, I.getOperand(0));
    ptr_node->addUseSite(site);
    site->setPointerOperand(ptr_node);
    if (value_node)
      value_node->addUseSite(site);
    break;
  }
  case Instruction::Store: {
    auto *stored = findOrCreateValueNode(graph, I.getOperand(0));
    auto *mem_node = graph.createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::StoreMemory, stored->getType(), &graph,
        block, nullptr, &I);
    mem_node->setDescription("store.mem");
    graph.mapStoreMemoryNode(I.getOperand(0), &I, mem_node);
    mem_node->addChild(stored);
    auto *site = graph.createSite<GuardedValueFlowDereferenceSite>(&graph, &I);
    auto *ptr_node = findOrCreateValueNode(graph, I.getOperand(1));
    ptr_node->addUseSite(site);
    stored->addUseSite(site);
    site->setPointerOperand(ptr_node);
    site->setValueOperand(stored);
    break;
  }
  case Instruction::Call:
  case Instruction::Invoke: {
    auto *site = graph.createSite<GuardedValueFlowCallSite>(&graph, &I);
    graph.mapCallSite(&I, site);

    if (auto *call = dyn_cast<CallBase>(&I)) {
      if (Function *callee = call->getCalledFunction())
        site->addCallee(callee);
      for (Value *arg : call->args())
        site->addCommonInput(findOrCreateValueNode(graph, arg));
      if (value_node) {
        auto *output = graph.createNode<GuardedValueFlowCallOutputNode>(
            GuardedValueFlowNode::Kind::CallSiteCommonOutput, I.getType(),
            &graph, block, &I, &I, nullptr);
        output->setDescription("call.output");
        site->setCommonOutput(output);
        graph.mapValueNode(&I, output);
      }
    }
    break;
  }
  case Instruction::PHI: {
    auto *phi = cast<PHINode>(&I);
    for (unsigned idx = 0; idx < phi->getNumIncomingValues(); ++idx) {
      auto *incoming = findOrCreateValueNode(graph, phi->getIncomingValue(idx));
      BasicBlock *incoming_bb = phi->getIncomingBlock(idx);
      ConditionRef cond = ConditionRef::none();
      if (auto *br = dyn_cast<BranchInst>(incoming_bb->getTerminator())) {
        if (br->isConditional()) {
          bool sense = br->getSuccessor(0) == block;
          cond = ConditionRef::fromGuard(
              sense ? gsa::GuardKind::BranchTrue
                    : gsa::GuardKind::BranchFalse,
              incoming_bb, block, br->getCondition());
        }
      }
      if (value_node)
        value_node->addChild(incoming, 1.0f, cond);
    }
    break;
  }
  case Instruction::Select: {
    auto *sel = cast<SelectInst>(&I);
    auto *true_node = findOrCreateValueNode(graph, sel->getTrueValue());
    auto *false_node = findOrCreateValueNode(graph, sel->getFalseValue());
    if (value_node) {
      value_node->addChild(
          true_node, 1.0f,
          ConditionRef::fromGuard(gsa::GuardKind::BranchTrue, block, block,
                                  sel->getCondition()));
      value_node->addChild(
          false_node, 1.0f,
          ConditionRef::fromGuard(gsa::GuardKind::BranchFalse, block, block,
                                  sel->getCondition()));
    }
    break;
  }
  case Instruction::GetElementPtr:
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt: {
    if (value_node)
      value_node->addChild(findOrCreateValueNode(graph, I.getOperand(0)));
    break;
  }
  default:
    break;
  }
}

} // namespace

char GuardedValueFlowGraphBuilderPass::ID = 0;
static RegisterPass<GuardedValueFlowGraphBuilderPass>
    X("gvg-builder", "GuardedValueFlowGraph builder", false, true);

GuardedValueFlowGraphBuilderPass::GuardedValueFlowGraphBuilderPass()
    : ModulePass(ID) {}

void GuardedValueFlowGraphBuilderPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<gsa::ControlDependenceAnalysisPass>();
  AU.addRequired<gsa::GateAnalysisPass>();
}

bool GuardedValueFlowGraphBuilderPass::runOnModule(Module &M) {
  graphs_.clear();
  auto &cda_pass = getAnalysis<gsa::ControlDependenceAnalysisPass>();
  (void)getAnalysis<gsa::GateAnalysisPass>();

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (!cda_pass.hasAnalysisFor(F))
      continue;
    graphs_[&F] = buildGraph(F);
  }
  return false;
}

bool GuardedValueFlowGraphBuilderPass::hasGraphFor(const Function &F) const {
  return graphs_.find(&F) != graphs_.end();
}

GuardedValueFlowGraph &
GuardedValueFlowGraphBuilderPass::getGraph(const Function &F) {
  auto it = graphs_.find(&F);
  assert(it != graphs_.end() && "Requested missing GuardedValueFlowGraph");
  return *it->second;
}

std::unique_ptr<GuardedValueFlowGraph>
GuardedValueFlowGraphBuilderPass::buildGraph(Function &F) {
  auto graph = std::make_unique<GuardedValueFlowGraph>(&F);
  auto &cda =
      getAnalysis<gsa::ControlDependenceAnalysisPass>().getControlDependenceAnalysis(F);

  buildRegions(*graph, F, cda);

  for (Argument &arg : F.args())
    (void)findOrCreateValueNode(*graph, &arg);

  if (!F.getReturnType()->isVoidTy()) {
    auto *ret_node = graph->createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::CommonReturn, F.getReturnType(),
        graph.get(), &F.getEntryBlock(), nullptr, F.getEntryBlock().getTerminator());
    ret_node->setDescription("return.common");
  }

  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      buildInstruction(*graph, I);

  return graph;
}

ModulePass *llvm::gvg::createGuardedValueFlowGraphBuilderPass() {
  return new GuardedValueFlowGraphBuilderPass();
}
