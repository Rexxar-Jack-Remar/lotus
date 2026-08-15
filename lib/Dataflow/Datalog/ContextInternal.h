#pragma once

#include "Dataflow/Datalog/Context.h"
#include "Dataflow/Datalog/Internal.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace lotus::datalog {

struct Context::Impl {
  std::vector<std::unique_ptr<internal::RelationStorage>> relations;
  std::vector<internal::VariableDefinition> variables;
  std::unordered_set<std::string> relation_names;
  std::mutex execution_mutex;
  bool running = false;
};

} // namespace lotus::datalog
