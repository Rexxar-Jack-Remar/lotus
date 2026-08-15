/**
 * @file RegionSampler.cpp
 * @brief Abstraction-based sampling using SymAbs + hit-and-run walk
 *
 * Fixes applied (original B-series):
 *  B19 – signed_range() now handles width == 64 as a special case (using
 *        INT64_MIN / INT64_MAX) instead of silently returning false and leaving
 *        64-bit variables unconstrained in the polytope.
 *  B20 – bv_from_int() is unchanged (already correct for width < 64); the fix
 *        is in signed_range() which now covers width == 64.
 *  B21 – superseded: candidate validation now asks a solver whether the
 *        sampled BV projection has an extension, rather than evaluating an
 *        arbitrarily completed model.
 *  B22 – build_constraints() logs a warning when a constraint references an
 *        unknown variable name (previously silently skipped with no
 * indication). B23 – collect_vars() deduplicates the variable list using a
 * name-based set so that variables appearing multiple times in the formula are
 * only added once.
 *
 * Additional fixes (new):
 *  RS-1 – superseded by solver-based candidate validation.
 *  RS-3 – initial_point() now falls back to assigning 0 for any variable
 *          whose value cannot be extracted by eval_model_value(), rather than
 *          aborting the entire run.
 *  RS-4 – superseded: the sampler uses signed two's-complement coordinates,
 *          matching SymAbs and model extraction.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/PolySampler.h"
#include "Solvers/SMT/SMTSampler/SMTSampler.h"
#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"

#include <fstream>
#include <iostream>
#include <random>
#include <unordered_map>
#include <unordered_set>

using namespace std;
using namespace z3;

namespace {
constexpr const char *kSamplerName = "RegionSampler";

void log_warn(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] WARN: " << msg << '\n';
}

void log_error(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] ERROR: " << msg << '\n';
}

struct VarInfo {
  z3::expr var;
  unsigned width;
  std::string name;
};

/**
 * @brief Computes the signed two's-complement range for a bit-vector.
 *
 * SymAbs converts bit-vectors to signed integers and eval_model_value() uses
 * the same representation.  The region bounds must therefore use it too.
 */
static bool signed_range(unsigned width, int64_t &min_out, int64_t &max_out) {
  if (width == 0 || width > 64)
    return false;
  if (width == 64) {
    min_out = std::numeric_limits<int64_t>::min();
    max_out = std::numeric_limits<int64_t>::max();
  } else {
    const int64_t half = 1LL << (width - 1);
    min_out = -half;
    max_out = half - 1;
  }
  return true;
}

static z3::expr bv_from_int(z3::context &ctx, int64_t value, unsigned width) {
  uint64_t u = static_cast<uint64_t>(value);
  if (width < 64) {
    uint64_t mask = (1ULL << width) - 1;
    u &= mask;
  }
  return ctx.bv_val(u, width);
}

/**
 * @brief Checks whether a candidate point satisfies the SMT formula.
 *
 * Candidate BV values are assumptions.  All remaining variables are left to
 * the solver, so validation is existential over Booleans, arrays, UFs, etc.
 */
static bool model_satisfies(z3::solver &validator,
                            const std::vector<VarInfo> &vars,
                            const std::vector<int64_t> &point) {
  if (point.size() != vars.size())
    return false;
  z3::context &ctx = validator.ctx();

  validator.push();
  for (size_t i = 0; i < vars.size(); ++i) {
    z3::expr val = bv_from_int(ctx, point[i], vars[i].width);
    validator.add(vars[i].var == val);
  }
  bool result = validator.check() == z3::sat;
  validator.pop();
  return result;
}

} // namespace

struct region_sampler {
  std::string input_file;
  int max_samples = 1000;
  double max_time_ms = 30000.0;
  RegionSampling::SampleConfig sample_config;

  SymAbs::AbstractionConfig abs_config;

  enum class Domain { Zone, Octagon };
  Domain domain = Domain::Octagon;

  RegionSampling::Walk walk = RegionSampling::Walk::HitAndRun;

  z3::context c;
  z3::expr smt_formula;
  z3::solver validator;
  std::vector<VarInfo> vars;
  std::vector<RegionSampling::LinearConstraint> constraints;

  std::mt19937_64 rng;

  explicit region_sampler(std::string input, int max_samples, double max_time)
      : input_file(std::move(input)), max_samples(max_samples),
        max_time_ms(max_time), smt_formula(c), validator(c) {
    rng.seed(static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
  }

  void parse_smt() {
    try {
      expr_vector evec = c.parse_file(input_file.c_str());
      smt_formula = mk_and(evec);
    } catch (const z3::exception &e) {
      log_error(std::string("Failed to parse SMT file: ") + e.msg());
      smt_formula = z3::expr(c);
    }
  }

  /**
   * @brief Collects all bit-vector variables from the SMT formula.
   *
   * B23: deduplicates by variable name so that variables appearing
   * multiple times in the formula are only added once.
   *
   * The native walk uses int64_t coordinates, so wider variables are rejected
   * explicitly instead of silently truncating their values.
   */
  void collect_vars() {
    expr_vector all_vars(c);
    get_expr_vars(smt_formula, all_vars);
    vars.clear();

    // B23: track seen names to avoid duplicates.
    std::unordered_set<std::string> seen_bv_names;

    for (unsigned i = 0; i < all_vars.size(); ++i) {
      const z3::expr &v = all_vars[i];
      std::string name = v.decl().name().str();
      if (v.get_sort().is_bv()) {
        if (seen_bv_names.insert(name).second) {
          if (v.get_sort().bv_size() > 64) {
            log_error("Bit-vector variable '" + name +
                      "' is wider than the supported 64-bit coordinate domain");
            vars.clear();
            return;
          }
          VarInfo info{v, v.get_sort().bv_size(), name};
          vars.push_back(info);
        }
      }
    }
  }

  /**
   * @brief Builds linear integer constraints from the SMT formula.
   *
   * B22: logs a warning when a constraint references a variable name not
   * found in the vars index (previously silently skipped).
   *
   * Uses signed ranges to match the coordinate system used by SymAbs.
   */
  bool build_constraints() {
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < vars.size(); ++i)
      index[vars[i].name] = i;

    constraints.clear();
    if (domain == Domain::Zone) {
      auto zone =
          SymAbs::alpha_zone_V(smt_formula, extract_exprs(), abs_config);
      for (const auto &cstr : zone) {
        RegionSampling::LinearConstraint lc;
        lc.coeffs.assign(vars.size(), 0);
        auto it_i = index.find(cstr.var_i.decl().name().str());
        if (it_i == index.end()) {
          // B22: warn instead of silently skipping.
          log_warn("Zone constraint references unknown variable: " +
                   cstr.var_i.decl().name().str());
          continue;
        }
        lc.coeffs[it_i->second] += 1;
        if (!cstr.unary) {
          auto it_j = index.find(cstr.var_j.decl().name().str());
          if (it_j == index.end()) {
            log_warn("Zone constraint references unknown variable: " +
                     cstr.var_j.decl().name().str());
            continue;
          }
          lc.coeffs[it_j->second] -= 1;
        }
        lc.bound = cstr.bound;
        constraints.push_back(std::move(lc));
      }
    } else {
      auto oct = SymAbs::alpha_oct_V(smt_formula, extract_exprs(), abs_config);
      for (const auto &cstr : oct) {
        RegionSampling::LinearConstraint lc;
        lc.coeffs.assign(vars.size(), 0);
        auto it_i = index.find(cstr.var_i.decl().name().str());
        if (it_i == index.end()) {
          log_warn("Octagon constraint references unknown variable: " +
                   cstr.var_i.decl().name().str());
          continue;
        }
        lc.coeffs[it_i->second] += cstr.lambda_i;
        if (!cstr.unary) {
          auto it_j = index.find(cstr.var_j.decl().name().str());
          if (it_j == index.end()) {
            log_warn("Octagon constraint references unknown variable: " +
                     cstr.var_j.decl().name().str());
            continue;
          }
          lc.coeffs[it_j->second] += cstr.lambda_j;
        }
        lc.bound = cstr.bound;
        constraints.push_back(std::move(lc));
      }
    }

    // Add bit-width bounds for each variable.
    for (size_t i = 0; i < vars.size(); ++i) {
      int64_t min_v = 0, max_v = 0;
      if (!signed_range(vars[i].width, min_v, max_v)) {
        log_error("Unsupported bit-vector width for '" + vars[i].name + "'");
        return false;
      }

      // upper bound: x[i] <= max_v
      RegionSampling::LinearConstraint upper;
      upper.coeffs.assign(vars.size(), 0);
      upper.coeffs[i] = 1;
      upper.bound = max_v;
      constraints.push_back(std::move(upper));

      // lower bound: -x[i] <= -min_v  (i.e., x[i] >= min_v).  For a
      // 64-bit coordinate min_v is INT64_MIN, which is already implied by
      // the coordinate type and whose negation is not representable.
      if (min_v != std::numeric_limits<int64_t>::min()) {
        RegionSampling::LinearConstraint lower;
        lower.coeffs.assign(vars.size(), 0);
        lower.coeffs[i] = -1;
        lower.bound = -min_v;
        constraints.push_back(std::move(lower));
      }
    }

    return !constraints.empty();
  }

  std::vector<z3::expr> extract_exprs() const {
    std::vector<z3::expr> out;
    out.reserve(vars.size());
    for (const auto &v : vars)
      out.push_back(v.var);
    return out;
  }

  /**
   * @brief Finds an initial satisfying assignment using the SMT solver.
   *
   * RS-3: if eval_model_value() fails for a variable, we fall back to 0
   * rather than aborting the entire run.  The initial point is then verified
   * against the formula before being used.
   */
  bool initial_point(std::vector<int64_t> &point) {
    solver s(c);
    s.add(smt_formula);
    if (s.check() != sat)
      return false;
    model m = s.get_model();
    point.clear();
    point.reserve(vars.size());
    bool any_fallback = false;
    for (const auto &v : vars) {
      int64_t val = 0;
      if (!SymAbs::eval_model_value(m, v.var, val)) {
        // RS-3: fall back to 0 instead of aborting.
        log_warn("eval_model_value failed for variable '" + v.name +
                 "'; using 0 as fallback");
        val = 0;
        any_fallback = true;
      }
      point.push_back(val);
    }
    if (any_fallback) {
      // Verify the fallback point actually satisfies the formula.
      if (!model_satisfies(validator, vars, point)) {
        log_warn("Fallback initial point does not satisfy formula; aborting");
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Main execution function for RegionSampler.
   */
  void run() {
    parse_smt();
    if (!static_cast<bool>(smt_formula)) {
      log_error("No SMT formula loaded; aborting run");
      return;
    }
    collect_vars();
    if (vars.empty()) {
      log_warn("No bit-vector variables found");
      return;
    }
    validator.add(smt_formula);
    if (!build_constraints()) {
      log_warn("No abstraction constraints built");
      return;
    }

    std::vector<int64_t> point;
    if (!initial_point(point)) {
      log_warn("Formula unsat or model extraction failed");
      return;
    }

    std::ofstream out(input_file + ".abs.samples");
    if (!out.is_open()) {
      log_error("Failed to open output file: " + input_file + ".abs.samples");
      return;
    }
    // Write header (variable names).
    for (size_t i = 0; i < vars.size(); ++i) {
      if (i)
        out << " ";
      out << vars[i].name;
    }
    out << "\n";

    sample_config.max_samples = max_samples;
    sample_config.max_time_ms = max_time_ms;

    const std::vector<VarInfo> &vars_ref = vars;

    // Acceptance criterion: must satisfy original SMT formula.
    auto accept = [this, &vars_ref](const std::vector<int64_t> &candidate) {
      return model_satisfies(validator, vars_ref, candidate);
    };

    auto samples = RegionSampling::sample_points(constraints, point, walk, rng,
                                                 sample_config, accept);
    for (const auto &sample : samples) {
      for (size_t i = 0; i < sample.size(); ++i) {
        if (i)
          out << " ";
        out << sample[i];
      }
      out << "\n";
    }
  }
};

namespace lotus::SMTSampler {

void runRegionSampler(const std::string &input_file, int max_samples,
                      double max_time_ms) {
  region_sampler(input_file, max_samples, max_time_ms).run();
}

} // namespace lotus::SMTSampler
