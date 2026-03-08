#ifndef NPA_INTERPROC_TAINT_H
#define NPA_INTERPROC_TAINT_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/Domains/TaintTransferDomain.h"

#include <map>

#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace npa {

class InterproceduralTaint {
public:
  struct Result {
    std::map<FunctionKey, TaintTransferDomain::value_type> summaries;
    std::map<BlockKey, llvm::APInt> blockFacts;
  };

  static Result run(llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis,
                    bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::Worklist);
};

} // namespace npa

#endif // NPA_INTERPROC_TAINT_H
