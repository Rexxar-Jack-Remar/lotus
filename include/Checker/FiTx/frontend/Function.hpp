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
#include "BasicBlock.hpp"
#include "Checker/FiTx/core/BasicBlock.hpp"
#include "Checker/FiTx/core/Function.hpp"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <vector>

#define NO_ERROR 0

namespace framework {

/// Per-function analysis summary (paper §4.3).
/// Holds: (1) basic_block_info_ = per-block value states and arg value states,
/// (2) return_info_ = map return_code -> set of blocks that return that code
/// (for return-code aware propagation at call sites), (3) value_collection_ for
/// related-value / parent-value lookup, (4) success/error blocks for summary application.
class FunctionInformation {
public:
  using WeakBasicBlockSet =
      std::set<std::weak_ptr<framework::BasicBlock>,
               std::owner_less<std::weak_ptr<framework::BasicBlock>>>;

  enum AnalysisStat { UNANALYZED, IN_PROGRESS, DIRTY, ANALYZED };
  /// Linux convention: negative = error, 0 = success (paper §4.3).
  static constexpr int kErrorCode = -1;
  static constexpr int kSuccessCode = 0;

  static constexpr int kMaxPredCheckDepth = 4;

  FunctionInformation(std::shared_ptr<framework::Function> function,
                      AnalysisStat stat = AnalysisStat::UNANALYZED);
  void setAnalysisStat(AnalysisStat stat);

  void
  setAnayzingBasicBlock(std::shared_ptr<framework::BasicBlock> basic_block);

  std::shared_ptr<framework::BasicBlock> currentBasicBlock() {
    return current_basicblock_;
  }

  std::shared_ptr<BasicBlockInformation> getCurrentBasicBlockInformation();

  std::shared_ptr<BasicBlockInformation>
  getBasicBlockInformation(std::shared_ptr<framework::BasicBlock> basic_block);

  std::shared_ptr<BasicBlockInformation> getBasicBlockPrevInformation(
      std::shared_ptr<framework::BasicBlock> basic_block);

  std::shared_ptr<BasicBlockInformation>
  createBasicBlockInfo(std::shared_ptr<framework::BasicBlock> basic_block,
                       const std::set<State> &states);

  void addValue(std::shared_ptr<framework::Value> value);
  void addValues(const ValueCollection &value);
  const ValueCollection &GetValueCollection() { return value_collection_; }

  bool basicBlockInfoExists(std::shared_ptr<framework::BasicBlock> basic_block);
  bool
  basicBlockPrevInfoExists(std::shared_ptr<framework::BasicBlock> basic_block);

  AnalysisStat Stat() { return stat_; }

  bool basicBlockInfoChanged(std::shared_ptr<framework::BasicBlock> block);

  const WeakBasicBlockSet &getErrorBlocks(int64_t error_code);
  const WeakBasicBlockSet &getSuccessBlock();
  void addReturnValueInfo(int64_t value,
                          std::weak_ptr<framework::BasicBlock> block_info);
  void addReturnValueInfo(int64_t value, WeakBasicBlockSet block_info);

  const std::map<int64_t, WeakBasicBlockSet> &getReturnValueInfo() {
    return return_info_;
  };

  std::shared_ptr<framework::Function> Function() {
    return framework_function_;
  }

  AliasValues &getAliasValues() { return alias_info_; }

  bool existsInRefcountFunctions(std::shared_ptr<framework::Function> function);
  void addRefcountFunction(std::shared_ptr<framework::Function> function);

private:
  struct BasicBlockInformationSet {
    std::shared_ptr<BasicBlockInformation> prev_info;
    std::shared_ptr<BasicBlockInformation> current_info;
  };

  // Found call inst as return value
  std::shared_ptr<framework::Function> framework_function_;
  AnalysisStat stat_;

  ValueCollection value_collection_;
  AliasValues alias_info_;

  std::shared_ptr<framework::BasicBlock> current_basicblock_;
  std::shared_ptr<framework::BasicBlock> return_block_;

  std::vector<std::shared_ptr<framework::Function>> called_refcount_functions_;

  std::map<std::shared_ptr<framework::BasicBlock>,
           std::shared_ptr<BasicBlockInformation>>
      basic_block_info_;

  std::map<std::shared_ptr<framework::BasicBlock>,
           std::shared_ptr<BasicBlockInformation>>
      prev_basic_block_info_;

  std::map<int64_t, WeakBasicBlockSet> return_info_;
};

}; // namespace framework
