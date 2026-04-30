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
    // Convert constant shifts to multiplication
    // 对应原来slot是否-m，原来默认关，现在默认开
    bool shiftToMultiply = true;

    if (!HasFlag(argc, argv, "-lo"))
    {
        std::cerr << "Must specify input file with -lo.\n";
        return 1;
    }

    char *inputFilename = GetFlag(argc, argv, "-lo");
    if (!inputFilename)
    {
        std::cerr << "Invalid input file name.\n";
        return 1;
    }

    llvm::LLVMContext lcx;
    llvm::SMDiagnostic error;
    std::unique_ptr<llvm::Module> lmodule = llvm::parseIRFile(inputFilename, error, lcx);

    llvm::Function *fun = lmodule->getFunction(LLVM_FUNCTION_NAME);

    // 其他代码保持不变
    context c;
    solver s(c);

    // 后端转换
    LLVMFunction f(shiftToMultiply, c, fun);
    auto backStart = high_resolution_clock::now();
    s.add(f.ToSMT());
    auto backEnd = high_resolution_clock::now();
    duration<double> backTime = backEnd - backStart;

    // 输出约束条件
    char *outputFilename = nullptr;
    if (HasFlag(argc, argv, "-o") && (outputFilename = GetFlag(argc, argv, "-o")))
    {
        std::ofstream out(outputFilename);
        out << s.to_smt2();
    }
    std::cout << std::fixed;
    std::cout << std::setprecision(7);
    std::cout << inputFilename << "," << backTime.count() << "\n";
}
