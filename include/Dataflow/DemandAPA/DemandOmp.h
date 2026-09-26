#pragma once

#ifdef _OPENMP
#include <omp.h>
#else
#include <chrono>

inline double omp_get_wtime() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
inline int omp_get_thread_num() { return 0; }
inline int omp_get_num_threads() { return 1; }
inline void omp_set_num_threads(int) {}
#endif
