#pragma once

#include "Dataflow/Datalog/Context.h"
#include "Dataflow/Datalog/Internal.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace lotus::datalog {

struct Context::Impl {
  std::vector<std::unique_ptr<internal::RelationStorage>> relations;
  std::vector<internal::VariableDefinition> variables;
  std::unordered_set<std::string> relation_names;
};

} // namespace lotus::datalog
