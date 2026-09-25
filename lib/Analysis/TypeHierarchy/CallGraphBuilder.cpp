#include "Analysis/TypeHierarchy/CallGraphBuilder.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"
#include "Analysis/TypeHierarchy/Resolver.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace lotus {

std::vector<const llvm::Function *>
getEntryPoints(const llvm::Module &M,
               llvm::ArrayRef<std::string> EntryPointNames) {
  std::vector<const llvm::Function *> EntryPoints;

  for (const auto &Name : EntryPointNames) {
    if (const auto *F = M.getFunction(Name)) {
      if (!F->isDeclaration()) {
        EntryPoints.push_back(F);
      }
    }
  }

  if (EntryPoints.empty()) {
    for (const auto &F : M) {
      if (!F.isDeclaration() && F.hasExternalLinkage()) {
        EntryPoints.push_back(&F);
      }
    }
  }

  return EntryPoints;
}

CallGraph buildCallGraph(const llvm::Module &M, Resolver &Res,
                         llvm::ArrayRef<const llvm::Function *> EntryPoints) {
  CallGraph CG;
  llvm::DenseSet<const llvm::Function *> VisitedFunctions;
  llvm::SmallVector<const llvm::Function *, 16> Worklist;

  for (const auto *F : EntryPoints) {
    if (F && !F->isDeclaration()) {
      Worklist.push_back(F);
      VisitedFunctions.insert(F);
    }
  }

  while (!Worklist.empty()) {
    const auto *F = Worklist.pop_back_val();

    for (const auto &I : llvm::instructions(F)) {
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!Call) {
        continue;
      }

      const auto *DirectCallee = llvm::dyn_cast_or_null<llvm::Function>(
          Call->getCalledOperand()->stripPointerCastsAndAliases());

      if (DirectCallee) {
        CG.addCallEdge(Call, DirectCallee);
        if (!DirectCallee->isDeclaration() &&
            VisitedFunctions.insert(DirectCallee).second) {
          Worklist.push_back(DirectCallee);
        }
      } else {
        auto Targets = Res.resolveIndirectCall(Call);
        for (const auto *Target : Targets) {
          CG.addCallEdge(Call, Target);
          if (!Target->isDeclaration() &&
              VisitedFunctions.insert(Target).second) {
            Worklist.push_back(Target);
          }
        }
      }
    }
  }

  return CG;
}

CallGraph buildCallGraph(const llvm::Module &M, Resolver &Res,
                         llvm::ArrayRef<std::string> EntryPointNames) {
  auto EntryPoints = getEntryPoints(M, EntryPointNames);
  return buildCallGraph(M, Res, EntryPoints);
}

CallGraph buildCallGraph(const llvm::Module &M, CallGraphAnalysisType CGType,
                         llvm::ArrayRef<std::string> EntryPointNames,
                         const DIBasedTypeHierarchy *TH,
                         const LLVMVFTableProvider *VTP) {
  std::unique_ptr<LLVMVFTableProvider> OwnedVTP;
  if (!VTP) {
    OwnedVTP = std::make_unique<LLVMVFTableProvider>(M);
    VTP = OwnedVTP.get();
  }

  std::unique_ptr<DIBasedTypeHierarchy> OwnedTH;
  if (!TH) {
    OwnedTH = std::make_unique<DIBasedTypeHierarchy>(M);
    TH = OwnedTH.get();
  }

  auto Res = Resolver::create(CGType, &M, VTP, TH);
  return buildCallGraph(M, *Res, EntryPointNames);
}

} // namespace lotus
