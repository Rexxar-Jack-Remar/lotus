#include "../common.hpp"
#include "../program_options.hpp"

#include <crab/analysis/concurrent_fwd_analyzer.hpp>
#include <crab/analysis/fwd_analyzer.hpp>

using namespace crab::cfg_impl;
using namespace crab::domain_impl;

namespace {

z_cfg_t *prog(variable_factory_t &vfac) {
  z_var x(vfac["x"], crab::INT_TYPE, 32);
  z_var y(vfac["y"], crab::INT_TYPE, 32);

  z_cfg_t *cfg = new z_cfg_t("entry", "ret");
  auto &entry = cfg->insert("entry");
  auto &pre = cfg->insert("pre");
  auto &head = cfg->insert("head");
  auto &body = cfg->insert("body");
  auto &exit = cfg->insert("exit");
  auto &ret = cfg->insert("ret");

  entry >> pre;
  pre >> head;
  head >> body;
  head >> exit;
  body >> head;
  exit >> ret;

  entry.assign(x, 0);
  pre.assign(y, 1);
  body.add(x, x, 1);
  body.add(y, y, 2);
  exit.assume(x <= y);

  return cfg;
}

template <typename Dom> bool equivalent(const Dom &x, const Dom &y) {
  return (x <= y) && (y <= x);
}

template <typename LhsAnalyzer, typename RhsAnalyzer>
void check_node(const char *name,
                LhsAnalyzer &lhs,
                RhsAnalyzer &rhs,
                const z_cfg_t &cfg,
                const z_cfg_t::basic_block_label_t &label) {
  auto lhs_pre = lhs.get_pre(label);
  auto rhs_pre = rhs.get_pre(label);
  auto lhs_post = lhs.get_post(label);
  auto rhs_post = rhs.get_post(label);
  if (!equivalent(lhs_pre, rhs_pre) || !equivalent(lhs_post, rhs_post)) {
    crab::errs() << "Mismatch for " << name << " at block " << label << "\n";
    crab::errs() << "lhs pre=" << lhs_pre << "\n";
    crab::errs() << "rhs pre=" << rhs_pre << "\n";
    crab::errs() << "lhs post=" << lhs_post << "\n";
    crab::errs() << "rhs post=" << rhs_post << "\n";
    std::exit(1);
  }
}

} // namespace

int main(int argc, char **argv) {
  bool stats_enabled = false;
  if (!crab_tests::parse_user_options(argc, argv, stats_enabled)) {
    return 0;
  }

  variable_factory_t vfac;
  z_var x(vfac["x"], crab::INT_TYPE, 32);
  std::unique_ptr<z_cfg_t> cfg(prog(vfac));
  crab::outs() << *cfg << "\n";

  crab::fixpoint_parameters params;
  params.get_widening_delay() = 1;
  params.get_descending_iterations() = 1;

  z_interval_domain_t absval_fac;

  using seq_analyzer_t =
      crab::analyzer::intra_fwd_analyzer<z_cfg_ref_t, z_interval_domain_t>;
  using conc_analyzer_t = crab::analyzer::intra_concurrent_fwd_analyzer<
      z_cfg_ref_t,
      z_interval_domain_t>;

  seq_analyzer_t seq(*cfg, absval_fac, nullptr, params);
  conc_analyzer_t conc(*cfg, absval_fac, nullptr, params);

  using assumption_map_t = typename seq_analyzer_t::assumption_map_t;
  assumption_map_t assumptions;
  z_interval_domain_t head_assumption;
  head_assumption += z_lin_cst_t(x >= 5);
  head_assumption += z_lin_cst_t(x <= 5);
  assumptions.emplace("head", head_assumption);
  z_interval_domain_t init;

  seq.run("head", init, assumptions);
  conc.run("head", init, assumptions);

  for (auto it = cfg->label_begin(), et = cfg->label_end(); it != et; ++it) {
    check_node("concurrent-vs-interleaved", seq, conc, *cfg, *it);
  }

  return 0;
}
