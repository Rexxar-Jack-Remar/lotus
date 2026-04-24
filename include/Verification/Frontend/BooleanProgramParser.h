#pragma once

#include "Verification/Frontend/BooleanProgram.h"

namespace lotus {
namespace verification {
namespace frontend {

BooleanProgram parseBooleanProgram(const std::string &text);
BooleanProgram parseBooleanProgramFile(const std::string &path);

} // namespace frontend
} // namespace verification
} // namespace lotus
