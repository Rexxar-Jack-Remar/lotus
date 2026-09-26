#pragma once

#include "llvm/Pass.h"

#include "Dataflow/APA/Analyses/Intra/AvailableExpressions.h"
#include "Dataflow/APA/Analyses/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Intra/Lockset.h"
#include "Dataflow/APA/Analyses/Intra/NonNull.h"
#include "Dataflow/APA/Analyses/Intra/Reachability.h"
#include "Dataflow/APA/Analyses/Intra/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Intra/Sign.h"
#include "Dataflow/APA/Analyses/Intra/UninitializedVariables.h"
#include "Dataflow/APA/Core/Options.h"

namespace elimination {

class ElimReachabilityPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimReachabilityPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Reachability";
  }

  const ReachabilityResult &getResult() const { return Result; }

private:
  ReachabilityResult Result;
};

class ElimConstantPropagationPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimConstantPropagationPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Constant Propagation";
  }

  const ConstantPropagationResult &getResult() const { return Result; }

private:
  ConstantPropagationResult Result;
};

class ElimReachingDefinitionsPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimReachingDefinitionsPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Reaching Definitions";
  }

  const ReachingDefinitionsResult &getResult() const { return Result; }

private:
  ReachingDefinitionsResult Result;
};

class ElimAvailableExpressionsPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimAvailableExpressionsPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Available Expressions";
  }

  const AvailableExpressionsResult &getResult() const { return Result; }

private:
  AvailableExpressionsResult Result;
};

class ElimUninitializedVariablesPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimUninitializedVariablesPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Uninitialized Variables";
  }

  const UninitializedVariablesResult &getResult() const { return Result; }

private:
  UninitializedVariablesResult Result;
};

class ElimLocksetPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimLocksetPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override { return "Elimination Lockset"; }

  const LocksetResult &getResult() const { return Result; }

private:
  LocksetResult Result;
};

class ElimNonNullPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimNonNullPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override { return "Elimination NonNull"; }

  const NonNullResult &getResult() const { return Result; }

private:
  NonNullResult Result;
};

class ElimSignPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimSignPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Sign Analysis";
  }

  const SignResult &getResult() const { return Result; }

private:
  SignResult Result;
};

} // namespace elimination

