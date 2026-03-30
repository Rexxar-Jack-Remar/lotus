/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/ScalarEvolutionReferencer.h"

using namespace llvm;

ScalarEvolutionReferentialExpander::ScalarEvolutionReferentialExpander(
    ScalarEvolution &SE,
    Function &F) {
  this->scevValueMapper = new SCEVValueMapper(SE, F);
}

ScalarEvolutionReferentialExpander::~ScalarEvolutionReferentialExpander() {
  delete this->scevValueMapper;
}

SCEVReference *ScalarEvolutionReferentialExpander::createReferenceTree(
    const SCEV *scev,
    std::set<Value *> valuesInScope) {
  ReferenceTreeBuilder builder(scev, *this->scevValueMapper, valuesInScope);
  return builder.getTree();
}

Value *ScalarEvolutionReferentialExpander::expandUsingReferenceValues(
    SCEVReference *tree,
    std::set<Value *> valuesToReferenceAndNotExpand,
    IRBuilder<> &expansionBuilder) {
  ReferenceTreeExpander expander(tree,
                                 valuesToReferenceAndNotExpand,
                                 expansionBuilder);
  return expander.getRootOfTree();
}

ReferenceTreeExpander::ReferenceTreeExpander(
    SCEVReference *tree,
    std::set<Value *> &valuesToReferenceAndNotExpand,
    IRBuilder<> &expansionBuilder)
    : currentNode{tree},
      valuesToReferenceAndNotExpand{valuesToReferenceAndNotExpand},
      expansionBuilder{expansionBuilder} {
  this->rootValue = visit(tree->getSCEV());
  this->currentNode = tree;
}

Value *ReferenceTreeExpander::getRootOfTree() { return this->rootValue; }

Value *ReferenceTreeExpander::visitConstant(const SCEVConstant *) {
  return this->currentNode->getValue();
}

Value *ReferenceTreeExpander::visitTruncateExpr(const SCEVTruncateExpr *) {
  return nullptr;
}

Value *ReferenceTreeExpander::visitZeroExtendExpr(const SCEVZeroExtendExpr *) {
  return nullptr;
}

Value *ReferenceTreeExpander::visitSignExtendExpr(const SCEVSignExtendExpr *) {
  return nullptr;
}

std::pair<Value *, Value *> ReferenceTreeExpander::visitTwoOperands(
    const SCEVNAryExpr *S) {
  if (this->currentNode->getNumChildReferences() < 2) {
    return std::make_pair(nullptr, nullptr);
  }

  auto *holder = this->currentNode;
  this->currentNode = holder->getChildReference(0);
  Value *LHS = visit(S->getOperand(0));
  this->currentNode = holder->getChildReference(1);
  Value *RHS = visit(S->getOperand(1));
  this->currentNode = holder;

  if (!LHS || !RHS) {
    return std::make_pair(nullptr, nullptr);
  }
  return std::make_pair(LHS, RHS);
}

Value *ReferenceTreeExpander::visitAddExpr(const SCEVAddExpr *S) {
  if (this->valuesToReferenceAndNotExpand.count(this->currentNode->getValue()) != 0) {
    return this->currentNode->getValue();
  }
  if (S->getNumOperands() != 2 || !S->getOperand(0)->getType()->isIntegerTy()
      || !S->getOperand(1)->getType()->isIntegerTy()) {
    return nullptr;
  }
  auto operands = visitTwoOperands(S);
  if (!operands.first) {
    return nullptr;
  }
  return this->expansionBuilder.CreateAdd(operands.first, operands.second);
}

Value *ReferenceTreeExpander::visitMulExpr(const SCEVMulExpr *S) {
  if (this->valuesToReferenceAndNotExpand.count(this->currentNode->getValue()) != 0) {
    return this->currentNode->getValue();
  }
  if (S->getNumOperands() != 2 || !S->getOperand(0)->getType()->isIntegerTy()
      || !S->getOperand(1)->getType()->isIntegerTy()) {
    return nullptr;
  }
  auto operands = visitTwoOperands(S);
  if (!operands.first) {
    return nullptr;
  }
  return this->expansionBuilder.CreateMul(operands.first, operands.second);
}

Value *ReferenceTreeExpander::visitUDivExpr(const SCEVUDivExpr *) { return nullptr; }
Value *ReferenceTreeExpander::visitAddRecExpr(const SCEVAddRecExpr *) { return nullptr; }

Value *ReferenceTreeExpander::visitSMaxExpr(const SCEVSMaxExpr *S) {
  if (this->valuesToReferenceAndNotExpand.count(this->currentNode->getValue()) != 0) {
    return this->currentNode->getValue();
  }
  if (S->getNumOperands() != 2) {
    return nullptr;
  }
  auto operands = visitTwoOperands(S);
  if (!operands.first) {
    return nullptr;
  }
  return this->expansionBuilder.CreateMaximum(operands.first, operands.second);
}

Value *ReferenceTreeExpander::visitUMaxExpr(const SCEVUMaxExpr *) { return nullptr; }
Value *ReferenceTreeExpander::visitSMinExpr(const SCEVSMinExpr *) { return nullptr; }
Value *ReferenceTreeExpander::visitUMinExpr(const SCEVUMinExpr *) { return nullptr; }

Value *ReferenceTreeExpander::visitUnknown(const SCEVUnknown *) {
  auto *value = this->currentNode->getValue();
  if (this->valuesToReferenceAndNotExpand.count(value) != 0) {
    return value;
  }
  return nullptr;
}

Value *ReferenceTreeExpander::visitSequentialUMinExpr(
    const SCEVSequentialUMinExpr *) {
  return nullptr;
}

Value *ReferenceTreeExpander::visitPtrToIntExpr(const SCEVPtrToIntExpr *) {
  return nullptr;
}

Value *ReferenceTreeExpander::visitCouldNotCompute(
    const SCEVCouldNotCompute *) {
  return nullptr;
}

ReferenceTreeBuilder::ReferenceTreeBuilder(const SCEV *scev,
                                           SCEVValueMapper &scevValueMapper,
                                           std::set<Value *> &valuesInScope)
    : valuesInScope{valuesInScope}, scevValueMapper{scevValueMapper} {
  this->tree = visit(scev);
}

SCEVReference *ReferenceTreeBuilder::getTree() { return this->tree; }

Value *ReferenceTreeBuilder::mapToSingleInScopeValue(const SCEV *S) {
  auto values = this->scevValueMapper.getValuesOf(S);
  Value *single = nullptr;
  for (auto *V : values) {
    if (this->valuesInScope.count(V) == 0) {
      continue;
    }
    if (single != nullptr) {
      return nullptr;
    }
    single = V;
  }
  return single;
}

SCEVReference *ReferenceTreeBuilder::createReferenceOfSingleInScopeValue(
    const SCEV *S) {
  auto *singleValue = mapToSingleInScopeValue(S);
  return singleValue ? new SCEVReference(singleValue, S) : nullptr;
}

SCEVReference *ReferenceTreeBuilder::createReferenceOfNArySCEV(
    const SCEVNAryExpr *S) {
  auto *reference = new SCEVReference(mapToSingleInScopeValue(S), S);
  for (auto *operand : S->operands()) {
    auto *operandReference = visit(operand);
    if (!operandReference) {
      break;
    }
    reference->addChildReference(operandReference);
  }

  if (static_cast<size_t>(reference->getNumChildReferences()) != S->getNumOperands()) {
    if (!reference->getValue()) {
      delete reference;
      return nullptr;
    }
  }

  return reference;
}

SCEVReference *ReferenceTreeBuilder::visitConstant(const SCEVConstant *S) {
  return new SCEVReference(S->getValue(), S);
}

SCEVReference *ReferenceTreeBuilder::visitUnknown(const SCEVUnknown *S) {
  auto *value = S->getValue();
  return this->valuesInScope.count(value) == 0 ? nullptr
                                               : new SCEVReference(value, S);
}

SCEVReference *ReferenceTreeBuilder::visitTruncateExpr(const SCEVTruncateExpr *S) {
  return createReferenceOfSingleInScopeValue(S);
}

SCEVReference *ReferenceTreeBuilder::visitZeroExtendExpr(
    const SCEVZeroExtendExpr *S) {
  return createReferenceOfSingleInScopeValue(S);
}

SCEVReference *ReferenceTreeBuilder::visitSignExtendExpr(
    const SCEVSignExtendExpr *S) {
  return createReferenceOfSingleInScopeValue(S);
}

SCEVReference *ReferenceTreeBuilder::visitAddExpr(const SCEVAddExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitMulExpr(const SCEVMulExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitUDivExpr(const SCEVUDivExpr *S) {
  auto *LHS = visit(S->getLHS());
  auto *RHS = visit(S->getRHS());
  auto *selfValue = mapToSingleInScopeValue(S);
  auto *reference = new SCEVReference(selfValue, S);
  if (LHS && RHS) {
    reference->addChildReference(LHS);
    reference->addChildReference(RHS);
    return reference;
  }
  if (LHS) {
    delete LHS;
  }
  if (RHS) {
    delete RHS;
  }
  if (!selfValue) {
    delete reference;
    return nullptr;
  }
  return reference;
}

SCEVReference *ReferenceTreeBuilder::visitPtrToIntExpr(const SCEVPtrToIntExpr *S) {
  return createReferenceOfSingleInScopeValue(S);
}

SCEVReference *ReferenceTreeBuilder::visitAddRecExpr(const SCEVAddRecExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitSMaxExpr(const SCEVSMaxExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitUMaxExpr(const SCEVUMaxExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitSMinExpr(const SCEVSMinExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitUMinExpr(const SCEVUMinExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitSequentialUMinExpr(
    const SCEVSequentialUMinExpr *S) {
  return createReferenceOfNArySCEV(S);
}

SCEVReference *ReferenceTreeBuilder::visitCouldNotCompute(
    const SCEVCouldNotCompute *) {
  return nullptr;
}

SCEVValueMapper::SCEVValueMapper(ScalarEvolution &SE, Function &F) {
  for (auto &A : F.args()) {
    if (!SE.isSCEVable(A.getType())) {
      continue;
    }
    auto *scev = SE.getSCEV(&A);
    this->scevToValues[scev].insert(&A);
    this->valueToSCEV[&A] = scev;
  }

  for (auto &B : F) {
    for (auto &I : B) {
      if (!SE.isSCEVable(I.getType())) {
        continue;
      }
      auto *scev = SE.getSCEV(&I);
      this->scevToValues[scev].insert(&I);
      this->valueToSCEV[&I] = scev;
    }
  }
}

Value *SCEVValueMapper::getSingleValueOf(const SCEV *scev) const {
  auto values = getValuesOf(scev);
  return values.size() == 1 ? *values.begin() : nullptr;
}

const std::set<Value *> SCEVValueMapper::getValuesOf(const SCEV *scev) const {
  auto it = this->scevToValues.find(scev);
  return it != this->scevToValues.end() ? it->second : std::set<Value *>{};
}

const SCEV *SCEVValueMapper::getSCEVOf(Value *value) const {
  auto it = this->valueToSCEV.find(value);
  return it != this->valueToSCEV.end() ? it->second : nullptr;
}

SCEVReference::SCEVReference(Value *V, const SCEV *scev)
    : value{V}, scev{scev}, childReferences{} {}

SCEVReference::~SCEVReference() {
  for (auto *child : this->childReferences) {
    delete child;
  }
}

Value *SCEVReference::getValue() const { return this->value; }
const SCEV *SCEVReference::getSCEV() const { return this->scev; }

iterator_range<std::vector<SCEVReference *>::iterator>
SCEVReference::getChildReferences() {
  return make_range(this->childReferences.begin(), this->childReferences.end());
}

SCEVReference *SCEVReference::getChildReference(int32_t idx) {
  return this->childReferences.at(idx);
}

int32_t SCEVReference::getNumChildReferences() {
  return this->childReferences.size();
}

void SCEVReference::addChildReference(SCEVReference *scevReference) {
  this->childReferences.push_back(scevReference);
}

std::set<SCEVReference *> SCEVReference::collectAllReferences() {
  std::set<SCEVReference *> references;
  references.insert(this);
  for (auto *child : this->childReferences) {
    auto childReferences = child->collectAllReferences();
    references.insert(childReferences.begin(), childReferences.end());
  }
  return references;
}
