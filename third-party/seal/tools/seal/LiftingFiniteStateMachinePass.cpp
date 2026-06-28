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
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Format.h>

#include "Core/DomInformationAnalysis.h"
#include "Core/Executor.h"
#include "Core/LoopInformationAnalysis.h"
#include "Core/SliceGraph.h"
#include "Core/SymbolicExecution.h"
#include "Core/SymbolicExecutionTree.h"
#include "LiftingFiniteStateMachinePass.h"
#include "Support/Debug.h"
#include "Support/DL.h"
#include "Support/TimeRecorder.h"
#include "Support/Z3.h"

#define DEBUG_TYPE "LiftingFiniteStateMachinePass"

using namespace llvm;

static cl::opt<std::string> EntryFunctionName("popeye-entry",
                                              cl::desc("specify from which function we start our analysis"),
                                              cl::init("popeye_main"));
static cl::opt<std::string> Dot("popeye-dot",
                                cl::desc("output intermediate graphic code representation"),
                                cl::init(""));

char LiftingFiniteStateMachinePass::ID = 0;
static RegisterPass<LiftingFiniteStateMachinePass> X(DEBUG_TYPE, "The core engine of Popeye.");
extern bool SummarizingFSMSkeleton;

void LiftingFiniteStateMachinePass::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<LoopInformationAnalysis>();
    AU.addRequired<DomInformationAnalysis>();
}

bool LiftingFiniteStateMachinePass::runOnModule(Module &M) {
    DL::initialize(M.getDataLayout());
    Z3::initialize();
    checkBuiltInFunctions(M);

    auto *Entry = M.getFunction(EntryFunctionName.getValue());
    if (!Entry) {
        std::string ErrorMsg("The entry function --- ");
        ErrorMsg.append(EntryFunctionName.getValue());
        ErrorMsg.append(" --- is not found!");
        POPEYE_WARN(ErrorMsg.c_str());

        for (auto &F: M) {
            if (!F.isDeclaration() && F.getName().startswith("popeye_main_")) {
                POPEYE_WARN("\tPossible entry --- " + F.getName().str());
            }
        }
        exit(1);
    }

    TimeRecorder Timer("Lifting the finite state machine");
    {
        TimeRecorder FSMTimer("Step 1: Lifting the FSM skeleton");
        SummarizingFSMSkeleton = true;
        Executor Exe(this);
        Exe.visit(Entry);
    }

    {
        TimeRecorder FSMTimer("Step 2: Lifting the FSM details");
        SummarizingFSMSkeleton = false;
    }

    return false;
}

void LiftingFiniteStateMachinePass::checkBuiltInFunctions(Module &M) {
    /*
     * void *popeye_make_object(uint64_t size);
     * void *popeye_make_named_object(uint64_t size, const char *);
     * void popeye_make_global(void *);
     * void *popeye_make_message(void);
     * uint16_t popeye_make_message_length(void);
     */

    auto *PopeyeMakeObject = M.getFunction("popeye_make_object");
    if (PopeyeMakeObject) {
        assert(PopeyeMakeObject->getReturnType()->isPointerTy());
        assert(PopeyeMakeObject->arg_size() == 1);
        assert(PopeyeMakeObject->getArg(0)->getType()->isIntegerTy());
    }

    auto *PopeyeMakeNamedObject = M.getFunction("popeye_make_named_object");
    if (PopeyeMakeNamedObject) {
        assert(PopeyeMakeNamedObject->getReturnType()->isPointerTy());
        assert(PopeyeMakeNamedObject->arg_size() == 2);
        assert(PopeyeMakeNamedObject->getArg(0)->getType()->isIntegerTy());
        assert(PopeyeMakeNamedObject->getArg(1)->getType()->isPointerTy());
    }

    auto *PopeyeMakeMessage = M.getFunction("popeye_make_message");
    if (PopeyeMakeMessage) {
        assert(PopeyeMakeMessage->getReturnType()->isPointerTy());
        assert(PopeyeMakeMessage->arg_size() == 0);
    }

    auto *PopeyeMakeMessageLength = M.getFunction("popeye_make_message_length");
    if (PopeyeMakeMessageLength) {
        assert(PopeyeMakeMessageLength->getReturnType()->isIntegerTy());
        assert(PopeyeMakeMessageLength->arg_size() == 0);
    }
}
