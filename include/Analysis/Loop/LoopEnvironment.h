/*
 * Copyright 2026  Lotus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef LOTUS_ANALYSIS_LOOP_LOOPENVIRONMENT_H
#define LOTUS_ANALYSIS_LOOP_LOOPENVIRONMENT_H

#include "Analysis/Loop/LoopDependenceGraph.h"

namespace lotus {
namespace analysis {
namespace loop {

class LoopEnvironment {
public:
  LoopEnvironment(LoopDependenceGraph *loopDG,
                  std::vector<BasicBlock *> const &exitBlocks);

  iterator_range<std::vector<Value *>::iterator> getProducers(void);
  iterator_range<std::set<int>::iterator> getEnvIDsOfLiveInVars(void) const;
  iterator_range<std::set<int>::iterator> getEnvIDsOfLiveOutVars(void) const;

  uint64_t size(void) const;
  uint64_t getNumberOfLiveIns(void) const;
  uint64_t getNumberOfLiveOuts(void) const;
  int64_t getExitBlockID(void) const;

  Type *typeOfEnvironmentLocation(uint64_t id) const;
  std::vector<Type *> getTypesOfEnvironmentLocations(void) const;

  bool isLiveIn(Value *val) const;
  bool isProducer(Value *producer) const;
  Value *getProducer(uint64_t id) const;
  std::set<Value *> consumersOf(Value *prod) const;

private:
  std::vector<Value *> envProducers;
  std::unordered_map<Value *, int> producerIDMap;
  std::set<int> liveInIDs;
  std::set<int> liveOutIDs;
  std::unordered_map<Value *, std::set<Value *>> prodConsumers;
  bool hasExitBlockEnv;
  Type *exitBlockType;

  uint64_t addProducer(Value *producer, bool liveIn);
  uint64_t addLiveInProducer(Value *producer);
  void addLiveOutProducer(Value *producer);
  uint64_t addLiveInValue(Value *newLiveInValue,
                          const std::unordered_set<Instruction *> &consumers);
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
