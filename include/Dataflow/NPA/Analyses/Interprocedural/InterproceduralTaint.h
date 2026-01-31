#ifndef NPA_INTERPROC_TAINT_H
#define NPA_INTERPROC_TAINT_H

#include "Dataflow/NPA/Domains/TaintTransferDomain.h"
#include <llvm/IR/Module.h>
#include <map>
#include <string>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace npa {

class InterproceduralTaint {
public:
  struct Result {
    std::map<std::string, TaintTransferDomain::value_type> summaries;
    std::map<std::string, llvm::APInt> blockFacts;
  };

  static Result run(llvm::Module &M,
                    lotus::AliasAnalysisWrapper &aliasAnalysis,
                    bool verbose = false);
};

} // namespace npa

#endif // NPA_INTERPROC_TAINT_H
