/*
 *  Author: rainoftime
 *  Date: 2025-04
 *  Description: Context-sensitive null flow analysis
 */


#include "Alias/DyckAA/DyckValueFlowAnalysis.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Analysis/NullPointer/API.h"
#include "Analysis/NullPointer/AliasAnalysisAdapter.h"
#include "Analysis/NullPointer/ContextSensitiveNullFlowAnalysis.h"
#include "Utils/LLVM/RecursiveTimer.h"
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<int> CSIncrementalLimits("csnfa-limit", cl::init(10), cl::Hidden,
                                      cl::desc("Determine how many non-null edges we consider a round in context-sensitive analysis."));

static cl::opt<unsigned> CSMaxContextDepth("csnfa-max-depth", cl::init(3), cl::Hidden,
                                         cl::desc("Maximum depth of calling context to consider."));

static cl::opt<unsigned> CSRound("csnfa-round", cl::init(10), cl::Hidden,
                               cl::desc("Maximum rounds for context-sensitive analysis."));

// DyckAA is now the only alias analysis option

char ContextSensitiveNullFlowAnalysis::ID = 0;
static RegisterPass<ContextSensitiveNullFlowAnalysis> X("csnfa", "context-sensitive null value flow");

ContextSensitiveNullFlowAnalysis::ContextSensitiveNullFlowAnalysis() 
    : ModulePass(ID), AAA(nullptr), DAA(nullptr), VFG(nullptr), MaxContextDepth(CSMaxContextDepth),
      OwnsAliasAnalysisAdapter(false) {
}

ContextSensitiveNullFlowAnalysis::~ContextSensitiveNullFlowAnalysis() {
    if (OwnsAliasAnalysisAdapter && AAA) {
        delete AAA;
    }
}

void ContextSensitiveNullFlowAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<DyckValueFlowAnalysis>();
    AU.addRequired<DyckAliasAnalysis>();
}

bool ContextSensitiveNullFlowAnalysis::runOnModule(Module &M) {
    RecursiveTimer Timer("Running Context-Sensitive NFA");
    
    // Get the value flow graph
    auto *VFA = &getAnalysis<DyckValueFlowAnalysis>();
    VFG = VFA->getDyckVFGraph();
    
    // Get DyckAliasAnalysis and create the adapter
    auto *DyckAA = &getAnalysis<DyckAliasAnalysis>();
    DAA = DyckAA;
    AAA = AliasAnalysisAdapter::createAdapter(&M, DyckAA);
    OwnsAliasAnalysisAdapter = true;

    // Initialize the basic context (empty context)
    Context EmptyContext;
    
    // Helper function to identify values that must not be null
    auto MustNotNull = [this](Value *V, Instruction *I) -> bool {
        V = V->stripPointerCastsAndAliases();
        if (isa<GlobalValue>(V)) return true;
        if (auto* CI = dyn_cast<Instruction>(V))
            return API::isMemoryAllocate(CI);
        return !AAA->mayNull(V, I);
    };
    
    // Initialize the analysis for each function
    std::set<DyckVFGNode *> MayNullNodes;
    for (auto &F: M) {
        if (!F.empty()) {
            NewNonNullEdges[{&F, EmptyContext}];
            NonNullEdges[{&F, EmptyContext}];
            NonNullNodes[{&F, EmptyContext}];
        }
        for (auto &I: instructions(&F)) {
            if (I.getType()->isPointerTy() && !MustNotNull(&I, &I)) {
                if (auto *INode = VFG->getVFGNode(&I)) {
                    MayNullNodes.insert(INode);
                }
            }
        }
    }
    
    // Perform context-sensitive analysis
    std::set<std::pair<Function*, Context>> WorkList;
    for (auto &F: M) {
        if (!F.empty()) {
            WorkList.insert({&F, EmptyContext});
        }
    }
    
    // Keep analyzing until we reach a fixed point
    while (!WorkList.empty()) {
        auto FuncCtx = *WorkList.begin();
        WorkList.erase(WorkList.begin());
        
        Function *F = FuncCtx.first;
        Context &Ctx = FuncCtx.second;
        
        // Process the function in this context
        // This part depends on the specific algorithm of your null flow analysis
        errs() << "Processing function " << F->getName() << " with context " 
               << getContextString(Ctx) << "\n";
        
        // Check all call sites in this function
        for (auto &I : instructions(F)) {
            if (auto *CI = dyn_cast<CallInst>(&I)) {
                auto *Callee = CI->getCalledFunction();
                if (!Callee || Callee->empty()) continue;
                
                // If we haven't reached max context depth, create a new context
                if (Ctx.size() < MaxContextDepth) {
                    Context NewCtx = extendContext(Ctx, CI);
                    
                    // Add the callee with the new context to the worklist
                    auto FuncCtxPair = std::make_pair(Callee, NewCtx);
                    if (NewNonNullEdges.find(FuncCtxPair) == NewNonNullEdges.end()) {
                        NewNonNullEdges[FuncCtxPair] = {};
                        NonNullEdges[FuncCtxPair] = {};
                        NonNullNodes[FuncCtxPair] = {};
                        WorkList.insert(FuncCtxPair);
                    }
                }
            }
        }
    }
    
    return false;
}

bool ContextSensitiveNullFlowAnalysis::recompute(std::set<std::pair<Function*, Context>> &NewNonNullFunctionContexts) {
    std::unordered_map<FunctionContextPair, std::set<DyckVFGNode *>> PossibleNonNullNodes;
    unsigned K = 0,
             Limits = CSIncrementalLimits < 0 ? UINT32_MAX : CSIncrementalLimits;
    for (auto &NIt : NewNonNullEdges) {
        auto &EdgeSet = NIt.second;
        auto &NNNodes = NonNullNodes[NIt.first];
        auto &NNEdges = NonNullEdges[NIt.first];
        auto EIt = EdgeSet.begin();
        while (EIt != EdgeSet.end()) {
            if (++K > Limits)
                break;
            auto *Src = EIt->first;
            auto *Tgt = EIt->second;
            assert(Src && Tgt);
            if (!NNNodes.count(Tgt))
                PossibleNonNullNodes[NIt.first].insert(Tgt);
            NNEdges.emplace(Src, Tgt);
            EIt = EdgeSet.erase(EIt);
        }
    }
    if (PossibleNonNullNodes.empty())
        return false;

    bool Changed = false;
    for (auto &Entry : PossibleNonNullNodes) {
        const FunctionContextPair &FuncCtx = Entry.first;
        auto &NNNodes = NonNullNodes[FuncCtx];
        auto &NNEdges = NonNullEdges[FuncCtx];

        std::vector<DyckVFGNode *> WorkList;
        WorkList.reserve(Entry.second.size());
        for (auto *N : Entry.second)
            WorkList.push_back(N);

        while (!WorkList.empty()) {
            auto *N = WorkList.back();
            WorkList.pop_back();
            if (!NNNodes.count(N)) {
                bool AllInNonNull = true;
                for (auto IIt = N->in_begin(), IE = N->in_end(); IIt != IE; ++IIt) {
                    auto *In = IIt->first;
                    if (!NNEdges.count(std::make_pair(In, N))) {
                        AllInNonNull = false;
                        break;
                    }
                }
                if (!AllInNonNull)
                    continue;
                NNNodes.insert(N);
                Changed = true;
                if (auto *NF = N->getFunction()) {
                    if (NF == FuncCtx.first) {
                        NewNonNullFunctionContexts.insert(FuncCtx);
                    } else {
                        NewNonNullFunctionContexts.insert({NF, FuncCtx.second});
                    }
                }
            }
            for (auto &T : *N)
                WorkList.push_back(T.first);
        }
    }

    return Changed;
}

bool ContextSensitiveNullFlowAnalysis::notNull(Value *Ptr, Context Ctx) const {
    if (!Ptr || !Ptr->getType()->isPointerTy())
        return false;
        
    // First check if the pointer is known to be non-null
    Ptr = Ptr->stripPointerCastsAndAliases();
    if (isa<GlobalValue>(Ptr)) return true;
    if (auto *I = dyn_cast<Instruction>(Ptr)) {
        if (API::isMemoryAllocate(I)) return true;
    }
    
    // Then check our context-sensitive analysis results
    Function *F = nullptr;
    Instruction *InstPoint = nullptr;
    if (auto *I = dyn_cast<Instruction>(Ptr)) {
        F = I->getFunction();
        InstPoint = I;
    } else {
        // If it's not an instruction, we need a more conservative approach
        return false;
    }

    DyckVFGNode *Node = VFG ? VFG->getVFGNode(Ptr) : nullptr;
    
    // Get all contexts that have the same k-suffix as our input context
    std::set<Context> MatchingContexts;
    
    // Start with the exact context
    MatchingContexts.insert(Ctx);
    
    // Get the k-suffix of our context
    Context KSuffix = Ctx;
    if (KSuffix.size() > MaxContextDepth) {
        KSuffix.erase(KSuffix.begin(), KSuffix.begin() + (KSuffix.size() - MaxContextDepth));
    }
    
    // Add all contexts that have the same k-suffix
    for (const auto &Entry : NonNullNodes) {
        if (Entry.first.first != F) continue;
        
        const Context &OtherCtx = Entry.first.second;
        Context OtherKSuffix = OtherCtx;
        
        if (OtherKSuffix.size() > MaxContextDepth) {
            OtherKSuffix.erase(OtherKSuffix.begin(), OtherKSuffix.begin() + (OtherKSuffix.size() - MaxContextDepth));
        }
        
        // If this context has the same k-suffix, add it to our matching contexts
        if (OtherKSuffix == KSuffix) {
            MatchingContexts.insert(OtherCtx);
        }
    }
    
    // For a value to be definitely NOT NULL, it must be NOT NULL in ALL matching contexts
    // This is the sound approach for k-limiting
    for (const Context &MatchingCtx : MatchingContexts) {
        auto FuncCtxPair = std::make_pair(F, MatchingCtx);
        auto it = NonNullNodes.find(FuncCtxPair);

        if (it == NonNullNodes.end()) {
            // If we don't have analysis for this context, we can't guarantee NOT_NULL
            return false;
        }

        // Check if this pointer is NOT NULL in this context
        bool IsNotNullInContext = false;

        if (Node && it->second.count(Node)) {
            IsNotNullInContext = true;
        }

        // Fall back to alias analysis if flow facts are unavailable
        if (!IsNotNullInContext && InstPoint && AAA && !AAA->mayNull(Ptr, InstPoint)) {
            IsNotNullInContext = true;
        }

        if (!IsNotNullInContext) {
            // If it's not definitely NOT NULL in any matching context, we can't guarantee NOT_NULL
            return false;
        }
    }
    
    // If we get here, the pointer is NOT NULL in all matching contexts
    return true;
}

void ContextSensitiveNullFlowAnalysis::add(Function *F, Context Ctx, Value *V1, Value *V2) {
    if (!V1 || !V1->getType()->isPointerTy())
        return;

    if (!V2) {
        void (ContextSensitiveNullFlowAnalysis::*AddSingle)(Function *, Context, Value *) =
            &ContextSensitiveNullFlowAnalysis::add;
        (this->*AddSingle)(F, Ctx, V1);
        return;
    }

    auto *V1N = VFG ? VFG->getVFGNode(V1) : nullptr;
    if (!V1N)
        return;
    auto *V2N = VFG->getVFGNode(V2);
    if (!V2N)
        return;

    auto FuncCtxPair = std::make_pair(F, Ctx);
    NonNullEdges[FuncCtxPair];
    NonNullNodes[FuncCtxPair];
    NewNonNullEdges[FuncCtxPair].emplace(V1N, V2N);
}

void ContextSensitiveNullFlowAnalysis::add(Function *F, Context Ctx, CallInst *CI, unsigned int K) {
    if (!CI) return;

    if (!DAA)
        return;

    auto *DCG = DAA->getDyckCallGraph();
    if (!DCG)
        return;
    auto *DCGNode = DCG->getFunction(F);
    if (!DCGNode)
        return;
    auto *C = DCGNode->getCall(CI);
    if (!C)
        return;
    auto *Actual = CI->getArgOperand(K);

    auto AddToCallee = [this, &Ctx, CI, Actual, K](Function *Callee) {
        if (!Callee)
            return;
        if (K >= Callee->arg_size()) {
            assert(Callee->isVarArg());
            return;
        }
        if (Ctx.size() >= MaxContextDepth)
            return;
        Context NewCtx = extendContext(Ctx, CI);
        add(Callee, NewCtx, Actual, Callee->getArg(K));
    };

    if (auto *CC = dyn_cast<CommonCall>(C)) {
        AddToCallee(CC->getCalledFunction());
    } else if (auto *PC = dyn_cast<PointerCall>(C)) {
        for (auto *Callee : *PC)
            AddToCallee(Callee);
    }
}

void ContextSensitiveNullFlowAnalysis::add(Function *F, Context Ctx, Value *Ret) {
    if (!Ret || !Ret->getType()->isPointerTy())
        return;

    auto *RetN = VFG ? VFG->getVFGNode(Ret) : nullptr;
    if (!RetN)
        return;

    auto FuncCtxPair = std::make_pair(F, Ctx);
    NonNullEdges[FuncCtxPair];
    NonNullNodes[FuncCtxPair];
    auto &Set = NewNonNullEdges[FuncCtxPair];
    for (auto &TargetIt : *RetN)
        Set.emplace(RetN, TargetIt.first);
}

std::string ContextSensitiveNullFlowAnalysis::getContextString(const Context& Ctx) const {
    std::string Result = "[";
    for (size_t i = 0; i < Ctx.size(); ++i) {
        if (i > 0) Result += ", ";
        if (Ctx[i]->hasName()) {
            Result += Ctx[i]->getName().str();
        } else {
            Result += "<unnamed call>";
        }
    }
    Result += "]";
    return Result;
}

Context ContextSensitiveNullFlowAnalysis::extendContext(const Context& Ctx, CallInst* CI) const {
    // Create a new context by appending the call instruction
    Context NewCtx = Ctx;
    NewCtx.push_back(CI);
    
    // Note: We don't limit the context here anymore - we'll handle k-limiting
    // at analysis time to ensure soundness by properly merging results
    return NewCtx;
} 
