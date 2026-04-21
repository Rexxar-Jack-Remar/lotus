#include "ToolSupport.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

namespace lotus {
namespace dataflow_tool {

std::unique_ptr<llvm::Module>
loadModuleOrReport(const std::string &InputFilename, llvm::LLVMContext &Context,
                   llvm::SMDiagnostic &Err, const char *Argv0) {
  auto M = llvm::parseIRFile(InputFilename, Err, Context);
  if (!M)
    Err.print(Argv0, llvm::errs());
  return M;
}

void prepareModule(llvm::Module &M) {
  llvm::legacy::PassManager PM;
  PM.add(llvm::createPromoteMemoryToRegisterPass());
  PM.add(llvm::createInstructionNamerPass());
  PM.run(M);
}

FunctionView buildFunctionView(llvm::Function &F) {
  FunctionView View{F, {}, {}};
  unsigned ArgIdx = 0;
  for (auto &Arg : F.args())
    View.ValueToId[&Arg] = "arg" + std::to_string(ArgIdx++);

  unsigned InstIdx = 0;
  for (auto &BB : F)
    for (auto &I : BB) {
      View.OrderedInsts.push_back(&I);
      View.ValueToId[&I] = "i" + std::to_string(InstIdx++);
    }

  return View;
}

std::unique_ptr<llvm::raw_fd_ostream>
openOutputFileOrReport(const std::string &OutDir, const char *Filename,
                       std::error_code &EC) {
  return std::make_unique<llvm::raw_fd_ostream>(OutDir + "/" + Filename, EC);
}

llvm::raw_ostream &
selectOutputStream(bool WriteToStdout, llvm::StringRef OutDir,
                   llvm::StringRef Filename,
                   std::unique_ptr<llvm::raw_fd_ostream> &FileOS,
                   llvm::raw_null_ostream &NullOS, std::error_code &EC) {
  if (OutDir.empty())
    return WriteToStdout ? llvm::outs()
                         : static_cast<llvm::raw_ostream &>(NullOS);

  FileOS = openOutputFileOrReport(OutDir.str(), Filename.data(), EC);
  if (EC)
    return NullOS;
  return *FileOS;
}

::elimination::EliminationOptions
parseEliminationOptions(llvm::StringRef MethodName) {
  ::elimination::EliminationOptions Opts;
  if (MethodName == "adt-simple")
    Opts.Method = ::elimination::EliminationMethod::ADTSimple;
  else if (MethodName == "adt-delayed")
    Opts.Method = ::elimination::EliminationMethod::ADTDelayed;
  else
    Opts.Method = ::elimination::EliminationMethod::StateElimination;
  return Opts;
}

void emitFunctionHeader(llvm::raw_ostream &OS, const llvm::Function &F) {
  OS << "FUNC " << F.getName() << "\n";
}

} // namespace dataflow_tool
} // namespace lotus
