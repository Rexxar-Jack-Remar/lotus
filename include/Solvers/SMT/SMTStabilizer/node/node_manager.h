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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "parser/dag.h"
#include "parser/kind.h"
#include "parser/parser.h"
#include "parser/sort.h"
namespace stabilizer::node {
using Node = std::shared_ptr<stabilizer::parser::DAGNode>;
using Sort = std::shared_ptr<stabilizer::parser::Sort>;

class NodeManager {
  public:
    NodeManager(stabilizer::parser::Parser &parser) : m_parser(parser) {}

    // Node mk_term(const stabilizer::parser::NODE_KIND& kind, const std::vector<Node>& children = {}, const std::vector<Node>& indices = {});

    std::vector<Node> assertions() const { return m_parser.getAssertions(); }

    std::string to_string();

    void replace_assertions(const std::vector<Node> &new_assertions) {
        m_parser.replaceAssertions(new_assertions);
        // m_parser.replaceAssertions({m_parser.mkAnd(new_assertions)});
    }

    std::unordered_map<std::string, size_t> &getVarNames() {
        return m_parser.getVarNames();
    }

    std::unordered_map<std::string, size_t> &getTempVarNames() {
        return m_parser.getTempVarNames();
    }

    std::unordered_map<std::string, std::shared_ptr<stabilizer::parser::DAGNode>> &getFunKeyMap() {
        return m_parser.getFunKeyMap();
    }

    std::vector<std::string> &getFunctionNames() {
        return m_parser.getFunctionNames();
    }

    std::unique_ptr<parser::NodeManager> getNodeManager() {
        return m_parser.getNodeManager();
    }

    void rebuild_node_manager() {
        m_parser.rebuild_node_manager();
    }

    void simplify_assertions() {
        auto assertions = m_parser.getAssertions();
        parser::NODE_KIND t = parser::NODE_KIND::NT_AND;
        Node simplified = m_parser.rewrite(t, assertions);
        if (simplified != nullptr)
            m_parser.replaceAssertions({simplified});
        else
            m_parser.replaceAssertions(assertions);
    }

    std::unordered_map<std::string, Sort> &getSortNames() {
        return m_parser.getSortNames();
    }

    Node mkUFVNode(const std::string &name, const Sort &sort) {
        return m_parser.mkUFVNode(name, sort);
    }

    std::vector<std::vector<parser::Parser::DTTypeDecl>> &getDatatypeBlocks() {
        return m_parser.getDatatypeBlocks();
    }

    Node mkAnd(const std::vector<Node> &children) {
        return m_parser.mkAnd(children);
    }

  private:
    stabilizer::parser::Parser &m_parser;
};
}  // namespace stabilizer::node