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
#include <vector>

#include "node/node_manager.h"
namespace stabilizer::util {

/**
 * @brief Compute a stable structural hash seed for a parser/node DAG node.
 *
 * The hash is used as an initial value before propagation-based refinement in
 * the kernel stages.
 */
size_t hash_node(const node::Node &node);

/**
 * @brief Mix one value into a hash seed.
 * @param seed Hash accumulator to update.
 * @param v Value to combine.
 */
void hash_combine(size_t &seed, const size_t &v);

/**
 * @brief Mix child-node hashes into a parent seed in argument order.
 * @param seed Parent hash accumulator to update.
 * @param children Child indices.
 * @param hash_table Per-node hash table.
 */
void hash_children(size_t &seed, const std::vector<size_t> &children, const std::vector<size_t> &hash_table);

/**
 * @brief Mix child-node hashes as a commutative multiset.
 * @param seed Parent hash accumulator to update.
 * @param children Child indices.
 * @param hash_table Per-node hash table.
 */
void hash_communative_children(size_t &seed, const std::vector<size_t> &children, const std::vector<size_t> &hash_table);

/**
 * @brief Propagate a node hash contribution to parent nodes.
 * @param seed Node hash contribution to propagate.
 * @param parents Parent metadata as (parent_index, operand_position).
 * @param hash_table Per-node hash table.
 */
void hash_parents(size_t &seed, const std::vector<std::pair<size_t, size_t>> &parents, const std::vector<size_t> &hash_table);
}  // namespace stabilizer::util