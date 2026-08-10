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

#include "node_helper.h"

#include <cstdint>
namespace stabilizer::util {
size_t hash_node(const node::Node &node) {
    size_t seed = 0;
    size_t h1 = node->getSort()->hash_without_name();
    size_t h2 = static_cast<size_t>(node->getKind());
    size_t h3 = node->getChildren().size();
    size_t h4 = node->isConst() ? std::hash<std::string>{}(node->toString()) : 0;

    hash_combine(seed, h1);
    hash_combine(seed, h2);
    hash_combine(seed, h3);
    hash_combine(seed, h4);

    return seed;
}

size_t hash_mix(size_t x) {
    constexpr size_t m = 0xe9846af9b1a615d;
    x ^= x >> 32;
    x *= m;
    x ^= x >> 32;
    x *= m;
    x ^= x >> 28;
    return x;
}

void hash_combine(size_t &seed, const size_t &value) {
    seed = hash_mix(seed + 0x9e3779b9 + value);
    // boost::hash_combine(seed, value);
}

void hash_children(size_t &seed, const std::vector<size_t> &children, const std::vector<size_t> &hash_table) {
    for (const auto &child_index : children)
        hash_combine(seed, hash_table.at(child_index));
    // hash_combine(seed, SIZE_MAX);
}

void hash_communative_children(size_t &seed, const std::vector<size_t> &children, const std::vector<size_t> &hash_table) {
    const size_t s2(seed);
    for (const auto &child : children) {
        size_t s3(s2);
        hash_combine(s3, hash_table.at(child));
        seed += s3;
    }
    // hash_combine(seed, SIZE_MAX);
}

void hash_parents(size_t &seed, const std::vector<std::pair<size_t, size_t>> &parents, const std::vector<size_t> &hash_table) {
    const size_t s2(seed);
    for (const auto &[parent_index, oper_index] : parents) {
        size_t s3(s2);
        hash_combine(s3, hash_table.at(parent_index));
        hash_combine(s3, oper_index);
        seed += s3;
    }
    // hash_combine(seed, SIZE_MAX);
}
}  // namespace stabilizer::util