#include "Concurrency/Thread/ThreadModelBuilder.h"

namespace lotus::concurrency {

ThreadModel ThreadModelBuilder::build(llvm::Module& M) {
    (void)M;
    ThreadModel model;
    // Scaffolding only: the TFG-construction extraction (processFunction,
    // handleThreadFork, ...) will populate the model's bookkeeping maps here.
    return model;
}

} // namespace lotus::concurrency