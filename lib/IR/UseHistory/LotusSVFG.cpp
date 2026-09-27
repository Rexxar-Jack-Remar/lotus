#include "IR/UseHistory/LotusSVFG.h"
#include "IR/UseHistory/ResourceHistory.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace lotus {
namespace usehistory {
namespace {

using NativeNode = analysis::SVFGNode;
using NativeEdge = analysis::SVFGEdge;

struct Placement {
  BlockID block = InvalidID;
  EdgeID edge = InvalidID;
  std::size_t position = 0;
};

struct Layout {
  FunctionLayout function;
  std::map<const llvm::BasicBlock *, BlockID> blocks;
  std::vector<const llvm::BasicBlock *> nativeBlocks;
  std::map<const llvm::Instruction *, SiteID> before, returned, after;
  std::map<const llvm::BasicBlock *, SiteID> phi;
  std::map<std::pair<const llvm::BasicBlock *, unsigned>, EdgeID> edges;
  std::map<SiteID, Placement> placements;
  SiteID exit = InvalidID;
};

const llvm::Function *owner(const NativeNode &node) {
  if (const auto *function = node.getFunction()) return function;
  if (const auto *instruction = node.getInstruction()) return instruction->getFunction();
  if (const auto *call = node.getCallSite()) return call->getFunction();
  if (const auto *value = llvm::dyn_cast_or_null<llvm::Instruction>(node.getValue()))
    return value->getFunction();
  return nullptr;
}

const llvm::Instruction *anchor(const NativeNode &node) {
  if (const auto *instruction = node.getInstruction()) return instruction;
  if (const auto *load = llvm::dyn_cast<analysis::LoadMuSVFGNode>(&node))
    return load->getLoadInst();
  if (const auto *store = llvm::dyn_cast<analysis::StoreChiSVFGNode>(&node))
    return store->getStoreInst();
  if (const auto *call = node.getCallSite()) return call;
  return llvm::dyn_cast_or_null<llvm::Instruction>(node.getValue());
}

bool expectedEntry(const NativeNode &node) {
  using analysis::SVFGK;
  switch (node.getNodeKind()) {
  case SVFGK::FormalParm:
  case SVFGK::FormalIn:
  case SVFGK::EntryChi:
  case SVFGK::VarArg:
  case SVFGK::NullPtr:
    return true;
  default:
    return llvm::isa_and_nonnull<llvm::GlobalValue>(node.getValue());
  }
}

std::string instructionText(const llvm::Instruction &instruction) {
  std::string text;
  llvm::raw_string_ostream out(text);
  instruction.print(out);
  return out.str();
}

Layout makeLayout(const llvm::Function &function, FunctionID id) {
  Layout layout;
  layout.function.id = id;
  layout.function.name = function.getName().str();
  Program &program = layout.function.control;
  auto addBlockSite = [&](BlockID block, std::string label) {
    SiteID site = program.addOperation(block, std::move(label));
    layout.placements.emplace(site, Placement{block, InvalidID,
                                      program.blocks()[block].operations.size() - 1});
    return site;
  };
  for (const auto &block : function) {
    layout.blocks.emplace(&block, program.addBlock(block.getName().str()));
    layout.nativeBlocks.push_back(&block);
  }
  for (const auto &block : function) {
    const auto *terminator = block.getTerminator();
    for (unsigned index = 0; index < terminator->getNumSuccessors(); ++index) {
      const auto *successor = terminator->getSuccessor(index);
      EdgeID edge = program.addEdge(layout.blocks.at(&block),
                                    layout.blocks.at(successor),
                                    "successor " + std::to_string(index));
      layout.edges.emplace(std::make_pair(&block, index), edge);
    }
  }
  BlockID exit = program.addBlock("usehistory.exit");
  for (const auto &block : function)
    if (llvm::isa<llvm::ReturnInst>(block.getTerminator()))
      program.addEdge(layout.blocks.at(&block), exit, "return");
  for (const auto &block : function) {
    BlockID current = layout.blocks.at(&block);
    layout.phi.emplace(&block, addBlockSite(current, "phi definitions"));
    for (const auto &instruction : block) {
      if (llvm::isa<llvm::PHINode>(instruction)) continue;
      const std::string description = instructionText(instruction);
      layout.before.emplace(&instruction,
                            addBlockSite(current, "before " + description));
      if (llvm::isa<llvm::CallBase>(instruction))
        layout.returned.emplace(&instruction,
                                addBlockSite(current, "returned " + description));
      layout.after.emplace(&instruction,
                           addBlockSite(current, "after " + description));
    }
  }
  layout.exit = addBlockSite(exit, "function exit");
  return layout;
}

SiteID definitionSite(const NativeNode &node, const Layout &layout) {
  using analysis::SVFGK;
  switch (node.getNodeKind()) {
  case SVFGK::FormalParm:
  case SVFGK::FormalIn:
  case SVFGK::EntryChi:
  case SVFGK::VarArg:
    return InvalidID;
  case SVFGK::FormalOut:
  case SVFGK::FormalRet:
  case SVFGK::RetMu:
    return layout.exit;
  default:
    break;
  }
  if (const auto *instruction = anchor(node)) {
    if (llvm::isa<llvm::PHINode>(instruction))
      return layout.phi.at(instruction->getParent());
    bool before = node.getNodeKind() == SVFGK::LoadMu ||
                  node.getNodeKind() == SVFGK::ActualIn ||
                  node.getNodeKind() == SVFGK::ActualParm ||
                  node.getNodeKind() == SVFGK::CallMu;
    if (node.getNodeKind() == SVFGK::ActualRet ||
        node.getNodeKind() == SVFGK::ActualOut ||
        node.getNodeKind() == SVFGK::CallChi)
      return layout.returned.at(instruction);
    return before ? layout.before.at(instruction) : layout.after.at(instruction);
  }
  if (const auto *icfg = node.getICFGNode()) {
    if (const auto *block = icfg->getBasicBlock()) {
      auto found = layout.phi.find(block);
      if (found != layout.phi.end()) return found->second;
    }
  }
  // Constants and global addresses have no instruction definition.
  return InvalidID;
}

std::vector<SiteID> phiConsumers(const NativeNode &from, const NativeNode &to,
                                  Layout &layout) {
  const auto *phi = llvm::dyn_cast_or_null<llvm::PHINode>(to.getInstruction());
  if (!phi) return {};
  std::vector<SiteID> result;
  for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
    if (phi->getIncomingValue(index) != from.getValue()) continue;
    const auto *predecessor = phi->getIncomingBlock(index);
    unsigned occurrence = 0;
    for (unsigned earlier = 0; earlier < index; ++earlier)
      if (phi->getIncomingBlock(earlier) == predecessor) ++occurrence;
    unsigned seen = 0;
    const auto *terminator = predecessor->getTerminator();
    for (unsigned successor = 0; successor < terminator->getNumSuccessors(); ++successor) {
      if (terminator->getSuccessor(successor) != phi->getParent()) continue;
      if (seen++ != occurrence) continue;
      EdgeID edge = layout.edges.at({predecessor, successor});
      SiteID site = layout.function.control.addEdgeOperation(edge, "phi input");
      layout.placements.emplace(site, Placement{InvalidID, edge,
                             layout.function.control.edges()[edge].operations.size() - 1});
      result.push_back(site);
      break;
    }
  }
  return result;
}

std::vector<SiteID> memoryPhiConsumers(const NativeNode &from,
                                        const NativeNode &to, Layout &layout) {
  const auto *phi = llvm::dyn_cast<analysis::MSSAPhiSVFGNode>(&to);
  const auto *icfg = to.getICFGNode();
  const auto *block = icfg ? icfg->getBasicBlock() : nullptr;
  if (!phi || !block) return {};
  std::vector<SiteID> result;
  std::vector<std::pair<const llvm::BasicBlock *, unsigned>> incoming;
  for (const auto *predecessor : llvm::predecessors(block)) {
    const auto *terminator = predecessor->getTerminator();
    for (unsigned successor = 0; successor < terminator->getNumSuccessors(); ++successor)
      if (terminator->getSuccessor(successor) == block)
        incoming.emplace_back(predecessor, successor);
  }
  for (auto it = phi->opVerBegin(); it != phi->opVerEnd(); ++it) {
    if (it->second.memReg != from.getMemReg() ||
        it->second.version != from.getSSAVersion() || it->first >= incoming.size())
      continue;
    EdgeID edge = layout.edges.at(incoming[it->first]);
    SiteID site = layout.function.control.addEdgeOperation(edge, "memory phi input");
    layout.placements.emplace(site, Placement{InvalidID, edge,
                           layout.function.control.edges()[edge].operations.size() - 1});
    result.push_back(site);
  }
  return result;
}

bool dominatesUse(const Layout &layout, const llvm::DominatorTree &dominators,
                  SiteID definition, SiteID consumer) {
  if (definition == InvalidID) return true;
  Placement from = layout.placements.at(definition);
  Placement to = layout.placements.at(consumer);
  if (from.edge != InvalidID)
    return from.edge == to.edge && from.position < to.position;
  if (to.edge != InvalidID) {
    BlockID predecessor = layout.function.control.edges()[to.edge].from;
    if (from.block == predecessor) return true;
    to.block = predecessor;
  } else if (from.block == to.block) {
    return from.position < to.position;
  }
  const llvm::BasicBlock *source = from.block < layout.nativeBlocks.size() ?
                                     layout.nativeBlocks[from.block] : nullptr;
  const llvm::BasicBlock *target = to.block < layout.nativeBlocks.size() ?
                                     layout.nativeBlocks[to.block] : nullptr;
  return source && target && dominators.dominates(source, target);
}

ObjectSet resourceObjects(const analysis::SVFG &svfg, const llvm::Value *pointer) {
  const auto &pointsTo = svfg.getObjectIds(pointer);
  if (pointsTo.empty()) return ObjectSet::unknown();
  std::vector<ObjectID> objects;
  for (auto object : pointsTo) {
    if (svfg.isUnknownObject(object)) return ObjectSet::unknown();
    objects.push_back(object);
  }
  return ObjectSet::known(std::move(objects));
}

void appendResourceFacts(SVFGHistoryResult &result, const analysis::SVFG &svfg,
                         const llvm::Module &module,
                         const std::map<const llvm::Function *, Layout> &layouts,
                         FunctionID firstLayer) {
  std::map<const llvm::Function *, std::vector<ResourceAccess>> accesses;
  std::set<ObjectID> universe;
  bool hasInternalCall = false;
  auto add = [&](const llvm::Function &function, SiteID site, ObjectSet objects,
                 Event event, Certainty certainty) {
    if (objects.isUnknown())
      result.graph.addIssue("resource points-to set is unknown in " +
                            function.getName().str());
    else
      universe.insert(objects.objects().begin(), objects.objects().end());
    accesses[&function].push_back({site, std::move(objects), event, certainty});
  };
  for (const auto &function : module) {
    auto layout = layouts.find(&function);
    if (layout == layouts.end()) continue;
    for (const auto &block : function) for (const auto &instruction : block) {
      if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
        add(function, layout->second.before.at(load),
            resourceObjects(svfg, load->getPointerOperand()), Event::Dereference,
            Certainty::May);
      if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
        add(function, layout->second.before.at(store),
            resourceObjects(svfg, store->getPointerOperand()), Event::Dereference,
            Certainty::May);
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (!call) continue;
      const auto *callee = call->getCalledFunction();
      if (callee && !callee->isDeclaration()) hasInternalCall = true;
      if (!callee) continue;
      auto name = callee->getName();
      if (name == "free" && call->arg_size() >= 1)
        add(function, layout->second.after.at(call),
            resourceObjects(svfg, call->getArgOperand(0)), Event::Release,
            Certainty::May);
      if ((name == "malloc" || name == "calloc") && call->getType()->isPointerTy())
        add(function, layout->second.after.at(call),
            resourceObjects(svfg, call), Event::Allocate, Certainty::May);
    }
  }
  if (hasInternalCall)
    result.graph.addIssue("resource histories across internal calls are not spliced");
  std::vector<ObjectID> allObjects(universe.begin(), universe.end());
  for (const auto &item : accesses) {
    const auto &layout = layouts.at(item.first);
    ResourceHistoryBuilder::append(result.graph, firstLayer++,
                                   layout.function.name + ".resources",
                                   layout.function.control, item.second,
                                   allObjects);
  }
}

} // namespace

SVFGHistoryResult buildUseHistoryFromLotusSVFG(const analysis::SVFG &svfg,
                                               const llvm::Module &module) {
  SVFGConstructionInput input;
  std::map<const llvm::Function *, Layout> layouts;
  FunctionID nextFunction = 1;
  for (const auto &function : module) {
    if (function.isDeclaration()) continue;
    layouts.emplace(&function, makeLayout(function, nextFunction++));
  }
  std::map<const llvm::Function *, llvm::DominatorTree> dominators;
  for (const auto &entry : layouts)
    dominators[entry.first].recalculate(*const_cast<llvm::Function *>(entry.first));
  FunctionLayout globals;
  globals.id = 0;
  globals.name = "module constants";
  globals.control.addBlock("entry");

  std::vector<const NativeNode *> nodes;
  for (const auto &item : svfg) nodes.push_back(item.second);
  std::sort(nodes.begin(), nodes.end(),
            [](const NativeNode *a, const NativeNode *b) { return a->getId() < b->getId(); });
  for (const NativeNode *node : nodes) {
    const llvm::Function *function = owner(*node);
    auto found = layouts.find(function);
    LocatedSVFGNode located;
    located.id = node->getId();
    located.function = found == layouts.end() ? 0 : found->second.function.id;
    located.definitionSite = found == layouts.end() ? InvalidID :
                             definitionSite(*node, found->second);
    located.label = "SVFG " + std::to_string(node->getId());
    if (function && found == layouts.end())
      input.issues.push_back("SVFG node " + std::to_string(node->getId()) +
                             " belongs to a function without a body");
    if (found != layouts.end() && located.definitionSite == InvalidID &&
        !expectedEntry(*node))
      input.issues.push_back("SVFG node " + std::to_string(node->getId()) +
                             " has no precise definition site");
    if (!function && !expectedEntry(*node))
      input.issues.push_back("SVFG node " + std::to_string(node->getId()) +
                             " has no owning function");
    input.nodes.push_back(std::move(located));
  }

  for (const auto &function : module) for (const auto &block : function)
    for (const auto &instruction : block) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (!call) continue;
      const auto *callee = call->getCalledFunction();
      if (!callee || (callee->isDeclaration() && !callee->isIntrinsic()))
        input.issues.push_back("call in " + function.getName().str() +
                               " has no complete native effect model");
    }

  NativeID nextEdge = 1;
  for (const NativeNode *node : nodes) {
    std::vector<const NativeEdge *> edges(node->getOutEdges().begin(),
                                           node->getOutEdges().end());
    std::sort(edges.begin(), edges.end(), [](const NativeEdge *a, const NativeEdge *b) {
      return std::make_tuple(a->getDstNode()->getId(), a->getEdgeKind()) <
             std::make_tuple(b->getDstNode()->getId(), b->getEdgeKind());
    });
    for (const NativeEdge *edge : edges) {
      const NativeNode &target = *edge->getDstNode();
      const llvm::Function *sourceFunction = owner(*node);
      const llvm::Function *targetFunction = owner(target);
      auto layout = layouts.find(targetFunction);
      std::vector<SiteID> sites;
      if (sourceFunction == targetFunction && layout != layouts.end()) {
        sites = phiConsumers(*node, target, layout->second);
        if (sites.empty()) sites = memoryPhiConsumers(*node, target, layout->second);
        if (sites.empty() &&
            (llvm::isa_and_nonnull<llvm::PHINode>(target.getInstruction()) ||
             llvm::isa<analysis::MSSAPhiSVFGNode>(&target)))
          input.issues.push_back("SVFG edge into phi " +
                                 std::to_string(target.getId()) +
                                 " has no matched incoming CFG edge");
        if (sites.empty()) sites.push_back(definitionSite(target, layout->second));
      } else {
        sites.push_back(InvalidID);
      }
      for (SiteID site : sites) {
        bool boundary = sourceFunction != targetFunction || site == InvalidID;
        bool nonDominating = false;
        if (!boundary &&
            !dominatesUse(layout->second, dominators.at(targetFunction),
                          definitionSite(*node, layout->second), site)) {
          input.issues.push_back("SVFG edge " + std::to_string(nextEdge) +
                                 " has a path-specific non-dominating source");
          boundary = true;
          nonDominating = true;
          site = InvalidID;
        }
        auto located = locateLotusSVFGEdge(*edge, nextEdge++, site, boundary);
        for (auto object : edge->getPointsTo())
          if (svfg.isUnknownObject(object)) located.objects = ObjectSet::unknown();
        if (sourceFunction && targetFunction && sourceFunction != targetFunction &&
            located.kind != FlowKind::Call && located.kind != FlowKind::Return)
          input.issues.push_back("cross-function SVFG edge " +
                                 std::to_string(located.id) +
                                 " has no call/return context");
        if ((located.kind == FlowKind::Call || located.kind == FlowKind::Return) &&
            located.callSite == NoNativeID) {
          const auto *call = node->getCallSite() ? node->getCallSite() : target.getCallSite();
          if (call) located.callSite = reinterpret_cast<std::uintptr_t>(call);
          else {
            located.kind = FlowKind::Summary;
            input.issues.push_back("SVFG edge " + std::to_string(located.id) +
                                   " has no call-site context");
          }
        }
        if (site == InvalidID && sourceFunction == targetFunction && !nonDominating)
          input.issues.push_back("SVFG edge " + std::to_string(located.id) +
                                 " has no local consumer site");
        input.edges.push_back(std::move(located));
      }
    }
  }
  input.functions.push_back(std::move(globals));
  for (const auto &item : layouts) input.functions.push_back(item.second.function);
  auto result = SVFGHistoryBuilder::build(input);
  appendResourceFacts(result, svfg, module, layouts, nextFunction);
  return result;
}

} // namespace usehistory
} // namespace lotus
