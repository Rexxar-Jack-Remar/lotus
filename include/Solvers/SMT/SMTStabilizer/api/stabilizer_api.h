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

#pragma once

#include <string>

namespace stabilizer::parser {
class Parser;
} // namespace stabilizer::parser

namespace stabilizer::api {

/**
 * @brief Public options for the SMTStabilizer API.
 *
 * This type is intentionally independent from parser::GlobalOptions so that
 * external users can control only the stable API surface.
 *
 * The default configuration keeps the current behavior of the command-line
 * tool: rewrite is enabled, context propagation is enabled, and subgraph
 * pruning is enabled.
 */
class SMTStabilizerOptions {
  public:
    SMTStabilizerOptions() = default;

    /** Enable or disable parser-side rewrite normalization. */
    void set_rewrite(bool rewrite) noexcept { d_rewrite = rewrite; }
    /** Return whether parser-side rewrite normalization is enabled. */
    bool get_rewrite() const noexcept { return d_rewrite; }

    /** Enable or disable context propagation in the kernel stage. */
    void set_context_propagation(bool value) noexcept { d_context_propagation = value; }
    /** Return whether context propagation is enabled in the kernel stage. */
    bool get_context_propagation() const noexcept { return d_context_propagation; }

    /** Enable or disable subgraph pruning in the kernel stage. */
    void set_subgraph_pruning(bool value) noexcept { d_subgraph_pruning = value; }
    /** Return whether subgraph pruning is enabled in the kernel stage. */
    bool get_subgraph_pruning() const noexcept { return d_subgraph_pruning; }

  private:
    bool d_rewrite = true;
    bool d_context_propagation = true;
    bool d_subgraph_pruning = true;
};

/**
 * @brief High-level facade for stabilizing SMT-LIB2 inputs.
 *
 * The facade accepts either a file path or an SMT-LIB2 script string, runs the
 * existing parser/node/kernel pipeline, and returns the normalized SMT2 text.
 *
 * API-to-kernel boundary:
 * - API methods validate input and instantiate parser state.
 * - run_pipeline creates node::NodeManager, simplifies assertions, and then
 *   transfers control to kernel::Kernel::apply.
 * - Kernel remains an internal implementation detail; only API options and
 *   stable return types are exposed to integrators.
 *
 * Instances are cheap to copy and can be configured independently.
 */
class SMTStabilizer {
  public:
    explicit SMTStabilizer(SMTStabilizerOptions options = {});

    /** Return the current API options. */
    const SMTStabilizerOptions &options() const noexcept;
    /** Replace the current API options. */
    void set_options(const SMTStabilizerOptions &options) noexcept;

    /**
     * @brief Apply the full pipeline to an SMT-LIB2 file.
     * @param file_path Path to an SMT-LIB2 input file.
     * @return Stabilized SMT2 text.
     * @throws std::invalid_argument if the path is empty.
     * @throws std::runtime_error if parsing or normalization fails.
     */
    std::string apply_file(const std::string &file_path) const;

    /**
     * @brief Apply the full pipeline to an SMT-LIB2 script provided as text.
     * @param smt2_text Full SMT-LIB2 input text.
     * @return Stabilized SMT2 text.
     * @throws std::invalid_argument if the text is empty.
     * @throws std::runtime_error if parsing or normalization fails.
     */
    std::string apply_text(const std::string &smt2_text) const;

  private:
    SMTStabilizerOptions d_options;

    /** Configure parser-global options before pipeline execution. */
    static void configure_parser(stabilizer::parser::Parser &parser, const SMTStabilizerOptions &options);

    /**
     * Run parser -> node manager -> kernel pipeline and serialize final output.
     */
    static std::string run_pipeline(stabilizer::parser::Parser &parser, const SMTStabilizerOptions &options);
};

}  // namespace stabilizer::api