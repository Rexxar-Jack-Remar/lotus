//
// Created by ljc on 24-10-28.
//

#include "SMTFormula.h"
#include "Feature.h"
#include "LLVMNode.h"
#include "Utility.h"
#include <llvm/Passes/StandardInstrumentations.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <streambuf>

#ifndef LLMAPPING
#define LLMAPPING std::map<std::string, llvm::Value *> &
#endif

#ifndef LLVM_FUNCTION_NAME
#define LLVM_FUNCTION_NAME "SMT"
#endif

using namespace SLOT;
using namespace std::chrono;

int LLVMFunction::varCounter = 0;

int main(int argc, char *argv[])
{
    if (!HasFlag(argc, argv, "-s"))
    {
        std::cerr << "Must specify input file with -s.\n";
        return 1;
    }

    char *inputFilename = GetFlag(argc, argv, "-s");
    if (!inputFilename)
    {
        std::cerr << "Invalid input file name.\n";
        return 1;
    }

    // LLVM 和 Z3 设置
    llvm::LLVMContext lcx;
    llvm::Module lmodule(inputFilename, lcx);
    llvm::IRBuilder<> builder(lcx);

    std::ifstream t(inputFilename);
    if (!t.is_open()) {
        std::cout << "Can't open file: " << inputFilename
                  << ", error: " << std::strerror(errno) << '\n';
        return 0;
    }
    std::ostringstream buffer;
    buffer << t.rdbuf();
    std::string smt_str = buffer.str();

    // 前端转换
    SMTFormula a(lcx, &lmodule, builder, smt_str, LLVM_FUNCTION_NAME);
    auto frontStart = high_resolution_clock::now();
    a.ToLLVM();
    auto frontEnd = high_resolution_clock::now();
    duration<double> frontTime = frontEnd - frontStart;

    char *luFilename = nullptr;
    if (HasFlag(argc, argv, "-lu") && (luFilename = GetFlag(argc, argv, "-lu")))
    {
        llvm::raw_fd_ostream file(luFilename, *(new std::error_code()));
        lmodule.print(file, nullptr);
    }
    std::cout << std::fixed;
    std::cout << std::setprecision(7);
    std::cout << inputFilename << "," << frontTime.count() << "\n";
}
