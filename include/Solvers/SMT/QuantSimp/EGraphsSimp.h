#pragma once

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <z3++.h>

namespace EGraphs {
// This is a sort of DataContract, it only stores data
class QuantifierArgs {
public:
  bool is_forall;
  unsigned weight;
  unsigned num_patterns;
  unsigned num_decls;
  Z3_sort *sorts;
  Z3_symbol *decl_names;
  Z3_pattern *patterns;

  QuantifierArgs(Z3_ast ast, z3::context *ctx);
  ~QuantifierArgs();
};

class Function {
public:
  std::vector<Function *> *UsedBy;
  Function *Parent;
  std::vector<Function *> *Inputs;
  Z3_func_decl Value;
  bool IsQuantifier;
  QuantifierArgs *quantifierArgs;
  bool IsBoundVar;
  Z3_ast boundVar;

public:
  Function(std::vector<Function *> *inputs, z3::expr value);
  Function(QuantifierArgs *args, Function *body);
  Function(Z3_ast boundVar);
  void ManualDestroy();

public:
  Z3_func_decl getName() const;
  Function *GetRoot();
  // tests complete equality, as in exact same node
  bool operator==(const Function &other) const;
  bool operator!=(const Function &other) const;
  bool IsEquivalent(Function *other);
  bool IsCongruent(Function *other);
};

// used to not create multiple nodes for the same term/formula
bool TryGetRealFunction(
    Function *function,
    std::map<Z3_func_decl, std::vector<Function *> *> functions,
    Function **outFunction);

class EGraph {

  std::map<Z3_func_decl, std::vector<Function *> *> _functions;
  std::map<Function *, std::vector<Function *> *> _class;
  std::vector<Function *> _in_equalities;
  std::set<Function *> _quantified_variables;
  z3::context *ctx;

  EGraph(z3::context *ctx);
  ~EGraph();

  // functions for parsing/transforming z3::expr to EGraph
  static EGraph *ExprToEGraph(z3::expr expr, z3::context *ctx);
  void ParseAnd(z3::expr expr);
  void ParseEq(z3::expr expr);
  void ParsePredicate(z3::expr expr);
  // this is eventually called by the above functions and it returns the created
  // node for this reason
  Function *ParseOther(z3::expr expr);

  // functions for building z3::expr
  z3::expr ToFormula(std::map<Function *, Function *> *repr,
                     std::set<Function *> *core);
  z3::expr NodeToTerm(Function *node, std::map<Function *, Function *> *repr);

  // add components
  Function *AddQuantifiedVariable(z3::expr value);
  Function *AddTerm(z3::expr value);
  Function *AddQuantifier(QuantifierArgs *args, Function *body);
  Function *AddBoundVar(z3::expr expr);
  void AddPredicate(std::vector<Function *> *functions, z3::expr value);
  void AddEquality(Function *first, Function *second, z3::expr value);
  Function *AddFunction(std::vector<Function *> *inputs, z3::expr value);

  // functions for equalities
  void MakeEqual(Function *first, Function *second);
  void CheckEqualities(Function *func);

  // QEL functions
  std::set<Function *> *FindCore(std::map<Function *, Function *> *repr);
  bool IsGround(Function *function);
  std::map<Function *, Function *> *FindDefs();
  std::map<Function *, Function *> *
  AssignRepresentatives(std::map<Function *, Function *> *repr,
                        std::vector<Function *> toBeAssigned);
  bool MakesCycle(Function *NewGround, std::map<Function *, Function *> *repr);
  bool MakesCycleAux(Function *NewGround,
                     std::map<Function *, Function *> *repr, Function *current,
                     std::map<Function *, bool> *ColoredGraph);
  std::map<Function *, Function *> *
  RefineDefs(std::map<Function *, Function *> *repr);

public:
  // this is QEL
  static z3::expr Simplify(z3::expr expr, z3::context *ctx);
};
} // namespace EGraphs

