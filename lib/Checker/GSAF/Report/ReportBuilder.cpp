#include "Checker/GSAF/Report/ReportBuilder.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/Support/CheckerServices.h"
#include "IR/GSA/GSA.h"

#include <stack>

#include <llvm/Support/Debug.h>

namespace lotus {
namespace gsaf {
using namespace llvm;
using namespace std;
using namespace llvm_utils;
using ir_expression::valueToString;

#define DEBUG_TYPE "gsaf-report-builder"

/**
 * VulnerabilityReportBuilder
 */

// NOTE: We use some key words to merge bug report steps, please pay attention
// to it
/* Key Words:
 * " assign" / " assigned" / " is assigned" / "Assign" / "defined":
 *                The text directly before/after it shall be the linkage value
 * to the following step " to " / " reaches " : Texts after these texts shall be
 * discarded when merging bug-report-step with the following step "gets value
 * from" : Used to track parameter idx for function calls
 */

// Change the capital letter in the start of a sentence to lower-case
static void toLowerCase(std::string &Str) {
  if (Str[0] >= 'A' && Str[0] <= 'Z') {
    Str[0] = Str[0] + 0x20; // Upper to Lower case
  }
}

// Link to tips (of two steps)
static std::string simpleLinkTip(std::string Tip1, std::string Tip2) {
  if (Tip2.empty())
    return Tip1;

  if (Tip1.empty())
    return Tip2;

  toLowerCase(Tip2);

  return Tip1 + " and " + Tip2;
}

// Trunk a debug tip for merging
// Return true if the result is ready for merging, otherwise, return false
static bool truncTip(std::string &Tip) {
  // String identified as "to" in the tip, which shall be followed by the target
  // value in the tip string.
  static const std::string ToStr[] = {" to ", " reaches "};
  // AssignStr[][0] is the original string identified as assign, and
  // AssignStr[][1] is the transformed string when merging tips
  static const std::string AssignStr[][2] = {{" is assigned", ""},
                                             {" assigned", ""},
                                             {" assign", ""},
                                             {"Assign the", "The"},
                                             {"defined", "defined"}};

  const int ToStrSize = sizeof(ToStr) / sizeof(ToStr[0]);
  const int AssignStrSize = sizeof(AssignStr) / sizeof(AssignStr[0]);

  bool AssignFound = false, ToFound = false;

  std::string::size_type Pos;
  for (int i = 0; i < ToStrSize; i++) {
    Pos = Tip.rfind(ToStr[i]);
    if (Pos != std::string::npos) {
      Tip = Tip.substr(0, Pos);
      ToFound = true;
      break;
    }
  }

  for (int i = 0; i < AssignStrSize; i++) {
    Pos = Tip.find(AssignStr[i][0]);
    if (Pos != std::string::npos) {
      Tip.replace(Pos, AssignStr[i][0].length(), AssignStr[i][1]);
      AssignFound = true;
      break;
    }
  }

  return AssignFound && ToFound;
}

VulnerabilityReportBuilder::VulnerabilityReportBuilder() {
  //  auto *PassMgr = CBPassMgr::get_manager();
  //  DIA = PassMgr->get_analysis<DebugInfoAnalysis>();
  //  IResolver = PassMgr->get_analysis<ir_expression::IRExpressionRenderer>();
  //  CDGs = PassMgr->get_analysis<gsa::ControlDependenceAnalysisPass>();
}

BugReport *VulnerabilityReportBuilder::buildReport(
    std::shared_ptr<VulnerabilityTrace> &Trace, DebugInfoAnalysis *DIA,
    ir_expression::IRExpressionRenderer *IResolver,
    gsa::ControlDependenceAnalysisPass *CDGs,
    std::shared_ptr<Vulnerability> Vuln) {
  this->DIA = DIA;
  this->IResolver = IResolver;
  this->CDGs = CDGs;
  BugReport *Report =
      new BugReport(BugReportMgr::get_instance().register_bug_type(
          Vuln ? Vuln->getName() : "GSAF"));

  auto HasDebugInfo = [](const GuardedValueFlowObject *object) {
    if (!object)
      return false;
    if (auto *node = dyn_cast<GuardedValueFlowNode>(object)) {
      using K = GuardedValueFlowNode::Kind;
      switch (node->getKind()) {
      case K::CommonReturn:
      case K::LoadMemory:
      case K::Region:
      case K::SimpleOpcode:
      case K::CastOpcode:
        return false;
      case K::StoreMemory:
        return node->getDebugInstruction() != nullptr;
      case K::SimpleOperand:
        return node->getLLVMValue() != nullptr;
      default:
        return true;
      }
    }
    return cast<GuardedValueFlowSite>(object)->getKind() !=
           GuardedValueFlowSite::Kind::Alloc;
  };

#define OBJECT 0
#define INDEX 1

  auto SrcIndexPair = Trace->find(0, HasDebugInfo);
  // assert(std::get<INDEX>(SrcIndexPair) == 0 && "The first
  // GuardedValueFlowObject in the trace must contain debug information");

  LLVM_DEBUG(errs() << "\n\n");

  BugDiagStep *LastDiagStep = nullptr;
  for (size_t I = 1, E = Trace->get_length(); I < E; I++) {
    auto NodeIndexPair = Trace->find(I, HasDebugInfo);

    auto *CurrObj = std::get<OBJECT>(SrcIndexPair);
    auto *NextObj = std::get<OBJECT>(NodeIndexPair);

    if (NextObj == CurrObj) {
      I = std::get<INDEX>(NodeIndexPair);
      SrcIndexPair = std::move(NodeIndexPair);
      continue;
    }

    // cannot find the next operation
    if (!NextObj)
      break;

    bool CurrObjIsSite = isa<GuardedValueFlowSite>(CurrObj);
    bool NextObjIsSite = isa<GuardedValueFlowSite>(NextObj);

    LLVM_DEBUG(errs() << "[" << std::get<INDEX>(SrcIndexPair) << "] "
                      << *CurrObj << "\n";
               errs() << "[" << std::get<INDEX>(NodeIndexPair) << "] "
                      << *NextObj << "\n";);

    bool HitConstantExpr = false;

    std::string Tip;
    Instruction *Operation;
    if (CurrObjIsSite && !NextObjIsSite) {
      // build tips for <site, operand>
      Tip = buildSiteNodeTip((const GuardedValueFlowSite *)CurrObj,
                             (const GuardedValueFlowNode *)NextObj);
      Operation = ((const GuardedValueFlowSite *)CurrObj)->getInstruction();
      if (auto *Output = dyn_cast<GuardedValueFlowCallOutputNode>(NextObj)) {
        Operation = Output->getCallSite();
      }

      assert(Operation);
    } else if (!CurrObjIsSite && NextObjIsSite) {
      // build tips for <operand, site>
      Tip = buildNodeSiteTip((const GuardedValueFlowNode *)CurrObj,
                             (const GuardedValueFlowSite *)NextObj);
      Operation = ((const GuardedValueFlowSite *)NextObj)->getInstruction();
      if (!Operation) {
        llvm_unreachable("Fatal error: a null operation");
        continue;
      }

      if (isa<GuardedValueFlowCallSite>(NextObj)) {
        auto NextSiteIndexPair =
            Trace->find(I, [](const GuardedValueFlowObject *O) {
              return O && isa<GuardedValueFlowSite>(O);
            });
        if (auto *US = dyn_cast<GuardedValueFlowSite>(
                std::get<OBJECT>(NextSiteIndexPair))) {
          OperationInCallee = US->getInstruction();
        } else {
          OperationInCallee = nullptr;
        }
      }
    } else if (!CurrObjIsSite && !NextObjIsSite) {
      // build tips for <operand, operand>
      if (auto *N = nodeOfKind<GuardedValueFlowNode::Kind::SimpleOperand,
                               GuardedValueFlowNode>(NextObj)) {
        Operation = dyn_cast<Instruction>(N->getLLVMValue());
        if (!Operation) {
          auto *CE = dyn_cast<ConstantExpr>(N->getLLVMValue());
          assert(CE &&
                 "It must be an constant expr if it is not an instruction!");
          Operation = CE->getAsInstruction();
          HitConstantExpr = true;
        }
      } else if (auto *N = nodeOfKind<GuardedValueFlowNode::Kind::StoreMemory,
                                      GuardedValueFlowNode>(NextObj)) {
        Operation = N->getDebugInstruction();
        assert(Operation);
      } else if (auto *N = dyn_cast<GuardedValueFlowPhiNode>(NextObj)) {
        Operation = dyn_cast<Instruction>(N->getLLVMValue());
        assert(Operation);
      } else if (auto *N =
                     nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoInput,
                                GuardedValueFlowCallOutputNode>(NextObj)) {
        Operation = N->getCallSite();
        assert(Operation);
      } else if (nodeOfKind<GuardedValueFlowNode::Kind::PseudoReturn,
                            GuardedValueFlowReturnNode>(NextObj)) {
        Operation = nullptr;
      } else {
        errs() << *NextObj << "\n";
        llvm_unreachable("Unknown trace pattern!");
        continue;
      }
      if (Operation)
        Tip = buildNodeNodeTip((const GuardedValueFlowNode *)CurrObj,
                               (const GuardedValueFlowNode *)NextObj);
    } else {
      llvm_unreachable(
          "One use site cannot be followed by the other use site in a trace!");
      continue;
    }

    LLVM_DEBUG(errs() << "[Tip] " << Tip << "\n";
               if (HitConstantExpr) errs() << "[Ins] " << *Operation << "\n\n";
               else if (Operation) errs()
               << "[Ins] " << DIA->getIRString(Operation) << "\n\n";
               else errs() << "[Ins] nullptr\n\n";);

    if (!Tip.empty() && Operation) {
      BugDiagStep *DiagStep =
          mergeWithLastBugStep(NextObj, LastDiagStep, Operation, Tip);
      if (DiagStep != LastDiagStep) {
        Report->append_step(DiagStep);
        LastDiagStep = DiagStep;
      }
    }

    I = std::get<INDEX>(NodeIndexPair);
    SrcIndexPair = std::move(NodeIndexPair);

    // we must del the operation manually
    // if the operation is created via
    // a constant expr
    if (HitConstantExpr)
      Operation->deleteValue();
  }

  if (!LastDiagStep) {
    errs() << "\n\nTrace Len: " << Trace->get_length() << "\n";
    errs() << *Trace.get() << "\n";
    llvm_unreachable("Nothing in report!");
  }

  // Highlight what the bug is
  if (Vuln.get()) {
    LastDiagStep->tip += " (" + color_str(Vuln->getDescription(), "red") + ")";
  }

  PhiSelectRecord.clear();
  return Report;
}

BugDiagStep *VulnerabilityReportBuilder::mergeWithLastBugStep(
    const GuardedValueFlowObject *NextObj, BugDiagStep *LastDiagStep,
    Instruction *Operation, std::string &Tip) {
  std::string FileStorage = DIA->getSourceFile(Operation);
  StringRef FileName(FileStorage);
  int Line = DIA->getSourceLine(Operation);

  BugDiagStep *DiagStep = LastDiagStep;

#define LINE_OBSCURE 0

  if (LastDiagStep && abs(Line - LastDiagStep->src_line) <= LINE_OBSCURE &&
      FileName == LastDiagStep->src_file) {
    if (isa<LoadInst>(LastDiagStep->inst) && isa<LoadInst>(Operation)) {
      // Same line, we do not put the load instruction into the trace
      // e.g. we do not break a->f->g to a->f and (a->f)->g
      DiagStep = LastDiagStep;
    } else if (isa<GuardedValueFlowDereferenceSite>(NextObj)) {
      if (isa<LoadInst>(Operation) || isa<StoreInst>(Operation)) {
        if (isa<LoadInst>(LastDiagStep->inst)) {
          DiagStep = LastDiagStep;
        } else if (isa<PHINode>(LastDiagStep->inst)) {
          // We do not merge PHI because the line number of PHI is Vague
          DiagStep = new BugDiagStep();
        } else {
          std::string LastTip = LastDiagStep->tip;
          bool ReadyForMerge = truncTip(LastTip);
          if (ReadyForMerge) {
            DiagStep = LastDiagStep;
            Tip = LastTip + " is dereferenced";
          } else {
            DiagStep = new BugDiagStep();
          }
        }
      } else {
        DiagStep = new BugDiagStep();
      }
    } else if (isa<StoreInst>(Operation) || isa<ReturnInst>(Operation) ||
               (isa<CallInst>(Operation) &&
                ir_expression::getEnclosingFunction(Operation) ==
                    ir_expression::getEnclosingFunction(LastDiagStep->inst)) ||
               isa<SelectInst>(Operation)) {
      // Directly store/return an expression
      std::string LastTip = LastDiagStep->tip;
      bool ReadyForMerge = truncTip(LastTip);

      if (!ReadyForMerge) {
        // Not easy to merge, we do not merge now
        DiagStep = new BugDiagStep();
      } else {
        // We only merge easy cases now
        DiagStep = LastDiagStep;
        toLowerCase(Tip);

        if (isa<StoreInst>(Operation)) {
          Value *baseVal;
          Tip = LastTip + " is stored to " +
                buildExpression(Operation->getOperand(1), baseVal);
        } else if (isa<ReturnInst>(Operation)) {
          Tip = LastTip + " is returned to caller";
        } else if (isa<SelectInst>(Operation)) {
          std::string Label =
              (Tip.find("Take true condition") != std::string::npos ? "true"
                                                                    : "false");
          Tip = LastTip + ", taking " + Label + " condition, is assigned to " +
                emph_str(IResolver->restore_value_expr(Operation));
        } else {
          // isa<CallInst>(Operation)
          std::string::size_type pos = Tip.find(" gets value from ");
          if (pos != std::string::npos) {
            Tip = Tip.substr(0, pos);
            Tip = LastTip + " is passed as " + Tip;
          } else {
            simpleLinkTip(LastTip, Tip);
          }
        }
      }
    } else {
      DiagStep = new BugDiagStep();
    }
  } else {
    if (LastDiagStep && LastDiagStep->src_line == 0 &&
        isa<StoreInst>(LastDiagStep->inst)) {
      // A store with no debug info is a temp store
      // We merge the steps
      DiagStep = LastDiagStep;
    } else {
      DiagStep = new BugDiagStep();
    }
  }

  DiagStep->inst = Operation;
  DiagStep->src_file = FileName;
  DiagStep->src_line = Line;
  DiagStep->tip = Tip;

  return DiagStep;
}

std::string VulnerabilityReportBuilder::buildNodeSiteTip(
    const GuardedValueFlowNode *Operand, const GuardedValueFlowSite *Site) {
  std::string Tip;
  if (auto *CS = dyn_cast<GuardedValueFlowCallSite>(Site)) {
    if (isa<GuardedValueFlowCallOutputNode>(Operand) &&
        callSite(cast<GuardedValueFlowCallOutputNode>(Operand)) == CS) {
      Tip = buildCallSiteOutputTip(
          CS, ((const GuardedValueFlowCallOutputNode *)Operand));
    } else {
      Tip = buildCallSiteInputTip(CS, Operand);
    }
  } else if (auto *RS = dyn_cast<GuardedValueFlowReturnSite>(Site)) {
    if (auto *PseudoReturn =
            nodeOfKind<GuardedValueFlowNode::Kind::PseudoReturn,
                       GuardedValueFlowReturnNode>(Operand)) {
      Tip = buildPseudoReturnTip(RS, PseudoReturn);
    } else {
      // common return
      Tip = buildInstructionTip(RS->getInstruction(), Operand->getLLVMValue());
    }
  } else if (auto *DS = operationalSite(Site)) {
    Tip = buildInstructionTip(DS->getInstruction(), Operand->getLLVMValue());
  }
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildSiteNodeTip(
    const GuardedValueFlowSite *Site, const GuardedValueFlowNode *Operand) {
  std::string Tip;
  if (auto *CS = dyn_cast<GuardedValueFlowCallSite>(Site)) {
    if (auto *Output = dyn_cast<GuardedValueFlowCallOutputNode>(Operand)) {
      Tip = buildCallSiteOutputTip(CS, Output);
    } else if (!isa<GuardedValueFlowArgumentNode>(Operand)) {
      Tip = buildCallSiteInputTip(CS, Operand);
    }
  } else if (isa<GuardedValueFlowReturnSite>(Site)) {
    assert(Site->getGraph()->getBaseFunc() !=
           Operand->getGraph()->getBaseFunc());
    auto *Output = dyn_cast<GuardedValueFlowCallOutputNode>(Operand);
    assert(Output);
    Tip = buildCallSiteOutputTip(callSite(Output), Output);
  } else if (auto *DS = operationalSite(Site)) {
    Tip = buildInstructionTip(DS->getInstruction(), Operand->getLLVMValue());
  }
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildCallSiteOutputTip(
    const GuardedValueFlowCallSite *CS,
    const GuardedValueFlowCallOutputNode *Output) {
  std::string Tip;
  if (auto *PseudoOutput =
          nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoOutput,
                     GuardedValueFlowCallOutputNode>(Output)) {
    Function *Callee = PseudoOutput->getCallee();
    Instruction *Call = PseudoOutput->getCallSite();
    Tip = "Calling to function " + emph_str(getReadableFunctionName(Callee)) +
          " has side-effect";
    if (Callee) {
      int Offset4Reading = isClassMemberFunction(*Callee) ? 0 : 1;
      const gvfg::AccessPath &AP = PseudoOutput->getAccessPath();
      Value *APBasePtr = AP.get_base_ptr();

      if (APBasePtr) {
        if (Argument *BaseArg = dyn_cast<Argument>(APBasePtr)) {
          int Idx = 0;
          for (Argument &CalleeArg : Callee->args()) {
            if (&CalleeArg == BaseArg) {
              Value *ActualArg =
                  cast<CallBase>(CS->getInstruction())->getArgOperand(Idx);
              std::string ActualArgDbgInfo =
                  IResolver->restore_value_expr(ActualArg, &PhiSelectRecord);
              std::string DbgInfo =
                  IResolver->restore_access_path_expr(AP, ActualArgDbgInfo);

              Tip += ": modifying the value " + emph_str(DbgInfo);
              Tip += ", where " + emph_str(ActualArgDbgInfo);
              if (Idx + Offset4Reading > 0)
                Tip += " is the " +
                       format_str("%d%s", Idx + Offset4Reading,
                                  ordinal_suffix(Idx + Offset4Reading)) +
                       " argument assigning to " +
                       emph_str(DIA->getVariableName(BaseArg));
              else
                Tip += " is the 'this' pointer";
              break;
            }
            Idx++;
          }
        } else if (isa<GlobalValue>(APBasePtr)) {
          std::string DbgInfo = IResolver->restore_access_path_expr(AP);
          Tip += ": modifying the value of " + emph_str(DbgInfo);
          Tip += ", where " + emph_str(DIA->getVariableName(APBasePtr)) +
                 " is a global variable";
        } else {
          std::string DbgInfo = IResolver->restore_access_path_expr(
              AP, AP.isFromReturn()
                      ? IResolver->restore_value_expr(Call, &PhiSelectRecord)
                      : "");
          Tip += ": modifying the value of " + emph_str(DbgInfo);
        }
      }
    }
  } else {
    // common callsite output
    Tip = buildInstructionTip(CS->getInstruction(), Output->getLLVMValue());
  }
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildCallSiteInputTip(
    const GuardedValueFlowCallSite *CS, const GuardedValueFlowNode *Input) {
  std::string Tip;
  if (auto *PseudoInput =
          nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoInput,
                     GuardedValueFlowCallOutputNode>(Input)) {
    // pseudo argument
    Function *Callee = PseudoInput->getCallee();
    if (Callee) {
      Tip += "Call function " + emph_str(getReadableFunctionName(Callee));

      int Offset4Reading = 1;
      if (Callee->arg_size() > 0) {
        if (isClassMemberFunction(*Callee)) {
          Offset4Reading = 0;
        }
      }

      const gvfg::AccessPath &AP = PseudoInput->getAccessPath();
      Value *APBasePtr = AP.get_base_ptr();
      if (APBasePtr) {
        if (Argument *BaseArg = dyn_cast<Argument>(APBasePtr)) {
          int Idx = 0;
          for (Argument &CalleeArg : Callee->args()) {
            if (&CalleeArg == BaseArg) {
              Value *Arg =
                  cast<CallBase>(CS->getInstruction())->getArgOperand(Idx);
              std::string ActualArgDbgInfo =
                  IResolver->restore_left_value_expr(Arg, &PhiSelectRecord);
              std::string APDbgInfo =
                  IResolver->restore_access_path_expr(AP, ActualArgDbgInfo);
              Tip += ", reading value " + emph_str(APDbgInfo);
              Tip += ", where " + emph_str(ActualArgDbgInfo);

              // idx start from 0, while it is better for human readers to start
              // from 1
              if (Idx + Offset4Reading > 0)
                Tip += " is the " +
                       format_str("%d%s", Idx + Offset4Reading,
                                  ordinal_suffix(Idx + Offset4Reading)) +
                       " Argument (" + emph_str(DIA->getVariableName(BaseArg)) +
                       ")";
              else
                Tip += " is the 'this' pointer";
              break;
            }
            Idx++;
          }
        } else if (isa<GlobalValue>(APBasePtr)) {
          Tip += ", where " + emph_str(DIA->getVariableName(APBasePtr)) +
                 " is a global variable";
        }
      }
    } else {
      Tip += "Enter Function Call";
    }
  } else if (CS->isCommonInput(Input)) {
    // common input
    Tip = buildInstructionTip(CS->getInstruction(), Input->getLLVMValue());
  } else {
    // A patch for Taint XXX
    if (LoadInst *LI = dyn_cast<LoadInst>(Input->getLLVMValue())) {
      Tip = buildInstructionTip(CS->getInstruction(), LI->getOperand(0));
    }
  }
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildPseudoReturnTip(
    const GuardedValueFlowReturnSite *RS,
    const GuardedValueFlowReturnNode *PseudoReturn) {
  std::string Tip = "Return to caller";

  const gvfg::AccessPath &AP = PseudoReturn->getAccessPath();
  Value *APBasePtr = AP.get_base_ptr();
  if (APBasePtr == nullptr) {
    // Escaping object, we do not track now
  } else {
    std::string APDbgInfo = IResolver->restore_access_path_expr(AP);
    Tip += ", updating the value " + emph_str(APDbgInfo);
    if (isa<GlobalValue>(APBasePtr)) {
      Tip += ", where " + emph_str(DIA->getVariableName(APBasePtr)) +
             " is a global variable";
    } else if (AP.isFromReturn()) {
      Tip += ", where " + emph_str(DIA->getVariableName(APBasePtr)) +
             " is the function return value";
    }
  }

  return (Tip);
}

std::string
VulnerabilityReportBuilder::buildNodeNodeTip(const GuardedValueFlowNode *From,
                                             const GuardedValueFlowNode *To) {
  std::string Tip;
  if (auto *TSt = nodeOfKind<GuardedValueFlowNode::Kind::StoreMemory,
                             GuardedValueFlowNode>(To)) {
    // anything -> store-mem-node
    if (auto *SI = dyn_cast<StoreInst>(TSt->getDebugInstruction())) {
      Tip = buildInstructionTip(SI, From->getLLVMValue());
    }
  } else if (nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoInput,
                        GuardedValueFlowCallOutputNode>(To)) {
    // TODO: This step is redundant, shall be removed when tested
    //	} else if (auto* TPseudoInput =
    // nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoInput,
    // GuardedValueFlowCallOutputNode>(To)) { anything -> pseudo callsite input
    //		if (nodeOfKind<GuardedValueFlowNode::Kind::StoreMemory,
    // GuardedValueFlowNode>(From)) { 			Tip = "The value yielded
    // in the last step"; 		} else { 			Tip =
    // IResolver->restore_access_path_expr(From->getLLVMValue());
    //		}
    //
    //		const gvfg::AccessPath& AP = TPseudoInput->getAccessPath();
    //		Value* APBasePtr = AP.get_base_ptr();
    //		if (APBasePtr == nullptr) {
    //			Tip += " will be a side-effect input of the call.";
    //		} else {
    //			std::string APDbgInfo =
    // IResolver->get_access_path_dbg_info(AP); 			Tip += "
    // is assigned to
    // "
    // + emph_str(APDbgInfo) + ", which will be a side-effect input of the
    // call."; 			if (isa<GlobalValue>(APBasePtr)) {
    // Tip
    // += " Here,
    // "
    // +
    // emph_str(DIA->getVariableName(APBasePtr))
    //+ 						" is a global variable";
    //			}
    //		}
  } else {
    // anything -> node that is not in {store-mem-node, pseudo callsite input}
    if (nodeOfKind<GuardedValueFlowNode::Kind::StoreMemory,
                   GuardedValueFlowNode>(From) ||
        isa<LoadInst>(To->getLLVMValue())) {
      if (!nodeOfKind<GuardedValueFlowNode::Kind::PseudoReturn,
                      GuardedValueFlowReturnNode>(To))
        Tip = buildInstructionTip(dyn_cast<Instruction>(To->getLLVMValue()),
                                  To->getLLVMValue());
    } else {
      // in case constant expr
      auto *Op = dyn_cast<Instruction>(To->getLLVMValue());
      auto *Op2 =
          (Op ? Op
              : dyn_cast<ConstantExpr>(To->getLLVMValue())->getAsInstruction());
      Tip = buildInstructionTip(Op2, From->getLLVMValue());
      if (!Op)
        Op2->deleteValue();
    }
  }
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildInstructionTip(Instruction *Inst,
                                                            Value *Val) {
  assert(Inst && "Are you sure to build reports for a nullptr instruction?");
  assert(Val && "You do not indicate what value is used in the instruction!");
  bool ValIsOperand = false;
  ;
  for (unsigned I = 0; I < Inst->getNumOperands(); I++) {
    if (Inst->getOperand(I) == Val) {
      ValIsOperand = true;
      break;
    }
  }
  if (!ValIsOperand && Val != Inst) {
    errs() << "\nInst: " << *Inst << "\n";
    errs() << "Value: " << *Val << "\n";
    llvm_unreachable(
        "The instruction does not contain the value as an operand!");
  }

  std::string Tip;
  switch (Inst->getOpcode()) {
  case Instruction::Ret:
    Tip = buildReturnInstTip((ReturnInst *)Inst, Val);
    break;
    // mem
  case Instruction::Load:
    Tip = buildLoadInstTip((LoadInst *)Inst, Val);
    break;
  case Instruction::Store:
    Tip = buildStoreInstTip((StoreInst *)Inst, Val);
    break;
    // others
  case Instruction::PHI:
    Tip = buildPhiInstTip((PHINode *)Inst, Val);
    break;
  case Instruction::Call:
    Tip = buildCallInstTip((CallInst *)Inst, Val);
    break;
  case Instruction::Select:
    Tip = buildSelectInstTip((SelectInst *)Inst, Val);
    break;

    // insts that are ignored
    // can extend in the future
    // -----------------------------------------
    // gep
  case Instruction::GetElementPtr:
    // var arg
  case Instruction::VAArg:
    // vector
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
    break;
    // binary inst
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::FRem:
  case Instruction::SRem:
    Tip = buildDivisionInstTip(Inst, Val);
    break;
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::Add:
  case Instruction::Sub:
    // conversion inst
  case Instruction::AddrSpaceCast:
  case Instruction::IntToPtr:
  case Instruction::PtrToInt:
  case Instruction::BitCast:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::SIToFP:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::FPToUI:
    // cmp
  case Instruction::ICmp:
  case Instruction::FCmp:
    break;
    // unsupported
  case Instruction::Alloca:
  case Instruction::ExtractValue:
  case Instruction::InsertValue:
  case Instruction::ShuffleVector:
  case Instruction::AtomicRMW:
  case Instruction::AtomicCmpXchg:
  case Instruction::Fence:
  case Instruction::Switch:
  case Instruction::Invoke:
  case Instruction::LandingPad:
  case Instruction::Resume:
  case Instruction::IndirectBr:
  case Instruction::Unreachable:
  case Instruction::Br:
  default:
    errs() << *Inst << "\n";
    llvm_unreachable("Unsupported instruction!");
    exit(1);
  }

  return (Tip);
}

std::string
VulnerabilityReportBuilder::buildGlobalVariableTip(GlobalVariable *GV,
                                                   Value *Val) {
  assert(GV->hasInitializer() &&
         "If no initializer, it should not be an item in a trace!");
  std::string Tip = "The global variable " +
                    emph_str(DIA->getVariableName(GV)) + " is initialized as " +
                    emph_str(DIA->getVariableName(GV->getInitializer()));
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildReturnInstTip(ReturnInst *RI,
                                                           Value *Val) {
  std::string Tip =
      "Return " +
      emph_str(IResolver->restore_value_expr(Val, &PhiSelectRecord));
  if (isa<UndefValue>(Val)) {
    Tip += ". It may miss a return statement, while the function requires one.";
  }
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildLoadInstTip(LoadInst *LI,
                                                         Value *Val) {
  Value *BasePtrVal;
  std::string PtrExpr = buildExpression(LI->getOperand(0), BasePtrVal);

  std::string Tip;
  StringRef LiName = LI->getName();

  if (LiName.startswith("vtable")) {
    // Might be used for calling a virtual function
    Tip = "Call a virtual function with " + PtrExpr;
  } else {
    Tip = "Load value from " + PtrExpr;
    if (DIA->hasVariableDebugName(LI)) {
      Tip += " and assign to " + emph_str(DIA->getVariableName(LI));
    }
  }

  return (Tip);
}

std::string VulnerabilityReportBuilder::buildStoreInstTip(StoreInst *ST,
                                                          Value *Val) {
  Value *StoreValue = ST->getOperand(0);
  Value *BasePtrVal;
  std::string Tip = "Store ";

  if (isa<CallInst>(StoreValue) && !DIA->hasVariableDebugName(StoreValue)) {
    // No name callsite
    CallInst *ci = cast<CallInst>(StoreValue);
    Function *func = ci->getCalledFunction();
    if (func) {
      Tip +=
          " the return of function " + emph_str(getReadableFunctionName(func));
    } else {
      auto *CS = cast<CallBase>(ci);
      if (isVirtualCall(CS)) {
        // TODO: Should print the virtual function name
        // But under current architecture, we don't know which function was in.
        Tip += " the return value of virtual function call";
      } else {
        Tip += " the return of indirect call " +
               emph_str(IResolver->restore_value_expr(ci->getCalledOperand()));
      }
    }
  } else {
    Tip += emph_str(IResolver->restore_value_expr(StoreValue));
  }

  Tip += " to " + buildExpression(ST->getOperand(1), BasePtrVal);
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildDivisionInstTip(Instruction *DI,
                                                             Value *Val) {
  Value *Op2 = DI->getOperand(1);
  std::string Tip = "Divide by " + emph_str(IResolver->restore_value_expr(Op2));
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildPhiInstTip(PHINode *Phi,
                                                        Value *Val) {
  assert(Val);
  PhiSelectRecord[Phi] = Val;

  // start the lambda
  auto generateDiagInfo = [&](BasicBlock *IncomingBB) {
    std::string IncomingPathTip;
    assert(IncomingBB &&
           "A null PHI node must have one of its argument is null.");

    // Get the condition of picking this operand
    BasicBlock *PhiBB = Phi->getParent();
    Instruction *Terminator = IncomingBB->getTerminator();

    if (BranchInst *Br = dyn_cast<BranchInst>(Terminator)) {
      int BrLine = DIA->getSourceLine(Terminator);
      if (BrLine && Br->isConditional()) {
        // Has line info for the nearest branch
        const char *BrLabel = branchConditionFromTo(IncomingBB, PhiBB);
        IncomingPathTip =
            format_str("Taking %s branch at line %d, ", BrLabel, BrLine);
      } else {
        // Seek the nearest if conditions that route to oprdBB for help
        Function *PhiFunc = PhiBB->getParent();
        assert(PhiFunc && "Phi instruction must have an enclosing function");

        // We print the key if-condition choices that lead to the oprdBB
        auto &CDG = CDGs->getControlDependenceAnalysis(*PhiFunc);
        auto CDeps = CDG.getCDBlocks(IncomingBB);
        std::vector<std::pair<const char *, int>> BrInfo;
        // Keep the original builder's repeated first-dependence policy.
        for (size_t Index = 0; Index < CDeps.size(); ++Index) {
          BasicBlock *CDBB = CDeps.front();
          if (CDBB) {
            Instruction *CDTerminator = CDBB->getTerminator();
            const char *BrLabel = "true";
            if (auto *Branch = dyn_cast<BranchInst>(CDTerminator))
              if (Branch->isConditional())
                BrLabel = CDG.isReachable(Branch->getSuccessor(0), IncomingBB)
                              ? "true"
                              : "false";
            int BrLine = DIA->getSourceLine(CDTerminator);
            if (BrLine) {
              auto BrPair = std::make_pair(BrLabel, BrLine);
              if (std::find(BrInfo.begin(), BrInfo.end(), BrPair) ==
                  BrInfo.end())
                BrInfo.push_back(BrPair);
            }
          }
        }

        if (!BrInfo.empty()) {
          for (int Index = 0; Index < BrInfo.size(); ++Index) {
            const char *BrLabel = BrInfo[Index].first;
            int BrLine = BrInfo[Index].second;
            if (Index == 0)
              IncomingPathTip =
                  format_str("Taking %s branch at line %d, ", BrLabel, BrLine);
            else {
              IncomingPathTip += format_str("or taking %s branch at line %d, ",
                                            BrLabel, BrLine);
            }
          }
        }
      }
    }

    return (IncomingPathTip);
    // end of the lambda
  };

  Function *ParentFunc = Phi->getParent()->getParent();

  bool ShouldFindPhi = !(PhiNodeCache.count(ParentFunc));
  std::unordered_set<PHINode *> &PhiNodes = PhiNodeCache[ParentFunc];

  if (ShouldFindPhi) {
    for (BasicBlock &BB : *ParentFunc) {
      for (Instruction &Inst : BB) {
        if (PHINode *CurPhi = dyn_cast<PHINode>(&Inst)) {
          PhiNodes.insert(CurPhi);
        }
      }
    }
  }

  std::set<std::string> PathTips;
  for (unsigned Index = 0; Index < Phi->getNumOperands(); ++Index) {
    if (Phi->getIncomingValue(Index) == Val) {
      BasicBlock *IncomingBB = Phi->getIncomingBlock(Index);
      std::string Tip = generateDiagInfo(IncomingBB);
      PathTips.insert(Tip);

      // Guess the path select for other Phi Nodes, which is important for Dbg
      // Info Generation
      for (PHINode *PhiNodeToGuess : PhiNodes) {

        // We only guess the value selection for PhiNodes that are not guessed
        if (PhiSelectRecord.count(PhiNodeToGuess))
          continue;

        int Idx = PhiNodeToGuess->getBasicBlockIndex(IncomingBB);
        if (Idx >= 0) {
          PhiSelectRecord[PhiNodeToGuess] =
              PhiNodeToGuess->getIncomingValue(Idx);
        }
      }
    }
  }

  std::string Tip;
  if (PathTips.size() > 1) {
    Tip += "Possible paths:\n";

    size_t I = 0;
    for (const std::string &Info : PathTips) {
      I++;
      Tip += (std::to_string(I) + ". " + Info);
      if (I < PathTips.size()) {
        Tip += "\n";
      }
    }
    Tip += "\n\n";
  } else {
    Tip = std::move(*PathTips.begin());
  }

  if (isa<Constant>(Val)) {
    int ValLine = DIA->getPhiOperandSourceLine(Phi, Val);
    Tip += emph_str(valueToString(Val));

    if (ValLine > 0)
      Tip += format_str(" defined on line %d", ValLine);

    Tip += " is assigned to " + emph_str(DIA->getVariableName(Phi));
  } else {
    // Restore the expression in source code for the operand
    int ValDefLine = DIA->getSourceLine(Val);

    Tip += emph_str(IResolver->restore_value_expr(Val, &PhiSelectRecord)) +
           " defined on line " + std::to_string(ValDefLine);
  }

  int PhiLine = DIA->getSourceLine(Phi);
  Tip += format_str(" reaches line %d", PhiLine);

  return (Tip);
}

std::string VulnerabilityReportBuilder::buildSelectInstTip(SelectInst *SL,
                                                           Value *Val) {
  Value *TrueBrValue = SL->getTrueValue();
  const char *Label = (TrueBrValue == Val ? "true" : "false");
  std::string Tip = "Take ";
  Tip += Label;
  Tip += " condition, assign ";
  Tip += emph_str(IResolver->restore_value_expr(Val, &PhiSelectRecord));
  Tip += " to " + emph_str(DIA->getVariableName(SL));
  return (Tip);
}

std::string VulnerabilityReportBuilder::buildCallInstTip(CallInst *CI,
                                                         Value *Val) {
  std::string Tip;

  Function *Callee = CI->getCalledFunction();
  if (CI == Val) {
    // Situation 1: Receive the function returned value
    Tip = "Assign the return value of";
    if (Callee) {
      Tip += " the function " + emph_str(getReadableFunctionName(Callee));
    } else {
      auto *CS = cast<CallBase>(CI);
      if (isVirtualCall(CS)) {
        // TODO: Should print the virtual function name
        // But under current architecture, we don't know which function was in.
        Tip += " virtual function call";
      } else {
        // Print the indirectly called function name
        Tip += " " +
               emph_str(IResolver->restore_value_expr(CI->getCalledOperand())) +
               " (an indirect call)";
      }
    }
    Tip +=
        " to " + emph_str(IResolver->restore_value_expr(CI, &PhiSelectRecord));
  } else if (Callee == Val) {
    // Situation 2: A function callsite but we don't analyze the callee
    Tip = "Call ";

    if (Function *ValF = dyn_cast<Function>(Val)) {
      Tip += emph_str(getReadableFunctionName(ValF));
    } else {
      Tip += emph_str(IResolver->restore_value_expr(Val, &PhiSelectRecord));
    }
  } else {
    // Situation 3: Pass argument to function and prepare for analyzing the
    // called function First obtain the argument index at the call site
    int ArgIndex = getArgumentIndex(CI, Val);

    // Tell the user how null value is passed into the callee
    if (!Callee) {
      if (OperationInCallee)
        Callee = ir_expression::getEnclosingFunction(OperationInCallee);

      if (!Callee)
        return std::to_string(ArgIndex) + ordinal_suffix(ArgIndex) +
               " argument in the call is invalid!";
    }

    int Offset4Reading = 1;
    if (Callee->arg_size()) {
      if (isClassMemberFunction(*Callee)) {
        Offset4Reading = 0;
      }
    }

    // Second obtain the parameter for the callee function
    if (Value *Parameter = getParameterAt(Callee, ArgIndex)) {
      // for human reading, add an offset
      int K = ArgIndex + Offset4Reading;
      if (K) {
        Tip = format_str("The %d%s parameter", K, ordinal_suffix(K));
        if (!(Callee->isDeclaration() || Callee->isIntrinsic()))
          Tip += " " + emph_str(DIA->getVariableName(Parameter));
      } else {
        Tip = format_str("The base pointer %s",
                         emph_str(DIA->getVariableName(Parameter)).c_str());
      }

    } else {
      // The argument is assigned to a variadic parameter
      Tip = "A variadic parameter";
    }

    Tip += " of function " + emph_str(getReadableFunctionName(Callee));

    Tip += " gets value from ";

    Tip += emph_str(IResolver->restore_value_expr(Val, &PhiSelectRecord));
  }

  return (Tip);
}

/**
 * Some private functions for assistance
 */

std::string VulnerabilityReportBuilder::buildExpression(Value *Val,
                                                        Value *&BaseVal) {
  std::string Tip;

  Value *OrigVal = Val;
  // Tell the user which pointer is dereferenced by which pointer expression
  while (isa<PHINode>(Val)) {
    Value *Pred = PhiSelectRecord[Val];
    /*
     * pred can be NULL because the \phi node is used to load a value stored at
     * other places. Of course, pred itself can be a ConstantPointerNull or
     * Undef
     */
    if (Pred == nullptr || isa<Constant>(Pred))
      break;
    Val = Pred;
  }

  if (isa<Constant>(Val)) {
    // Very rare case, try to get the field from a constant GEP expression
    Val = OrigVal;
  }

  // Show the user which field is taken by this GEP instruction
  std::string DerefExpr =
      IResolver->restore_access_path_expr(Val, true, &PhiSelectRecord);
  Tip += "the expression " + emph_str(DerefExpr);

  BaseVal = Val;
  return (Tip);
}

std::string VulnerabilityReportBuilder::getReadableFunctionName(Function *F) {
  return DIA->getDeclaredFunctionName(F);
}
} // namespace gsaf
} // namespace lotus

namespace lotus {
namespace gsaf {
using namespace llvm;
using namespace std;
using ir_expression::valueToString;

#undef DEBUG_TYPE
#define DEBUG_TYPE "gsaf-taint-report-builder"

BugReport *
TaintReportBuilder::buildReport(std::shared_ptr<VulnerabilityTrace> &Trace,
                                DebugInfoAnalysis *DIA,
                                ir_expression::IRExpressionRenderer *IResolver,
                                gsa::ControlDependenceAnalysisPass *CDGs,
                                std::shared_ptr<Vulnerability> Vuln) {
  LLVM_DEBUG(errs() << "\n"; errs() << *Trace.get() << "\n";
             errs() << "-------------\n";);

  assert(Trace->get_length() > 3);

  BugReport *Report =
      new BugReport(BugReportMgr::get_instance().register_bug_type(
          Vuln ? Vuln->getName() : "GSAF"));

  auto *SrcNode = Trace->at(0);
  auto *SrcSite = Trace->at(1);
  assert(SrcNode);
  assert(SrcSite);
  assert(isOperand(SrcNode));
  assert(isa<GuardedValueFlowSite>(SrcSite));

  std::shared_ptr<VulnerabilityTrace> StartSubTrace =
      std::make_shared<VulnerabilityTrace>();
  StartSubTrace->push(SrcNode);
  StartSubTrace->push(SrcSite);

  size_t WrapperTraceEnd = 2;
  if (Trace->at(2) != SrcNode && !Trace->at(3) &&
      isa<GuardedValueFlowArgumentNode>(Trace->at(2))) {
    // Find the taint wrapper
    std::stack<Function *> FrameStack;
    size_t I = 2;
    for (size_t E = Trace->get_length(); I < E; ++I) {
      auto *Obj = Trace->at(I);
      if (Obj) {
        if (isa<GuardedValueFlowArgumentNode>(Obj) && !Trace->at(I + 1)) {
          FrameStack.push(Obj->getGraph()->getBaseFunc());
        } else if (isa<GuardedValueFlowReturnSite>(Obj)) {
          FrameStack.pop();

          if (FrameStack.empty()) {
            break;
          }
        }

        if (Obj->getGraph()->getBaseFunc() != FrameStack.top()) {
          break;
        }

        StartSubTrace->push(Obj);
      } else {
        StartSubTrace->push(Obj);
      }
    }

    if (!FrameStack.empty()) {
      WrapperTraceEnd = I;
    } else {
      StartSubTrace->clear();
      StartSubTrace->push(SrcNode);
      StartSubTrace->push(SrcSite);
    }
  }

  {
    auto *SubReport = VulnerabilityReportBuilder::buildReport(
        StartSubTrace, DIA, IResolver, CDGs);
    for (auto *step : SubReport->get_steps())
      Report->append_step(new BugDiagStep(*step));
    delete SubReport;
  }

  LLVM_DEBUG(errs() << "Detected Taint Wrapper End: " << WrapperTraceEnd
                    << "\n");

  // produce tips for alias relations TODO
  // Searching the alias part
  size_t I = WrapperTraceEnd;
  for (size_t E = Trace->get_length(); I < E; ++I) {
    assert(I + 1 < E);

    auto *CurrObj = Trace->at(I);
    auto *NextObj = Trace->at(I + 1);

    if (!NextObj) {
      // Here it must be a format like:
      // src-node nullptr(src-site is nullptr) src-node
      assert(I + 2 < E);
      assert(CurrObj == Trace->at(I + 2));
      ++I;
      continue;
    }

    bool CurrObjIsSite = isa<GuardedValueFlowSite>(CurrObj);
    bool NextObjIsSite = isa<GuardedValueFlowSite>(NextObj);
    assert(!(CurrObjIsSite && NextObjIsSite) &&
           "Two GVFG sites cannot be adjacent to each other in the trace!");

    if (!CurrObjIsSite && !NextObjIsSite) {
      if (((const GuardedValueFlowNode *)NextObj)
              ->containsParent((const GuardedValueFlowNode *)CurrObj)) {
        // okay
      } else {
        break;
      }
    } else if (NextObjIsSite) {
      if (isa<GuardedValueFlowCallOutputNode>(CurrObj) &&
          callSite(cast<GuardedValueFlowCallOutputNode>(CurrObj)) == NextObj) {
        // search the output summary in alias part.
        // Thus, there must be something after the site
        assert(I + 3 < E);
        assert(isa<GuardedValueFlowArgumentNode>(Trace->at(I + 2)) &&
               "Output summary must start with an argument!");
        assert(!Trace->at(I + 3) &&
               "The source site of an output summary must be nullptr!");

        std::stack<Function *> FrameStack;

        size_t II = I + 2;
        for (; II < E; ++II) {
          auto *Obj = Trace->at(II);
          if (Obj) {
            if (isa<GuardedValueFlowArgumentNode>(Obj) && !Trace->at(II + 1)) {
              FrameStack.push(Obj->getGraph()->getBaseFunc());
              LLVM_DEBUG(errs() << "[" << II << "] Entering "
                                << Obj->getGraph()->getBaseFunc()->getName()
                                << " with frame stack size: "
                                << FrameStack.size() << "\n");
            } else if (isa<GuardedValueFlowReturnSite>(Obj)) {
              FrameStack.pop();
              LLVM_DEBUG(errs() << "[" << II << "] Leaving "
                                << Obj->getGraph()->getBaseFunc()->getName()
                                << " with frame stack size: "
                                << FrameStack.size() << "\n");

              if (FrameStack.empty()) {
                break;
              }
            }
          }
        }

        // set I be the index of the last ret site
        assert(II != E);
        I = II;
      } else {
        break;
      }
    } else {
      break;
    }
  }

  // produce tips for value flows
  std::shared_ptr<VulnerabilityTrace> SubTrace =
      std::make_shared<VulnerabilityTrace>();
  for (size_t E = Trace->get_length(); I < E; ++I) {
    SubTrace->push(Trace->at(I));
  }

  auto *SubReport = VulnerabilityReportBuilder::buildReport(
      SubTrace, DIA, IResolver, CDGs, Vuln);
  for (auto *step : SubReport->get_steps())
    Report->append_step(new BugDiagStep(*step));
  delete SubReport;

  return Report;
}
} // namespace gsaf
} // namespace lotus
