#include "IR/UseTraceSSA/SVFGBridge.h"
#include "IR/UseTraceSSA/InstructionLabels.h"
#include "IR/UseTraceSSA/TemporalHistory.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace lotus {
namespace usetracessa {
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

Layout makeLayout(const llvm::Function &function, FunctionID id,
                  const detail::InstructionLabels &labels) {
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
  BlockID exit = program.addBlock("usetracessa.exit");
  for (const auto &block : function)
    if (llvm::isa<llvm::ReturnInst>(block.getTerminator()))
      program.addEdge(layout.blocks.at(&block), exit, "return");
  for (const auto &block : function) {
    BlockID current = layout.blocks.at(&block);
    layout.phi.emplace(&block, addBlockSite(current, "phi definitions"));
    for (const auto &instruction : block) {
      if (llvm::isa<llvm::PHINode>(instruction)) continue;
      const std::string &description = labels.get(instruction);
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

struct ResourceProperty {
  Event events;
  Event sources;
  Event required;
  bool exits;
};

ResourceProperty resourceProperty(NativeHistoryMode mode) {
  const Event heap = Event::Allocate | Event::Release;
  switch (mode) {
  case NativeHistoryMode::DoubleFree: return {heap, heap, Event::Release, false};
  case NativeHistoryMode::UseAfterFree:
    return {heap | Event::Dereference, heap, Event::Release, false};
  case NativeHistoryMode::MemoryLeak:
    return {heap | Event::Escape, Event::Allocate, Event::Allocate, true};
  case NativeHistoryMode::FileLeak:
    return {Event::Open | Event::Close | Event::Escape, Event::Open, Event::Open, true};
  case NativeHistoryMode::Full:
    return {heap | Event::Dereference | Event::Open | Event::Close,
            Event::None, Event::None, true};
  }
  throw std::invalid_argument("UseTraceSSA: invalid native history mode");
}

Event resourceEvent(const llvm::Instruction &instruction, const ResourceProperty &property) {
  if (hasEvent(property.events, Event::Escape))
    if (const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
      if (ret->getReturnValue() && ret->getReturnValue()->getType()->isPointerTy())
        return Event::Escape;
  if (hasEvent(property.events, Event::Dereference) &&
      (llvm::isa<llvm::LoadInst>(instruction) ||
       llvm::isa<llvm::StoreInst>(instruction)))
    return Event::Dereference;
  const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
  const auto *callee = call ? call->getCalledFunction() : nullptr;
  if (!callee) return Event::None;
  auto name = callee->getName();
  Event event = Event::None;
  if (name == "free" && call->arg_size()) event = Event::Release;
  if ((name == "malloc" || name == "calloc") && call->getType()->isPointerTy())
    event = Event::Allocate;
  if (name == "fopen" && call->getType()->isPointerTy()) event = Event::Open;
  if (name == "fclose" && call->arg_size()) event = Event::Close;
  return hasEvent(property.events, event) ? event : Event::None;
}

bool structuralCall(const llvm::Instruction &instruction) {
  const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
  if (!call) return false;
  const auto *callee = call->getCalledFunction();
  // Internal calls retain their control/return behavior even when the callee
  // has no resource event. Unknown calls retain a conservative placeholder.
  if (!callee || !callee->isDeclaration()) return true;
  if (callee->isIntrinsic()) return false;
  auto name = callee->getName();
  return name != "free" && name != "malloc" && name != "calloc" &&
         name != "fopen" && name != "fclose";
}

struct ResourcePlan {
  ResourceProperty property;
  bool hasRequired = false;
  ObjectSet universe = ObjectSet::unknown();
  std::map<const llvm::Instruction *, GuardedEventEffect> effects;
  bool keeps(const llvm::Instruction &instruction) const {
    return effects.count(&instruction) || structuralCall(instruction);
  }
};

Layout makeResourceLayout(const llvm::Function &function, FunctionID id,
                          const ResourcePlan &plan, const detail::InstructionLabels &labels) {
  Layout layout;
  layout.function.id = id;
  layout.function.name = function.getName().str();
  Program &program = layout.function.control;
  for (const auto &block : function)
    layout.blocks.emplace(&block, program.addBlock(block.getName().str()));
  for (const auto &block : function) {
    const auto *terminator = block.getTerminator();
    for (unsigned successor = 0; successor < terminator->getNumSuccessors(); ++successor)
      program.addEdge(layout.blocks.at(&block),
                      layout.blocks.at(terminator->getSuccessor(successor)));
  }
  for (const auto &block : function) {
    BlockID current = layout.blocks.at(&block);
    for (const auto &instruction : block) {
      if (!plan.keeps(instruction)) continue;
      bool dereference = llvm::isa<llvm::LoadInst>(instruction) ||
                         llvm::isa<llvm::StoreInst>(instruction);
      SiteID returned = InvalidID;
      if (structuralCall(instruction) && plan.effects.count(&instruction))
        returned = program.addOperation(current, "returned " + labels.get(instruction));
      SiteID site = program.addOperation(current,
                           std::string(dereference ? "before " : "after ") +
                           labels.get(instruction));
      if (dereference) layout.before.emplace(&instruction, site);
      else {
        layout.after.emplace(&instruction, site);
        layout.returned.emplace(&instruction, returned == InvalidID ? site : returned);
      }
    }
  }
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

ResourcePlan planResources(const analysis::SVFG &svfg, const llvm::Module &module,
                           NativeHistoryMode mode) {
  ResourcePlan plan{resourceProperty(mode)};
  plan.hasRequired = mode == NativeHistoryMode::Full;
  std::set<const llvm::Function *> called;
  std::map<const llvm::Instruction *, ObjectID> synthetic;
  ObjectID nextSynthetic = ObjectID(1) << 63;
  for (const auto &function : module) for (const auto &block : function)
    for (const auto &instruction : block) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      const auto *callee = call ? call->getCalledFunction() : nullptr;
      if (callee && !callee->isDeclaration()) called.insert(callee);
      auto event = resourceEvent(instruction, plan.property);
      if (mode != NativeHistoryMode::Full && plan.property.exits &&
          hasEvent(event, plan.property.sources))
        synthetic.emplace(&instruction, nextSynthetic++);
    }
  auto directAcquisition = [](const llvm::Value *pointer, llvm::StringRef name) {
    const auto *call = llvm::dyn_cast<llvm::CallBase>(pointer->stripPointerCasts());
    const auto *callee = call ? call->getCalledFunction() : nullptr;
    return callee && callee->getName() == name;
  };
  auto effectFor = [&](const llvm::Instruction &instruction, Event event) {
    const llvm::Value *pointer = nullptr;
    Certainty certainty = Certainty::May;
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
      pointer = load->getPointerOperand();
    else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
      pointer = store->getPointerOperand();
    else if (const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
      pointer = ret->getReturnValue(); certainty = Certainty::Must;
    } else {
      const auto &call = llvm::cast<llvm::CallBase>(instruction);
      pointer = hasEvent(event, Event::Allocate | Event::Open) ? &instruction :
                                                               call.getArgOperand(0);
      if ((mode == NativeHistoryMode::MemoryLeak && event == Event::Release &&
           (directAcquisition(pointer, "malloc") || directAcquisition(pointer, "calloc"))) ||
          (mode == NativeHistoryMode::FileLeak && event == Event::Close &&
           directAcquisition(pointer, "fopen")))
        certainty = Certainty::Must;
    }
    auto objects = resourceObjects(svfg, pointer);
    if (objects.isUnknown()) {
      auto site = llvm::dyn_cast<llvm::Instruction>(pointer->stripPointerCasts());
      auto fallback = synthetic.find(site);
      if (fallback != synthetic.end()) objects = ObjectSet::known({fallback->second});
    }
    return GuardedEventEffect{event, std::move(objects), certainty};
  };

  std::vector<ObjectID> candidates;
  bool unknownSources = mode == NativeHistoryMode::Full;
  for (const auto &function : module) for (const auto &block : function)
    for (const auto &instruction : block) {
      auto event = resourceEvent(instruction, plan.property);
      if (!hasEvent(event, plan.property.sources)) continue;
      auto effect = effectFor(instruction, event);
      plan.hasRequired |= hasEvent(event, plan.property.required) && !effect.objects.empty();
      unknownSources |= effect.objects.isUnknown();
      candidates.insert(candidates.end(), effect.objects.objects().begin(),
                         effect.objects.objects().end());
      plan.effects.emplace(&instruction, std::move(effect));
    }
  if (!plan.hasRequired) return plan;
  if (!unknownSources) plan.universe = ObjectSet::known(std::move(candidates));

  for (const auto &function : module) for (const auto &block : function)
    for (const auto &instruction : block) {
      auto event = resourceEvent(instruction, plan.property);
      if (event == Event::None || hasEvent(event, plan.property.sources)) continue;
      if (event == Event::Escape && (called.count(&function) || function.hasAddressTaken()))
        continue;
      auto effect = effectFor(instruction, event);
      if (!plan.universe.isUnknown() && !effect.objects.isUnknown()) {
        std::vector<ObjectID> relevant;
        const auto &objects = effect.objects.objects();
        const auto &universe = plan.universe.objects();
        std::set_intersection(objects.begin(), objects.end(), universe.begin(), universe.end(),
                              std::back_inserter(relevant));
        effect.objects = ObjectSet::known(std::move(relevant));
      }
      if (!effect.objects.empty()) plan.effects.emplace(&instruction, std::move(effect));
    }
  return plan;
}

void appendTemporalFacts(SVFGHistoryResult &result, const analysis::SVFG &svfg,
                         const std::map<const llvm::Function *, Layout> &layouts,
                         FunctionID firstLayer, const ResourcePlan &plan) {
  std::map<const llvm::Function *, std::vector<TemporalEffect>> effects;
  std::map<const llvm::Instruction *, NativeID> provenance;
  for (const auto &item : svfg) if (const auto *instruction = anchor(*item.second)) {
    auto it = provenance.emplace(instruction, item.second->getId());
    if (!it.second) it.first->second = std::min<NativeID>(it.first->second, item.second->getId());
  }
  for (const auto &entry : plan.effects) {
    const auto *instruction = entry.first;
    const auto &effect = entry.second;
    const auto &layout = layouts.at(instruction->getFunction());
    SiteID site = effect.events == Event::Dereference ? layout.before.at(instruction) :
                                                       layout.after.at(instruction);
    auto native = provenance.find(instruction);
    effects[instruction->getFunction()].push_back({site, effect.objects, effect.events,
        effect.certainty, native == provenance.end() ? NoNativeID : native->second});
  }
  std::map<const llvm::Function *, TemporalHistory> histories;
  for (const auto &item : layouts) {
    auto history = TemporalHistory::append(result.graph, firstLayer++,
        item.second.function.name + ".temporal", item.second.function.control, effects[item.first],
        plan.property.exits);
    // A CFG sink such as unreachable/resume is not a normal return port.
    // Full layouts also have one explicit synthetic join of normal returns.
    for (auto exit = history.exits.begin(); exit != history.exits.end();) {
      bool normal = item.second.function.control.blocks()[exit->first].name == "usetracessa.exit";
      for (const auto &block : item.second.blocks)
        if (block.second == exit->first)
          normal = llvm::isa<llvm::ReturnInst>(block.first->getTerminator());
      if (normal) ++exit;
      else exit = history.exits.erase(exit);
    }
    histories.emplace(item.first, std::move(history));
  }
  for (const auto &item : layouts) {
    const auto &caller = histories.at(item.first);
    for (const auto &block : *item.first) for (const auto &instruction : block) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      const auto *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || !histories.count(callee)) continue;
      SiteID site = item.second.returned.at(call);
      if (!result.graph.layer(caller.id).history.use(caller.sites.at(site), caller.execution))
        continue; // Unreachable CFG site.
      if (llvm::isa<llvm::InvokeInst>(call)) {
        result.graph.addIssue("exceptional temporal call exits require edge-specific modeling");
        continue;
      }
      TemporalHistory::connectCall(result.graph, caller, site,
          reinterpret_cast<std::uintptr_t>(call), {histories.at(callee)});
    }
  }
}

std::vector<std::string> callIssues(const llvm::Module &module) {
  std::vector<std::string> issues;
  for (const auto &function : module) for (const auto &block : function)
    for (const auto &instruction : block) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (!call) continue;
      const auto *callee = call->getCalledFunction();
      if (!callee || (callee->isDeclaration() && !callee->isIntrinsic()))
        issues.push_back("call in " + function.getName().str() +
                         " has no complete native effect model");
    }
  return issues;
}

} // namespace

SVFGHistoryResult buildUseTraceSSAFromLotusSVFG(const analysis::SVFG &svfg,
                                               const llvm::Module &module,
                                               NativeHistoryMode mode) {
  SVFGConstructionInput input;
  std::map<const llvm::Function *, Layout> layouts;
  const auto plan = planResources(svfg, module, mode);
  if (!plan.hasRequired) {
    SVFGHistoryResult result;
    result.graph.addIssue("required native source events are absent for the selected property");
    return result;
  }
  FunctionID nextFunction = 1;
  {
    detail::InstructionLabels labels(module, [&](const llvm::Instruction &instruction) {
      return mode == NativeHistoryMode::Full ? !llvm::isa<llvm::PHINode>(instruction) :
                                             plan.keeps(instruction);
    });
    for (const auto &function : module) {
      if (function.isDeclaration()) continue;
      if (mode == NativeHistoryMode::Full)
        layouts.emplace(&function, makeLayout(function, nextFunction++, labels));
      else
        layouts.emplace(&function, makeResourceLayout(function, nextFunction++, plan, labels));
    }
  }
  std::map<const llvm::Function *, llvm::DominatorTree> dominators;
  if (mode == NativeHistoryMode::Full)
    for (const auto &entry : layouts)
      dominators[entry.first].recalculate(*const_cast<llvm::Function *>(entry.first));
  if (mode != NativeHistoryMode::Full) {
    SVFGHistoryResult result;
    appendTemporalFacts(result, svfg, layouts, 1, plan);
    for (const auto &issue : callIssues(module)) result.graph.addIssue(issue);
    if (!plan.universe.isUnknown())
      result.graph.setResourceUniverse(plan.universe.objects());
    return result;
  }
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

  for (auto issue : callIssues(module)) input.issues.push_back(std::move(issue));

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
  auto result = SVFGImporter::build(input);
  appendTemporalFacts(result, svfg, layouts, nextFunction, plan);
  return result;
}

} // namespace usetracessa
} // namespace lotus
