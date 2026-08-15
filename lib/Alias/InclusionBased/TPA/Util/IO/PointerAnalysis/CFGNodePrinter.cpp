#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/CFGNode.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/ProgramPoint.h"
#include "Alias/InclusionBased/TPA/Util/IO/PointerAnalysis/NodePrinter.h"
#include "Alias/InclusionBased/TPA/Util/IO/PointerAnalysis/Printer.h"

#include <llvm/IR/Function.h>

namespace util {
namespace io {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const tpa::CFGNode &node) {
  NodePrinter<tpa::CFGNode>(os).visit(node);
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const tpa::ProgramPoint &pp) {
  os << "(" << *pp.getContext() << ", " << *pp.getCFGNode() << ")";
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const tpa::FunctionContext &fc) {
  os << "(" << *fc.getContext() << ", " << fc.getFunction()->getName() << ")";
  return os;
}

} // namespace io
} // namespace util