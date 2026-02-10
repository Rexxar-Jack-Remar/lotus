/**
 * @file PointsToSet.cpp
 * @brief Points-to set implementations and configuration for AserPTA.
 *
 * Provides implementations of points-to sets using different backends:
 * - BitVectorPTS: Sparse bit vector implementation
 * - BDDPts: BDD-backed implementation for scalability
 * - PointedByPts: Reverse points-to sets (pointed-by sets)
 *
 * @author peiming
 */
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/BDDPts.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/PointedByPts.h"
#include <llvm/Support/CommandLine.h>
#include <string>

using namespace llvm;

llvm::cl::opt<bool> CollectStats("collect-stats", llvm::cl::desc("Dump the modified ir file"));

namespace aser {

// Command line option definition (declared as extern in BDDPts.h)
llvm::cl::opt<bool> ConfigUseBDDPts(
    "pta-use-bdd-pts",
    llvm::cl::desc("Use BDD-backed points-to sets instead of SparseBitVector"),
    llvm::cl::init(false));

llvm::cl::opt<bool> ConfigBDDPtsReorder(
    "pta-bdd-reorder",
    llvm::cl::desc("Enable dynamic variable reordering for BDD points-to sets"),
    llvm::cl::init(false));

llvm::cl::opt<std::string> ConfigBDDPtsReorderMethod(
    "pta-bdd-reorder-method",
    llvm::cl::desc(
        "BDD reordering heuristic (sift|sift-conv|symm-sift|symm-sift-conv|"
        "group-sift|group-sift-conv|window2|window3|window4|window2-conv|"
        "window3-conv|window4-conv|random|random-pivot|annealing|genetic|linear|"
        "linear-conv|lazy-sift|exact); used when --pta-bdd-reorder is set"),
    llvm::cl::init("sift"));

std::vector<BitVectorPTS::PtsTy> BitVectorPTS::ptsVec;
std::vector<BDDPts::PtsTy> BDDPts::ptsVec;
std::vector<PointedByPts::PtsTy> PointedByPts::pointsTo;
std::vector<PointedByPts::PtsTy> PointedByPts::pointedBy;

}  // namespace aser
