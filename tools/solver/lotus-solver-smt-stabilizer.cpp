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

#include <ctime>
#include <ios>
#include <iostream>

#include "kernel/kernel.h"
#include "node/node_manager.h"
#include "parser/parser.h"

int main(int argc, char *argv[]) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    stabilizer::parser::Parser p;
    p.getOptions()->setKeepLet(false);
    p.getOptions()->setExpandFunctions(false);
    bool context_propagation = true;
    bool symmetry_breaking_perturbation = true;
    if (argc > 2) {
        for (size_t i = 2; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--no-cp") {
                context_propagation = false;
            }
            else if (arg == "--no-sbp") {
                symmetry_breaking_perturbation = false;
            }
            else {
                std::cerr << "Unknown option: " << arg << std::endl;
                return 1;
            }
        }
    }
    if (argc >= 2)
        p.parse(argv[1]);
    else
        p.parse("");
    // p.parse("/pub/netdisk1/zhangx/benchmark-2025/non-incremental/MARIPOSA/unstable_ext/fs_dice-queries-ASN1.Spec.Value.INTEGER-25.smt2");

    stabilizer::node::NodeManager nm(p);

    nm.simplify_assertions();
    // stabilizer::rewrite::Rewriter rewriter(nm);

    // rewriter.apply();

    stabilizer::kernel::Kernel kernel(nm, context_propagation, symmetry_breaking_perturbation);
    // struct timespec t_start, t_end;
    // if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t_start))
    //     perror("clock gettime");

    kernel.apply(nm);

    // if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t_end))
    //     perror("clock gettime");
    // double time_used = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_nsec - t_start.tv_nsec) / 1e9;
    // std::cout << std::fixed << std::setprecision(2) << time_used << std::endl;
    std::cout << nm.to_string() << std::endl;
    return 0;
}
