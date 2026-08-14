#pragma once

#include "Concurrency/LinuxKernel/LinuxKernelAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <set>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace kernel;
using namespace lotus::unittest;

class LinuxKernelAnalysisTest : public LlvmModuleTest {};
