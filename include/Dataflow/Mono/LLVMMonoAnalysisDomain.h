#ifndef ANALYSIS_MONO_LLVMMONOANALYSISDOMAIN_H_
#define ANALYSIS_MONO_LLVMMONOANALYSISDOMAIN_H_

#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/ControlFlow/IntraCFG.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

#include <cstddef>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace mono {

template <typename ContainerT> struct LLVMMonoAnalysisDomain {
  using n_t = llvm::Instruction *;
  using d_t = llvm::Value *;
  using f_t = llvm::Function *;
  using t_t = llvm::Type *;
  using v_t = llvm::Value *;
  using db_t = llvm::Module;

  // Phasar-like CFG/ICFG associated types for LLVM mode.
  using c_t = ::dataflow::controlflow::IntraCFG;
  using i_t = ::dataflow::controlflow::InterCFG;

  // Placeholder points-to type for LLVM-only Mono analyses.
  using pt_t = lotus::AliasAnalysisWrapper *;

  using mono_container_t = ContainerT;
};

} // namespace mono

#endif // ANALYSIS_MONO_LLVMMONOANALYSISDOMAIN_H_
