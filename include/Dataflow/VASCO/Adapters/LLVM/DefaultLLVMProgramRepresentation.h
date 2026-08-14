#pragma once

#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/VASCO/Core/DirectedGraph.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"
#include "Utils/LLVM/CallUtils.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace vasco {
namespace llvmir {

class LLVMInstructionGraph final : public DirectedGraph<llvm::Instruction *> {
public:
  explicit LLVMInstructionGraph(llvm::Function *Function)
      : Function(Function) {}

  std::vector<llvm::Instruction *> nodes() const override {
    return CFG.getAllInstructionsOf(Function);
  }

  std::vector<llvm::Instruction *> heads() const override {
    return CFG.getStartPointsOf(Function,
                                dataflow::controlflow::FlowDirection::Forward);
  }

  std::vector<llvm::Instruction *> tails() const override {
    return CFG.getExitPointsOf(Function,
                               dataflow::controlflow::FlowDirection::Forward);
  }

  std::vector<llvm::Instruction *>
  predsOf(llvm::Instruction *const &Node) const override {
    return CFG.getPredsOf(Node, dataflow::controlflow::FlowDirection::Forward);
  }

  std::vector<llvm::Instruction *>
  succsOf(llvm::Instruction *const &Node) const override {
    return CFG.getSuccsOf(Node, dataflow::controlflow::FlowDirection::Forward);
  }

  std::size_t size() const override { return nodes().size(); }

private:
  llvm::Function *Function = nullptr;
  dataflow::controlflow::LLVMIntraCFG CFG;
};

class DefaultLLVMProgramRepresentation final
    : public ProgramRepresentation<llvm::Function *, llvm::Instruction *> {
public:
  using MethodType = llvm::Function *;
  using NodeType = llvm::Instruction *;
  using GraphPtr = std::shared_ptr<const DirectedGraph<NodeType>>;
  using ResolveTargetsFn = std::function<std::optional<std::vector<MethodType>>(
      MethodType, NodeType)>;

  explicit DefaultLLVMProgramRepresentation(
      llvm::Module *Module, std::vector<MethodType> EntryPoints = {},
      ResolveTargetsFn Resolver = {})
      : Module(Module), EntryPoints(std::move(EntryPoints)),
        Resolver(std::move(Resolver)) {
    if (this->EntryPoints.empty() && Module != nullptr) {
      if (auto *Main = Module->getFunction("main")) {
        this->EntryPoints.push_back(Main);
      }
    }
  }

  std::vector<MethodType> getEntryPoints() const override {
    return EntryPoints;
  }

  llvm::Module *getModule() const { return Module; }

  GraphPtr getControlFlowGraph(const MethodType &Method) const override {
    auto It = GraphCache.find(Method);
    if (It != GraphCache.end()) {
      return It->second;
    }

    auto Graph = std::make_shared<LLVMInstructionGraph>(Method);
    GraphCache[Method] = Graph;
    return Graph;
  }

  bool isCall(const NodeType &Node) const override {
    return llvm::isa<llvm::CallBase>(Node);
  }

  std::optional<std::vector<MethodType>>
  resolveTargets(const MethodType &CallerMethod,
                 const NodeType &CallNode) const override {
    if (Resolver) {
      return Resolver(CallerMethod, CallNode);
    }

    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallNode);
    if (Call == nullptr) {
      return std::vector<MethodType>{};
    }

    if (auto *Callee = lotus::llvm_utils::getDirectCallee(Call)) {
      if (isPhantomMethod(Callee)) {
        return std::nullopt;
      }
      return std::vector<MethodType>{Callee};
    }

    return std::nullopt;
  }

  bool isPhantomMethod(const MethodType &Method) const override {
    return Method == nullptr || Method->isDeclaration() || Method->empty();
  }

private:
  llvm::Module *Module = nullptr;
  std::vector<MethodType> EntryPoints;
  ResolveTargetsFn Resolver;
  mutable std::map<MethodType, GraphPtr> GraphCache;
};

} // namespace llvmir
} // namespace vasco
