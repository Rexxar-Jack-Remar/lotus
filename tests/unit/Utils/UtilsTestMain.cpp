#include <gtest/gtest.h>
#include <llvm/Support/CommandLine.h>

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "Lotus utilities tests\n");
  return RUN_ALL_TESTS();
}
