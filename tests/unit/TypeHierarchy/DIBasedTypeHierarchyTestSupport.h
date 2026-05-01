#pragma once

/**
 * @file DIBasedTypeHierarchyTest.cpp
 * @brief Unit tests for DIBasedTypeHierarchy
 *
 * This file contains comprehensive tests for the DIBasedTypeHierarchy class,
 * which reconstructs C++ type hierarchies from LLVM IR with debug information.
 * Tests are migrated from PhasarLLVM TypeHierarchy tests.
 */

#include "Analysis/TypeHirarchy/DIBasedTypeHierarchy.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace lotus;

namespace {

using lotus::unittest::loadModule;

// Helper function to get test file path
std::string getTestFilePath(const std::string &FileName) {
  return std::string(LOTUS_TYPE_HIERARCHY_LL_DIR) + "/" + FileName;
}

/*
---------------------------
BasicTHReconstruction Tests
---------------------------
*/
















/*
*/









/*
--------------------------------
TransitivelyReachableTypes Tests
--------------------------------
*/
























} // namespace
