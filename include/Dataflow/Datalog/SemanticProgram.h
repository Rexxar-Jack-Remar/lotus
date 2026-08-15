#pragma once

#include "Dataflow/Datalog/Context.h"
#include "Dataflow/Datalog/Program.h"
#include "Dataflow/Datalog/SemanticIR.h"

#include <any>
#include <string>
#include <typeindex>
#include <vector>

namespace lotus::datalog {

// Low-level bridge for serialized or non-template frontends. Native C++ users
// should prefer Context, Relation<T...>, Var<T>, and Program.
class SemanticProgram {
public:
  SemanticProgram();
  ~SemanticProgram();

  SemanticProgram(const SemanticProgram &) = delete;
  SemanticProgram &operator=(const SemanticProgram &) = delete;

  RelationId addRelation(
      std::string name, std::vector<ColumnType> columns,
      RelationKind kind = RelationKind::Set,
      std::function<bool(std::any &, const std::any &)> lattice_join = {});
  VarId addVariable(std::string name, std::type_index type,
                    bool anonymous = false);
  void addFact(RelationId relation, std::vector<std::any> row);
  void addRule(RuleIR rule);

  CompiledProgram compile() const;
  std::vector<std::vector<std::any>> rows(RelationId relation) const;

private:
  Context context_;
  Program program_;
};

} // namespace lotus::datalog
