#pragma once

#include "Dataflow/IFDS/IFDSFramework.h"

#include <llvm/ADT/Optional.h>
#include <set>

namespace ifds {

struct GLCAValue {
  enum Kind { Bottom, ConstantSet, Top } kind;
  std::set<int64_t> constants;

  GLCAValue() : kind(Bottom) {}
  explicit GLCAValue(Kind k) : kind(k) {}
  explicit GLCAValue(int64_t c) : kind(ConstantSet), constants{c} {}
  explicit GLCAValue(std::set<int64_t> c) : kind(ConstantSet), constants(std::move(c)) {}

  static GLCAValue bottom() { return GLCAValue(Bottom); }
  static GLCAValue top() { return GLCAValue(Top); }
  static GLCAValue singleton(int64_t c) { return GLCAValue(c); }

  bool operator==(const GLCAValue &other) const {
    return kind == other.kind && constants == other.constants;
  }
};

class IDEGeneralizedLCA
    : public DefaultNoAliasIDEProblem<const llvm::Value *, GLCAValue> {
public:
  using Fact = const llvm::Value *;
  using Value = GLCAValue;

  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction *stmt, const Fact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override;
  FactSet return_flow(const llvm::CallBase *call, const llvm::Function *callee,
                      const Fact &exit_fact, const Fact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const Fact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;

  Value top_value() const override { return Value::top(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override;

  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const Fact &src_fact,
                                    const Fact &tgt_fact) override;
  EdgeFunction call_edge_function(const llvm::CallBase *call,
                                  const Fact &src_fact,
                                  const Fact &tgt_fact) override;
  EdgeFunction return_edge_function(const llvm::CallBase *call,
                                    const Fact &exit_fact,
                                    const Fact &ret_fact) override;
  EdgeFunction call_to_return_edge_function(const llvm::CallBase *call,
                                            const Fact &src_fact,
                                            const Fact &tgt_fact) override;

private:
  static llvm::Optional<int64_t> as_const(const llvm::Value *v);
  static llvm::Optional<int64_t> apply_binop(unsigned opcode, int64_t a, int64_t b);
  static Value cap_constants(std::set<int64_t> values);
  static constexpr size_t kMaxSetSize = 8;
};

} // namespace ifds
