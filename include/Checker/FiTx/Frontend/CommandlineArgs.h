#include "Checker/Tooling/CheckerSubcommands.h"
#include "llvm/Support/CommandLine.h"

namespace fitx {
namespace CommandLineArgs {
llvm::cl::opt<bool> Flex(
    "flex", llvm::cl::desc("Print all possible errors"),
    llvm::cl::sub(lotus::checker::tooling::fitxSubCommand()));
} // namespace CommandLineArgs
} // namespace fitx
