#pragma once

#include <stdexcept>
#include <string>

namespace lotus::datalog {

class CompileError : public std::runtime_error {
public:
  explicit CompileError(const std::string &message)
      : std::runtime_error(message) {}
};

} // namespace lotus::datalog
