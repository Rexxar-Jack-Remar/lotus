/*
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MD5.h>
#include "Core/LoopSummaryStateMachine.h"
#include "Support/Debug.h"
#include "Support/PushPop.h"

#define DEBUG_TYPE "LoopSummaryStateMachine"

static cl::opt<std::string> DotFSM("popeye-dot-fsm",
                                   cl::desc("output intermediate graphic code representation"),
                                   cl::init(""));
static cl::opt<bool> DotFSMCond("popeye-dot-fsm-conditions",
                                    cl::desc("output transition conditions"),
                                    cl::init(false));
static cl::opt<bool> SplitHeuristic("popeye-enable-split",
                                    cl::desc("using split heuristic"),
                                    cl::init(true));

bool SummarizingFSMSkeleton = false;

raw_ostream &operator<<(raw_ostream &O, const LoopSummaryStateMachine &FSM) {
    unsigned K = 0;
    for (auto *S: FSM) {
        O << "<state " << K << ">\n";
        O << *S << "\n";
        O << "</state " << K << ">\n";
        O << "^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^";
        K++;
    }
    return O;
}

LoopSummaryState *LoopSummaryStateMachine::newState(unsigned ID) {
    auto *Ret = new LoopSummaryState(CurrLoop, ID);
    AllStates.push_back(Ret);
    return Ret;
}

LoopSummaryState *LoopSummaryStateMachine::recentNewState() const {
    return AllStates.back();
}

void LoopSummaryStateMachine::summarizeRecentNewState() {
    if (SummarizingFSMSkeleton) {
        summarizeRecentNewStateSkeleton();
        return;
    }

    unsigned SummarizationResult = 0;
    auto *NewState = recentNewState();
    assert(NewState && NewState == AllStates.back());
    if (AllStates.size() == 1) {
        // this is the first state, we do not need to do anything
    } else if (AllStates.size() == 2) {
        auto *PrevState = AllStates[0];
        if (NewState->Path == PrevState->Path) {
            SummarizationResult = PrevState->summarize(NewState);
            AllStates.pop_back();
            delete NewState;
        } else {
            POPEYE_WARN("[Loop] A different state is discovered, not supported yet!");
            SummarizationResult = 3;
        }
    } else {
        POPEYE_WARN("[Loop] Multi-state FSM discovered, not supported yet!");
        SummarizationResult = 3;
    }

    // we have summarized a new state, test if we finish summarizing or fail to summarize
    switch (SummarizationResult) {
        case 0:
        case 1:
            assert(FSMStatus == LSSM_Summarizing);
            break;
        case 2:
            FSMStatus = LSSM_Summarized;
            break;
        case 3:
            FSMStatus = LSSM_Fail2Summarize;
            break;
        default:
            llvm_unreachable("Error: unknown summarizing status!");
    }
}

void LoopSummaryStateMachine::replace(unsigned int K, LoopSummaryState *S1, LoopSummaryState *S2) {
//    auto *StateK = AllStates[K];
//
//    // we will rerun from S1 and S2, so removing self cycle does not affect correctness
//    StateK->NextStates.erase(StateK);
//    StateK->PrevStates.erase(StateK);
//
//    // the previous states of StateK, still go to S1 and S2
//    for (auto *Prev: StateK->PrevStates) {
//        auto It = Prev->NextStates.find(StateK);
//        assert(It != Prev->NextStates.end());
//        Prev->NextStates.erase(It);
//
//        auto Cond1 = precondition(*Prev, *S1, *StateK);
//        auto Cond2 = precondition(*Prev, *S2, *StateK);
//        Prev->NextStates.insert(std::make_pair(S1, Cond1));
//        Prev->NextStates.insert(std::make_pair(S2, Cond2));
//    }
//
//    // we do not connect S1 and S2 to StateK's next states
//    // the next states of S1 and S2 will be recomputed
//    for (auto &Next: StateK->NextStates) {
//        Next.first->PrevStates.erase(StateK);
//    }
//    assert(S1->NextStates.empty());
//    assert(S2->NextStates.empty());
//    CandidateRunningState.insert(S1);
//    CandidateRunningState.insert(S2);
//    if (CandidateRunningState.count(StateK)) CandidateRunningState.erase(StateK);
//
//    if (RunningState.count(StateK)) {
//        RunningState.erase(StateK);
//        RunningState.insert(S1);
//        RunningState.insert(S2);
//    }
//    AllStates[K] = S1;
//    AllStates.push_back(S2);
//    delete StateK;
}

static void find(const z3::expr &E, PushPopVector<std::pair<unsigned, unsigned>> &P,
                 std::vector<std::vector<unsigned>> &R) {
    P.push();
    if (Z3::is_phi(E)) {
        auto PhiID = Z3::phi_id(E);
        for (unsigned K = 0; K < E.num_args(); ++K) {
            P.push();
            P.add(std::make_pair(PhiID, K));
            find(E.arg(K), P, R);
            P.pop();
        }
    } else if (E.is_false()) {
        assert(!P.empty());
        R.emplace_back();
        for (auto &Pair: P.get()) {
            R.back().push_back(Pair.first);
            R.back().push_back(Pair.second);
        }
    } else {
        auto Vec = Z3::find_consecutive_ops(E, Z3_OP_AND);
        assert(!Vec.empty());
        for (auto V: Vec) {
            if (!Z3::is_phi(V)) continue;
            find(V, P, R);
        }
    }
    P.pop();
}

static void getPhiSelector(const z3::expr &E, std::vector<std::vector<unsigned>> &PhiSelectors) {
    auto Vec = Z3::find_consecutive_ops(E, Z3_OP_AND);
    assert(!Vec.empty());
    PushPopVector<std::pair<unsigned, unsigned>> P;
    for (auto V: Vec) {
        if (!Z3::is_phi(V)) continue;
        find(V, P, PhiSelectors);
        assert(P.empty());
    }
    LLVM_DEBUG(
            dbgs() << "[refine] " << E << "\n[refine] \t";
            for (auto &X: PhiSelectors) {
                for (unsigned K: X) {
                    dbgs() << K << ", ";
                }
                dbgs() << "\n[refine] \t";
            }
            dbgs() << "end\n";
    );
}

static bool relatedToInput(const z3::expr &C) {
    if (C.is_const() && Z3::to_string(C).find("in") != std::string::npos) {
        return true;
    } else {
        for (unsigned K = 0; K < C.num_args(); ++K) {
            if (relatedToInput(C.arg(K))) return true;
        }
    }
    return false;
}

static void split(unsigned P2SIndex, BasicBlock *B, std::vector<std::set<BasicBlock *>> &R,
                  BasicBlock *Header, BasicBlock *Latch) {
    auto *P2S = &R[P2SIndex];
    std::vector<unsigned> SuccIndexVec;
    for (auto K = 0; K < B->getTerminator()->getNumSuccessors(); ++K) {
        auto Succ = B->getTerminator()->getSuccessor(K);
        if (P2S->count(Succ))
            SuccIndexVec.push_back(K);
    }
    assert(SuccIndexVec.size() > 1);

    std::set<BasicBlock *> ResultPath;
    for (unsigned I = 0; I < SuccIndexVec.size(); ++I) {
        P2S = &R[P2SIndex];

        unsigned SuccIndex = SuccIndexVec[I];
        std::vector<BasicBlock *> DFSStack;
        DFSStack.push_back(Header);
        while (!DFSStack.empty()) {
            auto *Top = DFSStack.back();
            DFSStack.pop_back();
            if (ResultPath.count(Top)) continue;
            ResultPath.insert(Top);

            if (Top == B) {
                auto Succ = Top->getTerminator()->getSuccessor(SuccIndex);
                DFSStack.push_back(Succ);
            } else {
                // the successors of this block does not distinguish the two states
                for (auto It = succ_begin(Top), E = succ_end(Top); It != E; ++It) {
                    auto *Succ = *It;
                    if (P2S->count(Succ)) {
                    } else {
                        continue;
                    }
                    DFSStack.push_back(Succ);
                }
            }
        }
        assert(ResultPath.count(Header));
        assert(ResultPath.count(Latch));
        assert (ResultPath.size() != P2S->size());

        if (I == SuccIndexVec.size() - 1) {
            R[P2SIndex] = std::move(ResultPath);
        } else {
            R.push_back(std::move(ResultPath));
        }
    }
}

bool LoopSummaryStateMachine::split(LoopSummaryState *S) {
    if (!SplitHeuristic.getValue()) return false;

    std::vector<std::set<BasicBlock *>> PathVector;

    std::vector<BasicBlock *> KeyBlocks;
    auto &SuperPath = S->Path;
    for (auto *B: SuperPath) {
        auto BTerm = B->getTerminator();
        auto Num = 0;
        for (auto K = 0; K < BTerm->getNumSuccessors(); ++K) {
            auto Succ = BTerm->getSuccessor(K);
            if (SuperPath.count(Succ)) ++Num;
        }
        if (Num <= 1) continue;
        Value *ConditionalVal = nullptr;
        if (isa<BranchInst>(BTerm)) ConditionalVal = BTerm->getOperand(0);
        else if (isa<SwitchInst>(BTerm)) ConditionalVal = BTerm->getOperand(0);
        if (!ConditionalVal) continue;

        auto CondAbsVal = S->PostConditions->boundValue(ConditionalVal);
        if (!isa<ScalarValue>(CondAbsVal)) continue;
        if (CondAbsVal->poison()) continue;

        auto CondExpr = CondAbsVal->value();
        if (!relatedToInput(CondExpr)) {
            LLVM_DEBUG(dbgs() << "[split] key blocks " << CondExpr << "\n");
            LLVM_DEBUG(dbgs() << "[split] key blocks " << B->getName() << "\n");
            KeyBlocks.push_back(B);
        }
    }

    PathVector.push_back(SuperPath);
    while (!KeyBlocks.empty()) {
        auto *KeyB = KeyBlocks.back();
        KeyBlocks.pop_back();

        auto NumP = PathVector.size();
        for (unsigned J = 0; J < NumP; ++J) {
            auto &P = PathVector[J];
            auto BTerm = KeyB->getTerminator();
            auto Num = 0;
            for (auto K = 0; K < BTerm->getNumSuccessors(); ++K) {
                auto Succ = BTerm->getSuccessor(K);
                if (P.count(Succ)) ++Num;
            }

            if (Num > 1) {
                ::split(J, KeyB, PathVector, CurrLoop->getHeader(), CurrLoop->getLatch());
            }
        }
    }

    if (PathVector.size() <= 1)
        return false;
    for (auto &P: PathVector) {
        LLVM_DEBUG(
                dbgs() << "[split] ";
                for (auto *X: P) dbgs() << X->getName() << ", ";
                dbgs() << "\n"
        );
        addCandidateRunningState(RunningState, P);
    }
    return true;
}

void LoopSummaryStateMachine::summarizeRecentNewStateSkeleton() {
    auto *NewState = recentNewState();
    NewState->repairPath();
    assert(NewState && NewState == AllStates.back());
    LLVM_DEBUG(dbgs() << "[FSM] new state (" << NewState->ID << "): \n[FSM]\t");
    LLVM_DEBUG(for (auto *B: NewState->Path) dbgs() << B->getName() << ", ");
    LLVM_DEBUG(dbgs() << "\n");
    AllStates.pop_back(); // temporarily remove NewState

    start:
    if (NewState->Path.empty()) {
        delete NewState;
        NewState = nullptr;
    }

    // split(NewState):
    //  check path in new state, if there is a branch has multiple successor,
    //  and the branch condition is not related to input, we should consider split
    //  the new state to two
    if (!NewState) {
        LLVM_DEBUG(dbgs() << "[FSM] << failed to follow the guidance path\n");
    } else if (!split(NewState)) {
        std::vector<std::vector<unsigned>> PhiSelector;
        auto LatchPC = NewState->Iterations.back()->pc(CurrLoop->getLatch());
        getPhiSelector(LatchPC, PhiSelector);
        NewState->PostConditions->refine(PhiSelector);
        auto NewLatchPC = NewState->PostConditions->refine(LatchPC, PhiSelector);
        NewState->Iterations.back()->PathConditions.erase(CurrLoop->getLatch());
        NewState->Iterations.back()->PathConditions.insert(std::make_pair(CurrLoop->getLatch(), NewLatchPC));
        assert(RunningState);
        auto NewRunningStateLatchPC = Z3::bool_val(true);
        if (!RunningState->Path.empty()) {
            auto RunningStateLatchPC = RunningState->Iterations.back()->pc(CurrLoop->getLatch());
            NewRunningStateLatchPC = NewState->PostConditions->refine(RunningStateLatchPC, PhiSelector);
        } else {
            auto RunningStateLatchPC = RunningState->PostConditions->pc();
            NewRunningStateLatchPC = NewState->PostConditions->refine(RunningStateLatchPC, PhiSelector);
        }
        if (NewRunningStateLatchPC.is_false() || NewLatchPC.is_false()) {
            NewState->Path.clear();
            goto start;
        }
        auto TransitionCondition = std::make_pair(NewRunningStateLatchPC, NewLatchPC);

        std::set<BasicBlock *> P1, Common, P2;
        bool BrandNew = true;
        for (unsigned K = 1; K < AllStates.size(); ++K) {
            auto *StateK = AllStates[K];
            intersect(*NewState, *StateK, P1, Common, P2);
            if (BrandNew && !Common.empty()) BrandNew = false;

            if (NewState->Path == StateK->Path) {
                assert(P1.empty() && P2.empty());
                assert(Common.size() == NewState->Path.size() && Common.size() == StateK->Path.size());
                LLVM_DEBUG(dbgs() << "[FSM] same state found!\n");

                // RunningState -> StateK
                RunningState->NextStates.insert(std::make_pair(StateK, TransitionCondition));
                StateK->PrevStates.insert(RunningState);

                // NewState is equiv to an existing state, not used, so delete it
                delete NewState;
                NewState = nullptr;
                break;
            }
        }
        if (BrandNew) {
            assert(NewState);
            // RunningState -> State,
            RunningState->NextStates.insert(std::make_pair(NewState, TransitionCondition));
            NewState->PrevStates.insert(RunningState);

            // add the brand-new state to AllStates and CandidateRunningState
            AllStates.push_back(NewState);
            assert(NewState->NextStates.empty());
            addCandidateRunningState(NewState, {});
        } else if (NewState) {
            // not a brand-new state, not an existing state

            // construct the split map
            std::map<LoopSummaryState *, std::vector<std::set<BasicBlock *>>> SplitMap;
            for (unsigned K = 1; K < AllStates.size(); ++K) {
                auto *StateK = AllStates[K];

                intersect(*NewState, *StateK, P1, Common, P2);
                if (Common.empty()) continue; // no intersection, no split needed
                assert(Common.size() != NewState->Path.size() || Common.size() != StateK->Path.size());

                if (Common.size() != NewState->Path.size()) {
                    SplitMap[NewState].push_back(Common);
                }

                if (Common.size() != StateK->Path.size()) {
                    SplitMap[StateK].push_back(Common);
                    SplitMap[StateK].push_back(P2);

                    // remove state k from AllStates
                    AllStates[K] = AllStates.back();
                    AllStates.pop_back();
                    K--;
                }
            }

            auto It = SplitMap.find(NewState);
            if (It != SplitMap.end()) {
                P1.clear();
                for (auto &X: It->second) {
                    P1.insert(X.begin(), X.end());
                }
                if (P1.size() < NewState->Path.size()) {
                    It->second.emplace_back();
                    subtract(*NewState, P1, It->second.back());
                }
            }

            // add pred of state in SplitMap into CandidateRunningState
            for (auto &SIt: SplitMap) {
                auto *State2Split = SIt.first;
                if (State2Split == NewState) {
                    // this is just to keep a double-linked list structure
                    // the condition to insert is not important, just use true
                    NewState->PrevStates.insert(RunningState);
                    auto True = Z3::bool_val(true);
                    RunningState->NextStates.insert(std::make_pair(NewState, std::make_pair(True, True)));
                }

                for (auto &NIt: State2Split->NextStates) {
                    NIt.first->PrevStates.erase(State2Split);
                }
                State2Split->NextStates.clear();

                State2Split->PrevStates.erase(State2Split);
                for (auto *P: State2Split->PrevStates) {
                    if (SplitMap.count(P)) continue;
                    for (auto &Path: SIt.second) {
                        addCandidateRunningState(P, Path);
                    }
                    P->NextStates.erase(State2Split);
                }
            }

            // remove state in SplitMap
            for (auto &SIt: SplitMap) {
                auto CIt = CandidateRunningState.find(SIt.first);
                if (CIt != CandidateRunningState.end())
                    CandidateRunningState.erase(CIt);
                delete SIt.first;
            }
        }
    }

    LLVM_DEBUG(
            dbgs() << "[FSM] candidate running states for the next round:";
            dbgs() << " (num = " << CandidateRunningState.size() << ")\n";
            for (auto &It: CandidateRunningState) {
                auto *Curr = It.first;
                for (auto &Path: It.second) {
                    dbgs() << "[FSM] \tstate (" << Curr->ID << ") : ^^^  ";
                    for (auto *B: Curr->Path) dbgs() << B->getName() << ", ";
                    dbgs() << "  --->  ";
                    if (Path.empty()) {
                        dbgs() << "any  ";
                    } else {
                        for (auto *B: Path) dbgs() << B->getName() << ", ";
                    }
                    dbgs() << "\n";
                }
            }
    );

    if (CandidateRunningState.empty()) {
        // if the recent new state is consumed by existing states
        FSMStatus = LSSM_Summarized;
        dot();
        exit(0);
    } else {
        // update running state for next round
        auto It = CandidateRunningState.begin();
        RunningState = It->first;
        RunningPath = std::move(It->second.back());
        It->second.pop_back();
        if (It->second.empty()) CandidateRunningState.erase(It);
        LLVM_DEBUG(
                dbgs() << "[FSM] running state for the next round: \n[FSM]\t state (" << RunningState->ID << ") ";
                for (auto *B: RunningState->Path) dbgs() << B->getName() << ", ";
                dbgs() << "\n";
                if (!RunningState->Path.empty())
                    dbgs() << "[FSM]\t" << RunningState->Iterations.back()->pc(CurrLoop->getLatch()) << "\n";
                dbgs() << "[FSM] \tguidance: ";
                if (RunningPath.empty()) {
                    dbgs() << "empty\n";
                } else {
                    for (auto *B: RunningPath) dbgs() << B->getName() << ", ";
                    dbgs() << "\n";
                }
        );
    }

    if (AllStates.size() > 20) {
        // if too many states, we stop analyzing
        POPEYE_WARN("[Loop] FSM contains too many states, not supported yet!");
        FSMStatus = LSSM_Fail2Summarize;
        return;
    }
}

static std::set<std::string> AddedCandidateStates;

void LoopSummaryStateMachine::addCandidateRunningState(LoopSummaryState *S, const std::set<BasicBlock *> &P) {
    if (!S->Iterations.empty() && S->Iterations.back()->pc(CurrLoop->getLatch()).is_false()) {
        return;
    }

    MD5 Hash;
    for (auto *B : S->Path) {
        Hash.update(B->getName());
    }
    Hash.update("-->");
    for (auto *B : P) {
        Hash.update(B->getName());
    }
    MD5::MD5Result Res;
    Hash.final(Res);
    auto Digest = Res.digest().str().str();
    if (AddedCandidateStates.count(Digest)) return;
    AddedCandidateStates.insert(Digest);

    auto It = CandidateRunningState.find(S);
    if (It == CandidateRunningState.end()) {
        CandidateRunningState[S].push_back(P);
        return;
    }

    // todo, we may avoid duplicate in advance
    It->second.push_back(P);
}

void LoopSummaryStateMachine::dot() const {
    if (DotFSM.getValue().empty()) return;

    StringRef FileName(DotFSM.getValue());
    std::error_code EC;
    raw_fd_ostream DotStream(FileName, EC, sys::fs::OF_None);
    if (DotStream.has_error()) {
        errs() << "[Error] Cannot open the file <" << FileName << "> for writing.\n";
        return;
    }

    std::vector<std::pair<z3::expr, z3::expr>> ConditionVec;

    DotStream << "digraph fsm {\n";
    // dot fsm
    for (auto *State: AllStates) {
        if (State->Path.empty()) {
            auto StateStr = " [label=\"\", shape=\"circle\", style=\"filled\", fillcolor=\"black\"];";
            DotStream << "state_" << State->ID << StateStr << "\n\n";
        }
        for (auto &Ch: State->NextStates) {
            DotStream << "\tstate_" << State->ID << "->state_" << Ch.first->ID
                      << " [ label=\""
                      << "\\[" << ConditionVec.size() << "\\]"
                      << "\"];\n\n";
            ConditionVec.push_back(Ch.second);
        }
    }

    // dot conditions
    if (DotFSMCond.getValue()) {
        DotStream << "condition[shape=record label = \"{";
        for (unsigned C = 0; C < ConditionVec.size(); ++C) {
            DotStream << C << " | " << C;
            if (C != ConditionVec.size() - 1) {
                DotStream << " | ";
            }
        }
        DotStream << "} | \\\n {\\\n";
        for (unsigned C = 0; C < ConditionVec.size(); ++C) {
            DotStream << ConditionVec[C].first << " \\l \\\n";
            DotStream << " | ";
            DotStream << ConditionVec[C].second << " \\l \\\n";
            if (C != ConditionVec.size() - 1) {
                DotStream << " | ";
            }
        }
        DotStream << "}\" ];\n";
    }

    // end of graph body
    DotStream << "}\n";
    POPEYE_INFO(FileName << " dotted!");
}
