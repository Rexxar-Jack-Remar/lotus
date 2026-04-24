#include "Alias/InclusionBased/TPA/Context/Context.h"
#include "Alias/InclusionBased/TPA/Context/ProgramPoint.h"
#include "Alias/InclusionBased/TPA/Util/IO/Context/Printer.h"

#include <llvm/Support/raw_ostream.h>

namespace util {
namespace io {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const context::Context &ctx) {
  if (ctx.isGlobalContext())
    os << "[GLOBAL]";
  else
    os << "[CTX(" << ctx.size() << ") " << &ctx << "]";
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const context::ProgramPoint &p) {
  os << "(" << *p.getContext() << ", " << *p.getInstruction() << ")";
  return os;
}

} // namespace io
} // namespace util