//
// Created by ljc on 24-10-31.
//
#include "SMTFormula.h"
#include "LLVMNode.h"
#include "Utility.h"
#include "Feature.h"
#include <fstream>
#include <streambuf>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <llvm/Passes/PassBuilder.h>
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
    bool shiftToMultiply = false;

    // 命令行参数解析
    if (HasFlag(argc, argv, "-h"))
    {
        Help();
        return 0;
    }

    if (HasFlag(argc, argv, "-m"))
    {
        shiftToMultiply = true;
    }

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
    std::stringstream buffer;
    buffer << t.rdbuf();
    std::string smt_str = buffer.str();

    // 前端转换
    auto frontStart = high_resolution_clock::now();
    SMTFormula a(lcx, &lmodule, builder, smt_str, LLVM_FUNCTION_NAME);
    a.ToLLVM();
    auto frontEnd = high_resolution_clock::now();
    duration<double> frontTime = frontEnd - frontStart;

    llvm::Function *fun = lmodule.getFunction(LLVM_FUNCTION_NAME);
    
    // 创建 PassBuilder 和 ModulePassManager
    llvm::PassBuilder PB;
    llvm::FunctionPassManager FPM;

    // 解析传入的 Pass 文件（如果提供）
    if (HasFlag(argc, argv, "-p"))
    {
        char *passFilename = GetFlag(argc, argv, "-p");
        if (passFilename)
        {
            if (ParsePassesFromFile(passFilename, FPM, PB))
            {
                std::cerr << "Failed to parse passes from file.\n";
                return 1;
            }
        }
        else
        {
            std::cerr << "Invalid passes file name.\n";
            return 1;
        }
    }

    char *luFilename = nullptr;
    if (HasFlag(argc, argv, "-lu") && (luFilename = GetFlag(argc, argv, "-lu")))
    {
        llvm::raw_fd_ostream file(luFilename, *(new std::error_code()));
        lmodule.print(file, nullptr);
    }

    // 优化过程调用
    auto optStart = high_resolution_clock::now();
    int count = (*fun).getEntryBlock().sizeWithoutDebug();
    RunPasses(&count, *fun, FPM); // 使用 FunctionPassManager 来运行 Pass
    auto optEnd = high_resolution_clock::now();
    duration<double> optTime = optEnd - optStart;

    char *loFilename = nullptr;
    if (HasFlag(argc, argv, "-lo") && (loFilename = GetFlag(argc, argv, "-lo")))
    {
        llvm::raw_fd_ostream file(loFilename, *(new std::error_code()));
        lmodule.print(file, nullptr);
    }

    // 其他代码保持不变
    context c;
    solver s(c);

    // 后端转换
    auto backStart = high_resolution_clock::now();
    LLVMFunction f(shiftToMultiply, c, fun);
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
    else
    {
        std::cout << s.to_smt2();
    }

    // 打印统计信息
    char *statsFilename = nullptr;
    if (HasFlag(argc, argv, "-t") && (statsFilename = GetFlag(argc, argv, "-t")))
    {
        std::ofstream out(statsFilename, std::ios_base::app);
        out << inputFilename << "," << (shiftToMultiply ? "true" : "false") << "," << frontTime.count() << "," << optTime.count() << "," << backTime.count() << "\n";
    }
    else
    {
        std::cout << std::fixed; // 设置为固定小数点格式
        std::cout << std::setprecision(7); // 设置小数点后的位数
        std::cout << inputFilename << "," << (shiftToMultiply ? "true" : "false") << "," << frontTime.count() << "," << optTime.count() << "," << backTime.count() << "\n";
    }

    return 0;
}