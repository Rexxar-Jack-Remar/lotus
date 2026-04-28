/*
 * Migrated from IKOS: https://github.com/NASA-SW-VnV/ikos/blob/ac7f7c1738976cabc58c6a53413df6e458995c38/core/include/ikos/core/fixpoint/concurrenty_fwd_fixpoint_iterator.hpp
 * The construction of weak partial orderings is based on Sung Kook Kim's,
 * Arnaud J. Venet's, and Aditya V. Thakur's paper: "Deterministic Parallel
 * Fixpoint Computation", in POPL 2020.
*/
#pragma once

#include <crab/cfg/cfg_bgl.hpp>
#include <crab/fixpoint/fixpoint_iterators_api.hpp>
#include <crab/fixpoint/fixpoint_params.hpp>
#include <crab/fixpoint/wpo.hpp>
#include <crab/support/debug.hpp>
#include <crab/support/stats.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ikos {

template <typename CFG, typename AbstractValue>
class concurrent_fwd_fixpoint_iterator
    : public fixpoint_iterator<CFG, AbstractValue> {
public:
  using basic_block_t = typename CFG::basic_block_t;
  using basic_block_label_t = typename CFG::basic_block_label_t;
  using wpo_t = Wpo<CFG>;
  using assumption_map_t =
      std::unordered_map<basic_block_label_t, AbstractValue>;
  using invariant_table_t =
      std::unordered_map<basic_block_label_t, AbstractValue>;

private:
  using wpo_node_t = WpoNode<CFG>;
  using wpo_node_kind_t = typename wpo_node_t::Kind;
  using wpo_index_t = std::size_t;

  class WorkNode;

  enum class fixpoint_iteration_kind_t { Increasing, Decreasing };

  class WorkNode {
  private:
    using work_node_vector_t = std::vector<WorkNode *>;

    std::mutex _mutex;
    wpo_node_kind_t _kind;
    basic_block_label_t _node;
    wpo_index_t _index;
    concurrent_fwd_fixpoint_iterator &_iterator;
    std::atomic<std::size_t> _ref_count;
    work_node_vector_t _successors;
    fixpoint_iteration_kind_t _iteration_kind;
    unsigned _iteration_count;
    work_node_vector_t _predecessors;
    std::mutex _post_mutex;
    AbstractValue _seed_pre;
    AbstractValue _pre;
    AbstractValue _post;
    WorkNode *_head;

  public:
    WorkNode(wpo_node_kind_t kind,
             basic_block_label_t node,
             wpo_index_t index,
             concurrent_fwd_fixpoint_iterator &iterator,
             std::size_t ref_count,
             AbstractValue pre,
             AbstractValue post)
        : _kind(kind), _node(node), _index(index), _iterator(iterator),
          _ref_count(ref_count),
          _iteration_kind(fixpoint_iteration_kind_t::Increasing),
          _iteration_count(0), _seed_pre(pre), _pre(std::move(pre)),
          _post(std::move(post)), _head(nullptr) {
      this->_seed_pre.normalize();
      this->_pre.normalize();
      this->_post.normalize();
    }

    WorkNode(const WorkNode &other)
        : _kind(other._kind), _node(other._node), _index(other._index),
          _iterator(other._iterator), _ref_count(other._ref_count.load()),
          _successors(other._successors),
          _iteration_kind(other._iteration_kind),
          _iteration_count(other._iteration_count),
          _predecessors(other._predecessors), _seed_pre(other._seed_pre),
          _pre(other._pre),
          _post(other._post), _head(other._head) {}

    WorkNode(WorkNode &&other)
        : _kind(other._kind), _node(other._node), _index(other._index),
          _iterator(other._iterator), _ref_count(other._ref_count.load()),
          _successors(std::move(other._successors)),
          _iteration_kind(other._iteration_kind),
          _iteration_count(other._iteration_count),
          _predecessors(std::move(other._predecessors)),
          _seed_pre(std::move(other._seed_pre)), _pre(std::move(other._pre)),
          _post(std::move(other._post)),
          _head(other._head) {}

    WorkNode &operator=(const WorkNode &) = delete;
    WorkNode &operator=(WorkNode &&) = delete;

    wpo_node_kind_t kind() const { return this->_kind; }
    basic_block_label_t node() const { return this->_node; }

    const AbstractValue &pre() const {
      if (this->_kind == wpo_node_kind_t::Exit) {
        CRAB_ERROR("trying to get pre from an exit WPO node");
      }
      return this->_pre;
    }

    const AbstractValue &post() const {
      if (this->_kind == wpo_node_kind_t::Exit) {
        CRAB_ERROR("trying to get post from an exit WPO node");
      }
      return this->_post;
    }

    void set_head(WorkNode *head) {
      if (!head || this->_kind != wpo_node_kind_t::Exit) {
        CRAB_ERROR("invalid exit-to-head link while building concurrent WPO");
      }
      this->_head = head;
    }

    void add_successor(WorkNode *work_node) {
      if (!work_node) {
        CRAB_ERROR("null successor work node");
      }
      this->_successors.push_back(work_node);
    }

    void add_predecessor(WorkNode *work_node) {
      if (!work_node || this->_kind == wpo_node_kind_t::Exit) {
        CRAB_ERROR("invalid predecessor work node");
      }
      this->_predecessors.push_back(work_node);
    }

    const work_node_vector_t &update() {
      std::lock_guard<std::mutex> lock(this->_mutex);
      switch (this->_kind) {
      case wpo_node_kind_t::Plain:
        return this->update_plain();
      case wpo_node_kind_t::Head:
        return this->update_head();
      case wpo_node_kind_t::Exit:
        return this->update_exit();
      }
      CRAB_ERROR("unexpected WPO node kind");
      return this->_successors;
    }

    std::size_t decr_ref_count() { return --this->_ref_count; }

  private:
    void reset_ref_count() {
      this->_ref_count = this->_iterator.num_predecessors_reducible(this->_index);
    }

    AbstractValue get_post() {
      std::lock_guard<std::mutex> lock(this->_post_mutex);
      return this->_post;
    }

    void set_post(AbstractValue post) {
      post.normalize();
      std::lock_guard<std::mutex> lock(this->_post_mutex);
      this->_post = std::move(post);
    }

    AbstractValue analyze_edge(WorkNode *pred) {
      return this->_iterator.analyze_edge(pred->_node,
                                          this->_node,
                                          pred->get_post());
    }

    const work_node_vector_t &update_plain() {
      if (this->_node == this->_iterator.entry()) {
        this->_pre = this->_seed_pre;
      } else {
        this->_pre = this->_iterator.bottom();
        for (WorkNode *pred : this->_predecessors) {
          this->_pre |= this->analyze_edge(pred);
        }
      }

      this->_pre = this->_iterator.strengthen(this->_node, std::move(this->_pre));
      this->_pre.normalize();
      this->set_post(this->_iterator.analyze(this->_node,
                                             AbstractValue(this->_pre)));
      this->reset_ref_count();
      return this->_successors;
    }

    const work_node_vector_t &update_head() {
      if (this->_iteration_count == 0) {
        if (this->_node == this->_iterator.entry()) {
          this->_pre = this->_seed_pre;
        } else {
          this->_pre = this->_iterator.bottom();
          for (WorkNode *pred : this->_predecessors) {
            if (!this->_iterator.get_wpo().is_back_edge(this->_node,
                                                        pred->_node)) {
              this->_pre |= this->analyze_edge(pred);
            }
          }
        }

        this->_pre =
            this->_iterator.strengthen(this->_node, std::move(this->_pre));
        this->_pre.normalize();
        this->_iteration_count++;
      }

      this->set_post(this->_iterator.analyze(this->_node,
                                             AbstractValue(this->_pre)));
      return this->_successors;
    }

    const work_node_vector_t &update_exit() {
      bool converged = this->_head->update_head_backedge();
      this->reset_ref_count();
      if (converged) {
        this->handle_irreducible();
        return this->_successors;
      }
      return this->_head->update_head();
    }

    bool update_head_backedge() {
      AbstractValue new_pre_in =
          (this->_node == this->_iterator.entry()) ? this->_seed_pre
                                                   : this->_iterator.bottom();
      AbstractValue new_pre_back = this->_iterator.bottom();

      for (WorkNode *pred : this->_predecessors) {
        if (!this->_iterator.get_wpo().is_back_edge(this->_node, pred->_node)) {
          if (this->_node == this->_iterator.entry()) {
            continue;
          }
          new_pre_in |= this->analyze_edge(pred);
        } else {
          new_pre_back |= this->analyze_edge(pred);
        }
      }

      new_pre_in |= new_pre_back;
      new_pre_in =
          this->_iterator.strengthen(this->_node, std::move(new_pre_in));
      new_pre_in.normalize();
      AbstractValue new_pre(std::move(new_pre_in));

      if (this->_iteration_kind == fixpoint_iteration_kind_t::Increasing) {
        AbstractValue inv =
            this->_iterator.extrapolate(this->_node,
                                        this->_iteration_count,
                                        this->_pre,
                                        new_pre);
        inv.normalize();
        if (this->_iterator.is_increasing_iterations_fixpoint(
                this->_node,
                this->_iteration_count,
                this->_pre,
                inv)) {
          this->_pre = std::move(inv);
          if (this->_iterator.max_descending_iterations() == 0) {
            this->_iteration_kind = fixpoint_iteration_kind_t::Increasing;
            this->_iteration_count = 0;
            this->reset_ref_count();
            return true;
          }
          this->_iteration_kind = fixpoint_iteration_kind_t::Decreasing;
          this->_iteration_count = 1;
        } else {
          this->_pre = std::move(inv);
          this->_iteration_count++;
          return false;
        }
      }

      if (this->_iteration_kind == fixpoint_iteration_kind_t::Decreasing) {
        if (this->_iteration_count >
            this->_iterator.max_descending_iterations()) {
          this->_iteration_kind = fixpoint_iteration_kind_t::Increasing;
          this->_iteration_count = 0;
          this->reset_ref_count();
          return true;
        }

        AbstractValue inv = this->_iterator.refine(this->_node,
                                                   this->_iteration_count,
                                                   this->_pre,
                                                   new_pre);
        inv.normalize();
        if (this->_iterator.is_decreasing_iterations_fixpoint(
                this->_node,
                this->_iteration_count,
                this->_pre,
                inv)) {
          this->_pre = std::move(inv);
          this->_iteration_kind = fixpoint_iteration_kind_t::Increasing;
          this->_iteration_count = 0;
          this->reset_ref_count();
          return true;
        }

        this->_pre = std::move(inv);
        this->_iteration_count++;
        return false;
      }

      CRAB_ERROR("unexpected fixpoint iteration kind");
      return true;
    }

    void handle_irreducible() {
      for (const auto &p : this->_iterator.get_wpo().irreducibles(this->_index)) {
        this->_iterator._work_nodes[p.first]._ref_count += p.second;
      }
    }
  };

protected:
  CFG m_cfg;
  wpo_t m_wpo;
  AbstractValue m_bottom;
  const crab::fixpoint_parameters &m_params;
  std::vector<WorkNode> _work_nodes;
  std::unordered_map<basic_block_label_t, WorkNode *> _node_to_work;
  basic_block_label_t m_entry;
  const assumption_map_t *m_assumptions;
  std::vector<bool> m_active_wpo_nodes;
  std::vector<std::size_t> m_num_predecessors;
  std::vector<std::size_t> m_num_predecessors_reducible;
  bool m_converged;

protected:
  std::unordered_set<basic_block_label_t>
  compute_reachable_nodes(const basic_block_label_t &entry) const {
    std::unordered_set<basic_block_label_t> reachable;
    std::vector<basic_block_label_t> worklist = {entry};
    reachable.insert(entry);

    while (!worklist.empty()) {
      basic_block_label_t node = worklist.back();
      worklist.pop_back();
      for (auto succ : this->m_cfg.next_nodes(node)) {
        if (reachable.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }

    return reachable;
  }

  void initialize_active_wpo_nodes(const basic_block_label_t &entry) {
    auto reachable_nodes = this->compute_reachable_nodes(entry);
    std::size_t size = this->m_wpo.size();
    this->m_active_wpo_nodes.assign(size, false);
    this->m_num_predecessors.assign(size, 0);
    this->m_num_predecessors_reducible.assign(size, 0);

    for (std::size_t idx = 0; idx < size; ++idx) {
      this->m_active_wpo_nodes[idx] =
          (reachable_nodes.find(this->m_wpo.node(idx)) != reachable_nodes.end());
    }

    std::vector<std::size_t> irreducible_incoming(size, 0);
    for (std::size_t idx = 0; idx < size; ++idx) {
      if (!this->m_active_wpo_nodes[idx] || !this->m_wpo.is_exit(idx)) {
        continue;
      }
      for (const auto &kv : this->m_wpo.irreducibles(idx)) {
        if (kv.first < size && this->m_active_wpo_nodes[kv.first]) {
          irreducible_incoming[kv.first] += kv.second;
        }
      }
    }

    for (std::size_t idx = 0; idx < size; ++idx) {
      if (!this->m_active_wpo_nodes[idx]) {
        continue;
      }

      std::size_t total_preds = 0;
      for (auto pred : this->m_wpo.predecessors(idx)) {
        if (pred < size && this->m_active_wpo_nodes[pred]) {
          total_preds++;
        }
      }
      this->m_num_predecessors[idx] = total_preds;
      this->m_num_predecessors_reducible[idx] =
          (total_preds >= irreducible_incoming[idx])
              ? (total_preds - irreducible_incoming[idx])
              : 0;
    }
  }

  std::size_t num_predecessors(std::size_t idx) const {
    return this->m_num_predecessors[idx];
  }

  std::size_t num_predecessors_reducible(std::size_t idx) const {
    return this->m_num_predecessors_reducible[idx];
  }

  AbstractValue strengthen(const basic_block_label_t &node,
                           AbstractValue inv) const {
    if (this->m_assumptions) {
      auto it = this->m_assumptions->find(node);
      if (it != this->m_assumptions->end()) {
        inv &= it->second;
      }
    }
    return inv;
  }

  void run_workers(WorkNode *root) {
    std::deque<WorkNode *> ready_queue;
    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    std::size_t pending_tasks = 1;
    bool done = false;

    ready_queue.push_back(root);

    auto worker_loop = [&]() {
      while (true) {
        WorkNode *work_node = nullptr;
        {
          std::unique_lock<std::mutex> lock(ready_mutex);
          ready_cv.wait(lock, [&] { return done || !ready_queue.empty(); });

          if (done && ready_queue.empty()) {
            return;
          }

          work_node = ready_queue.front();
          ready_queue.pop_front();
        }

        const auto &successors = work_node->update();
        std::vector<WorkNode *> newly_ready;
        newly_ready.reserve(successors.size());
        for (WorkNode *successor : successors) {
          if (successor->decr_ref_count() == 0) {
            newly_ready.push_back(successor);
          }
        }

        {
          std::lock_guard<std::mutex> lock(ready_mutex);
          pending_tasks += newly_ready.size();
          for (WorkNode *successor : newly_ready) {
            ready_queue.push_back(successor);
          }

          pending_tasks--;
          if (pending_tasks == 0) {
            done = true;
          }
        }

        if (done) {
          ready_cv.notify_all();
        } else if (!newly_ready.empty()) {
          ready_cv.notify_all();
        } else {
          ready_cv.notify_one();
        }
      }
    };

    unsigned concurrency = std::thread::hardware_concurrency();
    std::size_t num_threads =
        std::max<std::size_t>(1, std::min<std::size_t>(this->_work_nodes.size(),
                                                       concurrency == 0 ? 1
                                                                        : concurrency));

    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
      workers.emplace_back(worker_loop);
    }

    for (auto &worker : workers) {
      worker.join();
    }
  }

  virtual AbstractValue analyze_edge(const basic_block_label_t &,
                                     const basic_block_label_t &,
                                     AbstractValue state) {
    return state;
  }

  virtual AbstractValue extrapolate(const basic_block_label_t &node,
                                    unsigned iteration,
                                    AbstractValue &before,
                                    AbstractValue &after) {
    crab::CrabStats::count("Fixpo.extrapolate");
    crab::ScopedCrabStats __st__("Fixpo.extrapolate");

    CRAB_VERBOSE_IF(
        1, crab::get_msg_stream()
               << "Widening " << iteration << " at "
               << crab::basic_block_traits<basic_block_t>::to_string(node)
               << "\n";);

    if (iteration <= m_params.get_widening_delay()) {
      return before | after;
    }
    return before || after;
  }

  virtual bool is_increasing_iterations_fixpoint(
      const basic_block_label_t &,
      unsigned,
      const AbstractValue &before,
      const AbstractValue &after) {
    return after <= before;
  }

  virtual AbstractValue refine(const basic_block_label_t &node,
                               unsigned iteration,
                               AbstractValue &before,
                               AbstractValue &after) {
    crab::CrabStats::count("Fixpo.refine");
    crab::ScopedCrabStats __st__("Fixpo.refine");

    CRAB_VERBOSE_IF(
        1, crab::get_msg_stream()
               << "Decreasing iteration=" << iteration << " at "
               << crab::basic_block_traits<basic_block_t>::to_string(node)
               << "\n";);

    if (iteration == 1) {
      return before & after;
    }
    return before && after;
  }

  virtual bool is_decreasing_iterations_fixpoint(
      const basic_block_label_t &,
      unsigned,
      const AbstractValue &before,
      const AbstractValue &after) {
    return before <= after;
  }

public:
  concurrent_fwd_fixpoint_iterator(
      CFG cfg,
      AbstractValue absval_fac,
      const crab::fixpoint_parameters &params)
      : m_cfg(cfg), m_wpo(cfg), m_bottom(absval_fac.make_bottom()),
        m_params(params), m_entry(cfg.entry()), m_assumptions(nullptr),
        m_converged(false) {
    if (m_params.get_max_thresholds() > 0) {
      CRAB_WARN("concurrent_fwd_fixpoint_iterator currently ignores widening ",
                "thresholds and falls back to plain widening");
    }
  }

  virtual ~concurrent_fwd_fixpoint_iterator() {}

  CFG get_cfg() const { return this->m_cfg; }
  const wpo_t &get_wpo() const { return this->m_wpo; }
  wpo_t &get_wpo() { return this->m_wpo; }
  basic_block_label_t entry() const { return this->m_entry; }
  const AbstractValue &bottom() const { return this->m_bottom; }
  bool converged() const { return this->m_converged; }

  AbstractValue get_pre(basic_block_label_t node) const {
    auto it = this->_node_to_work.find(node);
    return (it != this->_node_to_work.end()) ? it->second->pre() : this->m_bottom;
  }

  AbstractValue get_post(basic_block_label_t node) const {
    auto it = this->_node_to_work.find(node);
    return (it != this->_node_to_work.end()) ? it->second->post()
                                             : this->m_bottom;
  }

  void clear() {
    this->m_converged = false;
    this->_work_nodes.clear();
    this->_node_to_work.clear();
    this->m_assumptions = nullptr;
    this->m_entry = this->m_cfg.entry();
    this->m_active_wpo_nodes.clear();
    this->m_num_predecessors.clear();
    this->m_num_predecessors_reducible.clear();
  }

  void run(AbstractValue init) {
    assumption_map_t assumptions;
    this->run(this->m_cfg.entry(), std::move(init), assumptions);
  }

  void run(const basic_block_label_t &entry,
           AbstractValue init,
           const assumption_map_t &assumptions) {
    crab::ScopedCrabStats __st__("Fixpo");

    std::size_t size = this->m_wpo.size();
    this->clear();
    this->m_entry = entry;
    this->m_assumptions = &assumptions;
    this->initialize_active_wpo_nodes(entry);
    this->_work_nodes.reserve(size);

    for (std::size_t idx = 0; idx < size; ++idx) {
      wpo_node_kind_t kind = this->m_wpo.kind(idx);
      basic_block_label_t node = this->m_wpo.node(idx);
      AbstractValue pre = this->m_bottom;

      if (this->m_active_wpo_nodes[idx] && node == this->entry() &&
          kind != wpo_node_kind_t::Exit) {
        pre = this->strengthen(node, std::move(init));
      }

      this->_work_nodes.push_back(WorkNode(kind,
                                           node,
                                           idx,
                                           *this,
                                           this->num_predecessors(idx),
                                           std::move(pre),
                                           this->m_bottom));
    }

    for (std::size_t idx = 0; idx < size; ++idx) {
      WorkNode &work_node = this->_work_nodes[idx];
      if (this->m_active_wpo_nodes[idx] &&
          work_node.kind() != wpo_node_kind_t::Exit) {
        this->_node_to_work.emplace(work_node.node(), &work_node);
      }
    }

    for (std::size_t idx = 0; idx < size; ++idx) {
      WorkNode &work_node = this->_work_nodes[idx];

      for (std::size_t succ : this->m_wpo.successors(idx)) {
        if (this->m_active_wpo_nodes[idx] &&
            succ < size && this->m_active_wpo_nodes[succ]) {
          work_node.add_successor(&this->_work_nodes[succ]);
        }
      }

      if (!this->m_active_wpo_nodes[idx]) {
        continue;
      }

      if (work_node.kind() == wpo_node_kind_t::Exit) {
        work_node.set_head(&this->_work_nodes[this->m_wpo.head_of_exit(idx)]);
        continue;
      }

      for (auto pred : this->m_cfg.prev_nodes(work_node.node())) {
        auto it = this->_node_to_work.find(pred);
        if (it != this->_node_to_work.end()) {
          work_node.add_predecessor(it->second);
        }
      }
    }

    this->run_workers(this->_node_to_work.at(this->entry()));
    this->m_converged = true;

    for (WorkNode &work_node : this->_work_nodes) {
      std::size_t idx = static_cast<std::size_t>(&work_node - this->_work_nodes.data());
      if (this->m_active_wpo_nodes[idx] &&
          work_node.kind() != wpo_node_kind_t::Exit) {
        auto node = work_node.node();
        this->process_pre(node, work_node.pre());
        this->process_post(node, work_node.post());
      }
    }
  }

  unsigned max_descending_iterations() const {
    return this->m_params.get_descending_iterations();
  }
};

template <typename CFG, typename AbstractValue>
using concurrenty_fwd_fixpoint_iterator =
    concurrent_fwd_fixpoint_iterator<CFG, AbstractValue>;

} // namespace ikos
