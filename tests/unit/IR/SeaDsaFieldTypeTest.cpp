#include "Alias/UnificationBased/seadsa/FieldType.hh"
#include "Alias/UnificationBased/seadsa/Graph.hh"

#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;

namespace {

TEST(SeaDsaFieldTypeTest, TracksGlobalTypeAwareFlagConsistently) {
  LLVMContext context;
  Type *intPtrTy = Type::getInt32PtrTy(context);

  const bool oldValue = seadsa::g_IsTypeAware;

  seadsa::g_IsTypeAware = false;
  EXPECT_TRUE(seadsa::FieldType::IsNotTypeAware());
  EXPECT_TRUE(seadsa::FieldType(intPtrTy).isUnknown());

  seadsa::g_IsTypeAware = true;
  EXPECT_FALSE(seadsa::FieldType::IsNotTypeAware());
  EXPECT_TRUE(seadsa::FieldType(intPtrTy).isPointer());

  seadsa::g_IsTypeAware = oldValue;
}

} // namespace
