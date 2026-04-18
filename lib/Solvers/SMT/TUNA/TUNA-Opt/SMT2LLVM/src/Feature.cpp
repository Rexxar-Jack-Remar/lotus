#include "Feature.h"

void analyzeFunction(Function &F, std::string inputFilename, std::string featureDir) {
    int numBBWithPhiArgsGt5 = 0;
    int numBBWithPhiArgs1to5 = 0;
    int numBBWith1Pred = 0;
    int numBBWith1Pred1Succ = 0;
    int numBBWith1Pred2Succ = 0;
    int numBBWith1Succ = 0;
    int numBBWith2Pred = 0;
    int numBBWith2Pred1Succ = 0;
    int numBBWith2Pred2Succ = 0;
    int numBBWith2Succ = 0;
    int numBBWithMoreThan2Pred = 0;
    int numBBWithPhiInRange0to3 = 0;
    int numBBWithMoreThan3Phi = 0;
    int numBBWithNoPhi = 0;
    int totalBranches = 0;
    int numCallsReturningInt = 0;
    int totalEdges = 0;
    int num32BitConstants = 0;
    int num64BitConstants = 0;
    int numConstZero = 0;
    int numConstOne = 0;
    int numUnconditionalBranches = 0;
    int numBinaryOpsWithConst = 0;
    int numAShrInsts = 0;
    int numAddInsts = 0;
    int numAllocaInsts = 0;
    int numAndInsts = 0;
    int numBBWithInsts15to500 = 0;
    int numBBWithLessThan15Insts = 0;
    int numBitCastInsts = 0;
    int numBrInsts = 0;
    int numCallInsts = 0;
    int numGetElementPtrInsts = 0;
    int numICmpInsts = 0;
    int numLShrInsts = 0;
    int numLoadInsts = 0;
    int numMulInsts = 0;
    int numOrInsts = 0;
    int numPhiInsts = 0;
    int numRetInsts = 0;
    int numSExtInsts = 0;
    int numSelectInsts = 0;
    int numShlInsts = 0;
    int numStoreInsts = 0;
    int numSubInsts = 0;
    int numTruncInsts = 0;
    int numXorInsts = 0;
    int numZExtInsts = 0;
    int totalBasicBlocks = 0;
    int totalInstructions = 0;
    int numMemoryInstructions = 0;
    int numNonExternalFunctions = 0;
    int totalArgsToPhiNodes = 0;
    int numUnaryOps = 0;

    for (auto &BB : F) {
        totalBasicBlocks++;
        int phiArgs = 0;
        int phiCount = 0;
        int numInstsInBB = 0;

        for (auto &I : BB) {
            totalInstructions++;
            numInstsInBB++;
            if (auto *phi = dyn_cast<PHINode>(&I)) {
                phiArgs += phi->getNumIncomingValues();
                phiCount++;
            }
            if (auto *call = dyn_cast<CallInst>(&I)) {
                if (call->getType()->isIntegerTy())
                    numCallsReturningInt++;
                numCallInsts++;
            }
            if (auto *br = dyn_cast<BranchInst>(&I)) {
                numBrInsts++;
                if (br->isUnconditional())
                    numUnconditionalBranches++;
            }
            if (auto *binOp = dyn_cast<BinaryOperator>(&I)) {
                if (isa<ConstantInt>(binOp->getOperand(1)))
                    numBinaryOpsWithConst++;
                switch (binOp->getOpcode()) {
                    case Instruction::Add: numAddInsts++; break;
                    case Instruction::Sub: numSubInsts++; break;
                    case Instruction::Mul: numMulInsts++; break;
                    case Instruction::And: numAndInsts++; break;
                    case Instruction::Or: numOrInsts++; break;
                    case Instruction::Xor: numXorInsts++; break;
                    case Instruction::AShr: numAShrInsts++; break;
                    case Instruction::LShr: numLShrInsts++; break;
                    case Instruction::Shl: numShlInsts++; break;
                }
            }
            if (auto *allocaInst = dyn_cast<AllocaInst>(&I)) numAllocaInsts++;
            if (auto *bitCastInst = dyn_cast<BitCastInst>(&I)) numBitCastInsts++;
            if (auto *icmpInst = dyn_cast<ICmpInst>(&I)) numICmpInsts++;
            if (auto *loadInst = dyn_cast<LoadInst>(&I)) numLoadInsts++;
            if (auto *storeInst = dyn_cast<StoreInst>(&I)) numStoreInsts++;
            if (auto *getElementPtrInst = dyn_cast<GetElementPtrInst>(&I)) numGetElementPtrInsts++;
            if (auto *retInst = dyn_cast<ReturnInst>(&I)) numRetInsts++;
            if (auto *sExtInst = dyn_cast<SExtInst>(&I)) numSExtInsts++;
            if (auto *zExtInst = dyn_cast<ZExtInst>(&I)) numZExtInsts++;
            if (auto *truncInst = dyn_cast<TruncInst>(&I)) numTruncInsts++;
            if (auto *selectInst = dyn_cast<SelectInst>(&I)) numSelectInsts++;
            if (auto *constantInt = dyn_cast<ConstantInt>(&I)) {
                if (constantInt->getBitWidth() == 32) num32BitConstants++;
                if (constantInt->getBitWidth() == 64) num64BitConstants++;
                if (constantInt->isZero()) numConstZero++;
                if (constantInt->isOne()) numConstOne++;
            }
            if (isa<UnaryInstruction>(&I)) numUnaryOps++;
            if (isa<LoadInst>(&I) || isa<StoreInst>(&I)) numMemoryInstructions++;
        }

        totalArgsToPhiNodes += phiArgs;

        if (phiArgs > 5) numBBWithPhiArgsGt5++;
        else if (phiArgs > 0) numBBWithPhiArgs1to5++;

        if (phiCount > 3) numBBWithMoreThan3Phi++;
        else if (phiCount > 0) numBBWithPhiInRange0to3++;
        else numBBWithNoPhi++;

        if (numInstsInBB >= 15 && numInstsInBB <= 500) numBBWithInsts15to500++;
        if (numInstsInBB < 15) numBBWithLessThan15Insts++;

        int numPreds = std::distance(pred_begin(&BB), pred_end(&BB));
        int numSuccs = std::distance(succ_begin(&BB), succ_end(&BB));

        if (numPreds == 1) {
            numBBWith1Pred++;
            if (numSuccs == 1) numBBWith1Pred1Succ++;
            if (numSuccs == 2) numBBWith1Pred2Succ++;
        }
        if (numSuccs == 1) numBBWith1Succ++;
        if (numPreds == 2) {
            numBBWith2Pred++;
            if (numSuccs == 1) numBBWith2Pred1Succ++;
            if (numSuccs == 2) numBBWith2Pred2Succ++;
        }
        if (numSuccs == 2) numBBWith2Succ++;
        if (numPreds > 2) numBBWithMoreThan2Pred++;

        totalEdges += numSuccs;

        if (numSuccs > 1 && numPreds > 1) {
            totalBranches++;
        }
    }

    numNonExternalFunctions++;

    int sum = numBBWithPhiArgsGt5 + numBBWithPhiArgs1to5 + numBBWith1Pred + numBBWith1Pred1Succ + numBBWith1Pred2Succ +
          numBBWith1Succ + numBBWith2Pred + numBBWith2Pred1Succ + numBBWith2Pred2Succ + numBBWith2Succ +
          numBBWithMoreThan2Pred + numBBWithPhiInRange0to3 + numBBWithMoreThan3Phi + numBBWithNoPhi + totalBranches +
          numCallsReturningInt + totalEdges + num32BitConstants + num64BitConstants + numConstZero + numConstOne +
          numUnconditionalBranches + numBinaryOpsWithConst + numAShrInsts + numAddInsts + numAllocaInsts + numAndInsts +
          numBBWithInsts15to500 + numBBWithLessThan15Insts + numBitCastInsts + numBrInsts + numCallInsts +
          numGetElementPtrInsts + numICmpInsts + numLShrInsts + numLoadInsts + numMulInsts + numOrInsts + numPhiInsts +
          numRetInsts + numSExtInsts + numSelectInsts + numShlInsts + numStoreInsts + numSubInsts + numTruncInsts +
          numXorInsts + numZExtInsts + totalBasicBlocks + totalInstructions + numMemoryInstructions +
          numNonExternalFunctions + totalArgsToPhiNodes + numUnaryOps;
    char *outputFilename = nullptr;
    // Print collected features
    if (featureDir == "std"){
        std::cout << inputFilename << ",";
        std::cout << static_cast<float>(numBBWithPhiArgsGt5) / sum << ",";
        std::cout << static_cast<float>(numBBWithPhiArgs1to5) / sum << ",";
        std::cout << static_cast<float>(numBBWith1Pred) / sum << ",";
        std::cout << static_cast<float>(numBBWith1Pred1Succ) / sum << ",";
        std::cout << static_cast<float>(numBBWith1Pred2Succ) / sum << ",";
        std::cout << static_cast<float>(numBBWith1Succ) / sum << ",";
        std::cout << static_cast<float>(numBBWith2Pred) / sum << ",";
        std::cout << static_cast<float>(numBBWith2Pred1Succ) / sum << ",";
        std::cout << static_cast<float>(numBBWith2Pred2Succ) / sum << ",";
        std::cout << static_cast<float>(numBBWith2Succ) / sum << ",";
        std::cout << static_cast<float>(numBBWithMoreThan2Pred) / sum << ",";
        std::cout << static_cast<float>(numBBWithPhiInRange0to3) / sum << ",";
        std::cout << static_cast<float>(numBBWithMoreThan3Phi) / sum << ",";
        std::cout << static_cast<float>(numBBWithNoPhi) / sum << ",";
        std::cout << static_cast<float>(totalBranches) / sum << ",";
        std::cout << static_cast<float>(numCallsReturningInt) / sum << ",";
        std::cout << static_cast<float>(totalEdges) / sum << ",";
        std::cout << static_cast<float>(num32BitConstants) / sum << ",";
        std::cout << static_cast<float>(num64BitConstants) / sum << ",";
        std::cout << static_cast<float>(numConstZero) / sum << ",";
        std::cout << static_cast<float>(numConstOne) / sum << ",";
        std::cout << static_cast<float>(numUnconditionalBranches) / sum << ",";
        std::cout << static_cast<float>(numBinaryOpsWithConst) / sum << ",";
        std::cout << static_cast<float>(numAShrInsts) / sum << ",";
        std::cout << static_cast<float>(numAddInsts) / sum << ",";
        std::cout << static_cast<float>(numAllocaInsts) / sum << ",";
        std::cout << static_cast<float>(numAndInsts) / sum << ",";
        std::cout << static_cast<float>(numBBWithInsts15to500) / sum << ",";
        std::cout << static_cast<float>(numBBWithLessThan15Insts) / sum << ",";
        std::cout << static_cast<float>(numBitCastInsts) / sum << ",";
        std::cout << static_cast<float>(numBrInsts) / sum << ",";
        std::cout << static_cast<float>(numCallInsts) / sum << ",";
        std::cout << static_cast<float>(numGetElementPtrInsts) / sum << ",";
        std::cout << static_cast<float>(numICmpInsts) / sum << ",";
        std::cout << static_cast<float>(numLShrInsts) / sum << ",";
        std::cout << static_cast<float>(numLoadInsts) / sum << ",";
        std::cout << static_cast<float>(numMulInsts) / sum << ",";
        std::cout << static_cast<float>(numOrInsts) / sum << ",";
        std::cout << static_cast<float>(numPhiInsts) / sum << ",";
        std::cout << static_cast<float>(numRetInsts) / sum << ",";
        std::cout << static_cast<float>(numSExtInsts) / sum << ",";
        std::cout << static_cast<float>(numSelectInsts) / sum << ",";
        std::cout << static_cast<float>(numShlInsts) / sum << ",";
        std::cout << static_cast<float>(numStoreInsts) / sum << ",";
        std::cout << static_cast<float>(numSubInsts) / sum << ",";
        std::cout << static_cast<float>(numTruncInsts) / sum << ",";
        std::cout << static_cast<float>(numXorInsts) / sum << ",";
        std::cout << static_cast<float>(numZExtInsts) / sum << ",";
        std::cout << static_cast<float>(totalBasicBlocks) / sum << "\n";
    } else {
        std::ofstream outputFile( featureDir + "feature.csv", std::ios::app);
        if (!outputFile) {
            std::cerr << "Failed to open file: " << featureDir + "feature.csv" << std::endl;
            return;
        }
        outputFile << inputFilename << ",";
        outputFile << static_cast<float>(numBBWithPhiArgsGt5) / sum << ",";
        outputFile << static_cast<float>(numBBWithPhiArgs1to5) / sum << ",";
        outputFile << static_cast<float>(numBBWith1Pred) / sum << ",";
        outputFile << static_cast<float>(numBBWith1Pred1Succ) / sum << ",";
        outputFile << static_cast<float>(numBBWith1Pred2Succ) / sum << ",";
        outputFile << static_cast<float>(numBBWith1Succ) / sum << ",";
        outputFile << static_cast<float>(numBBWith2Pred) / sum << ",";
        outputFile << static_cast<float>(numBBWith2Pred1Succ) / sum << ",";
        outputFile << static_cast<float>(numBBWith2Pred2Succ) / sum << ",";
        outputFile << static_cast<float>(numBBWith2Succ) / sum << ",";
        outputFile << static_cast<float>(numBBWithMoreThan2Pred) / sum << ",";
        outputFile << static_cast<float>(numBBWithPhiInRange0to3) / sum << ",";
        outputFile << static_cast<float>(numBBWithMoreThan3Phi) / sum << ",";
        outputFile << static_cast<float>(numBBWithNoPhi) / sum << ",";
        outputFile << static_cast<float>(totalBranches) / sum << ",";
        outputFile << static_cast<float>(numCallsReturningInt) / sum << ",";
        outputFile << static_cast<float>(totalEdges) / sum << ",";
        outputFile << static_cast<float>(num32BitConstants) / sum << ",";
        outputFile << static_cast<float>(num64BitConstants) / sum << ",";
        outputFile << static_cast<float>(numConstZero) / sum << ",";
        outputFile << static_cast<float>(numConstOne) / sum << ",";
        outputFile << static_cast<float>(numUnconditionalBranches) / sum << ",";
        outputFile << static_cast<float>(numBinaryOpsWithConst) / sum << ",";
        outputFile << static_cast<float>(numAShrInsts) / sum << ",";
        outputFile << static_cast<float>(numAddInsts) / sum << ",";
        outputFile << static_cast<float>(numAllocaInsts) / sum << ",";
        outputFile << static_cast<float>(numAndInsts) / sum << ",";
        outputFile << static_cast<float>(numBBWithInsts15to500) / sum << ",";
        outputFile << static_cast<float>(numBBWithLessThan15Insts) / sum << ",";
        outputFile << static_cast<float>(numBitCastInsts) / sum << ",";
        outputFile << static_cast<float>(numBrInsts) / sum << ",";
        outputFile << static_cast<float>(numCallInsts) / sum << ",";
        outputFile << static_cast<float>(numGetElementPtrInsts) / sum << ",";
        outputFile << static_cast<float>(numICmpInsts) / sum << ",";
        outputFile << static_cast<float>(numLShrInsts) / sum << ",";
        outputFile << static_cast<float>(numLoadInsts) / sum << ",";
        outputFile << static_cast<float>(numMulInsts) / sum << ",";
        outputFile << static_cast<float>(numOrInsts) / sum << ",";
        outputFile << static_cast<float>(numPhiInsts) / sum << ",";
        outputFile << static_cast<float>(numRetInsts) / sum << ",";
        outputFile << static_cast<float>(numSExtInsts) / sum << ",";
        outputFile << static_cast<float>(numSelectInsts) / sum << ",";
        outputFile << static_cast<float>(numShlInsts) / sum << ",";
        outputFile << static_cast<float>(numStoreInsts) / sum << ",";
        outputFile << static_cast<float>(numSubInsts) / sum << ",";
        outputFile << static_cast<float>(numTruncInsts) / sum << ",";
        outputFile << static_cast<float>(numXorInsts) / sum << ",";
        outputFile << static_cast<float>(numZExtInsts) / sum << ",";
        outputFile << static_cast<float>(totalBasicBlocks) / sum << "\n";
        outputFile.close();
    }
}