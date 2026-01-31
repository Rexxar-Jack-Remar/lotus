#ifndef NPA_INTERPROC_RD_H
#define NPA_INTERPROC_RD_H

#include "Dataflow/NPA/Domains/GenKillDomain.h"
#include <llvm/IR/Module.h>
#include <map>
#include <string>

namespace npa {

class InterproceduralRD {
public:
    struct Result {
        std::map<std::string, GenKillDomain::value_type> summaries;
        std::map<std::string, llvm::APInt> blockFacts; 
    };

    static Result run(llvm::Module &M, bool verbose = false);
};

} // namespace npa
#endif
