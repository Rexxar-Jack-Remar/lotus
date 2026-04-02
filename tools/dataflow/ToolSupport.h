#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace llvm {
class raw_fd_ostream;
} // namespace llvm

namespace lotus {
namespace dataflow_tool {

using ValueIdMap = std::unordered_map<const llvm::Value *, std::string>;

struct FunctionView final {
  llvm::Function &Function;
  ValueIdMap ValueToId;
  std::vector<llvm::Instruction *> OrderedInsts;
};

std::unique_ptr<llvm::Module>
loadModuleOrReport(const std::string &InputFilename, llvm::LLVMContext &Context,
                   llvm::SMDiagnostic &Err, const char *Argv0);

void prepareModule(llvm::Module &M);

FunctionView buildFunctionView(llvm::Function &F);

std::unique_ptr<llvm::raw_fd_ostream>
openOutputFileOrReport(const std::string &OutDir, const char *Filename,
                       std::error_code &EC);

} // namespace dataflow_tool
} // namespace lotus
