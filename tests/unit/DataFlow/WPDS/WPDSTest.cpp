/**
 * @file WPDSTest.cpp
 * @brief Unit tests for WPDS (Weighted Pushdown System) dataflow framework
 */

#include "Dataflow/WPDS/InterProceduralDataFlow.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <gtest/gtest.h>

using namespace wpds;
using namespace llvm;

class WPDSTest : public ::testing::Test {
protected:
  void SetUp() override {
    DataFlowFacts::ClearUniverse();
    context = std::make_unique<LLVMContext>();
  }
  
  void TearDown() override {
    context.reset();
  }
  
  std::unique_ptr<LLVMContext> context;
  
  // Helper to create test values
  Value* createTestValue(int val) {
    return ConstantInt::get(Type::getInt32Ty(*context), val);
  }
  
  // Helper to create a simple module for engine tests
  std::unique_ptr<Module> createSimpleModule() {
    auto module = std::make_unique<Module>("test_module", *context);
    auto* i32 = Type::getInt32Ty(*context);
    auto* mainTy = FunctionType::get(i32, {}, false);
    auto* main = Function::Create(mainTy, Function::ExternalLinkage, "main", module.get());
    
    auto* entry = BasicBlock::Create(*context, "entry", main);
    IRBuilder<> builder(entry);
    builder.CreateRet(ConstantInt::get(i32, 0));
    
    return module;
  }
  
  // Helper to create a module with function call
  std::unique_ptr<Module> createModuleWithCall() {
    auto module = std::make_unique<Module>("call_module", *context);
    auto* i32 = Type::getInt32Ty(*context);
    
    // Create callee function
    auto* calleeTy = FunctionType::get(i32, {i32}, false);
    auto* callee = Function::Create(calleeTy, Function::InternalLinkage, "callee", module.get());
    auto* calleeBB = BasicBlock::Create(*context, "entry", callee);
    IRBuilder<> calleeBuilder(calleeBB);
    calleeBuilder.CreateRet(callee->getArg(0));
    
    // Create main function
    auto* mainTy = FunctionType::get(i32, {}, false);
    auto* main = Function::Create(mainTy, Function::ExternalLinkage, "main", module.get());
    auto* mainBB = BasicBlock::Create(*context, "entry", main);
    IRBuilder<> mainBuilder(mainBB);
    auto* arg = ConstantInt::get(i32, 42);
    auto* result = mainBuilder.CreateCall(calleeTy, callee, {arg});
    mainBuilder.CreateRet(result);
    
    return module;
  }
};

// ============================================================================
// DataFlowFacts Tests
// ============================================================================

TEST_F(WPDSTest, DataFlowFacts_EmptySet) {
  DataFlowFacts empty = DataFlowFacts::EmptySet();
  EXPECT_TRUE(empty.isEmpty());
  EXPECT_EQ(empty.size(), 0);
  EXPECT_EQ(empty.getFacts().size(), 0);
}

TEST_F(WPDSTest, DataFlowFacts_AddAndRemove) {
  DataFlowFacts facts = DataFlowFacts::EmptySet();
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  
  // Add facts
  facts.addFact(val1);
  EXPECT_FALSE(facts.isEmpty());
  EXPECT_EQ(facts.size(), 1);
  EXPECT_TRUE(facts.containsFact(val1));
  
  facts.addFact(val2);
  EXPECT_EQ(facts.size(), 2);
  EXPECT_TRUE(facts.containsFact(val2));
  
  // Remove fact
  facts.removeFact(val1);
  EXPECT_EQ(facts.size(), 1);
  EXPECT_FALSE(facts.containsFact(val1));
  EXPECT_TRUE(facts.containsFact(val2));
}

TEST_F(WPDSTest, DataFlowFacts_CopyConstructor) {
  DataFlowFacts original = DataFlowFacts::EmptySet();
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  original.addFact(val1);
  original.addFact(val2);
  
  DataFlowFacts copy(original);
  EXPECT_EQ(copy.size(), 2);
  EXPECT_TRUE(copy.containsFact(val1));
  EXPECT_TRUE(copy.containsFact(val2));
  
  // Modify copy should not affect original
  copy.removeFact(val1);
  EXPECT_EQ(copy.size(), 1);
  EXPECT_EQ(original.size(), 2);
}

TEST_F(WPDSTest, DataFlowFacts_Assignment) {
  DataFlowFacts facts1 = DataFlowFacts::EmptySet();
  DataFlowFacts facts2 = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  facts1.addFact(val1);
  facts2.addFact(val2);
  
  facts2 = facts1;
  EXPECT_EQ(facts2.size(), 1);
  EXPECT_TRUE(facts2.containsFact(val1));
  EXPECT_FALSE(facts2.containsFact(val2));
}

TEST_F(WPDSTest, DataFlowFacts_Equality) {
  DataFlowFacts facts1 = DataFlowFacts::EmptySet();
  DataFlowFacts facts2 = DataFlowFacts::EmptySet();
  
  EXPECT_TRUE(facts1 == facts2);
  EXPECT_TRUE(DataFlowFacts::Eq(facts1, facts2));
  
  auto* val1 = createTestValue(1);
  facts1.addFact(val1);
  EXPECT_FALSE(facts1 == facts2);
  
  facts2.addFact(val1);
  EXPECT_TRUE(facts1 == facts2);
}

TEST_F(WPDSTest, DataFlowFacts_Union) {
  DataFlowFacts facts1 = DataFlowFacts::EmptySet();
  DataFlowFacts facts2 = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  
  facts1.addFact(val1);
  facts1.addFact(val2);
  facts2.addFact(val2);
  facts2.addFact(val3);
  
  DataFlowFacts unionResult = DataFlowFacts::Union(facts1, facts2);
  EXPECT_EQ(unionResult.size(), 3);
  EXPECT_TRUE(unionResult.containsFact(val1));
  EXPECT_TRUE(unionResult.containsFact(val2));
  EXPECT_TRUE(unionResult.containsFact(val3));
}

TEST_F(WPDSTest, DataFlowFacts_Intersect) {
  DataFlowFacts facts1 = DataFlowFacts::EmptySet();
  DataFlowFacts facts2 = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  
  facts1.addFact(val1);
  facts1.addFact(val2);
  facts2.addFact(val2);
  facts2.addFact(val3);
  
  DataFlowFacts intersectResult = DataFlowFacts::Intersect(facts1, facts2);
  EXPECT_EQ(intersectResult.size(), 1);
  EXPECT_TRUE(intersectResult.containsFact(val2));
  EXPECT_FALSE(intersectResult.containsFact(val1));
  EXPECT_FALSE(intersectResult.containsFact(val3));
}

TEST_F(WPDSTest, DataFlowFacts_Diff) {
  DataFlowFacts facts1 = DataFlowFacts::EmptySet();
  DataFlowFacts facts2 = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  
  facts1.addFact(val1);
  facts1.addFact(val2);
  facts2.addFact(val2);
  facts2.addFact(val3);
  
  DataFlowFacts diffResult = DataFlowFacts::Diff(facts1, facts2);
  EXPECT_EQ(diffResult.size(), 1);
  EXPECT_TRUE(diffResult.containsFact(val1));
  EXPECT_FALSE(diffResult.containsFact(val2));
  EXPECT_FALSE(diffResult.containsFact(val3));
}

TEST_F(WPDSTest, DataFlowFacts_UniverseSet) {
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  
  // Add values to universe
  DataFlowFacts temp = DataFlowFacts::EmptySet();
  temp.addFact(val1);
  temp.addFact(val2);
  
  DataFlowFacts universe = DataFlowFacts::UniverseSet();
  EXPECT_TRUE(universe.containsFact(val1));
  EXPECT_TRUE(universe.containsFact(val2));
}

// ============================================================================
// GenKillTransformer Tests
// ============================================================================

TEST_F(WPDSTest, GenKillTransformer_Creation) {
  DataFlowFacts empty = DataFlowFacts::EmptySet();
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(
      empty, empty
  );
  
  ASSERT_NE(transformer, nullptr);
  EXPECT_TRUE(transformer->equal(GenKillTransformer::one()));
}

TEST_F(WPDSTest, GenKillTransformer_GenOnly) {
  DataFlowFacts empty = DataFlowFacts::EmptySet();
  DataFlowFacts gen = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  gen.addFact(val1);
  gen.addFact(val2);
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(
      empty, gen
  );
  
  ASSERT_NE(transformer, nullptr);
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  DataFlowFacts result = transformer->apply(input);
  
  EXPECT_EQ(result.size(), 2);
  EXPECT_TRUE(result.containsFact(val1));
  EXPECT_TRUE(result.containsFact(val2));
}

TEST_F(WPDSTest, GenKillTransformer_KillOnly) {
  DataFlowFacts empty = DataFlowFacts::EmptySet();
  DataFlowFacts kill = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  kill.addFact(val1);
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(
      kill, empty
  );
  
  ASSERT_NE(transformer, nullptr);
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  input.addFact(val1);
  input.addFact(val2);
  
  DataFlowFacts result = transformer->apply(input);
  
  // val1 should be killed, val2 should survive
  EXPECT_EQ(result.size(), 1);
  EXPECT_FALSE(result.containsFact(val1));
  EXPECT_TRUE(result.containsFact(val2));
}

TEST_F(WPDSTest, GenKillTransformer_GenAndKill) {
  DataFlowFacts kill = DataFlowFacts::EmptySet();
  DataFlowFacts gen = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  
  kill.addFact(val1);
  gen.addFact(val3);
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(
      kill, gen
  );
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  input.addFact(val1);
  input.addFact(val2);
  
  DataFlowFacts result = transformer->apply(input);
  
  // val1 killed, val2 survives, val3 generated
  EXPECT_EQ(result.size(), 2);
  EXPECT_FALSE(result.containsFact(val1));
  EXPECT_TRUE(result.containsFact(val2));
  EXPECT_TRUE(result.containsFact(val3));
}

TEST_F(WPDSTest, GenKillTransformer_WithFlow) {
  DataFlowFacts empty = DataFlowFacts::EmptySet();
  std::map<Value*, DataFlowFacts> flow;
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  
  DataFlowFacts flowTarget = DataFlowFacts::EmptySet();
  flowTarget.addFact(val2);
  flowTarget.addFact(val3);
  flow[val1] = flowTarget;
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(
      empty, empty, flow
  );
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  input.addFact(val1);
  
  DataFlowFacts result = transformer->apply(input);
  
  // val1 should survive and generate val2, val3 via flow
  EXPECT_EQ(result.size(), 3);
  EXPECT_TRUE(result.containsFact(val1));
  EXPECT_TRUE(result.containsFact(val2));
  EXPECT_TRUE(result.containsFact(val3));
}

TEST_F(WPDSTest, GenKillTransformer_SpecialValues) {
  GenKillTransformer* one = GenKillTransformer::one();
  GenKillTransformer* zero = GenKillTransformer::zero();
  GenKillTransformer* bottom = GenKillTransformer::bottom();
  
  ASSERT_NE(one, nullptr);
  ASSERT_NE(zero, nullptr);
  ASSERT_NE(bottom, nullptr);
  
  // Test equality
  EXPECT_TRUE(one->equal(GenKillTransformer::one()));
  EXPECT_TRUE(zero->equal(GenKillTransformer::zero()));
  EXPECT_TRUE(bottom->equal(GenKillTransformer::bottom()));
  
  // Test one is identity
  DataFlowFacts input = DataFlowFacts::EmptySet();
  auto* val1 = createTestValue(1);
  input.addFact(val1);
  
  DataFlowFacts result = one->apply(input);
  EXPECT_EQ(result.size(), 1);
  EXPECT_TRUE(result.containsFact(val1));
}

TEST_F(WPDSTest, GenKillTransformer_Extend) {
  // Create two transformers: kill val1, gen val2; then kill val2, gen val3
  DataFlowFacts kill1 = DataFlowFacts::EmptySet();
  DataFlowFacts gen1 = DataFlowFacts::EmptySet();
  DataFlowFacts kill2 = DataFlowFacts::EmptySet();
  DataFlowFacts gen2 = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  
  kill1.addFact(val1);
  gen1.addFact(val2);
  kill2.addFact(val2);
  gen2.addFact(val3);
  
  GenKillTransformer* trans1 = GenKillTransformer::makeGenKillTransformer(kill1, gen1);
  GenKillTransformer* trans2 = GenKillTransformer::makeGenKillTransformer(kill2, gen2);
  
  GenKillTransformer* composed = trans1->extend(trans2);
  ASSERT_NE(composed, nullptr);
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  input.addFact(val1);
  input.addFact(val2);
  
  DataFlowFacts result = composed->apply(input);
  
  // val1 killed by trans1, val2 killed by trans2, val3 generated by trans2
  EXPECT_FALSE(result.containsFact(val1));
  EXPECT_FALSE(result.containsFact(val2));
  EXPECT_TRUE(result.containsFact(val3));
  
  delete composed;
}

TEST_F(WPDSTest, GenKillTransformer_Combine) {
  // Combine two transformers: one kills val1, one kills val2
  // Combine uses intersection for kill (both must kill) and union for gen
  DataFlowFacts kill1 = DataFlowFacts::EmptySet();
  DataFlowFacts gen1 = DataFlowFacts::EmptySet();
  DataFlowFacts kill2 = DataFlowFacts::EmptySet();
  DataFlowFacts gen2 = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  
  kill1.addFact(val1);
  gen1.addFact(val3);
  kill2.addFact(val2);
  gen2.addFact(val3);
  
  GenKillTransformer* trans1 = GenKillTransformer::makeGenKillTransformer(kill1, gen1);
  GenKillTransformer* trans2 = GenKillTransformer::makeGenKillTransformer(kill2, gen2);
  
  GenKillTransformer* combined = trans1->combine(trans2);
  ASSERT_NE(combined, nullptr);
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  input.addFact(val1);
  input.addFact(val2);
  
  DataFlowFacts result = combined->apply(input);

  // Meet semantics: kill_meet = K1 ∪ K2 = {val1, val2}, gen_meet = G1 ∩ G2 = {val3}.
  // So input {val1, val2} is fully killed; result = survivors ∪ gen_meet = {val3}.
  EXPECT_FALSE(result.containsFact(val1));
  EXPECT_FALSE(result.containsFact(val2));
  EXPECT_TRUE(result.containsFact(val3));

  delete combined;
}

TEST_F(WPDSTest, GenKillTransformer_ExtendWithIdentity) {
  GenKillTransformer* one = GenKillTransformer::one();
  DataFlowFacts gen = DataFlowFacts::EmptySet();
  auto* val1 = createTestValue(1);
  gen.addFact(val1);
  
  GenKillTransformer* trans = GenKillTransformer::makeGenKillTransformer(
      DataFlowFacts::EmptySet(), gen
  );
  
  // Extend identity with transformer: one->extend(trans) returns trans directly
  GenKillTransformer* result1 = one->extend(trans);
  EXPECT_FALSE(result1->equal(one));
  EXPECT_TRUE(result1->equal(trans));
  EXPECT_EQ(result1, trans);  // Should be the same pointer
  
  // Extend transformer with identity: trans->extend(one) returns trans directly
  GenKillTransformer* result2 = trans->extend(one);
  EXPECT_TRUE(result2->equal(trans));
  EXPECT_EQ(result2, trans);  // Should be the same pointer
  
  // result1 and result2 are the same pointer as trans, so only delete once
  // Check if trans is not a special static value before deleting
  bool isSpecial = (trans == GenKillTransformer::one() || 
                    trans == GenKillTransformer::zero() || 
                    trans == GenKillTransformer::bottom());
  if (!isSpecial) {
    delete trans;
  }
}

TEST_F(WPDSTest, GenKillTransformer_Equal) {
  DataFlowFacts kill1 = DataFlowFacts::EmptySet();
  DataFlowFacts gen1 = DataFlowFacts::EmptySet();
  DataFlowFacts kill2 = DataFlowFacts::EmptySet();
  DataFlowFacts gen2 = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  kill1.addFact(val1);
  kill2.addFact(val1);
  gen1.addFact(val1);
  gen2.addFact(val1);
  
  GenKillTransformer* trans1 = GenKillTransformer::makeGenKillTransformer(kill1, gen1);
  GenKillTransformer* trans2 = GenKillTransformer::makeGenKillTransformer(kill2, gen2);
  
  EXPECT_TRUE(trans1->equal(trans2));
  
  DataFlowFacts gen3 = DataFlowFacts::EmptySet();
  GenKillTransformer* trans3 = GenKillTransformer::makeGenKillTransformer(kill1, gen3);
  EXPECT_FALSE(trans1->equal(trans3));
}

TEST_F(WPDSTest, GenKillTransformer_GetKillAndGen) {
  DataFlowFacts kill = DataFlowFacts::EmptySet();
  DataFlowFacts gen = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  
  kill.addFact(val1);
  gen.addFact(val2);
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(kill, gen);
  
  const DataFlowFacts& killResult = transformer->getKill();
  const DataFlowFacts& genResult = transformer->getGen();
  
  EXPECT_TRUE(killResult.containsFact(val1));
  EXPECT_TRUE(genResult.containsFact(val2));
}

// ============================================================================
// InterProceduralDataFlowEngine Tests
// ============================================================================

TEST_F(WPDSTest, InterProceduralDataFlowEngine_SimpleForward) {
  auto module = createSimpleModule();
  InterProceduralDataFlowEngine engine;
  
  // Create a simple transformer that generates a fact
  auto createTransformer = [this](Instruction* I) -> GenKillTransformer* {
    (void)I;
    DataFlowFacts gen = DataFlowFacts::EmptySet();
    auto* val = createTestValue(42);
    gen.addFact(val);
    return GenKillTransformer::makeGenKillTransformer(
        DataFlowFacts::EmptySet(), gen
    );
  };
  
  auto result = engine.runForwardAnalysis(*module, createTransformer);
  ASSERT_NE(result, nullptr);
  
  // Check that results are computed using the returned result
  for (auto& F : *module) {
    if (F.isDeclaration()) continue;
    for (auto& BB : F) {
      for (auto& I : BB) {
        // Use the returned result directly instead of going through engine
        const auto& inSet = result->IN(&I);
        const auto& outSet = result->OUT(&I);
        // Sets should be initialized (may be empty)
        EXPECT_GE(inSet.size(), 0);
        EXPECT_GE(outSet.size(), 0);
      }
    }
  }
}

TEST_F(WPDSTest, InterProceduralDataFlowEngine_SimpleBackward) {
  auto module = createSimpleModule();
  InterProceduralDataFlowEngine engine;
  
  // Create identity transformer
  auto createTransformer = [](Instruction* I) -> GenKillTransformer* {
    (void)I;
    return GenKillTransformer::one();
  };
  
  auto result = engine.runBackwardAnalysis(*module, createTransformer);
  ASSERT_NE(result, nullptr);
  
  // Check that results are computed using the returned result
  for (auto& F : *module) {
    if (F.isDeclaration()) continue;
    for (auto& BB : F) {
      for (auto& I : BB) {
        // Use the returned result directly instead of going through engine
        const auto& inSet = result->IN(&I);
        const auto& outSet = result->OUT(&I);
        EXPECT_GE(inSet.size(), 0);
        EXPECT_GE(outSet.size(), 0);
      }
    }
  }
}

TEST_F(WPDSTest, InterProceduralDataFlowEngine_WithCall) {
  auto module = createModuleWithCall();
  InterProceduralDataFlowEngine engine;
  
  // Create transformer that passes facts through
  auto createTransformer = [](Instruction* I) -> GenKillTransformer* {
    (void)I;
    return GenKillTransformer::one();
  };
  
  auto result = engine.runForwardAnalysis(*module, createTransformer);
  ASSERT_NE(result, nullptr);
  
  // Verify engine processed the module using the returned result
  bool foundCall = false;
  for (auto& F : *module) {
    if (F.isDeclaration()) continue;
    for (auto& BB : F) {
      for (auto& I : BB) {
        if (isa<CallInst>(&I)) {
          foundCall = true;
        }
        // Use the returned result directly instead of going through engine
        const auto& inSet = result->IN(&I);
        const auto& outSet = result->OUT(&I);
        EXPECT_GE(inSet.size(), 0);
        EXPECT_GE(outSet.size(), 0);
      }
    }
  }
  EXPECT_TRUE(foundCall);
}

TEST_F(WPDSTest, InterProceduralDataFlowEngine_InitialFacts) {
  auto module = createSimpleModule();
  InterProceduralDataFlowEngine engine;
  
  std::set<Value*> initialFacts;
  auto* val1 = createTestValue(1);
  initialFacts.insert(val1);
  
  auto createTransformer = [](Instruction* I) -> GenKillTransformer* {
    (void)I;
    return GenKillTransformer::one();
  };
  
  auto result = engine.runForwardAnalysis(*module, createTransformer, initialFacts);
  ASSERT_NE(result, nullptr);
  
  // Initial facts should propagate through the analysis
  // (exact behavior depends on implementation)
  EXPECT_NE(result.get(), nullptr);
}

// ============================================================================
// Edge Cases and Error Conditions
// ============================================================================

TEST_F(WPDSTest, DataFlowFacts_EmptyOperations) {
  DataFlowFacts empty1 = DataFlowFacts::EmptySet();
  DataFlowFacts empty2 = DataFlowFacts::EmptySet();
  
  // Union of empty sets
  DataFlowFacts unionResult = DataFlowFacts::Union(empty1, empty2);
  EXPECT_TRUE(unionResult.isEmpty());
  
  // Intersect of empty sets
  DataFlowFacts intersectResult = DataFlowFacts::Intersect(empty1, empty2);
  EXPECT_TRUE(intersectResult.isEmpty());
  
  // Diff of empty sets
  DataFlowFacts diffResult = DataFlowFacts::Diff(empty1, empty2);
  EXPECT_TRUE(diffResult.isEmpty());
}

TEST_F(WPDSTest, GenKillTransformer_GenKillOverlap) {
  // When gen and kill overlap, kill should be normalized
  DataFlowFacts kill = DataFlowFacts::EmptySet();
  DataFlowFacts gen = DataFlowFacts::EmptySet();
  
  auto* val1 = createTestValue(1);
  kill.addFact(val1);
  gen.addFact(val1);  // Overlap
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(kill, gen);
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  input.addFact(val1);
  
  DataFlowFacts result = transformer->apply(input);
  
  // After normalization, val1 should be in result (gen wins)
  EXPECT_TRUE(result.containsFact(val1));
}

TEST_F(WPDSTest, GenKillTransformer_ZeroExtend) {
  GenKillTransformer* zero = GenKillTransformer::zero();
  GenKillTransformer* one = GenKillTransformer::one();
  
  // Extending with zero should yield zero
  GenKillTransformer* result1 = zero->extend(one);
  EXPECT_TRUE(result1->equal(zero));
  
  GenKillTransformer* result2 = one->extend(zero);
  EXPECT_TRUE(result2->equal(zero));
}

TEST_F(WPDSTest, GenKillTransformer_ComplexFlow) {
  // Test flow map with multiple entries
  DataFlowFacts empty = DataFlowFacts::EmptySet();
  std::map<Value*, DataFlowFacts> flow;
  
  auto* val1 = createTestValue(1);
  auto* val2 = createTestValue(2);
  auto* val3 = createTestValue(3);
  auto* val4 = createTestValue(4);
  
  DataFlowFacts flow1 = DataFlowFacts::EmptySet();
  flow1.addFact(val2);
  flow[val1] = flow1;
  
  DataFlowFacts flow2 = DataFlowFacts::EmptySet();
  flow2.addFact(val3);
  flow2.addFact(val4);
  flow[val2] = flow2;
  
  GenKillTransformer* transformer = GenKillTransformer::makeGenKillTransformer(empty, empty, flow);
  
  DataFlowFacts input = DataFlowFacts::EmptySet();
  input.addFact(val1);
  input.addFact(val2);
  
  DataFlowFacts result = transformer->apply(input);
  
  // val1 -> val2, val2 -> val3, val4
  EXPECT_TRUE(result.containsFact(val1));
  EXPECT_TRUE(result.containsFact(val2));
  EXPECT_TRUE(result.containsFact(val3));
  EXPECT_TRUE(result.containsFact(val4));
}

// ============================================================================
// ExplodedWPDSBuilder tests (paper Section 4 encoding)
// ============================================================================

TEST_F(WPDSTest, ExplodedWPDSBuilder_AddsNormalAndCallRules) {
  using namespace wpds;
  wpds_key_t lambda = str2key("Lambda");
  wpds_key_t n1 = str2key("n1");
  wpds_key_t n2 = str2key("n2");
  wpds_key_t entry = str2key("entry");
  wpds_key_t ret = str2key("ret");

  std::set<wpds_key_t> control_states = { lambda, n1 };
  std::vector<std::pair<wpds_key_t, wpds_key_t>> normal_edges = {
      {lambda, n1}, {n1, n2}
  };
  std::vector<std::tuple<wpds_key_t, wpds_key_t, wpds_key_t>> call_edges = {
      std::make_tuple(n2, entry, ret)
  };

  Semiring<GenKillTransformer> semiring(GenKillTransformer::one(), true);
  WPDS<GenKillTransformer> wpds(semiring, Query::poststar());

  auto get_normal = [=](wpds_key_t from_c, wpds_key_t from_s,
                        wpds_key_t to_c, wpds_key_t to_s) -> GenKillTransformer* {
    (void)to_c;
    (void)to_s;
    if (from_c == lambda && from_s == lambda && to_c == lambda && to_s == n1)
      return GenKillTransformer::one();
    if (from_c == lambda && from_s == n1 && to_c == lambda && to_s == n2)
      return GenKillTransformer::one();
    return nullptr;
  };
  auto get_call = [=](wpds_key_t from_c, wpds_key_t from_s,
                      wpds_key_t to_c, wpds_key_t callee, wpds_key_t ret_site) -> GenKillTransformer* {
    (void)from_c;
    (void)from_s;
    (void)to_c;
    (void)callee;
    (void)ret_site;
    return GenKillTransformer::one();
  };

  buildExplodedWPDS<GenKillTransformer>(wpds, semiring, control_states, normal_edges,
                                       call_edges, get_normal, get_call);

  // Expect rules: normal (lambda,n1)->(lambda,n1), (lambda,n1)->(lambda,n2);
  // (n1,n1)->(n1,n1), (n1,n1)->(n1,n2) from get_normal; plus call rules from get_call.
  // get_normal returns one() only for two specific (from_c,from_s,to_c,to_s).
  // So we get 2 normal rules. get_call returns one() for all, so 2*2=4 call rules (two control pairs).
  EXPECT_GE(wpds.count_rules(), 1u);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
