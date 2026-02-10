/*
 * Prototype IDE Analysis (ported from Phasar IDEProtoAnalysis)
 *
 * This client is intentionally minimal and mainly serves as a reference
 * implementation for the IDEProblem interface.
 */

#pragma once

#include "Dataflow/IFDS/IFDSFramework.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace ifds {

class IDEProtoAnalysis : public DefaultNoAliasIDEProblem<const llvm::Value*, const llvm::Value*> {
public:
    using Fact = const llvm::Value*;
    using Value = const llvm::Value*;

    // IFDS interface
    Fact zero_fact() const override { return nullptr; }
    FactSet normal_flow(const llvm::Instruction* /*stmt*/, const Fact& fact) override;
    FactSet call_flow(const llvm::CallBase* /*call*/, const llvm::Function* /*callee*/,
                      const Fact& fact) override;
    FactSet return_flow(const llvm::CallBase* /*call*/, const llvm::Function* /*callee*/,
                        const Fact& /*exit_fact*/, const Fact& call_fact) override;
    FactSet call_to_return_flow(const llvm::CallBase* /*call*/, const Fact& fact) override;
    FactSet initial_facts(const llvm::Function* /*main*/) override;

    // Value domain
    Value top_value() const override { return nullptr; }
    Value bottom_value() const override { return nullptr; }
    Value join(const Value& /*v1*/, const Value& /*v2*/) const override { return nullptr; }

    // Edge functions (identity)
    EdgeFunction normal_edge_function(const llvm::Instruction* /*stmt*/, const Fact& /*src_fact*/,
                                      const Fact& /*tgt_fact*/) override;
    EdgeFunction call_edge_function(const llvm::CallBase* /*call*/, const Fact& /*src_fact*/,
                                    const Fact& /*tgt_fact*/) override;
    EdgeFunction return_edge_function(const llvm::CallBase* /*call*/, const Fact& /*exit_fact*/,
                                      const Fact& /*ret_fact*/) override;
    EdgeFunction call_to_return_edge_function(const llvm::CallBase* /*call*/, const Fact& /*src_fact*/,
                                              const Fact& /*tgt_fact*/) override;
};

} // namespace ifds
