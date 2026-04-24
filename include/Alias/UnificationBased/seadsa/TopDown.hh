#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "Alias/UnificationBased/seadsa/CallSite.hh"
#include "Alias/UnificationBased/seadsa/Graph.hh"

namespace llvm {
class CallGraph;
} // namespace llvm

namespace seadsa {

class TopDownAnalysis {

public:
  using GraphRef = std::shared_ptr<Graph>;
  using GraphMap = llvm::DenseMap<const llvm::Function *, GraphRef>;

private:
  llvm::CallGraph &m_cg;
  bool m_flowSensitiveOpt;
  bool m_noescape;

public:
  static void cloneAndResolveArguments(const DsaCallSite &CS, Graph &callerG,
                                       Graph &calleeG, bool flowSensitiveOpt = true, 
				       bool noescape = true);

  TopDownAnalysis(llvm::CallGraph &cg,
		  bool flowSensitiveOpt = true,
                  bool noescape = true /* TODO: CLI*/)
      : m_cg(cg),
        m_flowSensitiveOpt(flowSensitiveOpt), m_noescape(noescape) {}

  bool runOnModule(llvm::Module &M, GraphMap &graphs);
};

} // namespace seadsa
