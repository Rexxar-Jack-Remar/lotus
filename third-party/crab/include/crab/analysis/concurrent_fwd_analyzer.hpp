#pragma once

#include <crab/analysis/abs_transformer.hpp>
#include <crab/analysis/dataflow/liveness.hpp>
#include <crab/cfg/type_checker.hpp>
#include <crab/domains/abstract_domain_specialized_traits.hpp>
#include <crab/fixpoint/concurrenty_fwd_fixpoint_iterator.hpp>

#include <algorithm>
#include <memory>
#include <set>

namespace crab {
namespace analyzer {
namespace analyzer_internal_impl {

template <typename CFG, typename AbsTr>
class concurrent_fwd_analyzer
    : public ikos::concurrent_fwd_fixpoint_iterator<CFG,
                                                    typename AbsTr::abs_dom_t> {
public:
  using cfg_t = CFG;
  using basic_block_t = typename CFG::basic_block_t;
  using basic_block_label_t = typename CFG::basic_block_label_t;
  using variable_t = typename CFG::variable_t;
  using stmt_t = typename CFG::statement_t;
  using abs_dom_t = typename AbsTr::abs_dom_t;
  using abs_tr_t = AbsTr;

private:
  using fixpo_iterator_t =
      ikos::concurrent_fwd_fixpoint_iterator<CFG, abs_dom_t>;

public:
  using invariant_map_t = typename fixpo_iterator_t::invariant_table_t;
  using assumption_map_t = typename fixpo_iterator_t::assumption_map_t;
  using liveness_t = live_and_dead_analysis<CFG>;
  using wpo_t = typename fixpo_iterator_t::wpo_t;
  using iterator = typename invariant_map_t::iterator;
  using const_iterator = typename invariant_map_t::const_iterator;

private:
  using live_set_t = typename liveness_t::set_t;

  abs_tr_t *m_abs_tr;
  const liveness_t *m_live;
  live_set_t m_formals;
  invariant_map_t m_pre;
  invariant_map_t m_post;

  void set_pre(const basic_block_label_t &node, abs_dom_t inv) {
    auto res = m_pre.insert({node, std::move(inv)});
    if (!res.second) {
      res.first->second = std::move(inv);
    }
  }

  void set_post(const basic_block_label_t &node, abs_dom_t inv) {
    auto res = m_post.insert({node, std::move(inv)});
    if (!res.second) {
      res.first->second = std::move(inv);
    }
  }

  void prune_dead_variables(const basic_block_label_t &node, abs_dom_t &inv) {
    if (!m_live) {
      return;
    }
    crab::ScopedCrabStats __st__("Pruning dead variables");
    if (inv.is_bottom() || inv.is_top()) {
      return;
    }
    auto dead = m_live->dead_exit(node);
    dead -= m_formals;
    std::vector<variable_t> dead_vec(dead.begin(), dead.end());
    inv.forget(dead_vec);
  }

  void init_fwd_analyzer() {
    assert(m_abs_tr);
    CRAB_VERBOSE_IF(1, crab::outs() << "CFG with " << get_cfg().size()
                                    << " basic blocks\n";);
    CRAB_VERBOSE_IF(1, get_msg_stream() << "Type checking CFG ... ";);
    crab::CrabStats::resume("CFG type checking");
    crab::cfg::type_checker<CFG> tc(get_cfg());
    tc.run();
    crab::CrabStats::stop("CFG type checking");
    CRAB_VERBOSE_IF(1, get_msg_stream() << "OK\n";);

    if (::crab::CrabSanityCheckFlag && get_cfg().has_func_decl()) {
      crab::CrabStats::resume("Live symbols sanity check");
      CRAB_VERBOSE_IF(1, get_msg_stream() << "Live symbols sanity check ... ";);
      liveness_analysis<CFG> live_symbols(get_cfg(), false);
      live_symbols.exec();
      if (auto const *entry_ls = live_symbols.get_in(get_cfg().entry())) {
        auto const &fdecl = get_cfg().get_func_decl();
        typename liveness_analysis<CFG>::varset_domain_t suspicious_vars(
            *entry_ls);
        for (unsigned i = 0; i < fdecl.get_num_inputs(); i++) {
          suspicious_vars -= fdecl.get_input_name(i);
        }
        if (!suspicious_vars.is_bottom()) {
          crab::outs() << "\n*** Sanity check failed: " << suspicious_vars
                       << " might not be initialized in "
                       << get_cfg().get_func_decl().get_func_name() << "\n";
        } else {
          CRAB_VERBOSE_IF(1, crab::outs() << "OK";);
        }
      }
      CRAB_VERBOSE_IF(1, crab::outs() << "\n";);
      crab::CrabStats::stop("Live symbols sanity check");
    }

    if (m_live && get_cfg().has_func_decl()) {
      auto const &fdecl = get_cfg().get_func_decl();
      for (unsigned i = 0; i < fdecl.get_num_inputs(); i++) {
        m_formals += fdecl.get_input_name(i);
      }
      for (unsigned i = 0; i < fdecl.get_num_outputs(); i++) {
        m_formals += fdecl.get_output_name(i);
      }
    }
  }

protected:
  abs_dom_t analyze(const basic_block_label_t &node, abs_dom_t &&inv) override {
    auto &b = get_cfg().get_node(node);
    abs_tr_t abs_tr(std::move(inv));
    for (auto &s : b) {
      s.accept(&abs_tr);
    }
    abs_dom_t &res = abs_tr.get_abs_value();
    prune_dead_variables(node, res);
    return res;
  }

  void process_pre(const basic_block_label_t &node, abs_dom_t inv) override {
    set_pre(node, std::move(inv));
  }

  void process_post(const basic_block_label_t &node, abs_dom_t inv) override {
    set_post(node, std::move(inv));
  }

public:
  concurrent_fwd_analyzer(CFG cfg,
                          abs_tr_t *abs_tr,
                          abs_dom_t absval_fac,
                          const liveness_t *live_and_dead_symbols,
                          const fixpoint_parameters &params)
      : fixpo_iterator_t(cfg, absval_fac, params), m_abs_tr(abs_tr),
        m_live(live_and_dead_symbols) {
    init_fwd_analyzer();
  }

  void run_forward(abs_dom_t init) {
    m_pre.clear();
    m_post.clear();
    this->run(std::move(init));
  }

  void run_forward(const basic_block_label_t &entry,
                   abs_dom_t init,
                   const assumption_map_t &assumptions) {
    m_pre.clear();
    m_post.clear();
    this->run(entry, std::move(init), assumptions);
  }

  abs_dom_t operator[](const basic_block_label_t &b) const { return get_pre(b); }

  abs_dom_t get_pre(const basic_block_label_t &b) const {
    auto it = m_pre.find(b);
    return (it != m_pre.end()) ? it->second : this->bottom();
  }

  abs_dom_t get_post(const basic_block_label_t &b) const {
    auto it = m_post.find(b);
    return (it != m_post.end()) ? it->second : this->bottom();
  }

  const invariant_map_t &get_pre_invariants() const { return m_pre; }
  const invariant_map_t &get_post_invariants() const { return m_post; }
  iterator pre_begin() { return m_pre.begin(); }
  iterator pre_end() { return m_pre.end(); }
  const_iterator pre_begin() const { return m_pre.begin(); }
  const_iterator pre_end() const { return m_pre.end(); }
  iterator post_begin() { return m_post.begin(); }
  iterator post_end() { return m_post.end(); }
  const_iterator post_begin() const { return m_post.begin(); }
  const_iterator post_end() const { return m_post.end(); }

  wpo_t &get_wpo() { return fixpo_iterator_t::get_wpo(); }
  const wpo_t &get_wpo() const { return fixpo_iterator_t::get_wpo(); }

  void clear() {
    m_pre.clear();
    m_post.clear();
    fixpo_iterator_t::clear();
  }

  CFG get_cfg() const { return this->m_cfg; }

  abs_tr_t &get_abs_transformer() { return *m_abs_tr; }

  void get_safe_assertions(std::set<const stmt_t *> &) const {}
};

template <typename CFG, typename AbsDomain, typename AbsTr>
class intra_concurrent_fwd_analyzer_wrapper {
  using fwd_analyzer_t = concurrent_fwd_analyzer<CFG, AbsTr>;

public:
  using abs_dom_t = AbsDomain;
  using liveness_t = live_and_dead_analysis<CFG>;
  using cfg_t = CFG;
  using basic_block_label_t = typename CFG::basic_block_label_t;
  using varname_t = typename CFG::varname_t;
  using number_t = typename CFG::number_t;
  using stmt_t = typename CFG::statement_t;
  using abs_tr_t = typename fwd_analyzer_t::abs_tr_t;
  using wto_t = typename fwd_analyzer_t::wpo_t;
  using wpo_t = wto_t;
  using assumption_map_t = typename fwd_analyzer_t::assumption_map_t;
  using invariant_map_t = typename fwd_analyzer_t::invariant_map_t;
  using iterator = typename fwd_analyzer_t::iterator;
  using const_iterator = typename fwd_analyzer_t::const_iterator;

private:
  std::unique_ptr<abs_tr_t> m_abs_tr;
  fwd_analyzer_t m_analyzer;

public:
  intra_concurrent_fwd_analyzer_wrapper(CFG cfg,
                                        AbsDomain absval_fac,
                                        const liveness_t *live,
                                        const fixpoint_parameters &fixpo_params)
      : m_abs_tr(new abs_tr_t(absval_fac.make_top())),
        m_analyzer(cfg, &*m_abs_tr, absval_fac, live, fixpo_params) {}

  void run(abs_dom_t init) { m_analyzer.run_forward(std::move(init)); }

  void run(const basic_block_label_t &entry,
           abs_dom_t init,
           const assumption_map_t &assumptions) {
    m_analyzer.run_forward(entry, std::move(init), assumptions);
  }

  iterator pre_begin() { return m_analyzer.pre_begin(); }
  iterator pre_end() { return m_analyzer.pre_end(); }
  const_iterator pre_begin() const { return m_analyzer.pre_begin(); }
  const_iterator pre_end() const { return m_analyzer.pre_end(); }
  iterator post_begin() { return m_analyzer.post_begin(); }
  iterator post_end() { return m_analyzer.post_end(); }
  const_iterator post_begin() const { return m_analyzer.post_begin(); }
  const_iterator post_end() const { return m_analyzer.post_end(); }

  const invariant_map_t &get_pre_invariants() const {
    return m_analyzer.get_pre_invariants();
  }

  const invariant_map_t &get_post_invariants() const {
    return m_analyzer.get_post_invariants();
  }

  abs_dom_t operator[](const basic_block_label_t &b) const {
    return m_analyzer.get_pre(b);
  }

  abs_dom_t get_pre(const basic_block_label_t &b) const {
    return m_analyzer.get_pre(b);
  }

  abs_dom_t get_post(const basic_block_label_t &b) const {
    return m_analyzer.get_post(b);
  }

  wto_t &get_wto() { return m_analyzer.get_wpo(); }
  const wto_t &get_wto() const { return m_analyzer.get_wpo(); }

  void clear() { m_analyzer.clear(); }
  CFG get_cfg() { return m_analyzer.get_cfg(); }
  abs_tr_t &get_abs_transformer() { return *m_abs_tr; }
  void get_safe_assertions(std::set<const stmt_t *> &out) const {
    m_analyzer.get_safe_assertions(out);
  }
};

} // namespace analyzer_internal_impl

template <typename CFG, typename AbsDomain>
using intra_concurrent_fwd_analyzer =
    analyzer_internal_impl::intra_concurrent_fwd_analyzer_wrapper<
        CFG,
        AbsDomain,
        intra_abs_transformer<typename CFG::basic_block_t, AbsDomain>>;

} // namespace analyzer
} // namespace crab
