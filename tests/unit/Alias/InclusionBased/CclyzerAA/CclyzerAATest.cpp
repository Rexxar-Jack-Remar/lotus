#include "Alias/InclusionBased/CclyzerAA/CclyzerAA.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>
#include <vector>

using namespace llvm;
using namespace lotus;
using namespace lotus::cclyzer;
using namespace lotus::unittest;

class CclyzerAATest : public LlvmModuleTest {};

TEST_F(CclyzerAATest, ConfigurationAndOptions) {
  CclyzerAA aa;
  EXPECT_FALSE(aa.isInitialized());

  CclyzerOptions defaultOpts = aa.getOptions();
  EXPECT_EQ(defaultOpts.analysis, AnalysisKind::Subset);
  EXPECT_EQ(defaultOpts.context, ContextKind::Insensitive);
  EXPECT_FALSE(defaultOpts.checkAssertions);

  CclyzerOptions customOpts;
  customOpts.analysis = AnalysisKind::Unification;
  customOpts.context = ContextKind::CallSite2;
  customOpts.checkAssertions = true;

  aa.setOptions(customOpts);
  EXPECT_EQ(aa.getOptions().analysis, AnalysisKind::Unification);
  EXPECT_EQ(aa.getOptions().context, ContextKind::CallSite2);
  EXPECT_TRUE(aa.getOptions().checkAssertions);
}

TEST_F(CclyzerAATest, UninitializedBehavior) {
  CclyzerAA aa;
  std::vector<const Value *> pts;
  EXPECT_FALSE(aa.getPointsToSet(nullptr, pts));

  std::set<const Value *> nulls;
  EXPECT_FALSE(aa.getNullPtrSet(nulls));
  EXPECT_FALSE(aa.isNullPointer(nullptr));

  std::vector<const Value *> targets;
  EXPECT_FALSE(aa.getIndirectCallTargets(nullptr, targets));
}

TEST_F(CclyzerAATest, SimpleModuleAnalysis) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %y = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  CclyzerAA aa;
  if (CclyzerAA::isAvailable()) {
    EXPECT_TRUE(aa.run(*module));
    EXPECT_TRUE(aa.isInitialized());

    Function *F = module->getFunction("test");
    ASSERT_NE(F, nullptr);

    auto it = F->getEntryBlock().begin();
    Instruction *x = &*it++;
    Instruction *y = &*it++;

    EXPECT_EQ(aa.alias(x, y), AliasResult::NoAlias);
    EXPECT_FALSE(aa.mustAlias(x, y));
  } else {
    EXPECT_FALSE(aa.run(*module));
    EXPECT_FALSE(aa.isInitialized());
  }
}

TEST_F(CclyzerAATest, WrapperIntegration) {
  const char *source = R"(
    define void @test() {
      %a = alloca i32
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AAConfig cfgDefault = AAConfig::CclyzerAA_Default();
  EXPECT_EQ(cfgDefault.impl, AAConfig::Implementation::CclyzerAA);
  EXPECT_EQ(cfgDefault.getName(), "CclyzerAA(NoCtx)");

  AAConfig cfg1CFA = AAConfig::CclyzerAA_1CFA();
  EXPECT_EQ(cfg1CFA.impl, AAConfig::Implementation::CclyzerAA);
  EXPECT_EQ(cfg1CFA.getName(), "CclyzerAA(1-CFA)");

  if (CclyzerAA::isAvailable()) {
    AliasAnalysisWrapper wrapper(*module, cfgDefault);
    EXPECT_TRUE(wrapper.isInitialized());
  } else {
    AliasAnalysisWrapper wrapper(*module, cfgDefault);
    EXPECT_FALSE(wrapper.isInitialized());
  }
}
