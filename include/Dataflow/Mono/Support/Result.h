#ifndef LOTUS_DATAFLOW_MONO_SUPPORT_RESULT_H_
#define LOTUS_DATAFLOW_MONO_SUPPORT_RESULT_H_

#include "Utils/LLVM/SystemHeaders.h"

#include <map>

namespace mono {

template <typename ContainerT> class DataFlowResultT {
public:
  DataFlowResultT() = default;

  ContainerT &GEN(Instruction *inst) { return gens[inst]; }
  ContainerT &KILL(Instruction *inst) { return kills[inst]; }
  ContainerT &IN(Instruction *inst) { return ins[inst]; }
  ContainerT &OUT(Instruction *inst) { return outs[inst]; }

private:
  std::map<Instruction *, ContainerT> gens;
  std::map<Instruction *, ContainerT> kills;
  std::map<Instruction *, ContainerT> ins;
  std::map<Instruction *, ContainerT> outs;
};

class DataFlowResult {
public:
  DataFlowResult() = default;

  std::set<Value *> &GEN(Instruction *inst) { return gens[inst]; }
  std::set<Value *> &KILL(Instruction *inst) { return kills[inst]; }
  std::set<Value *> &IN(Instruction *inst) { return ins[inst]; }
  std::set<Value *> &OUT(Instruction *inst) { return outs[inst]; }

private:
  std::map<Instruction *, std::set<Value *>> gens;
  std::map<Instruction *, std::set<Value *>> kills;
  std::map<Instruction *, std::set<Value *>> ins;
  std::map<Instruction *, std::set<Value *>> outs;
};

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_SUPPORT_RESULT_H_
