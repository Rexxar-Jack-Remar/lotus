#pragma once

#include "Dataflow/Datalog/Core/Aggregate.h"
#include "Dataflow/Datalog/Core/Atom.h"
#include "Dataflow/Datalog/Semantic/SemanticIR.h"

#include <vector>

namespace lotus::datalog {

class Body {
public:
  Body() = delete;

  Context *context() const { return context_; }
  const std::vector<BodyItemIR> &items() const { return items_; }

private:
  explicit Body(const Atom &atom)
      : context_(atom.context()), items_{atom.ir()} {}
  explicit Body(const Condition &condition)
      : context_(condition.context()), items_{condition.ir()} {}
  explicit Body(const Negation &negation)
      : context_(negation.context()), items_{negation.ir()} {}
  explicit Body(const AggregateClause &aggregate)
      : context_(aggregate.context()), items_{aggregate.ir()} {}

  void append(Context *context, BodyItemIR item);

  Context *context_ = nullptr;
  std::vector<BodyItemIR> items_;

  friend Body operator&&(const Atom &, const Atom &);
  friend Body operator&&(Body, const Atom &);
  friend Body operator&&(const Condition &, const Atom &);
  friend Body operator&&(const Atom &, const Condition &);
  friend Body operator&&(Body, const Condition &);
  friend Body operator&&(const Negation &, const Atom &);
  friend Body operator&&(const Atom &, const Negation &);
  friend Body operator&&(Body, const Negation &);
  friend Body operator&&(const Atom &, const AggregateClause &);
  friend Body operator&&(Body, const AggregateClause &);
  friend Body operator&&(const AggregateClause &, const Atom &);
  friend class Program;
};

Body operator&&(const Atom &lhs, const Atom &rhs);
Body operator&&(Body lhs, const Atom &rhs);
Body operator&&(const Condition &lhs, const Atom &rhs);
Body operator&&(const Atom &lhs, const Condition &rhs);
Body operator&&(Body lhs, const Condition &rhs);
Body operator&&(const Negation &lhs, const Atom &rhs);
Body operator&&(const Atom &lhs, const Negation &rhs);
Body operator&&(Body lhs, const Negation &rhs);
Body operator&&(const Atom &lhs, const AggregateClause &rhs);
Body operator&&(Body lhs, const AggregateClause &rhs);
Body operator&&(const AggregateClause &lhs, const Atom &rhs);

using body = Body;
using rule = RuleIR;

} // namespace lotus::datalog
