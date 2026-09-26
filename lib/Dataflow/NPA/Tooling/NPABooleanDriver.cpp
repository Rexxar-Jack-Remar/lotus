#include "Dataflow/NPA/Tooling/NPABooleanDriver.h"

#include "Dataflow/NPA/NPA.h"
#include "Verification/Frontend/BooleanNPAProgram.h"
#include "Verification/Frontend/BooleanProgramParser.h"

#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace {

using D = npa::PredicateRelationDomain;
using Program = lotus::verification::frontend::BooleanNPAProgram;

npa::LinearStrategy linearStrategy(const std::string &name) {
  if (name == "tensor")
    return npa::LinearStrategy::TensorProduct;
  if (name == "adaptive_scc")
    return npa::LinearStrategy::AdaptiveScc;
  return npa::LinearStrategy::SCC;
}

npa::NewtonRoundStrategy roundStrategy(const std::string &name) {
  if (name == "static")
    return npa::NewtonRoundStrategy::Static;
  if (name == "always_maybe")
    return npa::NewtonRoundStrategy::AlwaysMaybe;
  if (name == "sparse")
    return npa::NewtonRoundStrategy::Sparse;
  return npa::NewtonRoundStrategy::Dense;
}

} // namespace

int runNpaBoolean(const std::string &input_filename, llvm::raw_ostream &os,
                  const std::string &solver, const std::string &linear_solver,
                  const std::string &newton_round, const std::string &entry,
                  const std::string &query,
                  bool print_block_results, bool require_tensor,
                  bool prepare_only, bool verify_solvers) {
  try {
    const auto start = std::chrono::steady_clock::now();
    const auto source =
        lotus::verification::frontend::parseBooleanProgramFile(input_filename);
    Program program =
        lotus::verification::frontend::buildBooleanNPAProgram(source, entry);
    std::optional<std::pair<unsigned, unsigned>> query_node;
    if (!query.empty()) {
      const auto separator = query.find(':');
      if (separator == std::string::npos)
        throw std::invalid_argument("--bp-query needs <procedure>:<label>");
      const auto procedure_name = query.substr(0, separator);
      const auto label = query.substr(separator + 1);
      for (unsigned p = 0; p < program.lowered.procedures.size(); ++p) {
        const auto &lowered = program.lowered.procedures[p];
        if (lowered.procedure != procedure_name)
          continue;
        auto alias = lowered.label_aliases.find(label);
        const auto &canonical =
            alias == lowered.label_aliases.end() ? label : alias->second;
        for (unsigned n = 0; n < lowered.nodes.size(); ++n) {
          if (lowered.nodes[n].label == canonical)
            query_node = std::make_pair(p, n);
        }
      }
      if (!query_node)
        throw std::invalid_argument("Boolean-program label not found: " + query);
    }
    const double construction_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();

    if (prepare_only) {
      os << "[npa:predicate:prepare]\n"
         << "  [profile] construction_seconds=" << construction_seconds
         << " procedures=" << program.lowered.procedures.size()
         << " equations=" << program.equations.size() << "\n";
      return 0;
    }

    std::vector<std::pair<npa::Symbol, D::value_type>> values;
    npa::Stat stats;
    if (solver == "kleene") {
      auto result = npa::KleeneSolver<D>::solve(program.equations);
      values = std::move(result.first);
      stats = std::move(result.second);
    } else {
      npa::SolveOptions options;
      options.linear_strategy = linearStrategy(linear_solver);
      options.newton_round_strategy = roundStrategy(newton_round);
      auto result = npa::NPASolver<D>::solve(program.equations, options);
      values = std::move(result.first);
      stats = std::move(result.second);
    }
    if (!stats.converged)
      throw std::runtime_error("Boolean-program NPA solve did not converge");
    if (require_tensor && stats.tensor_rounds == 0)
      throw std::runtime_error("tensor backend was not used");

    if (verify_solvers) {
      auto kleene = npa::KleeneSolver<D>::solve(program.equations);
      auto scc = npa::NPASolver<D>::solve(
          program.equations, false, -1, npa::LinearStrategy::SCC);
      auto tensor = npa::NPASolver<D>::solve(
          program.equations, false, -1, npa::LinearStrategy::TensorProduct);
      if (!kleene.second.converged || !scc.second.converged ||
          !tensor.second.converged)
        throw std::runtime_error("a comparison solver did not converge");
      if (kleene.first.size() != scc.first.size() ||
          kleene.first.size() != tensor.first.size())
        throw std::runtime_error("comparison solver result size mismatch");
      for (unsigned i = 0; i < kleene.first.size(); ++i) {
        const auto &symbol = kleene.first[i].first;
        if (symbol != scc.first[i].first || symbol != tensor.first[i].first ||
            !D::equal(kleene.first[i].second, scc.first[i].second) ||
            !D::equal(kleene.first[i].second, tensor.first[i].second))
          throw std::runtime_error("solver mismatch at equation " + symbol);
      }
    }

    std::unordered_map<npa::Symbol, D::value_type> solutions;
    solutions.reserve(values.size());
    for (auto &value : values)
      solutions.emplace(value.first, std::move(value.second));

    os << "[npa:predicate:" << solver;
    if (solver == "newton")
      os << ":linear=" << linear_solver << ":round=" << newton_round;
    os << "]\n";
    os << "  [profile] construction_seconds=" << construction_seconds
       << " solve_seconds=" << stats.time
       << " equations=" << stats.equation_count
       << " iterations=" << stats.iters
       << " tensor_rounds=" << stats.tensor_rounds
       << " tensor_fallback_rounds=" << stats.tensor_fallback_rounds << "\n";
    os << "  [profile] linear_solve_seconds=" << stats.linear_solve_time
       << " adaptive_tensor_sccs=" << stats.adaptive_scc_tensor_count
       << " adaptive_fallback_sccs="
       << stats.adaptive_scc_tensor_fallback_count << "\n";
    if (verify_solvers)
      os << "  [verification] kleene=scc=tensor\n";

    auto reached_from_entry = [&](unsigned p, unsigned n) {
      return D::extend(solutions.at(program.entry_reach_symbols[p]),
                       solutions.at(program.forward_node_symbols[p][n]));
    };
    bool error_reachable = false;
    for (unsigned p = 0; p < program.lowered.procedures.size(); ++p) {
      const auto &lowered = program.lowered.procedures[p];
      for (const auto &error_label : lowered.error_exit_labels) {
        for (unsigned n = 0; n < lowered.nodes.size(); ++n) {
          if (lowered.nodes[n].label == error_label &&
              !D::equal(reached_from_entry(p, n), D::zero()))
            error_reachable = true;
        }
      }
    }
    os << "  [program] entry=" << entry << " assertion_or_abort_reachable="
       << (error_reachable ? "yes" : "no") << "\n";
    if (query_node) {
      os << "  [query] " << query << " reachable="
         << (D::equal(reached_from_entry(query_node->first,
                                         query_node->second),
                      D::zero())
                 ? "no"
                 : "yes")
         << "\n";
    }

    for (unsigned p = 0; p < program.lowered.procedures.size(); ++p) {
      const auto &lowered = program.lowered.procedures[p];
      const auto &summary = solutions.at(program.summary_symbols[p]);
      const auto &error_summary =
          solutions.at(program.error_summary_symbols[p]);
      const bool has_path = !D::equal(summary, D::zero());
      os << "  [procedure] " << lowered.procedure
         << " path_to_exit=" << (has_path ? "yes" : "no")
         << " error_path="
         << (D::equal(error_summary, D::zero()) ? "no" : "yes") << "\n";
      if (!print_block_results)
        continue;
      for (unsigned n = 0; n < lowered.nodes.size(); ++n) {
        const auto &path = solutions.at(program.node_symbols[p][n]);
        const auto &error_path =
            solutions.at(program.error_node_symbols[p][n]);
        os << "    " << lowered.nodes[n].label << " path_to_exit="
           << (D::equal(path, D::zero()) ? "no" : "yes")
           << " error_path="
           << (D::equal(error_path, D::zero()) ? "no" : "yes")
           << " reachable_from_entry="
           << (D::equal(reached_from_entry(p, n), D::zero()) ? "no" : "yes")
           << "\n";
      }
    }
    return 0;
  } catch (const lotus::verification::frontend::FrontendException &error) {
    llvm::errs() << "error: " << input_filename << ":" << error.error().line
                 << ":" << error.error().column << ": " << error.what()
                 << "\n";
    return 1;
  } catch (const std::exception &error) {
    llvm::errs() << "error: " << error.what() << "\n";
    return 1;
  }
}
