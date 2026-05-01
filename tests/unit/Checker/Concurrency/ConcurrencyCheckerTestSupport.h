#pragma once

/**
 * @file ConcurrencyCheckerTest.cpp
 * @brief Unit tests for Concurrency checker
 *
 * The Concurrency checker detects concurrency-related bugs including:
 * - Data races
 * - Deadlocks
 * - Atomicity violations
 * - Lock mismatches
 */

#include "Checker/Concurrency/ConcurrencyChecker.h"

#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

using namespace llvm;

class ConcurrencyCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    return lotus::unittest::parseModule(context, source,
                                        "ConcurrencyCheckerTest");
  }
};

// Test 1: Lock acquire/release pattern

// Test 2: Thread creation

// Test 3: Shared variable access

// Test 4: Lock order inconsistency

// Test 5: Atomic operation

// Test 6: Thread join

// Test 7: Condition variable wait

// Test 8: Once initialization

// Test 9: Memory fence

// Test 10: Compare-and-swap







































