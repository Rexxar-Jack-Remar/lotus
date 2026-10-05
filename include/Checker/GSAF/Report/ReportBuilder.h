#pragma once

#include "Analysis/DebugInfo/IRExpressionRenderer.h"
#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/GSAF/API/Trace.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "IR/GSA/GSA.h"

#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace lotus {
namespace gsaf {
using namespace llvm;
using namespace std;
using ir_expression::valueToString;

using namespace llvm;

class Vulnerability;

class VulnerabilityReportBuilder {
protected:
  // Cache all the phiNodes in the analysed module
  // TODO: This design does not work for multi-threading
  std::unordered_map<Function *, std::unordered_set<PHINode *>> PhiNodeCache;

  // This is used to help build bug reports for phi instruction
  // TODO: This design does not work for multi-threading
  std::unordered_map<Value *, Value *> PhiSelectRecord;

  // This is used to identify the callee for an indirect call.
  Instruction *OperationInCallee = nullptr;

  DebugInfoAnalysis *DIA = nullptr;
  ir_expression::IRExpressionRenderer *IResolver = nullptr;
  gsa::ControlDependenceAnalysisPass *CDGs = nullptr;

public:
  VulnerabilityReportBuilder();
  virtual ~VulnerabilityReportBuilder() {}

  // Build a report for a trace.
  virtual BugReport *buildReport(std::shared_ptr<VulnerabilityTrace> &Trace,
                                 DebugInfoAnalysis *DIA,
                                 ir_expression::IRExpressionRenderer *IResolver,
                                 gsa::ControlDependenceAnalysisPass *CDGs,
                                 std::shared_ptr<Vulnerability> Vuln = nullptr);

protected:
  // build bug report for the value flow from an operand node to a use site
  virtual std::string buildNodeSiteTip(const GuardedValueFlowNode *,
                                       const GuardedValueFlowSite *);

  // build bug report for the value flow from a use site to an operand node
  virtual std::string buildSiteNodeTip(const GuardedValueFlowSite *,
                                       const GuardedValueFlowNode *);

  // build bug report for a call site that returns something
  virtual std::string
  buildCallSiteOutputTip(const GuardedValueFlowCallSite *,
                         const GuardedValueFlowCallOutputNode *);

  // build bug report for a call site that uses an operand node as its input
  virtual std::string buildCallSiteInputTip(const GuardedValueFlowCallSite *,
                                            const GuardedValueFlowNode *);

  // build bug report for side-effect return
  virtual std::string buildPseudoReturnTip(const GuardedValueFlowReturnSite *,
                                           const GuardedValueFlowReturnNode *);

  // build bug report for the value flow from an operand node to the next
  // operand node
  virtual std::string buildNodeNodeTip(const GuardedValueFlowNode *,
                                       const GuardedValueFlowNode *);

  // build bug report for a global variable
  virtual std::string buildGlobalVariableTip(GlobalVariable *GV, Value *Val);

  // build bug report for an instruction
  virtual std::string buildInstructionTip(Instruction *Inst, Value *Val);

  virtual std::string buildReturnInstTip(ReturnInst *RI, Value *Val);
  virtual std::string buildLoadInstTip(LoadInst *LI, Value *Val);
  virtual std::string buildStoreInstTip(StoreInst *ST, Value *Val);
  virtual std::string buildPhiInstTip(PHINode *Phi, Value *Val);
  virtual std::string buildSelectInstTip(SelectInst *SL, Value *Val);
  virtual std::string buildCallInstTip(CallInst *CI, Value *Val);
  virtual std::string buildDivisionInstTip(Instruction *DI, Value *Val);

private:
  // Restore the expression in the src code for a given value,
  //         and update BaseVal as the source of the queried Value
  std::string buildExpression(Value *Val, Value *&BaseVal);

  // Demangle and translate back the LLVM intrinsic names for C stdlib functions
  std::string getReadableFunctionName(Function *F);

  // Based on current \p Tip and last diagnostic step \p LastDiagStep, modify
  // last step or generate a new step
  BugDiagStep *mergeWithLastBugStep(const GuardedValueFlowObject *NextObj,
                                    BugDiagStep *LastDiagStep,
                                    Instruction *Operation, std::string &Tip);
};

using namespace llvm;
using namespace std;
using ir_expression::valueToString;

class TaintReportBuilder : public VulnerabilityReportBuilder {
public:
  TaintReportBuilder() : VulnerabilityReportBuilder() {}
  virtual ~TaintReportBuilder() {}

  // Build a report for a trace.
  virtual BugReport *
  buildReport(std::shared_ptr<VulnerabilityTrace> &Trace,
              DebugInfoAnalysis *DIA,
              ir_expression::IRExpressionRenderer *IResolver,
              gsa::ControlDependenceAnalysisPass *CDGs,
              std::shared_ptr<Vulnerability> Vuln = nullptr) override;
};

} // namespace gsaf
} // namespace lotus
