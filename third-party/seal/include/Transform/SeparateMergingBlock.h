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


#ifndef TRANSFORM_SEPARATEMERGINGBLOCK_H
#define TRANSFORM_SEPARATEMERGINGBLOCK_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

using namespace llvm;

class SeparateMergingBlock : public FunctionPass {
public:
    static char ID;

    SeparateMergingBlock() : FunctionPass(ID) {}

    ~SeparateMergingBlock() override = default;

    void getAnalysisUsage(AnalysisUsage &) const override;

    bool runOnFunction(Function &) override;
};


#endif //TRANSFORM_SEPARATEMERGINGBLOCK_H
