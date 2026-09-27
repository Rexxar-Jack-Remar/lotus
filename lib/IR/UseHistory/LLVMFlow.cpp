#include "IR/UseHistory/LLVMFlow.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Type.h>

namespace lotus {
namespace usehistory {
LLVMHistoryResult appendLLVMHistory(FlowGraph &graph, FunctionID id,
                                    const llvm::Function &function,
                                    LLVMHistoryOptions options) {
  auto history = LLVMHistoryBuilder::build(function, options);
  graph.addHistory(id, function.getName().str(), history.graph());
  for (const auto &guard : history.nullGuards())
    if (history.graph().use(guard.site, guard.value))
      graph.annotate(graph.after(id, guard.site, guard.value),
                     guard.nonnull ? Event::NonNull : Event::IsNull);
  return history;
}
void appendLLVMScalarTransfers(FlowGraph &graph, FunctionID id,
                                const LLVMHistoryResult &history,
                                bool includePointerResults) {
  for (const auto &block : history.function()) for (const auto &inst : block) {
    if (inst.getType()->isVoidTy() ||
        (!includePointerResults && inst.getType()->isPointerTy()) ||
        llvm::isa<llvm::LoadInst>(inst) || llvm::isa<llvm::StoreInst>(inst) ||
        llvm::isa<llvm::CallBase>(inst) || llvm::isa<llvm::AtomicRMWInst>(inst) ||
        llvm::isa<llvm::AtomicCmpXchgInst>(inst) || llvm::isa<llvm::VAArgInst>(inst)) continue;
    auto definition = history.definition(inst);
    if (definition == InvalidID) continue;
    for (const auto &operand : inst.operands()) for (const auto &use : history.uses(operand)) {
      FlowEdge edge;
      edge.from = graph.version(id, use.history.after);
      edge.to = graph.version(id, definition);
      bool dependence = llvm::isa<llvm::AllocaInst>(inst) ||
                        (llvm::isa<llvm::SelectInst>(inst) && operand.getOperandNo() == 0);
      edge.kind = dependence ? FlowKind::Dependence : FlowKind::Direct;
      graph.addEdge(edge);
    }
  }
}
} // namespace usehistory
} // namespace lotus
