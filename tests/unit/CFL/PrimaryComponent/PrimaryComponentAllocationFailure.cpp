// SPDX-License-Identifier: MIT
// Separate executable: fail each allocation position of an update, one at a
// time. This validates fail-closed queries and destruction of partially updated
// state under ASan/LSan, without changing the production allocator or code.
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.h"

#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
std::ptrdiff_t remaining = -1;
void *allocate(std::size_t size) {
  if (remaining == 0) {
    remaining = -1;
    throw std::bad_alloc();
  }
  if (remaining > 0)
    --remaining;
  if (void *p = std::malloc(size == 0 ? 1 : size))
    return p;
  throw std::bad_alloc();
}
} // namespace
void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

int main(int argc, char **argv) {
  using namespace lotus::cfl::dynamic_dyck;
  try {
    const auto backend =
        argc == 2 && std::string(argv[1]) == "hdt"
            ? PrimaryComponentConnectivityBackend::HDT
            : PrimaryComponentConnectivityBackend::Deterministic;
    std::size_t failures = 0;
    for (int operation = 0; operation < 8; ++operation) {
      bool finished = false;
      for (std::ptrdiff_t budget = 0; budget < 1000; ++budget) {
        PrimaryComponentSolver solver(
            PrimaryComponentEdgeSemantics::ReferenceCounted, backend);
        for (const Edge &edge : {Edge{0, 1, 0, Parenthesis::Close},
                                 Edge{0, 2, 0, Parenthesis::Close},
                                 Edge{0, 5, 0, Parenthesis::Close},
                                 Edge{1, 3, 1, Parenthesis::Close},
                                 Edge{2, 4, 1, Parenthesis::Close},
                                 Edge{3, 1, 2, Parenthesis::Close},
                                 Edge{4, 2, 2, Parenthesis::Close},
                                 Edge{5, 6, 1, Parenthesis::Close}})
          solver.insertEdge(edge);
        if (operation == 7)
          solver.insertEdge({0, 1, 0, Parenthesis::Close});
        bool failed = false;
        remaining = budget;
        try {
          switch (operation) {
          case 0:
            solver.addVertex(200);
            break;
          case 1:
            solver.insertEdge({0, 7, 0, Parenthesis::Close});
            break;
          case 2:
            solver.deleteEdge({0, 2, 0, Parenthesis::Close});
            break;
          case 3:
            solver.deleteEdge({0, 1, 0, Parenthesis::Close});
            break;
          case 4: // Two fresh endpoints force an existing universe to grow.
            solver.insertEdge({900, 901, 0, Parenthesis::Close});
            break;
          case 5: // Head deletion as opposed to interior/tail deletion above.
            solver.deleteEdge({0, 5, 0, Parenthesis::Close});
            break;
          case 6: // Count-only insertion must not enter any component update.
            solver.insertEdge({0, 1, 0, Parenthesis::Close});
            break;
          case 7: // Count-only deletion likewise.
            solver.deleteEdge({0, 1, 0, Parenthesis::Close});
            break;
          }
        } catch (const std::bad_alloc &) {
          failed = true;
        }
        remaining = -1;
        if (failed) {
          ++failures;
          bool refused = false;
          try {
            solver.connected(0, 0);
          } catch (const std::logic_error &) {
            refused = true;
          }
          if (!refused)
            throw std::runtime_error("partial update remained queryable");
          // Move-assigning a fresh instance must safely destroy the failed
          // state.
          solver = PrimaryComponentSolver{
              PrimaryComponentEdgeSemantics::ReferenceCounted, backend};
          if (solver.connected(0, 0))
            throw std::runtime_error("reinitialized solver retained vertices");
        } else {
          std::string error;
          if (!solver.validate(&error))
            throw std::runtime_error(error);
          std::cout << "operation=" << operation
                    << " allocation_failures=" << budget << '\n';
          finished = true;
          break;
        }
      }
      if (!finished)
        throw std::runtime_error("allocation-failure budget exhausted");
    }
    std::cout << "PASS allocation_failures=" << failures << '\n';
    return 0;
  } catch (const std::exception &error) {
    remaining = -1;
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
