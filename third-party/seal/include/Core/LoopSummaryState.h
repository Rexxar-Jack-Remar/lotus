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


#ifndef CORE_LOOPSUMMARYSTATE_H
#define CORE_LOOPSUMMARYSTATE_H

#include "Core/ExecutionState.h"
#include "Core/LoopInformationAnalysis.h"
#include "Memory/AbstractValue.h"

#include <map>
#include <vector>

struct LoopIteration;
typedef std::shared_ptr<LoopIteration> LoopIterationRef;

struct LoopIteration {
    z3::expr TripCount;
    z3::expr MinIndex;
    z3::expr MaxIndex;
    z3::expr Sugar;

    unsigned MinMergeID = UINT32_MAX;
    unsigned MaxMergeID = 0;

    /// bytes loaded in an iteration
    std::set<z3::expr, Z3::less_than> LoadedBytes;

    /// revised register values
    std::map<Instruction *, AbstractValue *> RegisterValues;

    /// revised memory values after an exiting/latch block
    std::map<std::pair<BasicBlock *, AbstractValue *>, AbstractValue *> MemoryValues;

    /// pc after an exiting/latch block
    std::map<BasicBlock *, z3::expr> PathConditions;

    /// phi conditions
    std::map<unsigned, z3::expr_vector> PhiConditions;

    /// conditional values used in branch/switch instructions
    std::map<Value *, z3::expr> ConditionalValues;

    LoopIteration(unsigned TC) : TripCount(Z3::bv_val(TC, 32)), MinIndex(Z3::bool_val(true)),
                                 MaxIndex(MinIndex), Sugar(MinIndex) {}

    LoopIteration(const z3::expr &TC) : TripCount(TC), MinIndex(Z3::bool_val(true)),
                                        MaxIndex(MinIndex), Sugar(MinIndex) {}

    ~LoopIteration() {
        for (auto &It: RegisterValues) delete It.second;
        for (auto &It: MemoryValues) delete It.second;
    }

    AbstractValue *reg(Instruction *I) const {
        auto It = RegisterValues.find(I);
        if (It == RegisterValues.end()) return nullptr;
        return It->second;
    }

    AbstractValue *mem(const std::pair<BasicBlock *, AbstractValue *> &K) const {
        auto It = MemoryValues.find(K);
        if (It == MemoryValues.end()) return nullptr;
        return It->second;
    }

    z3::expr pc(BasicBlock *B) const {
        auto It = PathConditions.find(B);
        assert(It != PathConditions.end());
        return It->second;
    }

    z3::expr_vector phi(unsigned PhiID) const {
        auto It = PhiConditions.find(PhiID);
        assert(It != PhiConditions.end());
        return It->second;
    }

    z3::expr condition(BasicBlock *B, unsigned K) const;
};

/// each abstract state stands for a (merged) path in the loop
class LoopSummaryState {
private:
    /// the owner
    SingleLoop *CurrLoop;

    /// the path this abstract state represents, it must start with the loop header and end with the loop latch
    std::set<BasicBlock *> Path;

    /// @{
    unsigned LoopAnalysisID = 0;
    /// @}

    /// the latest post-conditions of this state
    ExecutionState *PostConditions = nullptr;

    /// a loop summary state consists of a single summarized iteration or multiple concrete iterations
    std::vector<LoopIterationRef> Iterations;

    /// model state transition
    /// @{
    std::map<LoopSummaryState *, std::pair<z3::expr, z3::expr>> NextStates;
    std::set<LoopSummaryState *> PrevStates;
    /// @}

    /// just use it to distinguish states, for debugging purposes
    unsigned ID;

public:
    LoopSummaryState(SingleLoop *L, unsigned ID);

    ~LoopSummaryState();

    /// collect information from the execution of each loop iteration
    /// @{
    void update(unsigned TripCount, Instruction *Val, AbstractValue *AbsVal);

    void update(unsigned TripCount, BasicBlock *Block, AbstractValue *Val, AbstractValue *AbsVal);

    void update(unsigned TripCount, BasicBlock *Block, const z3::expr &PC);

    void update(unsigned TripCount, const z3::expr &ByteLoaded);

    void update(unsigned TripCount, Value *Cond, const z3::expr &CondExpr);

    void update(unsigned TripCount, unsigned MergeID);

    void update(BasicBlock *B);
    /// @}

    /// summarize a new state, i.e., merge a new state into this one
    /// return 0 - no summarization was performed, just collect info of a loop iteration
    /// return 1 - just summarized all concrete states
    /// return 2 - has been included in some summarized states
    /// return 3 - cannot summarize
    unsigned summarize(LoopSummaryState *);

private:
    /// return true if summarization succeeds
    /// @{
    void summarize();

    void summarizeBase();

    void summarizeTrip();
    /// @}

    void repairPath();

public:
    /// the state machine
    friend class LoopSummaryStateMachine;

    /// the summarization class
    friend class LoopSummaryAnalysis;

    friend raw_ostream &operator<<(llvm::raw_ostream &, const LoopSummaryState &);

    friend void intersect(const LoopSummaryState &S1,
                          const LoopSummaryState &S2,
                          std::set<BasicBlock *> &P1,
                          std::set<BasicBlock *> &Common,
                          std::set<BasicBlock *> &P2);

    friend void subtract(const LoopSummaryState &S, std::set<BasicBlock *> &P, std::set<BasicBlock *> &R);
};

raw_ostream &operator<<(llvm::raw_ostream &, const LoopSummaryState &);

void intersect(const LoopSummaryState &S1,
               const LoopSummaryState &S2,
               std::set<BasicBlock *> &P1,
               std::set<BasicBlock *> &Common,
               std::set<BasicBlock *> &P2);

void subtract(const LoopSummaryState &S, std::set<BasicBlock *> &P, std::set<BasicBlock *> &R);

#endif //CORE_LOOPSUMMARYSTATE_H
