#include "Utility.h"
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/Error.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <fstream>
#include <sstream>

void Help()
{
    std::cout << "SLOT arguments:\n";
    std::cout << "   -h             : See help menu\n";
    std::cout << "   -s <file>      : The input SMTLIB2 format file (required)\n";
    std::cout << "   -o <file>      : The output file. If not provided, output is sent to stdout\n";
    std::cout << "   -lu <file>     : Output intermediate LLVM IR before optimization (optional)\n";
    std::cout << "   -lo <file>     : Output intermediate LLVM IR after optimization (optional)\n";
    std::cout << "   -m             : Convert constant shifts to multiplication\n";
    std::cout << "   -t <file>      : Output statistics file. If not provided, output is sent to stdout\n";
    std::cout << "   -p <file>      : Specify a file containing a list of LLVM passes to run (optional)\n";
}

char *GetFlag(int argc, char *argv[], const std::string &flag)
{
    for (int i = 1; i < argc - 1; i++)
    {
        if (flag.compare(argv[i]) == 0)
        {
            return argv[i + 1];
        }
    }
    return nullptr;
}

bool HasFlag(int argc, char *argv[], const std::string &flag)
{
    for (int i = 1; i < argc; i++)
    {
        if (flag.compare(argv[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

unsigned short ParsePassesFromFile(const std::string &filename, llvm::FunctionPassManager &FPM, llvm::PassBuilder &PB)
{
    std::ifstream infile(filename);
    std::string passPipeline;
    std::ostringstream pipelineStream;

    while (std::getline(infile, passPipeline))
    {
        pipelineStream << passPipeline << ",";
    }

    std::string pipelineStr = pipelineStream.str();
    if (!pipelineStr.empty())
    {
        pipelineStr.pop_back();
    }

    llvm::LLVMContext context;

    // 初始化所有分析管理器
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    // 注册所有需要的分析
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);

    // 注册代理
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::PipelineTuningOptions PTO;
    if (pipelineStr == "") {
        return 0;
    }
    if (auto Err = PB.parsePassPipeline(FPM, pipelineStr))
    {
        llvm::errs() << "Error parsing pass pipeline: " << llvm::toString(std::move(Err)) << "\n";
        return 1;
    }
    return 0;
}

unsigned short RunPasses(int *count, llvm::Function &function, llvm::FunctionPassManager &FPM)
{
    // 初始化所有分析管理器
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB;

    // 注册所有需要的分析
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);

    // 注册代理
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    FPM.run(function, FAM);

    if (function.getEntryBlock().sizeWithoutDebug() != *count)
    {
        *count = function.getEntryBlock().sizeWithoutDebug();
        return 1;
    }

    return 0;
}