/// \file Analyzer.hpp
/// FiTx CFG-based typestate analyzer: path-insensitive, inter-procedural,
/// return-code aware state propagation (paper Section 4.2, 4.3).
#pragma once
#include "llvm/ADT/APFloat.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

// include STL
#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Frontend/BasicBlock.h"
#include "Checker/FiTx/Frontend/Function.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/FiTx/Frontend/StateTransition.h"
#include "Checker/FiTx/Frontend/Utils.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <vector>

// Type Alias Analysis
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/ValueTypeAlias.h"

namespace framework {

/// CFG-based typestate analyzer: traverses basic blocks, merges states from
/// predecessors (paper §4.2), applies transitions, and uses return-code aware
/// propagation when copying callee summaries (paper §4.3).
class Analyzer {
public:
  Analyzer(llvm::Module &llvm_module, framework::StateManager &state_manager,
           framework::LoggingClient &client);

  void analyze();

  /// Analyze one function (bottom-up summary-based; paper §4.3).
  void analyzeFunction(std::shared_ptr<framework::Function> F);
  void analyzeCallInst(std::shared_ptr<framework::Instruction> I);
  void analyzeStoreInst(std::shared_ptr<framework::Instruction> I);
  void analyzeLoadInst(std::shared_ptr<framework::Instruction> I);
  bool analyzeFunctionCall(std::shared_ptr<framework::CallInst> call_inst);
  void analyzeReturnValue(std::shared_ptr<framework::Function> function);

  void analyzePrevBlockBranch(std::shared_ptr<framework::BasicBlock> B);

  void changeValueState(std::vector<Transition> transitions,
                        std::shared_ptr<framework::Value> value,
                        std::shared_ptr<framework::Instruction> inst);

  void generateError(BugNotificationTiming timing,
                     const std::set<std::shared_ptr<framework::Value>> values =
                         std::set<std::shared_ptr<framework::Value>>());

  bool functionInformationExists(std::shared_ptr<framework::Function> function);
  void copyFunctionValues(std::shared_ptr<framework::Function> called_func,
                          std::shared_ptr<framework::CallInst> call_inst);

  bool
  addPendingFunctionValues(std::shared_ptr<framework::Function> called_func,
                           std::shared_ptr<framework::CallInst> call_inst);

  std::shared_ptr<FunctionInformation> currentFunctionInformation() {
    return function_info_[analyzing_function_.top()];
  };

  std::shared_ptr<FunctionInformation>
  getFunctionInformation(std::shared_ptr<framework::Function> function);

  void checkAlias(std::shared_ptr<framework::StoreInst> store_inst);

private:
  llvm::Module &llvm_module_;
  framework::StateManager &state_manager_;
  framework::LoggingClient &log_;

  std::stack<std::shared_ptr<framework::Function>> analyzing_function_;

  std::map<std::shared_ptr<framework::Function>,
           std::shared_ptr<FunctionInformation>>
      function_info_;

  std::shared_ptr<framework::BasicBlockInformation> bb_info_;
};
} // namespace framework
