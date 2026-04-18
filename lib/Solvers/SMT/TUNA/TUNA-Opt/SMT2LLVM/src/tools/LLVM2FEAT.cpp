//
// Created by ljc on 24-10-29.
//

#include "SMTFormula.h"
#include "LLVMNode.h"
#include "Utility.h"
#include "Feature.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <llvm/Passes/StandardInstrumentations.h>

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

    if (!HasFlag(argc, argv, "-f"))
    {
        std::cerr << "Must specify input file with -f.\n";
        return 1;
    }
    char *featureDir = GetFlag(argc, argv, "-f");

    llvm::LLVMContext lcx;
    llvm::SMDiagnostic error;
    std::unique_ptr<llvm::Module> lmodule = llvm::parseIRFile(inputFilename, error, lcx);

    llvm::Function *fun = lmodule->getFunction(LLVM_FUNCTION_NAME);

    std::string inputFilenameStr(inputFilename);
    std::size_t found = inputFilenameStr.find_last_of("/\\");
    std::string filename = inputFilenameStr.substr(found + 1);
    analyzeFunction(*fun, inputFilenameStr, featureDir);
}
