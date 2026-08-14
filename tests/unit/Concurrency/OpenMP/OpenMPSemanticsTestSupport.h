#pragma once

#include "Concurrency/OpenMP/OpenMPSemantics.h"
#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace OpenMP;

class OpenMPSemanticsTest : public lotus::unittest::LlvmModuleTest {};
