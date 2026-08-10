/*
 * Copyright (c) 2026 XiangZhang
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once
#include <cstddef>
#include <vector>

// #include "node/node.h"
#include "node/node_manager.h"
namespace stabilizer::kernel {

/**
 * @brief Internal canonicalization kernel for SMTStabilizer.
 *
 * The kernel owns graph-oriented views over the assertion DAG and applies
 * multi-stage hash propagation to derive a deterministic symbol/function
 * ordering. It is intentionally documented for maintainers even though most
 * stage methods are private.
 */
class Kernel {
  public:
    Kernel() = delete;

    /**
     * @brief Build kernel state from the current node manager assertions.
     * @param nm Node manager that provides the DAG roots and symbol tables.
     * @param context_propagation Enable fixed-point context propagation.
     * @param symmetry_breaking_perturbation Enable iterative subgraph-based
     * tie breaking for colliding symbols.
     */
    Kernel(node::NodeManager &nm, const bool &context_propagation = true, const bool &symmetry_breaking_perturbation = true);

    /**
     * @brief Run the full stabilization pass and mutate node manager state.
     *
     * Side effects include:
     * - Renaming symbols and UF/function declarations to canonical names.
     * - Reordering assertions and selected declaration blocks.
     * - Updating datatype and sort naming maps when applicable.
     *
     * @param nm Node manager to be rewritten in place.
     */
    void apply(node::NodeManager &nm);

  private:
    std::vector<std::vector<size_t>> d_graph;
    std::vector<std::vector<std::pair<size_t, size_t>>> d_reversed_graph;
    std::vector<size_t> d_hash_table;
    std::vector<size_t> d_context_hash;
    std::vector<node::Node> d_nodes;  // reversed topological order
    std::vector<size_t> d_processing;

    std::vector<size_t> d_symbols;
    std::vector<size_t> d_unique_symbols;
    // std::vector<size_t> d_propagate_num;

    std::vector<uint8_t> d_visited;
    std::vector<uint8_t> d_is_commutative;
    std::vector<uint8_t> d_is_symbol;

    // std::unordered_map<size_t, size_t> d_spe_hash_count;

    size_t d_symbol_num;

    bool d_context_propagation = true;
    bool d_symmetry_breaking_perturbation = true;

    /**
     * @brief Check whether node kind at index @p i is treated as commutative.
     * @param i Node index in the internal node vector.
     * @param from_cache Use cached commutativity when true.
     */
    bool is_commutative(const size_t &i, const bool &from_cache = true);
    // void context_propagate();

    /**
     * @brief Run propagation to a fixed point over a selected subgraph.
     * @param processing Node indices participating in propagation.
     * @param symbols Symbol indices monitored for convergence.
     */
    void context_propagate(const std::vector<size_t> &processing, const std::vector<size_t> &symbols);

    /**
     * @brief Apply local perturbation to break remaining hash collisions.
     */
    void specific_propagate();

    /**
     * @brief Keep only currently ambiguous graph regions for next iteration.
     */
    void rebuild_graph();  // graph symbols

    /**
     * @brief Incorporate sort/datatype structure into propagation and naming.
     * @param sort_key_map Mutable sort name map from node manager/parser.
     * @param datatype_blocks Mutable datatype declaration blocks.
     */
    void sort_propagate(std::unordered_map<std::string, node::Sort> &sort_key_map, std::vector<std::vector<parser::Parser::DTTypeDecl>> &datatype_blocks);
    // void propagate();

    /**
     * @brief One directional propagation pass on the selected nodes.
     * @param processing Node indices to process in the current round.
     */
    void propagate(const std::vector<size_t> &processing);
};
}  // namespace stabilizer::kernel

// #pragma once
// #include <cstddef>
// #include <vector>

// #include "node/node_manager.h"
// #include "util/node_helper.h"
// namespace stabilizer::kernel {
// class Kernel {
//   public:
//     Kernel(const node::NodeManager &nm);
//     ~Kernel();

//     bool done();

//     void apply();

//   private:
//     std::vector<std::vector<size_t>> d_graph;
//     std::vector<std::vector<std::pair<size_t, size_t>>> d_reversed_graph;
//     std::vector<size_t> d_hash_table;
//     std::vector<size_t> d_context_hash;
//     std::vector<size_t> d_processing;

//     std::vector<size_t> d_symbols;
//     std::vector<size_t> d_unique_symbols;
//     // std::vector<size_t> d_propagate_num;

//     std::vector<bool> d_visited;
//     std::vector<bool> d_is_commutative;
//     std::vector<bool> d_is_symbol;

//     size_t d_symbol_num;

//     bool is_commutative(const size_t &i, const bool &from_cache = true);
//     void context_propagate(const std::vector<size_t> &processing, const std::vector<size_t> &symbols);
//     void specific_propagate();
//     void rebuild_graph();  // graph symbols
//     void propagate(const std::vector<size_t> &processing);
// };
// }  // namespace stabilizer::kernel