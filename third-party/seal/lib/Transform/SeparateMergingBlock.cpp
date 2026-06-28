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

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>
#include "Transform/SeparateMergingBlock.h"

#define DEBUG_TYPE "SeparateMergingBlock"

char SeparateMergingBlock::ID = 0;
static RegisterPass<SeparateMergingBlock> X(DEBUG_TYPE, "Each merging block only has two preds");

static void transform(BasicBlock &B) {
    std::vector<BasicBlock *> PredVec;
    std::vector<PHINode *> PhiVec;
    std::vector<std::vector<Value *>> PhiIncomingValueVec;
    for (auto &I: B) {
        if (!isa<PHINode>(I)) break;
        auto *Phi = (PHINode *) &I;
        assert(Phi->getNumIncomingValues() == pred_size(&B));
        if (PredVec.empty()) {
            // keep the order the same as phi incoming blocks
            for (auto K = 0; K < Phi->getNumIncomingValues(); ++K) {
                PredVec.push_back(Phi->getIncomingBlock(K));
            }
        } else {
            for (auto K = 0; K < Phi->getNumIncomingValues(); ++K) {
                assert(Phi->getIncomingBlock(K) == PredVec[K]);
            }
        }
        PhiVec.push_back(Phi);
        PhiIncomingValueVec.emplace_back();
        for (unsigned K = 0; K < Phi->getNumIncomingValues(); ++K) {
            PhiIncomingValueVec.back().emplace_back(Phi->getIncomingValue(K));
        }
    }

    if (PredVec.empty()) {
        for (auto It = pred_begin(&B), E = pred_end(&B); It != E; ++It) {
            PredVec.push_back(*It);
        }
    }

    for (unsigned K = PredVec.size(); K > 2; --K) {
        auto B1 = PredVec[K - 1];
        auto B2 = PredVec[K - 2];

        // create a successor for B1 and B2
        auto *NB = BasicBlock::Create(B.getContext(), "", B.getParent());

        for (auto &PhiValues: PhiIncomingValueVec) {
            auto *Phi = PHINode::Create(PhiValues[0]->getType(), 2, "", NB);
            Phi->addIncoming(PhiValues[K - 1], B1);
            Phi->addIncoming(PhiValues[K - 2], B2);

            PhiValues.pop_back();
            PhiValues.pop_back();
            PhiValues.push_back(Phi);
        }

        // B1 -> NB & B2 -> NB
        auto B1Term = B1->getTerminator();
        for (unsigned J = 0; J < B1Term->getNumSuccessors(); ++J) {
            if (B1Term->getSuccessor(J) == &B) {
                B1Term->setSuccessor(J, NB);
                break;
            }
        }
        auto B2Term = B2->getTerminator();
        for (unsigned J = 0; J < B2Term->getNumSuccessors(); ++J) {
            if (B2Term->getSuccessor(J) == &B) {
                B2Term->setSuccessor(J, NB);
                break;
            }
        }

        // NB -> B
        BranchInst::Create(&B, NB);
        PredVec.pop_back();
        PredVec.pop_back();
        PredVec.push_back(NB);
    }

    assert(PredVec.size() == 2);
    for (unsigned J = 0; J < PhiVec.size(); ++J) {
        auto *Phi = PhiVec[J];
        assert(Phi->getIncomingBlock(0) == PredVec[0]);
        assert(Phi->getIncomingValue(0) == PhiIncomingValueVec[J][0]);
        Phi->setIncomingValue(1, PhiIncomingValueVec[J].back());
        Phi->setIncomingBlock(1, PredVec.back());
        while (Phi->getNumIncomingValues() > 2) {
            Phi->removeIncomingValue(Phi->getNumIncomingValues() - 1);
        }
    }
}

static bool transform(Function &F) {
    std::vector<BasicBlock *> BV;
    for (auto &B: F) {
        if (pred_size(&B) > 2)
            BV.push_back(&B);
    }
    for (auto *B: BV) {
        transform(*B);
    }
    return !BV.empty();
}

static bool transform2(Function &F) {
    std::vector<BasicBlock *> BV;
    for (auto &B: F) {
        if (B.getTerminator()->getNumSuccessors() >= 2)
            BV.push_back(&B);
    }
    for (auto *B : BV) {
        auto NumSucc = B->getTerminator()->getNumSuccessors();
        for (unsigned K = 0; K < NumSucc; ++K) {
            auto *OrigSucc = B->getTerminator()->getSuccessor(K);
            auto *NB = BasicBlock::Create(F.getContext(), "", &F);
            BranchInst::Create(OrigSucc, NB);
            B->getTerminator()->setSuccessor(K, NB);

            for (auto &I : *OrigSucc) {
                if (auto* Phi = dyn_cast<PHINode>(&I)) {
                    for (unsigned Op = 0, NumOps = Phi->getNumOperands(); Op != NumOps; ++Op)
                        if (Phi->getIncomingBlock(Op) == B) {
                            Phi->setIncomingBlock(Op, NB);
                            break;
                        }
                } else {
                    break;
                }
            }
        }
    }
    return !BV.empty();
}

void SeparateMergingBlock::getAnalysisUsage(AnalysisUsage &AU) const {
}

bool SeparateMergingBlock::runOnFunction(Function &F) {
    if (F.empty()) return false;

    auto Changed1 = transform(F);
    auto Changed2 = transform2(F);
    if ((Changed1 || Changed2) && verifyFunction(F, &errs())) {
        llvm_unreachable("Error: SeparateMergingBlock fails...");
    }
    return Changed1 || Changed2;
}