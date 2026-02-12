/**
 * @file WPDSUninitializedVariables.cpp
 * @brief Implementation of uninitialized variables analysis using WPDS-based dataflow engine
 * Author: rainoftime
 */

#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/WPDS/Clients/WPDSUninitializedVariables.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using wpds::DataFlowFacts;
using wpds::GenKillTransformer;
using wpds::InterProceduralDataFlowEngine;

static GenKillTransformer *createUninitTransformer(Instruction *I) {

    std::set<Value *> genSet;
    std::set<Value *> killSet;
    std::map<Value*, DataFlowFacts> flowMap;
    
    // Helper to add flow x -> y
    auto addFlow = [&](Value* src, Value* dst) {
        if (!flowMap.count(src)) {
            flowMap[src] = DataFlowFacts::EmptySet();
        }
        flowMap[src].addFact(dst);
    };

    if (auto *AI = dyn_cast<AllocaInst>(I)) {
        // Newly allocated local is uninitialized until stored
        genSet.insert(AI);
    } else if (auto *SI = dyn_cast<StoreInst>(I)) {
        // Store initializes the destination memory
        Value *ptr = SI->getPointerOperand();
        killSet.insert(ptr);
        
        // Also kill aliases? Not easy without full alias analysis.
        // But WPDS flow tracks aliases if we propagated them!
        // If p -> q (q is alias of p).
        // If p is killed, does q get killed?
        // In our Flow model: f(S) = (S \ K) U Flow(S \ K).
        // If p is in K, p is removed. Flow(p) is NOT added.
        // So if flow[p] = {q}, and p is killed, q is NOT regenerated from p.
        // BUT if q was already in S (from previous step), q remains unless q is in K.
        // So to kill q, we must explicitly add q to K.
        // Without reverse flow map, we can't find q from p.
        // So this simple flow model handles "Propagation of Property", not "Update of State".
        // Uninitialized is a property of the *variable holding the address*.
        // If p and q point to same memory. "p is uninit" means "memory at p is uninit".
        // If we store to p, memory becomes init.
        // So both p and q should lose the "uninit" property.
        // This requires killing all aliases.
        // Limitation: We only kill the pointer operand.
        
    } else if (auto *CI = dyn_cast<CallBase>(I)) {
        // Assume function call initializes passed pointers (safe approximation)
        for (auto &Arg : CI->args()) {
            if (Arg->getType()->isPointerTy()) {
                killSet.insert(Arg.get());
            }
        }
    } else if (auto *BC = dyn_cast<BitCastInst>(I)) {
        // p2 = bitcast p1
        // If p1 uninit, p2 uninit
        addFlow(BC->getOperand(0), BC);
    } else if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
        // p2 = gep p1
        addFlow(GEP->getPointerOperand(), GEP);
    } else if (auto *PHI = dyn_cast<PHINode>(I)) {
        for (Value* inc : PHI->incoming_values()) {
            addFlow(inc, PHI);
        }
    } else if (auto *SEL = dyn_cast<SelectInst>(I)) {
        addFlow(SEL->getTrueValue(), SEL);
        addFlow(SEL->getFalseValue(), SEL);
    }

    DataFlowFacts gen(genSet);
    DataFlowFacts kill(killSet);
    return GenKillTransformer::makeGenKillTransformer(kill, gen, flowMap);
}

void demoUninitializedVariablesAnalysis(Module &module) {
    InterProceduralDataFlowEngine engine;
    std::set<Value *> initial; // start with empty fact set
    auto result = engine.runForwardAnalysis(module, createUninitTransformer, initial);

    // Report: a load of a possibly uninitialized location
    for (auto &F : module) {
        if (F.isDeclaration()) continue;
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Value *ptr = LI->getPointerOperand();
                    const std::set<Value *> &in = result->IN(&I);
                    
                    // Check if ptr or any source of ptr is in IN set
                    if (in.count(ptr)) {
                        errs() << "[WPDS][Uninit] Potentially uninitialized read at: ";
                        if (!I.getFunction()->getName().empty()) {
                            errs() << I.getFunction()->getName();
                            errs() << ": ";
                        }
                        if (!I.getName().empty()) errs() << I.getName();
                        else errs() << "<unnamed-inst>";
                        errs() << " (Pointer: " << ptr->getName() << ")\n";
                    }
                }
            }
        }
    }
}

std::unique_ptr<mono::DataFlowResult> runUninitializedVariablesAnalysis(Module &module) {
    InterProceduralDataFlowEngine engine;
    std::set<Value *> initial;
    return engine.runForwardAnalysis(module, createUninitTransformer, initial);
}

static void printValueSet(raw_ostream &OS, const std::set<Value *> &S) {
    OS << "{";
    bool first = true;
    for (auto *V : S) {
        if (!first) OS << ", ";
        first = false;
        if (auto *I = dyn_cast<Instruction>(V)) {
            if (!I->getName().empty()) OS << I->getName();
            else OS << "<inst>";
        } else if (auto *A = dyn_cast<Argument>(V)) {
            if (!A->getName().empty()) OS << A->getName();
            else OS << "<arg>";
        } else if (auto *G = dyn_cast<GlobalValue>(V)) {
            OS << G->getName();
        } else {
            OS << "<val>";
        }
    }
    OS << "}";
}

void queryAnalysisResults(Module &module, const mono::DataFlowResult &result, Instruction *targetInst) {
    (void)module;
    if (!targetInst) return;
    auto *itF = targetInst->getFunction();
    (void)itF;
    errs() << "[WPDS][Query] IN  = ";
    printValueSet(errs(), const_cast<mono::DataFlowResult&>(result).IN(targetInst));
    errs() << "\n";
    errs() << "[WPDS][Query] GEN = ";
    printValueSet(errs(), const_cast<mono::DataFlowResult&>(result).GEN(targetInst));
    errs() << "\n";
    errs() << "[WPDS][Query] KILL= ";
    printValueSet(errs(), const_cast<mono::DataFlowResult&>(result).KILL(targetInst));
    errs() << "\n";
    errs() << "[WPDS][Query] OUT = ";
    printValueSet(errs(), const_cast<mono::DataFlowResult&>(result).OUT(targetInst));
    errs() << "\n";
}
