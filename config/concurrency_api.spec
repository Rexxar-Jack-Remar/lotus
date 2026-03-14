# Structured concurrency runtime metadata.
# Format:
#   SymbolOrPrefix TD_TYPE library=<name> semantic=<tag> [traits=<...>] [match=exact|prefix]
#
# Note: OpenMP and MPI APIs have been moved to separate files:
#   - config/openmp_api.spec
#   - config/mpi_api.spec
#
# This file now contains only C++ and other miscellaneous concurrency APIs.

# C++20 Semaphores
__pthread_sem_init TD_BAR_INIT library=cpp semantic=sem-init
__pthread_sem_wait TD_SEMAPHORE_ACQUIRE library=cpp semantic=sem-acquire
__pthread_sem_post TD_SEMAPHORE_RELEASE library=cpp semantic=sem-release
