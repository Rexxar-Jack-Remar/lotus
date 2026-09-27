#include "IR/UseHistory/LLVMHistory.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <stdexcept>
#include <utility>

namespace lotus {
namespace usehistory {
namespace {

std::string valueName(const llvm::Value &value) {
  std::string result;
  llvm::raw_string_ostream out(result);
  value.printAsOperand(out, false);
  out.flush();
  return result;
}

std::string instructionText(const llvm::Instruction &instruction) {
  std::string result;
  llvm::raw_string_ostream out(result);
  instruction.print(out);
  out.flush();
  return result;
}

bool track(const llvm::Value &value, const LLVMHistoryOptions &options) {
  const llvm::Type *type = value.getType();
  if (type->isVoidTy() || type->isLabelTy() || type->isMetadataTy() ||
      type->isTokenTy())
    return false;
  if (options.pointersOnly && !type->isPointerTy()) return false;
  return llvm::isa<llvm::Argument>(value) || llvm::isa<llvm::Instruction>(value) ||
         (options.trackGlobalAddresses && llvm::isa<llvm::GlobalVariable>(value));
}

std::string edgeLabel(const llvm::Instruction &terminator, unsigned index) {
  if (const auto *branch = llvm::dyn_cast<llvm::BranchInst>(&terminator)) {
    if (branch->isConditional())
      return std::string(index == 0 ? "true: " : "false: ") +
             valueName(*branch->getCondition());
    return "unconditional";
  }
  if (llvm::isa<llvm::InvokeInst>(terminator))
    return index == 0 ? "normal" : "unwind";
  if (const auto *sw = llvm::dyn_cast<llvm::SwitchInst>(&terminator)) {
    if (index == 0) return "default: " + valueName(*sw->getCondition());
    auto it = sw->case_begin();
    for (unsigned i = 1; i < index; ++i) ++it;
    return "case " + valueName(*it->getCaseValue()) + ": " +
           valueName(*sw->getCondition());
  }
  return std::string(terminator.getOpcodeName()) + " successor " +
         std::to_string(index);
}

} // namespace

ValueID LLVMHistoryResult::valueID(const llvm::Value &value) const {
  auto found = ValueIDs.find(&value);
  return found == ValueIDs.end() ? InvalidID : found->second;
}

VersionID LLVMHistoryResult::definition(const llvm::Value &value) const {
  ValueID id = valueID(value);
  return id == InvalidID ? InvalidID : History.definition(id);
}

BlockID LLVMHistoryResult::blockID(const llvm::BasicBlock &block) const {
  auto found = BlockIDs.find(&block);
  return found == BlockIDs.end() ? InvalidID : found->second;
}

SiteID LLVMHistoryResult::siteID(const llvm::Instruction &instruction) const {
  auto found = InstructionSites.find(&instruction);
  return found == InstructionSites.end() ? InvalidID : found->second;
}

std::vector<LLVMOperandVersion>
LLVMHistoryResult::uses(const llvm::Use &operand) const {
  std::vector<LLVMOperandVersion> result;
  auto found = Bindings.find(&operand);
  if (found == Bindings.end()) return result;
  for (const Binding &binding : found->second)
    if (const UseVersion *use = History.use(binding.site, binding.value))
      result.push_back({binding.edge, *use});
  return result;
}

LLVMHistoryResult LLVMHistoryBuilder::build(const llvm::Function &function,
                                   LLVMHistoryOptions options) {
  LLVMHistoryResult result;
  result.Function = &function;
  Program program;
  if (function.isDeclaration()) {
    result.History = Graph::build(std::move(program));
    return result;
  }
  std::string diagnostic;
  llvm::raw_string_ostream diagnostics(diagnostic);
  if (llvm::verifyFunction(function, &diagnostics)) {
    diagnostics.flush();
    throw std::invalid_argument("UseHistory: invalid LLVM function: " + diagnostic);
  }

  for (const llvm::BasicBlock &block : function)
    result.BlockIDs[&block] = program.addBlock(valueName(block));
  auto addValue = [&](const llvm::Value &value) {
    if (!track(value, options) || result.ValueIDs.count(&value)) return;
    ValueID id = program.addValue(valueName(value));
    result.ValueIDs[&value] = id;
    result.Values.push_back(&value);
  };
  for (const llvm::Argument &arg : function.args()) addValue(arg);
  for (const llvm::BasicBlock &block : function)
    for (const llvm::Instruction &instruction : block) {
      addValue(instruction);
      if (options.trackGlobalAddresses)
        for (const llvm::Use &operand : instruction.operands())
          if (llvm::isa<llvm::GlobalVariable>(operand.get())) addValue(*operand.get());
    }

  // Preserve LLVM successor indices, including parallel switch edges.
  std::unordered_map<const llvm::BasicBlock *, std::vector<EdgeID>> edgeIDs;
  for (const llvm::BasicBlock &block : function) {
    const llvm::Instruction &term = *block.getTerminator();
    for (unsigned i = 0; i < term.getNumSuccessors(); ++i) {
      const llvm::BasicBlock *dest = term.getSuccessor(i);
      EdgeID id = program.addEdge(result.BlockIDs.at(&block),
                                  result.BlockIDs.at(dest), edgeLabel(term, i));
      edgeIDs[&block].push_back(id);
      result.Edges.push_back({&block, dest, i});
    }
  }

  std::vector<ValueID> entryDefinitions;
  for (ValueID v = 0; v < result.Values.size(); ++v)
    if (!llvm::isa<llvm::Instruction>(result.Values[v]))
      entryDefinitions.push_back(v);
  program.addOperation(result.BlockIDs.at(&function.getEntryBlock()),
                       "live on entry", {}, std::move(entryDefinitions));

  // Register exact operand identities while grouping repeated values at one
  // site. The core creates one psi for each distinct value at that site.
  auto addSite = [&](BlockID block, EdgeID edge, const std::string &label,
                     const std::vector<const llvm::Use *> &operands,
                     std::vector<ValueID> definitions) {
    std::vector<ValueID> uses;
    std::vector<const llvm::Use *> tracked;
    for (const llvm::Use *operand : operands) {
      auto found = result.ValueIDs.find(operand->get());
      if (found == result.ValueIDs.end()) continue;
      uses.push_back(found->second);
      tracked.push_back(operand);
    }
    SiteID site = edge == InvalidID
                      ? program.addOperation(block, label, std::move(uses),
                                              std::move(definitions))
                      : program.addEdgeOperation(edge, label, std::move(uses),
                                                  std::move(definitions));
    for (const llvm::Use *operand : tracked)
      result.Bindings[operand].push_back(
          {site, result.ValueIDs.at(operand->get()), edge});
    return site;
  };

  for (const llvm::BasicBlock &block : function) {
    BlockID blockID = result.BlockIDs.at(&block);
    std::vector<ValueID> phiDefinitions;
    for (const llvm::Instruction &instruction : block) {
      if (!llvm::isa<llvm::PHINode>(instruction)) break;
      auto found = result.ValueIDs.find(&instruction);
      if (found != result.ValueIDs.end()) phiDefinitions.push_back(found->second);
    }
    if (!phiDefinitions.empty())
      program.addOperation(blockID, "original phi results", {},
                           std::move(phiDefinitions));
    for (const llvm::Instruction &instruction : block) {
      if (llvm::isa<llvm::PHINode>(instruction) ||
          llvm::isa<llvm::DbgInfoIntrinsic>(instruction))
        continue;
      std::vector<const llvm::Use *> operands;
      for (const llvm::Use &operand : instruction.operands())
        operands.push_back(&operand);
      std::vector<ValueID> definitions;
      auto found = result.ValueIDs.find(&instruction);
      if (found != result.ValueIDs.end()) {
        if (llvm::isa<llvm::CallBrInst>(instruction))
          throw std::invalid_argument(
              "UseHistory: tracked non-void callbr results are unsupported; "
              "their edge availability differs across LLVM versions");
        if (!llvm::isa<llvm::InvokeInst>(instruction))
          definitions.push_back(found->second);
      }
      result.InstructionSites[&instruction] =
          addSite(blockID, InvalidID, instructionText(instruction), operands,
                  std::move(definitions));
      if (found != result.ValueIDs.end() &&
          llvm::isa<llvm::InvokeInst>(instruction)) {
        // LLVM defines the result on the normal edge, before phi operand uses.
        program.addEdgeOperation(edgeIDs.at(&block).at(0),
                                  "invoke result " + valueName(instruction), {},
                                  {found->second});
      }
    }
  }

  // A comparison is not a sanitizer. Record facts only on the branch edge
  // that establishes them, before the incoming PHI operand batch.
  if (options.recordNullGuards) {
    for (EdgeID edge = 0; edge < result.Edges.size(); ++edge) {
      const auto &desc = result.Edges[edge];
      const auto *branch = llvm::dyn_cast<llvm::BranchInst>(desc.from->getTerminator());
      if (!branch || !branch->isConditional()) continue;
      const auto *compare = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
      if (!compare || (compare->getPredicate() != llvm::CmpInst::ICMP_EQ &&
                       compare->getPredicate() != llvm::CmpInst::ICMP_NE)) continue;
      const llvm::Value *pointer = nullptr;
      if (llvm::isa<llvm::ConstantPointerNull>(compare->getOperand(0)))
        pointer = compare->getOperand(1);
      else if (llvm::isa<llvm::ConstantPointerNull>(compare->getOperand(1)))
        pointer = compare->getOperand(0);
      if (!pointer) continue;
      auto found = result.ValueIDs.find(pointer);
      if (found == result.ValueIDs.end()) continue;
      bool truth = desc.successorIndex == 0;
      bool nonnull = (compare->getPredicate() == llvm::CmpInst::ICMP_NE) == truth;
      SiteID site = program.addEdgeOperation(edge,
          std::string("assume ") + valueName(*pointer) + (nonnull ? " != null" : " == null"),
          {found->second});
      result.NullGuards.push_back({edge, site, found->second, nonnull});
    }
  }

  // Existing LLVM phi operands execute on their incoming edges. All phi reads
  // on one edge form a parallel batch, avoiding a fabricated ordering between
  // parallel phi assignments. Their output SSA values remain distinct roots.
  for (EdgeID edge = 0; edge < result.Edges.size(); ++edge) {
    const LLVMEdge &llvmEdge = result.Edges[edge];
    unsigned occurrence = 0;
    const llvm::Instruction *term = llvmEdge.from->getTerminator();
    for (unsigned i = 0; i < llvmEdge.successorIndex; ++i)
      if (term->getSuccessor(i) == llvmEdge.to) ++occurrence;
    std::vector<const llvm::Use *> operands;
    for (const llvm::Instruction &instruction : *llvmEdge.to) {
      const auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
      if (!phi) break;
      unsigned seen = 0;
      bool matched = false;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
        if (phi->getIncomingBlock(i) == llvmEdge.from && seen++ == occurrence) {
          operands.push_back(&phi->getOperandUse(i));
          matched = true;
          break;
        }
      if (!matched)
        throw std::invalid_argument("UseHistory: phi/CFG edge multiplicity mismatch");
    }
    if (!operands.empty())
      addSite(InvalidID, edge, "parallel incoming phi operands", operands, {});
  }
  result.History = Graph::build(std::move(program));
  return result;
}

} // namespace usehistory
} // namespace lotus
