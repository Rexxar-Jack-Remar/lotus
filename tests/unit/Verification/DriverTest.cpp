#include "Verification/Driver/Backend.h"

#include <algorithm>

#include <gtest/gtest.h>

using namespace lotus::verification::driver;

TEST(DriverRegistryTest, AvailableDrivers) {
  DriverRegistry &reg = DriverRegistry::instance();
  auto drivers = reg.availableDrivers();
  
  EXPECT_GT(drivers.size(), 0u);
  EXPECT_NE(std::find(drivers.begin(), drivers.end(), "seahorn"), drivers.end());
  EXPECT_NE(std::find(drivers.begin(), drivers.end(), "clam"), drivers.end());
}

TEST(DriverRegistryTest, CreateDriver) {
  DriverRegistry &reg = DriverRegistry::instance();
  
  auto seahorn = reg.create("seahorn");
  ASSERT_NE(seahorn, nullptr);
  EXPECT_STREQ(seahorn->name(), "seahorn");
  
  auto clam = reg.create("clam");
  ASSERT_NE(clam, nullptr);
  EXPECT_STREQ(clam->name(), "clam");
  
  auto invalid = reg.create("nonexistent");
  EXPECT_EQ(invalid, nullptr);
}

TEST(DriverRegistryTest, RecommendDrivers) {
  DriverRegistry &reg = DriverRegistry::instance();
  
  auto memSafetyDrivers = reg.recommend(PropertyClass::MemSafety);
  EXPECT_GT(memSafetyDrivers.size(), 0u);
  
  auto reachabilityDrivers = reg.recommend(PropertyClass::Reachability);
  EXPECT_GT(reachabilityDrivers.size(), 0u);
}

TEST(DriverTest, SeahornSupportsAllProperties) {
  DriverRegistry &reg = DriverRegistry::instance();
  auto driver = reg.create("seahorn");
  ASSERT_NE(driver, nullptr);
  
  EXPECT_TRUE(driver->supports(PropertyClass::Reachability));
  EXPECT_TRUE(driver->supports(PropertyClass::MemSafety));
  EXPECT_TRUE(driver->supports(PropertyClass::Overflow));
  EXPECT_TRUE(driver->supports(PropertyClass::Termination));
}

TEST(DriverTest, ParseSeahornResult) {
  DriverRegistry &reg = DriverRegistry::instance();
  auto driver = reg.create("seahorn");
  ASSERT_NE(driver, nullptr);
  
  // Test safe result
  VerificationResultInfo safe = driver->parseResult("unsat\n", 0);
  EXPECT_EQ(safe.result, VerificationResult::True);
  EXPECT_TRUE(safe.isSafe());
  
  // Test unsafe result
  VerificationResultInfo unsafe = driver->parseResult("sat\nError found\n", 0);
  EXPECT_EQ(unsafe.result, VerificationResult::False);
  EXPECT_TRUE(unsafe.hasError());
  
  // Test timeout
  VerificationResultInfo timeout = driver->parseResult("timeout\n", 124);
  EXPECT_EQ(timeout.result, VerificationResult::Timeout);
}

TEST(DriverTest, ParseClamResult) {
  DriverRegistry &reg = DriverRegistry::instance();
  auto driver = reg.create("clam");
  ASSERT_NE(driver, nullptr);
  
  VerificationResultInfo safe = driver->parseResult("safe\n", 0);
  EXPECT_EQ(safe.result, VerificationResult::True);
  
  VerificationResultInfo unsafe = driver->parseResult("unsafe\n", 0);
  EXPECT_EQ(unsafe.result, VerificationResult::False);
}

TEST(PropertyClassTest, ParsePropertyClass) {
  EXPECT_EQ(parsePropertyClass("unreach-call"), PropertyClass::Reachability);
  EXPECT_EQ(parsePropertyClass("memsafety"), PropertyClass::MemSafety);
  EXPECT_EQ(parsePropertyClass("overflow"), PropertyClass::Overflow);
  EXPECT_EQ(parsePropertyClass("termination"), PropertyClass::Termination);
  EXPECT_EQ(parsePropertyClass("unknown"), PropertyClass::Unknown);
}

TEST(PropertyClassTest, ToStringPropertyClass) {
  EXPECT_EQ(toString(PropertyClass::Reachability), "reachability");
  EXPECT_EQ(toString(PropertyClass::MemSafety), "memsafety");
  EXPECT_EQ(toString(PropertyClass::Overflow), "overflow");
}

TEST(VerificationResultTest, ParseResultFromString) {
  EXPECT_EQ(parseResultFromString("true"), VerificationResult::True);
  EXPECT_EQ(parseResultFromString("safe"), VerificationResult::True);
  EXPECT_EQ(parseResultFromString("false"), VerificationResult::False);
  EXPECT_EQ(parseResultFromString("unsafe"), VerificationResult::False);
  EXPECT_EQ(parseResultFromString("timeout"), VerificationResult::Timeout);
  EXPECT_EQ(parseResultFromString("error"), VerificationResult::Error);
}

TEST(VerificationResultTest, ToStringResult) {
  EXPECT_EQ(toString(VerificationResult::True), "true");
  EXPECT_EQ(toString(VerificationResult::False), "false");
  EXPECT_EQ(toString(VerificationResult::Unknown), "unknown");
  EXPECT_EQ(toString(VerificationResult::Timeout), "timeout");
  EXPECT_EQ(toString(VerificationResult::Error), "error");
}
