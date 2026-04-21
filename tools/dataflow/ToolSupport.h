#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Core/Options.h"

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
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

llvm::raw_ostream &
selectOutputStream(bool WriteToStdout, llvm::StringRef OutDir,
                   llvm::StringRef Filename,
                   std::unique_ptr<llvm::raw_fd_ostream> &FileOS,
                   llvm::raw_null_ostream &NullOS, std::error_code &EC);

::elimination::EliminationOptions
parseEliminationOptions(llvm::StringRef MethodName);

void emitFunctionHeader(llvm::raw_ostream &OS, const llvm::Function &F);

template <typename T>
void formatValueSet(llvm::raw_ostream &OS, const std::set<T> &Values,
                    const ValueIdMap &ValueToId) {
  std::vector<std::string> ids;
  for (const llvm::Value *V : Values) {
    auto It = ValueToId.find(V);
    if (It != ValueToId.end())
      ids.push_back(It->second);
  }
  std::sort(ids.begin(), ids.end());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i)
      OS << ",";
    OS << ids[i];
  }
}

template <typename MapT, typename Formatter>
void formatValueMap(llvm::raw_ostream &OS, const MapT &Map,
                    const ValueIdMap &ValueToId, Formatter &&FormatValue) {
  std::vector<std::string> entries;
  for (const auto &Entry : Map) {
    std::ostringstream ss;
    auto It = ValueToId.find(Entry.first);
    ss << (It != ValueToId.end() ? It->second : "v") << "="
       << FormatValue(Entry.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

template <typename Printer>
void printInstructionStates(llvm::raw_ostream &OS, const FunctionView &View,
                            Printer &&PrintState) {
  for (auto *I : View.OrderedInsts) {
    OS << "  " << View.ValueToId.at(I) << " IN: ";
    PrintState(I);
    OS << "\n";
  }
}

template <typename Callback>
void forEachDefinedFunction(llvm::Module &M, llvm::raw_ostream &OS,
                            Callback &&Fn) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    auto View = buildFunctionView(F);
    emitFunctionHeader(OS, F);
    Fn(View);
  }
}

template <typename HandlerT, size_t N>
const HandlerT *findHandler(llvm::StringRef Name,
                            const HandlerT (&Handlers)[N]) {
  for (const auto &Handler : Handlers)
    if (Handler.Name == Name)
      return &Handler;
  return nullptr;
}

} // namespace dataflow_tool
} // namespace lotus
