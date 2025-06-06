//===-- CallGraph.cc - Build global call-graph------------------===//
//
// This pass builds a global call-graph. The targets of an indirect
// call are identified based on various type-based analyses.
//
//===-----------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/CFG.h"

#include "Checker/ESSS/Common.h"
#include "Checker/ESSS/MLTA.h"

#include <map>
#include <vector>


using namespace llvm;


//
// Implementation
//
pair<Type *, int> typeidx_c(Type *Ty, int Idx) {
    return make_pair(Ty, Idx);
}
pair<size_t, int> hashidx_c(size_t Hash, int Idx) {
    return make_pair(Hash, Idx);
}

Argument *getParamByArgNo(Function *F, uint8_t ArgNo) {
    if (ArgNo >= F->arg_size())
        return nullptr;
    return F->getArg(ArgNo);
}

bool MLTA::fuzzyTypeMatch(Type *Ty1, Type *Ty2,
                          Module *M1, Module *M2) {

    if (Ty1 == Ty2)
        return true;

    while (Ty1->isPointerTy() && Ty2->isPointerTy()) {
        Ty1 = Ty1->getPointerElementType();
        Ty2 = Ty2->getPointerElementType();
    }

    if (Ty1->isStructTy() && Ty2->isStructTy() &&
        (Ty1->getStructName().equals(Ty2->getStructName())))
        return true;
    if (Ty1->isIntegerTy() && Ty2->isIntegerTy() &&
        Ty1->getIntegerBitWidth() == Ty2->getIntegerBitWidth())
        return true;
    // TODO: more types to be supported.

    // Make the type analysis conservative: assume general
    // pointers, i.e., "void *" and "char *", are equivalent to
    // any pointer type and integer type.
    if (
            (Ty1 == Int8PtrTy[M1] &&
             (Ty2->isPointerTy() || Ty2 == IntPtrTy[M2]))
            ||
            (Ty2 == Int8PtrTy[M1] &&
             (Ty1->isPointerTy() || Ty1 == IntPtrTy[M2]))
            )
        return true;

    return false;
}


// Find targets of indirect calls based on function-type analysis: as
// long as the number and type of parameters of a function matches
// with the ones of the callsite, we say the function is a possible
// target of this call.
void MLTA::findCalleesWithType(CallInst *CI, FuncSet &S) {

    if (CI->isInlineAsm())
        return;

    //
    // Performance improvement: cache results for types
    //
    size_t CIH = callHash(CI);
    if (MatchedFuncsMap.find(CIH) != MatchedFuncsMap.end()) {
        if (!MatchedFuncsMap[CIH].empty())
            S.insert(MatchedFuncsMap[CIH].begin(),
                     MatchedFuncsMap[CIH].end());
        return;
    }

    CallBase *CB = dyn_cast<CallBase>(CI);
    for (Function *F : AddressTakenFuncs) {
        // VarArg
        if (F->getFunctionType()->isVarArg()) {
            // Compare only known args in VarArg.
        }
            // otherwise, the numbers of args should be equal.
        else if (F->arg_size() != CB->arg_size()) {
            continue;
        }

        if (F->isIntrinsic()) {
            continue;
        }

        // Types completely match
        if (callHash(CI) == funcHash(F)) {
            S.insert(F);
            continue;
        }

        Module *CalleeM = F->getParent();
        Module *CallerM = CI->getFunction()->getParent();

        // Type matching on args.
        bool Matched = true;
        User::op_iterator AI = CB->arg_begin();
        for (Function::arg_iterator FI = F->arg_begin(),
                     FE = F->arg_end();
             FI != FE; ++FI, ++AI) {
            // Check type mis-matches.
            // Get defined type on callee side.
            Type *DefinedTy = FI->getType();
            // Get actual type on caller side.
            Type *ActualTy = (*AI)->getType();

            if (!fuzzyTypeMatch(DefinedTy, ActualTy, CalleeM, CallerM)) {
                Matched = false;
                break;
            }
        }

        // If args are matched, further check return types
        if (Matched) {
            Type *RTy1 = F->getReturnType();
            Type *RTy2 = CI->getType();
            if (!fuzzyTypeMatch(RTy1, RTy2, CalleeM, CallerM)) {
                Matched = false;
            }
        }

        if (Matched) {
            S.insert(F);
        }
    }
    MatchedFuncsMap[CIH] = S;
}


void MLTA::unrollLoops(Function *F) {

    if (F->isDeclaration())
        return;

    DominatorTree DT = DominatorTree();
    DT.recalculate(*F);
    LoopInfo *LI = new LoopInfo();
    LI->releaseMemory();
    LI->analyze(DT);

    // Collect all loops in the function
    set<Loop *> LPSet;
    for (LoopInfo::iterator i = LI->begin(), e = LI->end(); i!=e; ++i) {

        Loop *LP = *i;
        LPSet.insert(LP);

        list<Loop *> LPL;

        LPL.push_back(LP);
        while (!LPL.empty()) {
            LP = LPL.front();
            LPL.pop_front();
            vector<Loop *> SubLPs = LP->getSubLoops();
            for (auto SubLP : SubLPs) {
                LPSet.insert(SubLP);
                LPL.push_back(SubLP);
            }
        }
    }

    for (Loop *LP : LPSet) {

        // Get the header,latch block, exiting block of every loop
        BasicBlock *HeaderB = LP->getHeader();

        SmallVector<BasicBlock *, 4> LatchBS;

        LP->getLoopLatches(LatchBS);

        for (BasicBlock *LatchB : LatchBS) {
            if (!HeaderB || !LatchB) {
                OP<<"ERROR: Cannot find Header Block or Latch Block\n";
                continue;
            }
            // Two cases:
            // 1. Latch Block has only one successor:
            // 	for loop or while loop;
            // 	In this case: set the Successor of Latch Block to the
            //	successor block (out of loop one) of Header block
            // 2. Latch Block has two successor:
            // do-while loop:
            // In this case: set the Successor of Latch Block to the
            //  another successor block of Latch block

            // get the last instruction in the Latch block
            Instruction *TI = LatchB->getTerminator();
            // Case 1:
            if (LatchB->getSingleSuccessor() != NULL) {
                for (succ_iterator sit = succ_begin(HeaderB);
                     sit != succ_end(HeaderB); ++sit) {

                    BasicBlock *SuccB = *sit;
                    BasicBlockEdge BBE = BasicBlockEdge(HeaderB, SuccB);
                    // Header block has two successor,
                    // one edge dominate Latch block;
                    // another does not.
                    if (DT.dominates(BBE, LatchB))
                        continue;
                    else {
                        TI->setSuccessor(0, SuccB);
                    }
                }
            }
                // Case 2:
            else {
                for (succ_iterator sit = succ_begin(LatchB);
                     sit != succ_end(LatchB); ++sit) {

                    BasicBlock *SuccB = *sit;
                    // There will be two successor blocks, one is header
                    // we need successor to be another
                    if (SuccB == HeaderB)
                        continue;
                    else{
                        TI->setSuccessor(0, SuccB);
                    }
                }
            }
        }
    }
}

bool MLTA::isCompositeType(Type *Ty) {
    if (Ty->isStructTy()
        || Ty->isArrayTy()
        || Ty->isVectorTy())
        return true;
    else
        return false;
}

Type *MLTA::getFuncPtrType(Value *V) {
    Type *Ty = V->getType();
    if (PointerType *PTy = dyn_cast<PointerType>(Ty)) {
        Type *ETy = PTy->getPointerElementType();
        if (ETy->isFunctionTy())
            return ETy;
    }

    return NULL;
}

Value *MLTA::recoverBaseType(Value *V) {
    if (Instruction *I = dyn_cast<Instruction>(V)) {
        map<Value *, Value *> &AliasMap
                = AliasStructPtrMap[I->getFunction()];
        if (AliasMap.find(V) != AliasMap.end()) {
            return AliasMap[V];
        }
    }
    return NULL;
}

// This function analyzes globals to collect information about which
// types functions have been assigned to.
// The analysis is field sensitive.
bool MLTA::typeConfineInInitializer(GlobalVariable *GV, const DataLayout* DL) {

    Constant *Ini = GV->getInitializer();
    if (!isa<ConstantAggregate>(Ini))
        return false;

    list<pair<Type *, int>>NestedInit;
    map<Value *, pair<Value *, int>>ContainersMap;
    set<Value *>FuncOperands;
    list<User *>LU;
    set<Value *>Visited;
    LU.push_back(Ini);

    while (!LU.empty()) {
        User *U = LU.front();
        LU.pop_front();
        if (Visited.find(U) != Visited.end()) {
            continue;
        }
        Visited.insert(U);

        Type *UTy = U->getType();
        assert(!UTy->isFunctionTy());

        if (StructType *STy = dyn_cast<StructType>(U->getType())) {
            if (U->getNumOperands() > 0)
                assert(STy->getNumElements() == U->getNumOperands());
            else
                continue;
        }

        for (auto oi = U->op_begin(), oe = U->op_end();
             oi != oe; ++oi) {

            Value *O = *oi;
            Type *OTy = O->getType();

            ContainersMap[O] = make_pair(U, oi->getOperandNo());

            Function *FoundF = NULL;
            // Case 1: function address is assigned to a type
            if (Function *F = dyn_cast<Function>(O)) {
                FoundF = F;
            }
                // Case 2: a composite-type object (value) is assigned to a
                // field of another composite-type object
            else if (isCompositeType(OTy)) {
                // confine composite types
                //Type *ITy = U->getType();
                //int ONo = oi->getOperandNo();

                // recognize nested composite types
                User *OU = dyn_cast<User>(O);
                LU.push_back(OU);
            }
            else if (PtrToIntOperator *PIO = dyn_cast<PtrToIntOperator>(O)) {

                Function *F = dyn_cast<Function>(PIO->getOperand(0));
                if (F)
                    FoundF = F;
                else {
                    User *OU = dyn_cast<User>(PIO->getOperand(0));
                    LU.push_back(OU);
                }
            }
                // now consider if it is a bitcast from a function
                // address
            else if (BitCastOperator *CO = dyn_cast<BitCastOperator>(O)) {
                // Virtual functions will always be cast by
                // inserting the first parameter
                Function *CF = dyn_cast<Function>(CO->getOperand(0));
                if (CF) {
                    Type *ITy = U->getType();
                    // FIXME: Assume this is VTable
                    if (!ITy->isStructTy()) {
                        VTableFuncsMap[GV].insert(CF);
                    }

                    FoundF = CF;
                }
                else {
                    User *OU = dyn_cast<User>(CO->getOperand(0));
                    LU.push_back(OU);
                }
            }
                // Case 3: a reference (i.e., pointer) of a composite-type
                // object is assigned to a field of another composite-type
                // object
            else if (PointerType *POTy = dyn_cast<PointerType>(OTy)) {
                if (isa<ConstantPointerNull>(O))
                    continue;
                // if the pointer points a composite type, conservatively
                // treat it as a type cap (we cannot get the next-layer type
                // if the type is a cap)
                User *OU = dyn_cast<User>(O);
                LU.push_back(OU);
                if (GlobalVariable *GO = dyn_cast<GlobalVariable>(OU)) {
                    Type *Ty = POTy->getPointerElementType();
                    // FIXME: take it as a confinement instead of a cap
                    if (Ty->isStructTy())
                        typeCapSet.insert(typeHash(Ty, DL));
                }
            }
            else {
                // TODO: Type escaping?
            }

            // Found a function
            if (FoundF && !FoundF->isIntrinsic()) {
                // Add the function type to all containers
                Value *CV = O;
                set<Value *>Visited; // to avoid loop
                while (ContainersMap.find(CV) != ContainersMap.end()) {
                    auto Container = ContainersMap[CV];

                    Type *CTy = Container.first->getType();
                    set<size_t> TyHS;
                    //if (StructType *STy = dyn_cast<StructType>(CTy)) {
                    //    structTypeHash(STy, TyHS);
                    //}
                    //else
                        TyHS.insert(typeHash(CTy, DL));

                    //DBG<<"[INSERT-INIT] Container type: "<<*CTy
                    //   <<"; Idx: "<<Container.second
                    //   <<"\n\t --> FUNC: "<<FoundF->getName()<<"; Module: "
                    //   <<FoundF->getParent()->getName()<<"\n";

                    for (auto TyH : TyHS) {
#ifdef MLTA_FIELD_INSENSITIVE
                        typeIdxFuncsMap[TyH][0].insert(FoundF);
#else
                        typeIdxFuncsMap[TyH][Container.second].insert(FoundF);
#endif
                        //DBG<<"[HASH] "<<TyH<<"\n";
                    }

                    Visited.insert(CV);
                    if (Visited.find(Container.first) != Visited.end())
                        break;

                    CV = Container.first;
                }
            }
        }
    }

    return true;
}

// This function analyzes instructions to collect information about
// which types functions have been assigned to.
// The analysis is field sensitive.
bool MLTA::typeConfineInFunction(Function *F, const DataLayout* DL) {

    for (inst_iterator i = inst_begin(F), e = inst_end(F);
         i != e; ++i) {

        Instruction *I = &*i;

        if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
            Value *PO = SI->getPointerOperand();
            Value *VO = SI->getValueOperand();

            Function *CF = getBaseFunction(VO->stripPointerCasts());
            if (!CF)
                continue;
            if (F->isIntrinsic())
                continue;

            confineTargetFunction(PO, CF, DL);
        }
        else if (CallInst *CI = dyn_cast<CallInst>(I)) {
            for (User::op_iterator OI = I->op_begin(),
                         OE = I->op_end();
                 OI != OE; ++OI) {
                if (Function *F = dyn_cast<Function>(*OI)) {
                    if (F->isIntrinsic())
                        continue;
                    if (CI->isIndirectCall()) {
                        confineTargetFunction(*OI, F, DL);
                        continue;
                    }
                    Value *CV = CI->getCalledOperand();
                    Function *CF = dyn_cast<Function>(CV);
                    if (!CF)
                        continue;
                    if (CF->isDeclaration())
                        CF = Ctx->GlobalFuncs[CF->getGUID()];
                    if (!CF)
                        continue;
                    if (Argument *Arg = getParamByArgNo(CF, OI->getOperandNo())) {
                        for (auto U : Arg->users()) {
                            if (isa<StoreInst>(U) || isa<BitCastOperator>(U)) {
                                confineTargetFunction(U, F, DL);
                            }
                        }
                    }
                    // TODO: track into the callee to avoid marking the
                    // function type as a cap
                }
            }
        }
    }

    return true;
}

bool MLTA::typePropInFunction(Function *F, const DataLayout* DL) {

    // Two cases for propagation: store and cast.
    // For store, LLVM may use memcpy
    set<User *>CastSet;
    for (inst_iterator i = inst_begin(F), e = inst_end(F);
         i != e; ++i) {

        Instruction *I = &*i;

        Value *PO = NULL, *VO = NULL;
        if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
            PO = SI->getPointerOperand();
            VO = SI->getValueOperand();
        }
        else if (CallInst *CI = dyn_cast<CallInst>(I)) {
            Value *CV = CI->getCalledOperand();
            Function *CF = dyn_cast<Function>(CV);
            if (CF) {
                // LLVM may optimize struct assignment into a call to
                // intrinsic memcpy
                if (CF->getName() == "llvm.memcpy.p0i8.p0i8.i64") {
                    PO = CI->getOperand(0);
                    VO = CI->getOperand(1);
                }
            }
        }

        if (PO && VO) {
            //
            // TODO: if VO is a global with an initializer, this should be
            // taken as a confinement instead of propagation, which can
            // improve the precision
            //
            if (isa<ConstantAggregate>(VO) || isa<ConstantData>(VO))
                continue;

            list<typeidx_t>TyList;
            Value *NextV = NULL;
            set<Value *> Visited;
            nextLayerBaseType(VO, TyList, NextV, Visited);
            if (!TyList.empty()) {
                for (auto TyIdx : TyList) {
                    propagateType(PO, TyIdx.first, DL, TyIdx.second);
                }
                continue;
            }

            Visited.clear();
            Type *BTy = getBaseType(VO, Visited);
            // Composite type
            if (BTy) {
                propagateType(PO, BTy, DL);
                continue;
            }

            Type *FTy = getFuncPtrType(VO->stripPointerCasts());
            // Function-pointer type
            if (FTy) {
                if (!getBaseFunction(VO)) {
                    propagateType(PO, FTy, DL);
                    continue;
                }
                else
                    continue;
            }

            if (!VO->getType()->isPointerTy())
                continue;
            else {
                // General-pointer type for escaping
                escapeType(PO, DL);
            }

        }


        // Handle casts
        if (CastInst *CastI = dyn_cast<CastInst>(I)) {
            // Record the cast, handle later
            CastSet.insert(CastI);
        }

        // Operands of instructions can be BitCastOperator
        for (User::op_iterator OI = I->op_begin(),
                     OE = I->op_end();
             OI != OE; ++OI) {
            if (BitCastOperator *CO = dyn_cast<BitCastOperator>(*OI)) {
                CastSet.insert(CO);
            }
        }
    }

    for (auto Cast : CastSet) {

        // TODO: we may not need to handle casts as casts are already
        // stripped out in confinement and propagation analysis. Also for
        // a function pointer to propagate, it is supposed to be stored
        // in memory.

        // The conservative escaping policy can be optimized
        Type *FromTy = Cast->getOperand(0)->getType();
        Type *ToTy = Cast->getType();
        if (FromTy->isPointerTy() && ToTy->isPointerTy()) {
            Type *EFromTy = FromTy->getPointerElementType();
            Type *EToTy = ToTy->getPointerElementType();
            if (EFromTy->isStructTy() && EToTy->isStructTy()) {
                //propagateType(Cast, EFromTy, -1);
            }
        }
    }

    return true;
}

// This function precisely collect alias types for general pointers
void MLTA::collectAliasStructPtr(Function *F) {

    map<Value *, Value *> &AliasMap = AliasStructPtrMap[F];
    set<Value *>ToErase;
    for (inst_iterator i = inst_begin(F), e = inst_end(F);
         i != e; ++i) {

        Instruction *I = &*i;

        if (CastInst *CI = dyn_cast<CastInst>(I)) {
            Value *FromV = CI->getOperand(0);
            // TODO: we only consider calls for now
            if (!isa<CallInst>(FromV))
                continue;

            Type *FromTy = FromV->getType();
            Type *ToTy = CI->getType();
            if (Int8PtrTy[F->getParent()] != FromTy)
                continue;

            if (!ToTy->isPointerTy())
                continue;

            if (!isCompositeType(ToTy->getPointerElementType()))
                continue;

            if (AliasMap.find(FromV) != AliasMap.end()) {
                ToErase.insert(FromV);
                continue;
            }
            AliasMap[FromV] = CI;
        }
    }
    for (auto Erase : ToErase)
        AliasMap.erase(Erase);
}


void MLTA::escapeType(Value *V, const DataLayout* DL) {

    list<typeidx_t> TyChain;
    bool Complete = true;
    getBaseTypeChain(TyChain, V, Complete, DL);
    for (auto T : TyChain) {
        //DBG<<"[Escape] Type: "<<*(T.first)<<"; Idx: "<<T.second<<"\n";
        typeEscapeSet.insert(typeIdxHash(T.first, DL, T.second));
    }
}

void MLTA::confineTargetFunction(Value *V, Function *F, const DataLayout* DL) {
    if (F->isIntrinsic())
        return;

    list<typeidx_t> TyChain;
    bool Complete = true;
    getBaseTypeChain(TyChain, V, Complete, DL);
    for (auto TI : TyChain) {
        //DBG<<"[INSERT-FUNC] Container type: "<<*(TI.first)<<"; Idex: "<<TI.second
        //   <<"\n\t --> FUNC:  "<<F->getName()<<"; Module: "
        //   <<F->getParent()->getName()<<"\n";
        //DBG<<"[HASH] "<<typeHash(TI.first)<<"\n";
        typeIdxFuncsMap[typeHash(TI.first, DL)][TI.second].insert(F);
    }
    if (!Complete) {
        if (!TyChain.empty())
            typeCapSet.insert(typeHash(TyChain.back().first, DL));
        else
            typeCapSet.insert(funcHash(F));
    }
}

void MLTA::propagateType(Value *ToV, Type *FromTy, const DataLayout* DL, int Idx) {

    list<typeidx_t> TyChain;
    bool Complete = true;
    getBaseTypeChain(TyChain, ToV, Complete, DL);
    for (auto T : TyChain) {

        if (typeHash(T.first, DL) == typeHash(FromTy, DL) && T.second == Idx)
            continue;

        typeIdxPropMap[typeHash(T.first, DL)]
        [T.second].insert(hashidx_c(typeHash(FromTy, DL), Idx));
        //DBG<<"[PROP] "<<*(FromTy)<<": "<<Idx
        //   <<"\n\t===> "<<*(T.first)<<" "<<T.second<<"\n";
    }
}

void MLTA::intersectFuncSets(FuncSet &FS1, FuncSet &FS2,
                             FuncSet &FS) {
    FS.clear();
    for (auto F : FS1) {
        if (FS2.find(F) != FS2.end())
            FS.insert(F);
    }
}

Value *MLTA::getVTable(Value *V) {
    if (BitCastOperator *BCO =
            dyn_cast<BitCastOperator>(V)) {
        return getVTable(BCO->getOperand(0));
    }
    else if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {
        return getVTable(GEP->getPointerOperand());
    }
    else if (VTableFuncsMap.find(V) != VTableFuncsMap.end())
        return V;
    else
        return NULL;
}

// Get the chain of base types for V
// Complete: whether the chain's end is not escaping---it won't
// propagate further
bool MLTA::getBaseTypeChain(list<typeidx_t> &Chain, Value *V,
                            bool &Complete, const DataLayout* DL) {

    Complete = true;
    Value *CV = V, *NextV = NULL;
    list<typeidx_t> TyList;
    set<Value *>Visited;

    Type *BTy = getBaseType(V, Visited);
    if (BTy) {
        // 0 vs. -1?
        Chain.push_back(typeidx_c(BTy, 0));
    }
    Visited.clear();

    while (nextLayerBaseType(CV, TyList, NextV, Visited)) {
        CV = NextV;
    }
    for (auto TyIdx : TyList) {
        Chain.push_back(typeidx_c(TyIdx.first, TyIdx.second));
    }

    // Checking completeness
    if (!NextV) {
        Complete = false;
    }
    else if (isa<Argument>(NextV) && NextV->getType()->isPointerTy()) {
        Complete = false;
    }
    else {
        for (auto U : NextV->users()) {
            if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
                if (NextV == SI->getPointerOperand()) {
                    Complete = false;
                    break;
                }
            }
        }
        // TODO: other cases like store?
    }

    if (!Chain.empty() && !Complete) {
        typeCapSet.insert(typeHash(Chain.back().first, DL));
    }

    return true;
}

// This function is to get the base type in the current layer.
// To get the type of next layer (with GEP and Load), use
// nextLayerBaseType() instead.
Type *MLTA::getBaseType(Value *V, set<Value *> &Visited) {

    if (!V)
        return NULL;

    if (!Visited.insert(V).second)
        return NULL;

    Type *Ty = V->getType();

    if (isCompositeType(Ty)) {
        return Ty;
    }
        // The value itself is a pointer to a composite type
    else if (Ty->isPointerTy()) {

        Type *ETy = Ty->getPointerElementType();
        if (isCompositeType(ETy)) {
            return ETy;
        }
        else if (Value *BV = recoverBaseType(V))
            return BV->getType()->getPointerElementType();
    }

    if (BitCastOperator *BCO =
            dyn_cast<BitCastOperator>(V)) {
        return getBaseType(BCO->getOperand(0), Visited);
    }
    else if (SelectInst *SelI = dyn_cast<SelectInst>(V)) {
        // Assuming both operands have same type, so pick the first
        // operand
        return getBaseType(SelI->getTrueValue(), Visited);
    }
    else if (PHINode *PN = dyn_cast<PHINode>(V)) {
        // TODO: tracking incoming values
        return _getPhiBaseType(PN, Visited);
    }
    else if (LoadInst *LI = dyn_cast<LoadInst>(V)) {
        return getBaseType(LI->getPointerOperand(), Visited);
    }

    return NULL;
}

Type *MLTA::_getPhiBaseType(PHINode *PN, set<Value *> &Visited) {

    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
        Value *IV = PN->getIncomingValue(i);

        Type *BTy = getBaseType(IV, Visited);
        if (BTy)
            return BTy;
    }

    return NULL;
}

bool MLTA::getGEPLayerTypes(GEPOperator *GEP, list<typeidx_t> &TyList) {

    Value *PO = GEP->getPointerOperand();
    Type *ETy = GEP->getSourceElementType();

    vector<int> Indices;
    list<typeidx_t> TmpTyList;
    // FIXME: handle downcasting: the GEP may get a field outside the
    // base type
    // Or use O0 to avoid this issue
    ConstantInt *ConstI =
            dyn_cast<ConstantInt>(GEP->idx_begin()->get());
    if (ConstI && ConstI->getSExtValue() != 0) {

        //
        // FIXME: The following is an attempt to handle the intentional
        // out-of-bound access; however, it is not fully working, so I
        // skip it for now
        //
        Instruction *I = dyn_cast<Instruction>(PO);
        Value *BV = recoverBaseType(PO);
        if (BV) {
            ETy = BV->getType()->getPointerElementType();
            APInt Offset (ConstI->getBitWidth(),
                          ConstI->getZExtValue());
            Type *BaseTy = ETy;
            SmallVector<APInt>IndiceV = DLMap[I->getModule()]
                    ->getGEPIndicesForOffset(BaseTy, Offset);
            for (auto Idx : IndiceV) {
                Indices.push_back(*Idx.getRawData());
            }
        }
        else if (StructType *STy = dyn_cast<StructType>(ETy)) {
            for (auto User : GEP->users()) {
                if (BitCastOperator *BCO =
                        dyn_cast<BitCastOperator>(User)) {
#ifdef SOUND_MODE
                    // TODO: This conservative decision results may cases
					// disqualifying MLTA. Need an analysis to recover the base
					// types, or use O0 to avoid the optimization
					return false;
#endif
                }
            }
        }
    }

    if (Indices.empty()) {
        for (auto it = GEP->idx_begin(); it != GEP->idx_end(); it++) {
            ConstantInt *ConstI = dyn_cast<ConstantInt>(it->get());
            if (ConstI)
                Indices.push_back(ConstI->getSExtValue());
            else
                Indices.push_back(-1);
        }
    }


    for (auto it = Indices.begin() + 1; it != Indices.end(); it++) {

        int Idx = *it;
#ifdef MLTA_FIELD_INSENSITIVE
        TmpTyList.push_front(typeidx_c(ETy, 0));
#else
        TmpTyList.push_front(typeidx_c(ETy, Idx));
#endif

        // Continue to parse subty
        Type* SubTy = NULL;
        if (StructType *STy = dyn_cast<StructType>(ETy)) {
            SubTy = STy->getElementType(Idx);
        }
        else if (ArrayType *ATy = dyn_cast<ArrayType>(ETy)) {
            SubTy = ATy->getElementType();
        }
        else if (VectorType *VTy = dyn_cast<VectorType>(ETy)) {
            SubTy = VTy->getElementType();
        }
        assert(SubTy);

        ETy = SubTy;
    }
    // This is a trouble caused by compiler optimization that
    // eliminates the access path when the index of a field is 0.
    // Conservatively assume a base-struct pointer can serve as a
    // pointer to its first field
    StructType *STy = dyn_cast<StructType>(ETy);
    if (STy && STy->getNumElements() > 0) {
        // Get the type of its first field
        Type *Ty0 = STy->getElementType(0);
        for (auto U : GEP->users()) {
            if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(U)) {
                if (PointerType *PTy
                        = dyn_cast<PointerType>(BCO->getType())) {

                    Type *ToTy = PTy->getPointerElementType();
                    if (Ty0 == ToTy)
                        TmpTyList.push_front(typeidx_c(ETy, 0));
                }
            }
        }
    }

    if (!TmpTyList.empty()) {
        // Reorder
        for (auto TyIdx : TmpTyList) {
            TyList.push_back(TyIdx);
        }
        return true;
    }
    else
        return false;
}

// Get the composite type of the lower layer. Layers are split by
// memory loads or GEP
bool MLTA::nextLayerBaseType(Value *V, list<typeidx_t> &TyList,
                             Value * &NextV, set<Value *> &Visited) {

    if (!V || isa<Argument>(V)) {
        NextV = V;
        return false;
    }

    if (Visited.find(V) != Visited.end()) {
        NextV = V;
        return false;
    }
    Visited.insert(V);

    // The only way to get the next layer type: GetElementPtrInst or
    // GEPOperator
    if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {

        NextV = GEP->getPointerOperand();
        bool ret = getGEPLayerTypes(GEP, TyList);
        if (!ret)
            NextV = NULL;
        return ret;
    }
    else if (LoadInst *LI = dyn_cast<LoadInst>(V)) {

        NextV = LI->getPointerOperand();
        return nextLayerBaseType(LI->getOperand(0), TyList, NextV, Visited);
    }
    else if (BitCastOperator *BCO =
            dyn_cast<BitCastOperator>(V)) {

        NextV = BCO->getOperand(0);
        return nextLayerBaseType(BCO->getOperand(0), TyList, NextV, Visited);
    }
        // Phi and Select
    else if (PHINode *PN = dyn_cast<PHINode>(V)) {
        // FIXME: tracking incoming values
        bool ret = false;
        set<Value *> NVisited;
        list<typeidx_t> NTyList;
        for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
            Value *IV = PN->getIncomingValue(i);
            NextV = IV;
            NVisited = Visited;
            NTyList = TyList;
            ret = nextLayerBaseType(IV, NTyList, NextV, NVisited);
            if (NTyList.size() > TyList.size()) {
                break;
            }
        }
        TyList = NTyList;
        Visited = NVisited;
        return ret;
    }
    else if (SelectInst *SelI = dyn_cast<SelectInst>(V)) {
        // Assuming both operands have same type, so pick the first
        // operand
        NextV = SelI->getTrueValue();
        return nextLayerBaseType(SelI->getTrueValue(), TyList, NextV, Visited);
    }
        // Other unary instructions
        // FIXME: may introduce false positives
    else if (UnaryOperator *UO = dyn_cast<UnaryOperator>(V)) {

        NextV = UO->getOperand(0);
        return nextLayerBaseType(UO->getOperand(0), TyList, NextV, Visited);
    }

    NextV = NULL;
    return false;
}

bool MLTA::getDependentTypes(Type *Ty, int Idx,
                             set<hashidx_t> &PropSet, const DataLayout* DL) {

    list<hashidx_t>LT;
    LT.push_back(hashidx_c(typeHash(Ty, DL), Idx));
    set<hashidx_t>Visited;

    while (!LT.empty()) {
        hashidx_t TI = LT.front();
        LT.pop_front();
        if (Visited.find(TI) != Visited.end()) {
            continue;
        }
        Visited.insert(TI);

        for (auto Prop : typeIdxPropMap[TI.first][TI.second]) {
            PropSet.insert(Prop);
            LT.push_back(Prop);
        }
        for (auto Prop : typeIdxPropMap[TI.first][-1]) {
            PropSet.insert(Prop);
            LT.push_back(Prop);
        }
    }
    return true;
}


Function *MLTA::getBaseFunction(Value *V) {

    if (Function *F = dyn_cast<Function>(V))
        if (!F->isIntrinsic())
            return F;

    Value *CV = V;
    while (BitCastOperator *BCO
            = dyn_cast<BitCastOperator>(CV)) {
        Value *O = BCO->getOperand(0);
        if (Function *F = dyn_cast<Function>(O))
            if (!F->isIntrinsic())
                return F;
        CV = O;
    }
    return NULL;
}

// Get all possible targets of the given type
bool MLTA::getTargetsWithLayerType(size_t TyHash, int Idx,
                                   FuncSet &FS) {

    // Get the direct funcset in the current layer, which
    // will be further unioned with other targets from type
    // casting
    if (Idx == -1) {
        for (const auto& FSet : typeIdxFuncsMap[TyHash]) {
            FS.insert(FSet.second.begin(), FSet.second.end());
        }
    }
    else {
        FS = typeIdxFuncsMap[TyHash][Idx];
        FS.insert(typeIdxFuncsMap[TyHash][-1].begin(),
                  typeIdxFuncsMap[TyHash][-1].end());
    }

    return true;
}

// The API for MLTA: it returns functions for an indirect call
bool MLTA::findCalleesWithMLTA(CallInst *CI,
                               FuncSet &FS) {

    // Initial set: first-layer results
    // TODO: handling virtual functions
    FS = Ctx->sigFuncsMap[callHash(CI)];

    if (FS.empty()) {
        // No need to go through MLTA if the first layer is empty
        return false;
    }

    FuncSet FS1, FS2;
    Type *PrevLayerTy = (dyn_cast<CallBase>(CI))->getFunctionType();
    Value *CV = CI->getCalledOperand();
    Value *NextV = NULL;
    int LayerNo = 1;

    const DataLayout* DL = DLMap[CI->getModule()];

    // Get the next-layer type
    list<typeidx_t> TyList;
    bool  ContinueNextLayer = true;
    while (ContinueNextLayer) {

        // Check conditions
        if (LayerNo >= MAX_TYPE_LAYER)
            break;

#ifdef SOUND_MODE
        if (typeCapSet.find(typeHash(PrevLayerTy, DL)) != typeCapSet.end()) {
			break;
		}
#endif

        set<Value *> Visited;
        nextLayerBaseType(CV, TyList, NextV, Visited);
        if (TyList.empty()) {
            if (LayerNo == 1) {
                //printSourceCodeInfo(CI, "NOBASE");
            }
            break;
        }

        for (auto TyIdx : TyList) {

            if (LayerNo >= MAX_TYPE_LAYER)
                break;
            ++LayerNo;

            //DBG<<"[CONTAINER] Type: "<<*(TyIdx.first)
            //   <<"; Idx: "<<TyIdx.second<<"\n";
            //DBG<<"[HASH] "<<typeHash(TyIdx.first)<<"\n";

            size_t TyIdxHash = typeIdxHash(TyIdx.first, DL, TyIdx.second);
            // -1 represents all possible fields of a struct
            size_t TyIdxHash_1 = typeIdxHash(TyIdx.first, DL, -1);

            // Caching for performance
            if (MatchedFuncsMap.find(TyIdxHash) != MatchedFuncsMap.end()) {
                FS1 = MatchedFuncsMap[TyIdxHash];
            }
            else {

#ifdef SOUND_MODE
                if (typeEscapeSet.find(TyIdxHash)
						!= typeEscapeSet.end()) {

					break;
				}
				if (typeEscapeSet.find(TyIdxHash_1)
						!= typeEscapeSet.end()) {
					break;
				}
#endif

#if 0
                // If the previous layer propagates to the next layer, no need
				// to continue, as all targets of previous layer are assumed to
				// be propagated to the next layer.
				if (PrevLayerTy) {
					if ((typeIdxPropMap[typeHash(TyIdx.first)]
								[TyIdx.second].find(hashidx_c(typeHash(PrevLayerTy), PrevIdx))
								!= typeIdxPropMap[typeHash(TyIdx.first)]
								[TyIdx.second].end()) ||
							typeIdxPropMap[typeHash(TyIdx.first)]
							[-1].find(hashidx_c(typeHash(PrevLayerTy), PrevIdx))
							!= typeIdxPropMap[typeHash(TyIdx.first)]
							[-1].end()) {
						break;
					}
				}
#endif

                getTargetsWithLayerType(typeHash(TyIdx.first, DL), TyIdx.second, FS1);

                // Collect targets from dependent types that may propagate
                // targets to it
                set<hashidx_t> PropSet;
                getDependentTypes(TyIdx.first, TyIdx.second, PropSet, DL);
                for (auto Prop : PropSet) {
                    getTargetsWithLayerType(Prop.first, Prop.second, FS2);
                    FS1.insert(FS2.begin(), FS2.end());
                }
                MatchedFuncsMap[TyIdxHash] = FS1;
            }

            // Next layer may not always have a subset of the previous layer
            // because of casting, so let's do intersection
            intersectFuncSets(FS1, FS, FS2);
            FS = FS2;

            CV = NextV;

#ifdef SOUND_MODE
            if (typeCapSet.find(typeHash(TyIdx.first, DL)) != typeCapSet.end()) {
				ContinueNextLayer = false;
				break;
			}
#endif

            PrevLayerTy = TyIdx.first;
        }
        TyList.clear();
    }

#if 0
    if (LayerNo > 1) {
        Ctx->NumSecondLayerTypeCalls++;
        Ctx->NumSecondLayerTargets += FS.size();
    }
    else {
        Ctx->NumFirstLayerTargets += Ctx->sigFuncsMap[callHash(CI)].size();
        Ctx->NumFirstLayerTypeCalls += 1;
    }
#endif

#if 0
    FuncSet FSBase = Ctx->sigFuncsMap[callHash(CI)];
	saveCalleesInfo(CI, FSBase, false);
	saveCalleesInfo(CI, FSBase, true);
#endif

    return true;
}
