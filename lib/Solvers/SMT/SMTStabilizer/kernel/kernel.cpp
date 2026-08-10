// Copyright (c) 2026 XiangZhang
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "kernel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "node/node_manager.h"
#include "parser/kind.h"
#include "parser/sort.h"
#include "util/node_helper.h"

namespace stabilizer::kernel {

/**
 * @brief Construct graph caches and reverse-topological node storage.
 *
 * Algorithm outline:
 * 1. Traverse assertion roots and collect reachable DAG nodes.
 * 2. Build forward and reverse adjacency with operator-position metadata.
 * 3. Classify symbol-like nodes and initialize base hash seeds.
 */
Kernel::Kernel(node::NodeManager &nm, const bool &context_propagation, const bool &symmetry_breaking_perturbation) : d_context_propagation(context_propagation), d_symmetry_breaking_perturbation(symmetry_breaking_perturbation) {
    std::vector<node::Node> visit(nm.assertions());
    std::unordered_map<node::Node, size_t> node2index;
    // node2index.emplace(nm.assertion(), 0);

    while (!visit.empty()) {
        auto node = visit.back();
        auto [itr, success] = node2index.emplace(node, SIZE_MAX);
        if (success) {
            for (const auto &child : node->getChildren()) {
                visit.emplace_back(child);
            }
            if (node->isUFApplication())
                visit.emplace_back(nm.mkUFVNode(node->getName(), node->getSort()));
        }
        else if (itr->second == SIZE_MAX) {
            // std::cout << node->toString() << ' ' << node->getSort()->isDec() << std::endl;
            visit.pop_back();
            itr->second = d_nodes.size();
            d_nodes.emplace_back(node);
            d_is_commutative.emplace_back(is_commutative(d_nodes.size() - 1, false));
            d_is_symbol.emplace_back(node->isVar() || (node->isArray() && !node->isConstArray()) || node->isVUF() || node->isUFName());
            d_graph.emplace_back();
            d_reversed_graph.emplace_back();
            if (d_is_symbol.back())
                d_symbols.emplace_back(d_nodes.size() - 1);

            d_hash_table.emplace_back(util::hash_node(node));
            for (size_t i = 0, isz = node->getChildrenSize(); i < isz; ++i) {
                const auto &child = node->getChild(i);
                const size_t &child_index = node2index.at(child);
                d_graph.back().emplace_back(child_index);
                if (child->getKind() == stabilizer::parser::NODE_KIND::NT_FP_ADD ||
                    child->getKind() == stabilizer::parser::NODE_KIND::NT_FP_MUL ||
                    child->getKind() == stabilizer::parser::NODE_KIND::NT_EXISTS ||
                    child->getKind() == stabilizer::parser::NODE_KIND::NT_FORALL)
                    d_reversed_graph.at(child_index).emplace_back(d_nodes.size() - 1, std::min<size_t>(i, 0));
                else
                    d_reversed_graph.at(child_index).emplace_back(d_nodes.size() - 1, d_is_commutative.back() ? 0 : i);
            }
            if (node->isUFApplication()) {
                const size_t &uf_index = node2index.at(nm.mkUFVNode(node->getName(), node->getSort()));
                d_graph.back().emplace_back(uf_index);
                d_reversed_graph.at(uf_index).emplace_back(d_nodes.size() - 1, node->getChildrenSize());
            }
        }
        else
            visit.pop_back();
    }

    d_processing.resize(d_nodes.size());
    std::iota(d_processing.begin(), d_processing.end(), 0);
    d_visited.resize(d_nodes.size(), false);
    d_symbol_num = d_symbols.size();

    for (const auto &assertion : nm.assertions()) {
        util::hash_combine(d_hash_table.at(node2index.at(assertion)), static_cast<size_t>(parser::NODE_KIND::NT_AND));
    }
    // std::cout << d_reversed_graph.at(d_symbols.at(0)).size() << ' ' << d_reversed_graph.at(d_symbols.at(1)).size() << std::endl;
    // std::cout << d_nodes.size() << std::endl;
}

bool Kernel::is_commutative(const size_t &i, const bool &from_cache) {
    // return false;
    if (from_cache)
        return d_is_commutative.at(i);
    const auto &node = d_nodes.at(i);
    switch (node->getKind()) {
        case stabilizer::parser::NODE_KIND::NT_ADD:
        case stabilizer::parser::NODE_KIND::NT_MUL:
        case stabilizer::parser::NODE_KIND::NT_AND:
        case stabilizer::parser::NODE_KIND::NT_IAND:
        case stabilizer::parser::NODE_KIND::NT_OR:
        case stabilizer::parser::NODE_KIND::NT_XOR:
        case stabilizer::parser::NODE_KIND::NT_BV_ADD:
        case stabilizer::parser::NODE_KIND::NT_BV_MUL:
        case stabilizer::parser::NODE_KIND::NT_BV_AND:
        case stabilizer::parser::NODE_KIND::NT_BV_OR:
        case stabilizer::parser::NODE_KIND::NT_BV_XOR:
        case stabilizer::parser::NODE_KIND::NT_EQ:
        case stabilizer::parser::NODE_KIND::NT_DISTINCT:
        case stabilizer::parser::NODE_KIND::NT_MAX:
        case stabilizer::parser::NODE_KIND::NT_MIN:
        case stabilizer::parser::NODE_KIND::NT_BV_NAND:
        case stabilizer::parser::NODE_KIND::NT_BV_NOR:
        case stabilizer::parser::NODE_KIND::NT_BV_COMP:
        case stabilizer::parser::NODE_KIND::NT_BV_SADDO:
        case stabilizer::parser::NODE_KIND::NT_BV_UADDO:
        case stabilizer::parser::NODE_KIND::NT_BV_UMULO:
        case stabilizer::parser::NODE_KIND::NT_BV_XNOR:
        case stabilizer::parser::NODE_KIND::NT_FP_EQ:
        case stabilizer::parser::NODE_KIND::NT_FP_MIN:
        case stabilizer::parser::NODE_KIND::NT_FP_MAX:

        // Quantifiers are treated as commutative for hashing purposes
        case stabilizer::parser::NODE_KIND::NT_FORALL:
        case stabilizer::parser::NODE_KIND::NT_EXISTS:

        // FP_ADD, FP_MUL, SUB, DIV_REAL are commutative from index one
        case stabilizer::parser::NODE_KIND::NT_FP_ADD:
        case stabilizer::parser::NODE_KIND::NT_FP_MUL:
            // case stabilizer::parser::NODE_KIND::NT_SUB:
            // case stabilizer::parser::NODE_KIND::NT_DIV_REAL:
            return true;
        default:
            return false;
    }
}

void Kernel::propagate(const std::vector<size_t> &processing) {
    if (!d_context_propagation) {
        static std::vector<size_t> pre_hash_table = d_hash_table;
        for (const auto &node : processing)
            pre_hash_table.at(node) = d_hash_table.at(node);
        std::vector<size_t> v(processing);
        for (const auto &node : v) {
            const auto &children = d_graph.at(node);
            if (children.empty()) continue;
            if (is_commutative(node)) {
                util::hash_communative_children(pre_hash_table.at(node), children, d_hash_table);
            }
            else {
                util::hash_children(pre_hash_table.at(node), children, d_hash_table);
            }
            util::hash_parents(pre_hash_table.at(node), d_reversed_graph.at(node), d_hash_table);
        }

        for (const auto &node : v)
            d_hash_table.at(node) = pre_hash_table.at(node);
    }
    else {
        for (const auto &node : processing) {
            const auto &children = d_graph.at(node);
            if (children.empty()) continue;
            if (is_commutative(node)) {
                util::hash_communative_children(d_hash_table.at(node), children, d_hash_table);
            }
            else {
                util::hash_children(d_hash_table.at(node), children, d_hash_table);
            }
        }
        for (auto it = processing.rbegin(); it != processing.rend(); ++it) {
            const auto &node = *it;
            const auto &parents = d_reversed_graph.at(node);
            if (parents.empty())
                continue;
            util::hash_parents(d_hash_table.at(node), parents, d_hash_table);
        }
    }
}

/**
 * @brief Repeat propagation until symbol hash cardinality converges.
 *
 * The stage tracks unique symbol hashes after each round and stops when the
 * number of unique values no longer increases.
 */
void Kernel::context_propagate(const std::vector<size_t> &processing, const std::vector<size_t> &symbols) {
    size_t hash_count = 0, prev_hash_count = 0;
    std::vector<size_t> unique_hashes(symbols.size());
    do {
        prev_hash_count = hash_count;
        propagate(processing);
        std::transform(symbols.begin(), symbols.end(), unique_hashes.begin(), [this](size_t index) { return d_hash_table.at(index); });
        std::sort(unique_hashes.begin(), unique_hashes.end());
        hash_count = 1;
        for (size_t i = 1, isz = unique_hashes.size(); i < isz; ++i)
            if (unique_hashes[i] != unique_hashes[i - 1])
                ++hash_count;
    } while (prev_hash_count != hash_count);
}

/**
 * @brief Resolve collisions within connected regions by targeted perturbation.
 *
 * Algorithm outline:
 * 1. Explore one unvisited symbol region at a time.
 * 2. Compute region fingerprint and inject region-level tie-break data.
 * 3. If duplicate symbol hashes remain, perturb one selected symbol and rerun
 *    context propagation on the region.
 */
void Kernel::specific_propagate() {
    std::unordered_map<size_t, size_t> hash_count;

    for (const auto &node : d_symbols) {
        if (d_visited.at(node))
            continue;
        std::vector<size_t> processing;
        std::vector<size_t> symbols;
        d_visited.at(node) = true;
        std::vector<size_t> stack{node};
        while (!stack.empty()) {
            auto current = stack.back();
            stack.pop_back();
            processing.emplace_back(current);
            if (d_is_symbol.at(current)) {
                symbols.emplace_back(current);
                // std::cout << d_hash_table.at(current) << ' ' << d_nodes.at(current)->toString() << std::endl;
            }
            for (const auto &[father_index, oper_index] : d_reversed_graph.at(current)) {
                if (!d_visited.at(father_index)) {
                    d_visited.at(father_index) = true;
                    stack.emplace_back(father_index);
                }
            }
            for (const auto &child_index : d_graph.at(current)) {
                if (!d_visited.at(child_index)) {
                    d_visited.at(child_index) = true;
                    stack.emplace_back(child_index);
                }
            }
        }

        size_t symbols_hash = 0;
        util::hash_communative_children(symbols_hash, symbols, d_hash_table);
        hash_count[symbols_hash]++;
        size_t count = hash_count[symbols_hash];
        for (const auto &node : processing) {
            util::hash_combine(d_hash_table.at(node), symbols_hash);
            util::hash_combine(d_hash_table.at(node), count);
        }

        std::vector<size_t> symbol_hashes(symbols.size());
        std::transform(symbols.begin(), symbols.end(), symbol_hashes.begin(), [this](size_t index) { return d_hash_table.at(index); });
        std::sort(symbol_hashes.begin(), symbol_hashes.end());
        size_t idx_hash = 0;
        bool is_select = false;
        for (size_t i = 1, isz = symbol_hashes.size(); i < isz; ++i) {
            if (symbol_hashes[i] == symbol_hashes[i - 1]) {
                idx_hash = symbol_hashes.at(i);
                is_select = true;
                break;
            }
        }

        if (is_select) {
            size_t idx = 0;
            for (size_t i = 0, isz = symbols.size(); i < isz; ++i) {
                if (d_hash_table.at(symbols.at(i)) == idx_hash) {
                    idx = symbols.at(i);
                    break;
                }
            }
            util::hash_combine(d_hash_table.at(idx), 0);
            std::sort(processing.begin(), processing.end());
            context_propagate(processing, symbols);
        }
    }
    for (const auto &node : d_processing)
        d_visited.at(node) = false;
}

/**
 * @brief Rebuild active processing graph to unresolved nodes only.
 *
 * Nodes with unique hash values are pruned from subsequent iterations; the
 * remaining ambiguous nodes keep only edges to other ambiguous nodes.
 */
void Kernel::rebuild_graph() {
    std::unordered_map<size_t, size_t> hash_count;
    for (const auto &node : d_processing)
        hash_count[d_hash_table.at(node)]++;
    for (const auto &node : d_processing) {
        if (hash_count.at(d_hash_table.at(node)) > 1)
            d_visited.at(node) = true;
    }

    d_symbols.clear();
    std::vector<size_t> unique_symbols;
    std::vector<size_t> new_processing;
    for (const auto &node : d_processing) {
        if (!d_visited.at(node)) {
            d_graph.at(node).clear();
            d_reversed_graph.at(node).clear();
            if (d_is_symbol.at(node))
                unique_symbols.emplace_back(node);
            continue;
        }
        new_processing.emplace_back(node);
        d_graph.at(node).erase(std::remove_if(d_graph.at(node).begin(), d_graph.at(node).end(), [this](const size_t &child_index) {
                                   return !d_visited.at(child_index);
                               }),
                               d_graph.at(node).end());
        d_reversed_graph.at(node).erase(std::remove_if(d_reversed_graph.at(node).begin(), d_reversed_graph.at(node).end(), [this](const std::pair<size_t, size_t> &father_info) {
                                            return !d_visited.at(father_info.first);
                                        }),
                                        d_reversed_graph.at(node).end());
        if (d_is_symbol.at(node) && d_visited.at(node))
            d_symbols.emplace_back(node);
    }
    std::sort(unique_symbols.begin(), unique_symbols.end(), [this](const size_t &a, const size_t &b) {
        if (d_context_hash.at(a) == d_context_hash.at(b))
            return d_hash_table.at(a) < d_hash_table.at(b);
        return d_context_hash.at(a) < d_context_hash.at(b);
    });
    for (const auto &node : unique_symbols) {
        d_unique_symbols.emplace_back(node);
    }

    for (const auto &node : d_processing)
        d_visited.at(node) = false;
    d_processing.swap(new_processing);
    // std::cout << d_processing.size() << std::endl;
}

/**
 * @brief Propagate sort/datatype constraints into hash refinement.
 *
 * This stage temporarily augments the graph with sort vertices, stabilizes
 * their ordering, renames sort/datatype symbols, and then restores core graph
 * structures while keeping updated hash information.
 */
void Kernel::sort_propagate(std::unordered_map<std::string, node::Sort> &sort_key_map, std::vector<std::vector<parser::Parser::DTTypeDecl>> &datatype_blocks) {
    std::unordered_map<std::string, node::Sort> new_sort_key_map;
    std::unordered_map<node::Sort, size_t> tmp_dts;
    size_t _cnt = 0;
    for (auto &block : datatype_blocks) {
        ++_cnt;
        for (size_t i = 0, isz = block.size(); i < isz; ++i) {
            auto &td = block.at(i);
            if (sort_key_map.find(td.name) != sort_key_map.end()) {
                tmp_dts.emplace(sort_key_map.at(td.name), _cnt);
                sort_key_map.erase(td.name);
            }
        }
    }

    std::unordered_map<node::Sort, size_t> sort_idx;
    std::vector<std::vector<size_t>> graph;
    std::vector<std::vector<std::pair<size_t, size_t>>> reversed_graph;

    for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
        if (d_nodes.at(i)->getSort()->isDec()) {
            if (sort_idx.find(d_nodes.at(i)->getSort()) == sort_idx.end()) {
                sort_idx[d_nodes.at(i)->getSort()] = sort_idx.size();
                graph.emplace_back();
                reversed_graph.emplace_back();
            }
        }
    }

    for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
        if (d_nodes.at(i)->getSort()->children.empty())
            continue;
        for (auto &child_sort : d_nodes.at(i)->getSort()->children) {
            if ((child_sort->isDec() || child_sort->isDatatype()) && sort_idx.find(child_sort) == sort_idx.end()) {
                new_sort_key_map.emplace("UNUSED_SORT", child_sort);
                child_sort->setName("UNUSED_SORT");
            }
        }
    }

    for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
        graph.emplace_back(d_graph.at(i));
        reversed_graph.emplace_back(d_reversed_graph.at(i));
        for (auto &child : graph.back()) {
            child += sort_idx.size();
        }
        for (auto &[father_index, oper_index] : reversed_graph.back()) {
            father_index += sort_idx.size();
        }
        if (d_nodes.at(i)->getSort()->isDec()) {
            size_t sort_index = sort_idx.at(d_nodes.at(i)->getSort());
            // graph.at(sort_index).emplace_back(i + sort_idx.size());
            // reversed_graph.at(i + sort_idx.size()).emplace_back(sort_index, 0);
            graph.at(i + sort_idx.size()).emplace_back(sort_index);
            reversed_graph.at(sort_index).emplace_back(i + sort_idx.size(), 0);
        }
    }

    std::vector<size_t> hash_table(d_hash_table);
    std::reverse(hash_table.begin(), hash_table.end());
    hash_table.resize(hash_table.size() + sort_idx.size(), 0);
    std::reverse(hash_table.begin(), hash_table.end());
    for (const auto &[sort, index] : tmp_dts) {
        if (sort_idx.find(sort) != sort_idx.end()) {
            util::hash_combine(hash_table.at(sort_idx.at(sort)), index);
            util::hash_combine(hash_table.at(sort_idx.at(sort)), index);
        }
    }
    std::vector<uint8_t> is_commutative(d_is_commutative);
    std::reverse(is_commutative.begin(), is_commutative.end());
    is_commutative.resize(is_commutative.size() + sort_idx.size(), false);
    std::reverse(is_commutative.begin(), is_commutative.end());

    d_graph.swap(graph);
    d_reversed_graph.swap(reversed_graph);
    d_hash_table.swap(hash_table);
    d_is_commutative.swap(is_commutative);

    std::vector<size_t> processing(d_graph.size());
    std::iota(processing.begin(), processing.end(), 0);

    std::vector<size_t> symbols(sort_idx.size());
    std::iota(symbols.begin(), symbols.end(), 0);
    for (size_t i = 0, isz = d_symbols.size(); i < isz; ++i) {
        auto idx = d_symbols.at(i);
        if (!d_nodes.at(idx)->getSort()->isDec()) {
            symbols.emplace_back(idx + sort_idx.size());
        }
    }
    // std::cout << processing.size() << ' ' << symbols.size() << std::endl;
    std::vector<size_t> unique_hashes(sort_idx.size());

    // apply_nauty(d_graph, d_hash_table);
    // exit(0);

    do {
        // std::cout << "!!!\n";
        context_propagate(processing, symbols);
        std::transform(symbols.begin(), symbols.begin() + sort_idx.size(), unique_hashes.begin(), [this](size_t index) { return d_hash_table.at(index); });
        std::sort(unique_hashes.begin(), unique_hashes.end());
        // for (size_t i = 0, isz = unique_hashes.size(); i < isz; ++i)
        //     std::cout << d_hash_table.at(i) << ' ';
        // std::cout << std::endl;
        // exit(0);
        size_t hash_value = 0;
        bool is_select = false;
        for (size_t i = 1, isz = unique_hashes.size(); i < isz; ++i)
            if (unique_hashes[i] == unique_hashes[i - 1]) {
                hash_value = unique_hashes.at(i);
                is_select = true;
                break;
            }
        if (!is_select)
            break;
        size_t sort_index = 0;
        for (size_t i = 0, isz = sort_idx.size(); i < isz; ++i) {
            if (d_hash_table.at(symbols.at(i)) == hash_value) {
                sort_index = symbols.at(i);
                break;
            }
        }
        // std::cout << d_hash_table.at(sort_index) << ' ' << hash_value << std::endl;
        util::hash_combine(d_hash_table.at(sort_index), 0);
        // std::cout << "!!!" << d_hash_table.at(sort_index) << std::endl;

    } while (true);

    std::vector<std::pair<size_t, node::Sort>> sort_hashes;
    sort_hashes.reserve(sort_idx.size());
    for (const auto &[sort, index] : sort_idx) {
        sort_hashes.emplace_back(d_hash_table.at(index), sort);
    }

    std::sort(sort_hashes.begin(), sort_hashes.end());
    std::unordered_map<std::string, std::string> dt_name_map;
    size_t sort_count = 0, dt_count = 0;
    for (size_t i = 0, isz = sort_hashes.size(); i < isz; ++i) {
        if (sort_key_map.find(sort_hashes.at(i).second->name) == sort_key_map.end()) {
            dt_name_map[sort_hashes.at(i).second->name] = "DT" + std::to_string(dt_count);
            sort_hashes.at(i).second->setName("DT" + std::to_string(dt_count));
            new_sort_key_map["DT" + std::to_string(dt_count)] = sort_hashes.at(i).second;
            ++dt_count;
        }
        else {
            sort_hashes.at(i).second->setName("SORT" + std::to_string(sort_count));
            new_sort_key_map["SORT" + std::to_string(sort_count)] = sort_hashes.at(i).second;
            ++sort_count;
        }
    }

    std::unordered_map<std::string, size_t> uf_name_id;
    for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
        if (d_nodes.at(i)->isUFName())
            uf_name_id[d_nodes.at(i)->getName()] = i + sort_idx.size();
    }

    for (auto &block : datatype_blocks) {
        for (size_t i = 0, isz = block.size(); i < isz; ++i) {
            auto &td = block.at(i);
            if (dt_name_map.find(td.name) != dt_name_map.end()) {
                td.name = dt_name_map.at(td.name);
                for (auto &cd : td.ctors) {
                    if (uf_name_id.find(cd.name) != uf_name_id.end()) {
                        util::hash_combine(d_hash_table.at(uf_name_id.at(cd.name)), std::hash<std::string>{}(td.name));
                        // for (auto &sd : cd.selectors) {
                        //     if (uf_name_id.find(sd.name) != uf_name_id.end()) {
                        //         util::hash_combine(d_hash_table.at(uf_name_id.at(sd.name)), d_hash_table.at(uf_name_id.at(cd.name)));
                        //     }
                        // }
                        for (size_t i = 0, isz = cd.selectors.size(); i < isz; ++i) {
                            auto &sd = cd.selectors.at(i);
                            if (uf_name_id.find(sd.name) != uf_name_id.end()) {
                                util::hash_combine(d_hash_table.at(uf_name_id.at(sd.name)), d_hash_table.at(uf_name_id.at(cd.name)));
                                util::hash_combine(d_hash_table.at(uf_name_id.at(sd.name)), i);
                            }
                        }
                    }
                    else {
                        // for (auto &sd : cd.selectors) {
                        //     if (uf_name_id.find(sd.name) != uf_name_id.end()) {
                        //         util::hash_combine(d_hash_table.at(uf_name_id.at(sd.name)), std::hash<std::string>{}(td.name));
                        //     }
                        // }
                        for (size_t i = 0, isz = cd.selectors.size(); i < isz; ++i) {
                            auto &sd = cd.selectors.at(i);
                            if (uf_name_id.find(sd.name) != uf_name_id.end()) {
                                util::hash_combine(d_hash_table.at(uf_name_id.at(sd.name)), std::hash<std::string>{}(td.name));
                                util::hash_combine(d_hash_table.at(uf_name_id.at(sd.name)), i);
                            }
                        }
                    }
                }
            }
        }
    }

    sort_key_map.swap(new_sort_key_map);
    d_graph.swap(graph);
    d_reversed_graph.swap(reversed_graph);
    // d_hash_table.swap(hash_table);
    std::reverse(d_hash_table.begin(), d_hash_table.end());
    d_hash_table.resize(d_hash_table.size() - sort_idx.size());
    std::reverse(d_hash_table.begin(), d_hash_table.end());
    d_is_commutative.swap(is_commutative);
}

/**
 * @brief Execute the full kernel pipeline on node manager state.
 *
 * High-level flow:
 * 1. Seed hashes for function definitions and run global context propagation.
 * 2. Optionally refine with sort/datatype-aware propagation.
 * 3. Resolve collisions via specific propagation and graph rebuild rounds.
 * 4. Canonicalize symbol/function names and reorder declarations/assertions.
 */
void Kernel::apply(node::NodeManager &nm) {
    std::vector<std::string> &function_names = nm.getFunctionNames();
    std::unordered_map<std::string, std::shared_ptr<stabilizer::parser::DAGNode>> &function_key_map = nm.getFunKeyMap();

    std::unordered_map<std::string, size_t> def_fun_id;
    size_t def_fun_count = 0;
    for (const auto &func_name : function_names) {
        if (function_key_map.at(func_name)->isFuncDef())
            def_fun_id[func_name] = def_fun_count++;
    }

    for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
        if (d_nodes.at(i)->isFuncDef()) {
            util::hash_combine(d_hash_table.at(i), def_fun_id.at(d_nodes.at(i)->getName()));
        }
    }
    context_propagate(d_processing, d_symbols);
    d_context_hash = d_hash_table;

    if (std::any_of(d_nodes.begin(), d_nodes.end(), [this](const node::Node &node) { return node->getSort()->isDec() || node->getSort()->isDatatype(); })) {
        sort_propagate(nm.getSortNames(), nm.getDatatypeBlocks());
    }
    else {
        // exit(0);
        nm.getSortNames().clear();
        nm.getDatatypeBlocks().clear();
    }
    if (!d_symmetry_breaking_perturbation) {
        while (true) {
            std::unordered_map<size_t, size_t> hash_count;
            bool flag = true;
            for (const auto &node : d_processing) {
                hash_count[d_hash_table.at(node)]++;
                if (hash_count[d_hash_table.at(node)] > 1) {
                    flag = false;
                    break;
                }
            }
            if (flag)
                break;
            else
                specific_propagate();
        }
    }
    else {
        while (d_unique_symbols.size() != d_symbol_num) {
            rebuild_graph();
            specific_propagate();
        }
    }

    // d_hash_table = d_context_hash;
    std::unordered_map<std::string, size_t> &var_names = nm.getVarNames();
    std::unordered_map<std::string, size_t> new_var_names;
    std::unordered_map<std::string, size_t> &temp_var_names = nm.getTempVarNames();
    std::unordered_map<std::string, size_t> new_temp_var_names;
    std::unordered_map<std::string, std::string> var_names_map;

    std::unordered_map<std::string, std::string> function_names_map;
    size_t symbol_count = 0, uf_count = 0;
    for (size_t i = 0, isz = d_unique_symbols.size(); i < isz; ++i) {
        if (d_nodes.at(d_unique_symbols.at(i))->isUFName()) {
            // std::cout << d_context_hash.at(d_unique_symbols.at(i)) << ' ' << d_hash_table.at(d_unique_symbols.at(i)) << std::endl;
            d_hash_table.at(d_unique_symbols.at(i)) = ++uf_count;
            util::hash_combine(d_hash_table.at(d_unique_symbols.at(i)), 0);
            std::string uf_name = d_nodes.at(d_unique_symbols.at(i))->getName();
            d_nodes.at(d_unique_symbols.at(i))->setName("UF" + std::to_string(uf_count));
            function_names_map[uf_name] = "UF" + std::to_string(uf_count);
        }
        else {
            d_hash_table.at(d_unique_symbols.at(i)) = ++symbol_count;
            std::string var_name = d_nodes.at(d_unique_symbols.at(i))->getName();
            // std::cout << var_name << std::endl;
            // if (symbol_count == 749)
            //     std::cout << var_name << std::endl;
            // d_nodes.at(d_unique_symbols.at(i))->setName("|S<" + var_name + ">" + std::to_string(symbol_count) + "|");
            // var_names_map[var_name] = "|S<" + var_name + ">" + std::to_string(symbol_count) + "|";
            d_nodes.at(d_unique_symbols.at(i))->setName("S" + std::to_string(symbol_count));
            var_names_map[var_name] = "S" + std::to_string(symbol_count);
        }
    }
    // for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
    //     if (d_nodes.at(i)->isLe()) {
    //         std::cout << d_nodes.at(i)->toString() << ' '
    //                   << parser::kindToString(d_nodes.at(i)->getKind()) << ' '
    //                   << static_cast<size_t>(d_nodes.at(i)->getKind()) << ' '
    //                   << d_nodes.at(i)->getChildrenSize() << ' '
    //                   << d_nodes.at(i)->getSort()->toString() << ' '
    //                   << d_hash_table.at(i) << ' '
    //                   << d_hash_table.at(d_graph.at(i).front()) << ' '
    //                   << d_hash_table.at(d_graph.at(i).back())
    //                   << std::endl;
    //     }
    // }
    std::vector<std::pair<size_t, size_t>> dt_testers;
    for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
        if (d_nodes.at(i)->getKind() == parser::NODE_KIND::NT_DT_TESTER) {
            std::string vuf_name = d_nodes.at(i)->getName();
            if (function_names_map.find(vuf_name) != function_names_map.end()) {
                // std::cout << "!!!" << std::endl;
                d_nodes.at(i)->setName(function_names_map.at(vuf_name));
            }
            else {
                // std::cout << d_hash_table.at(i) << std::endl;
                dt_testers.emplace_back(d_hash_table.at(i), i);
            }
        }
    }

    std::sort(dt_testers.begin(), dt_testers.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });

    for (const auto &[hash_value, idx] : dt_testers) {
        uf_count++;
        d_hash_table.at(idx) = ++uf_count;
        util::hash_combine(d_hash_table.at(idx), 0);
        function_names_map[d_nodes.at(idx)->getName()] = "UF" + std::to_string(uf_count);
        d_nodes.at(idx)->setName("UF" + std::to_string(uf_count));
    }

    size_t con_idx = 0;
    auto new_blocks = nm.getDatatypeBlocks();
    new_blocks.clear();
    for (auto &block : nm.getDatatypeBlocks()) {
        std::sort(block.begin(), block.end(), [](const auto &a, const auto &b) {
            return a.name < b.name;
        });
        auto new_block = block;
        new_block.clear();
        for (auto &td : block) {
            if (td.name.compare(0, 2, "DT") != 0 &&
                td.name.compare(0, 4, "SORT") != 0) {
                continue;
            }
            auto new_ctors = td.ctors;
            new_ctors.clear();
            for (auto &cd : td.ctors) {
                if (function_names_map.find(cd.name) != function_names_map.end()) {
                    cd.name = function_names_map.at(cd.name);
                }
                else {
                    cd.name = "";
                }
                auto new_selectors = cd.selectors;
                new_selectors.clear();
                for (auto &sd : cd.selectors) {
                    if (function_names_map.find(sd.name) != function_names_map.end()) {
                        sd.name = function_names_map.at(sd.name);
                        new_selectors.emplace_back(sd);
                    }
                    // else {
                    //     sd.name = "";
                    // }
                    else if (sd.sort->name.compare(0, 4, "SORT") == 0 ||
                             sd.sort->name.compare(0, 2, "DT") == 0) {
                        sd.name = "";
                        new_selectors.emplace_back(sd);
                    }
                    else if (sd.sort->isDec() || sd.sort->isDatatype()) {
                        sd.sort->setName("UNUSED_SORT");
                        nm.getSortNames().emplace("UNUSED_SORT", sd.sort);
                        sd.name = "";
                        new_selectors.emplace_back(sd);
                    }
                    else {
                        sd.name = "";
                        new_selectors.emplace_back(sd);
                    }
                }

                if (cd.name.empty()) {
                    bool selected = false;
                    for (const auto &sd : new_selectors) {
                        if (!sd.name.empty()) {
                            selected = true;
                            cd.selectors.swap(new_selectors);
                            break;
                        }
                    }
                    if (!selected)
                        continue;
                }
                else
                    cd.selectors.swap(new_selectors);
                // if (selected == false) {
                //     cd.selectors.clear();
                // }
                // cd.selectors.swap(new_selectors);
                // if (cd.name == "UF296") {
                //     for (const auto& sd : cd.selectors) {
                //         std::cout << sd.name << ' ' << sd.sort->toString() << std::endl;
                //     }
                // }
                // {
                //     std::unordered_map<node::Sort, std::vector<decltype(cd.selectors)::value_type>> buckets;
                //     buckets.reserve(cd.selectors.size());
                //     for (const auto &sd : cd.selectors) {
                //         buckets[sd.sort].push_back(sd);
                //     }

                //     for (auto &[s, vec] : buckets) {
                //         std::sort(vec.begin(), vec.end(), [](const auto &a, const auto &b) {
                //             return a.name < b.name;
                //         });
                //     }

                //     std::unordered_map<node::Sort, size_t> idx;
                //     idx.reserve(buckets.size());
                //     std::vector<decltype(cd.selectors)::value_type> reordered;
                //     reordered.reserve(cd.selectors.size());
                //     for (const auto &sd : cd.selectors) {
                //         auto &i = idx[sd.sort];
                //         reordered.emplace_back(buckets[sd.sort][i]);
                //         ++i;
                //     }
                //     cd.selectors.swap(reordered);
                // }
                size_t idx = 0;
                for (size_t i = 0, isz = cd.selectors.size(); i < isz; ++i) {
                    if (cd.selectors.at(i).name.empty())
                        cd.selectors.at(i).name = "VAR" + std::to_string(idx++);
                }
                // std::sort(cd.selectors.begin(), cd.selectors.end(), [](const auto& a, const auto& b) {
                //     return a.name < b.name;
                // });
                new_ctors.emplace_back(cd);
            }
            bool need_two = td.ctors.size() >= 2;
            bool has_zero = false;
            bool only_zero = true;
            for (const auto &cd : td.ctors) {
                if (!cd.selectors.empty()) {
                    only_zero = false;
                }
                else {
                    has_zero = true;
                }
                if (has_zero && !only_zero) {
                    break;
                }
            }
            td.ctors.swap(new_ctors);
            std::sort(td.ctors.begin(), td.ctors.end(), [](const auto &a, const auto &b) {
                if (a.name != b.name)
                    return a.name < b.name;
                else if (a.selectors.size() == b.selectors.size()) {
                    for (size_t i = 0, isz = a.selectors.size(); i < isz; ++i) {
                        if (a.selectors.at(i).name != b.selectors.at(i).name)
                            return a.selectors.at(i).name < b.selectors.at(i).name;
                    }
                    return false;
                }
                else
                    return a.selectors.size() < b.selectors.size();
            });

            for (auto &cd : td.ctors) {
                if (cd.name.empty())
                    cd.name = "CON" + std::to_string(con_idx++);
            }

            while (td.ctors.empty() || (td.ctors.size() < 2 && need_two)) {
                td.ctors.emplace_back();
                if (only_zero)
                    td.ctors.back().name = "CON" + std::to_string(con_idx++);
                else if (td.ctors.empty() || !has_zero || td.ctors.front().selectors.empty()) {
                    td.ctors.back().name = "CON" + std::to_string(con_idx++);
                    td.ctors.back().selectors.emplace_back(td.ctors.back().name + "_TVAR0", std::make_shared<parser::Sort>(parser::SORT_KIND::SK_DEC, "UNUSED_SORT"));
                    td.ctors.back().selectors.back().sort->setName("UNUSED_SORT");
                    nm.getSortNames().emplace("UNUSED_SORT", td.ctors.back().selectors.back().sort);
                }
                else {
                    td.ctors.back().name = "CON" + std::to_string(con_idx++);
                }
            }
            size_t idx = 0;
            for (auto &cd : td.ctors) {
                for (auto &sd : cd.selectors) {
                    if (sd.name.compare(0, 3, "VAR") == 0)
                        sd.name = "DT_SEL" + std::to_string(idx++);
                }
            }
            // std::sort(td.ctors.begin(), td.ctors.end(), [](const auto& a, const auto& b) {
            //     return a.name < b.name;
            // });
            new_block.emplace_back(td);
        }
        block.swap(new_block);
        if (!block.empty())
            new_blocks.emplace_back(block);

        // std::sort(new_block.begin(), new_block.end(), [](const parser::Parser::DTTypeDecl& a, const parser::Parser::DTTypeDecl& b) {
        //     return a.name < b.name;
        // });
    }
    nm.getDatatypeBlocks().swap(new_blocks);

    for (const auto &[name, index] : var_names) {
        auto itr = var_names_map.find(name);
        if (itr != var_names_map.end()) {
            new_var_names[itr->second] = index;
        }
    }
    for (const auto &[name, index] : temp_var_names) {
        auto itr = var_names_map.find(name);
        if (itr != var_names_map.end()) {
            new_temp_var_names[itr->second] = index;
        }
    }

    var_names.swap(new_var_names);
    temp_var_names.swap(new_temp_var_names);

    std::unordered_map<node::Node, size_t> node2index;

    std::vector<node::Node> func_dec, func_rec, func_def;

    std::unordered_map<std::string, std::vector<size_t>> uf_buckets;
    std::unordered_map<std::string, std::vector<size_t>> func_buckets;
    for (size_t i = 0, isz = d_nodes.size(); i < isz; ++i) {
        if (d_nodes.at(i)->isUFName()) {
            continue;
        }

        // std::cout << d_nodes.at(i)->toString() << ' ' << d_hash_table.at(i) << std::endl;
        if (d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_UF_APPLY) {
            // std::cout << d_nodes.at(i)->toString() << std::endl;
            d_nodes.at(i)->setName(function_names_map.at(d_nodes.at(i)->getName()));
            util::hash_combine(d_hash_table.at(i), std::hash<std::string>{}(d_nodes.at(i)->getName()));
            // uf_buckets[d_nodes.at(i)->getName()].emplace_back(i);
            // d_nodes.at(i)->setName("UF_" + d_nodes.at(i)->getChild(d_nodes.at(i)->getChildrenSize() - 1)->getName());
        }

        // std::cout << d_nodes.at(i)->toString() << ' ' << d_hash_table.at(i) << std::endl;

        node2index.emplace(d_nodes.at(i), i);
        auto children = d_nodes.at(i)->getChildren();

        if (is_commutative(i)) {
            bool del = (d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_FP_ADD ||
                        d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_FP_MUL ||
                        d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_EXISTS ||
                        d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_FORALL);
            std::sort(children.begin() + del, children.end(), [this, &node2index](const node::Node &a, const node::Node &b) {
                if (d_context_hash.at(node2index.at(a)) == d_context_hash.at(node2index.at(b)))
                    return d_hash_table.at(node2index.at(a)) < d_hash_table.at(node2index.at(b));
                else
                    return d_context_hash.at(node2index.at(a)) < d_context_hash.at(node2index.at(b));
                // return d_hash_table.at(node2index.at(a)) < d_hash_table.at(node2index.at(b));
            });

            d_nodes.at(i)->replace_children(children);
        }

        // std::vector<size_t> children_index(d_nodes.at(i)->getChildrenSize());
        // std::transform(children.begin(), children.end(), children_index.begin(), [&node2index](const node::Node &child) {
        //     return node2index.at(child);
        // });

        if (d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_FUNC_DEC)
            func_dec.emplace_back(d_nodes.at(i));
        else if (d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_FUNC_REC)
            func_rec.emplace_back(d_nodes.at(i));
        else if (d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_FUNC_DEF)
            func_def.emplace_back(d_nodes.at(i));
        else if (d_nodes.at(i)->getKind() == stabilizer::parser::NODE_KIND::NT_FUNC_APPLY) {
            // std::cout << d_nodes.at(i)->toString() << std::endl;
            func_buckets[d_nodes.at(i)->getName()].emplace_back(i);
        }
    }

    std::sort(func_dec.begin(), func_dec.end(), [this, &node2index](const node::Node &a, const node::Node &b) {
        return d_hash_table.at(node2index.at(a)) < d_hash_table.at(node2index.at(b));
    });
    std::sort(func_rec.begin(), func_rec.end(), [this, &node2index](const node::Node &a, const node::Node &b) {
        return d_hash_table.at(node2index.at(a)) < d_hash_table.at(node2index.at(b));
    });
    std::sort(func_def.begin(), func_def.end(), [this, &node2index](const node::Node &a, const node::Node &b) {
        return d_hash_table.at(node2index.at(a)) < d_hash_table.at(node2index.at(b));
    });

    for (size_t i = 0, isz = func_dec.size(); i < isz; i++) {
        function_names_map[func_dec.at(i)->getName()] = "FDEC" + std::to_string(i);
        func_dec.at(i)->setName("FDEC" + std::to_string(i));
    }
    for (size_t i = 0, isz = func_rec.size(); i < isz; i++) {
        function_names_map[func_rec.at(i)->getName()] = "FREC" + std::to_string(i);
        func_rec.at(i)->setName("FREC" + std::to_string(i));
    }
    for (size_t i = 0, isz = func_def.size(); i < isz; i++) {
        for (const auto &index : func_buckets[func_def.at(i)->getName()]) {
            d_nodes.at(index)->setName("FDEF" + std::to_string(i));
        }

        function_names_map[func_def.at(i)->getName()] = "FDEF" + std::to_string(i);
        func_def.at(i)->setName("FDEF" + std::to_string(i));
        util::hash_combine(d_hash_table.at(node2index.at(func_def.at(i))), std::hash<std::string>{}("FDEF" + std::to_string(i)));
    }

    std::unordered_map<std::string, std::shared_ptr<stabilizer::parser::DAGNode>> new_function_key_map;

    std::vector<std::string> ndec, nrec, ndef;
    for (const auto &name : function_names) {
        auto itr = function_names_map.find(name);

        if (itr != function_names_map.end()) {
            if (function_key_map.at(name)->isFuncDec())
                ndec.emplace_back(itr->second);
            else if (function_key_map.at(name)->isFuncRec())
                nrec.emplace_back(itr->second);
            else if (function_key_map.at(name)->isFuncDef())
                ndef.emplace_back(itr->second);

            new_function_key_map[itr->second] = function_key_map.at(name);
            new_function_key_map[itr->second]->setName(itr->second);
            auto fun = new_function_key_map[itr->second];
            if (fun->isFuncDef()) {
                // if (fun->getChildrenSize() == 3)
                //     std::cout << fun->getChildrenSize() << ' ' << fun->getChild(1) << ' ' << fun->getChild(2) << std::endl;
                for (size_t i = 1, isz = fun->getChildrenSize(); i < isz; ++i) {
                    fun->getChild(i)->setName("VAR" + std::to_string(i - 1));
                }
            }
        }
    }
    std::vector<std::string> new_function_names;
    std::sort(ndec.begin(), ndec.end());
    for (const auto &name : ndec)
        new_function_names.emplace_back(name);
    std::sort(nrec.begin(), nrec.end());
    for (const auto &name : nrec)
        new_function_names.emplace_back(name);
    for (const auto &name : ndef)
        new_function_names.emplace_back(name);

    function_names.swap(new_function_names);
    function_key_map.swap(new_function_key_map);

    auto assertions = nm.assertions();

    std::sort(assertions.begin(), assertions.end(), [this, &node2index](const node::Node &a, const node::Node &b) {
        if (d_context_hash.at(node2index.at(a)) == d_context_hash.at(node2index.at(b)))
            return d_hash_table.at(node2index.at(a)) < d_hash_table.at(node2index.at(b));
        else
            return d_context_hash.at(node2index.at(a)) < d_context_hash.at(node2index.at(b));
    });

    // assertions = {nm.mkAnd(assertions)};
    nm.replace_assertions(assertions);
    return;
}  // namespace stabilizer::kernel

}  // namespace stabilizer::kernel
