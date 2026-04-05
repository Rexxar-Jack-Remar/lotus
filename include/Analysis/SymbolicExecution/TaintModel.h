#ifndef ANALYSIS_SYMBOLICEXECUTION_TAINTMODEL_H
#define ANALYSIS_SYMBOLICEXECUTION_TAINTMODEL_H

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;

class TaintModel {
public:
  TaintModel();
  void getTransferDstVect(const CallBase *CS, Value *Arg,
                          std::vector<Value *> &DstVect) const;
  bool isFunctionRetAsSource(const Function *func) const;
  bool isFunctionArgAsSource(const Function *func) const;
  const std::vector<int> *getTaintSourceArguments(Function *func) const;

private:
  std::unordered_map<std::string, std::vector<int>> ArgAsSourceFunctions;
  std::set<std::string> RetAsSourceFunctions;
  std::multimap<std::string, std::vector<int>> DataTransferFunctions;
  std::unordered_map<std::string, std::vector<int>> SinkFunctions;
};

#endif
