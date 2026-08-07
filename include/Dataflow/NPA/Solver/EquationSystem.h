#ifndef NPA_EQUATION_SYSTEM_H
#define NPA_EQUATION_SYSTEM_H

/**
 * \file
 * \brief Solver-independent iteration helper for equation-system façades.
 */

#include <chrono>
#include <iostream>
#include <utility>

namespace npa {

template <class State> struct IterationResult {
  State value;
  int iterations = 0;
  bool stabilized = false;
  double seconds = 0.0;
};

template <class State, class Step, class Equal>
IterationResult<State> iterate_until_stable(State initial, Step step,
                                            Equal equal, int max_iterations,
                                            bool verbose) {
  auto start = std::chrono::high_resolution_clock::now();
  State current = std::move(initial);
  int iteration = 0;
  bool stabilized = false;
  while (max_iterations < 0 || iteration < max_iterations) {
    State next = step(current);
    stabilized = equal(current, next);
    current = std::move(next);
    ++iteration;
    if (stabilized) {
      if (verbose)
        std::cerr << "[conv] " << iteration << "\n";
      break;
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  return {std::move(current), iteration, stabilized,
          std::chrono::duration<double>(end - start).count()};
}

} // namespace npa

#endif // NPA_EQUATION_SYSTEM_H
