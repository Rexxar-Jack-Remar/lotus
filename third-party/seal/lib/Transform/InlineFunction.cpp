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

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include "Transform/InlineFunction.h"

#define DEBUG_TYPE "InlineFunction"

char InlineFunction::ID = 0;
static RegisterPass<::InlineFunction> X(DEBUG_TYPE, "Naming each block for dbg");

void InlineFunction::getAnalysisUsage(AnalysisUsage &AU) const {
}

static bool callerInlineCallee(Function *Caller) {
    if (!Caller) return false;
    if (Caller->empty()) return false;

    std::vector<CallInst *> CV;
    for (auto &B : *Caller) {
        for (auto &I : B) {
            if (auto *CI = dyn_cast<CallInst>(&I)) {
                if (CI->getCalledFunction() && !CI->getCalledFunction()->empty()) {
                    CV.push_back(CI);
                }
            }
        }
    }

    for (auto *C : CV) {
        InlineFunctionInfo IFI;
        llvm::InlineFunction(*C, IFI);
    }
    return !CV.empty();
}

static bool calleeInlineCaller(Function *Callee) {
    if (!Callee) return false;
    if (Callee->empty()) return false;

    std::vector<CallInst *> CV;
    for (auto UIt = Callee->user_begin(), E = Callee->user_end(); UIt != E; ++UIt) {
        auto *Inst = dyn_cast<CallInst>(*UIt);
        if (Inst && Inst->getCalledFunction() == Callee) {
            CV.push_back(Inst);
        }
    }

    for (auto *C : CV) {
        InlineFunctionInfo IFI;
        llvm::InlineFunction(*C, IFI);
    }
    return !CV.empty();
}

bool InlineFunction::runOnModule(Module &M) {
    auto *F = M.getFunction("mode_smartrtl_run");
    bool Ret = callerInlineCallee(F);

    F = M.getFunction("mode_rtl_run");
    Ret = callerInlineCallee(F) || Ret;

    F = M.getFunction("mode_zigzag_run");
    Ret = callerInlineCallee(F) || Ret;

    F = M.getFunction("default_mode");
    Ret = calleeInlineCaller(F) || Ret;

    F = M.getFunction("set_default_mode");
    Ret = calleeInlineCaller(F) || Ret;

    F = M.getFunction("return_to_manual_control");
    Ret = calleeInlineCaller(F) || Ret;

    return Ret;
}