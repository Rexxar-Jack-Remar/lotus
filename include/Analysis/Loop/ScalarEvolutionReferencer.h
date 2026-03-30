/*
 * Copyright 2026 Lotus contributors
 */
#ifndef LOTUS_ANALYSIS_LOOP_SCALAREVOLUTIONREFERENCER_H
#define LOTUS_ANALYSIS_LOOP_SCALAREVOLUTIONREFERENCER_H

#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/IRBuilder.h"

#include <unordered_map>

namespace llvm {

class SCEVReference;
class SCEVValueMapper;

class ScalarEvolutionReferentialExpander {
public:
  ScalarEvolutionReferentialExpander(ScalarEvolution &SE, Function &F);
  ~ScalarEvolutionReferentialExpander();

  SCEVReference *createReferenceTree(const SCEV *scev,
                                     std::set<Value *> valuesInScope);

  Value *expandUsingReferenceValues(
      SCEVReference *tree,
      std::set<Value *> valuesToReferenceAndNotExpand,
      IRBuilder<> &expansionBuilder);

private:
  SCEVValueMapper *scevValueMapper;
};

class ReferenceTreeExpander : SCEVVisitor<ReferenceTreeExpander, Value *> {
public:
  ReferenceTreeExpander(SCEVReference *tree,
                        std::set<Value *> &valuesToReferenceAndNotExpand,
                        IRBuilder<> &expansionBuilder);

  Value *getRootOfTree();

private:
  friend struct SCEVVisitor<ReferenceTreeExpander, Value *>;

  Value *visitConstant(const SCEVConstant *S);
  Value *visitTruncateExpr(const SCEVTruncateExpr *S);
  Value *visitZeroExtendExpr(const SCEVZeroExtendExpr *S);
  Value *visitSignExtendExpr(const SCEVSignExtendExpr *S);
  Value *visitAddExpr(const SCEVAddExpr *S);
  Value *visitMulExpr(const SCEVMulExpr *S);
  Value *visitUDivExpr(const SCEVUDivExpr *S);
  Value *visitAddRecExpr(const SCEVAddRecExpr *S);
  Value *visitSMaxExpr(const SCEVSMaxExpr *S);
  Value *visitUMaxExpr(const SCEVUMaxExpr *S);
  Value *visitSMinExpr(const SCEVSMinExpr *S);
  Value *visitUMinExpr(const SCEVUMinExpr *S);
  Value *visitUnknown(const SCEVUnknown *S);
  Value *visitCouldNotCompute(const SCEVCouldNotCompute *S);
  Value *visitSequentialUMinExpr(const SCEVSequentialUMinExpr *S);
  Value *visitPtrToIntExpr(const SCEVPtrToIntExpr *S);

  std::pair<Value *, Value *> visitTwoOperands(const SCEVNAryExpr *S);

  Value *rootValue;
  SCEVReference *currentNode;
  std::set<Value *> &valuesToReferenceAndNotExpand;
  IRBuilder<> &expansionBuilder;
};

class ReferenceTreeBuilder
    : SCEVVisitor<ReferenceTreeBuilder, SCEVReference *> {
public:
  ReferenceTreeBuilder(const SCEV *scev,
                       SCEVValueMapper &scevValueMapper,
                       std::set<Value *> &valuesInScope);

  SCEVReference *getTree();

private:
  friend struct SCEVVisitor<ReferenceTreeBuilder, SCEVReference *>;

  SCEVReference *visitConstant(const SCEVConstant *S);
  SCEVReference *visitTruncateExpr(const SCEVTruncateExpr *S);
  SCEVReference *visitZeroExtendExpr(const SCEVZeroExtendExpr *S);
  SCEVReference *visitSignExtendExpr(const SCEVSignExtendExpr *S);
  SCEVReference *visitAddExpr(const SCEVAddExpr *S);
  SCEVReference *visitMulExpr(const SCEVMulExpr *S);
  SCEVReference *visitUDivExpr(const SCEVUDivExpr *S);
  SCEVReference *visitAddRecExpr(const SCEVAddRecExpr *S);
  SCEVReference *visitSMaxExpr(const SCEVSMaxExpr *S);
  SCEVReference *visitUMaxExpr(const SCEVUMaxExpr *S);
  SCEVReference *visitSMinExpr(const SCEVSMinExpr *S);
  SCEVReference *visitUMinExpr(const SCEVUMinExpr *S);
  SCEVReference *visitUnknown(const SCEVUnknown *S);
  SCEVReference *visitCouldNotCompute(const SCEVCouldNotCompute *S);
  SCEVReference *visitSequentialUMinExpr(const SCEVSequentialUMinExpr *S);
  SCEVReference *visitPtrToIntExpr(const SCEVPtrToIntExpr *S);

  Value *mapToSingleInScopeValue(const SCEV *S);
  SCEVReference *createReferenceOfSingleInScopeValue(const SCEV *S);
  SCEVReference *createReferenceOfNArySCEV(const SCEVNAryExpr *S);

  SCEVReference *tree;
  std::set<Value *> &valuesInScope;
  SCEVValueMapper &scevValueMapper;
};

class SCEVValueMapper {
public:
  SCEVValueMapper(ScalarEvolution &SE, Function &F);

  Value *getSingleValueOf(const SCEV *scev) const;
  const std::set<Value *> getValuesOf(const SCEV *scev) const;
  const SCEV *getSCEVOf(Value *value) const;

private:
  std::unordered_map<const SCEV *, std::set<Value *>> scevToValues;
  std::unordered_map<Value *, const SCEV *> valueToSCEV;
};

class SCEVReference {
public:
  SCEVReference(Value *V, const SCEV *scev);
  ~SCEVReference();

  Value *getValue() const;
  const SCEV *getSCEV() const;
  iterator_range<std::vector<SCEVReference *>::iterator> getChildReferences();
  SCEVReference *getChildReference(int32_t idx);
  int32_t getNumChildReferences();

  void addChildReference(SCEVReference *scevReference);
  std::set<SCEVReference *> collectAllReferences();

private:
  Value *value;
  const SCEV *scev;
  std::vector<SCEVReference *> childReferences;
};

} // namespace llvm

#endif
