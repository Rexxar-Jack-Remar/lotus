# Structured concurrency runtime metadata.
# Format:
#   SymbolOrPrefix TD_TYPE library=<name> semantic=<tag> [traits=<...>] [match=exact|prefix]
#
# Note: OpenMP and MPI APIs have been moved to separate files:
#   - config/openmp_api.spec
#   - config/mpi_api.spec
#
# This file now contains only C++ and other miscellaneous concurrency APIs.

# POSIX semaphores default to capacity-bearing semantics.
sem_wait TD_ACQUIRE library=custom semantic=sem-wait traits=semaphore
sem_post TD_RELEASE library=custom semantic=sem-post traits=semaphore

# Example binary semaphore wrappers can opt into exclusion semantics explicitly.
binary_sem_wait TD_ACQUIRE library=custom semantic=binary-sem-wait traits=semaphore,binary-semaphore
binary_sem_post TD_RELEASE library=custom semantic=binary-sem-post traits=semaphore,binary-semaphore

# POSIX timed/clock synchronization variants.
pthread_cond_timedwait TD_COND_WAIT library=pthread semantic=cond-wait
pthread_cond_clockwait TD_COND_WAIT library=pthread semantic=cond-wait
pthread_mutex_timedlock TD_ACQUIRE library=pthread semantic=timed-lock conditional=true success=zero
pthread_mutex_clocklock TD_ACQUIRE library=pthread semantic=clock-lock conditional=true success=zero
pthread_rwlock_timedrdlock TD_RWLOCK_RDLOCK library=pthread semantic=timed-read-lock conditional=true success=zero
pthread_rwlock_clockrdlock TD_RWLOCK_RDLOCK library=pthread semantic=clock-read-lock conditional=true success=zero
pthread_rwlock_timedwrlock TD_RWLOCK_WRLOCK library=pthread semantic=timed-write-lock conditional=true success=zero
pthread_rwlock_clockwrlock TD_RWLOCK_WRLOCK library=pthread semantic=clock-write-lock conditional=true success=zero
pthread_rwlock_tryrdlock TD_RWLOCK_RDLOCK library=pthread semantic=try-read-lock traits=try-lock conditional=true success=zero
pthread_rwlock_trywrlock TD_RWLOCK_WRLOCK library=pthread semantic=try-write-lock traits=try-lock conditional=true success=zero

# C++20 Semaphores
__pthread_sem_init TD_BAR_INIT library=cpp semantic=sem-init
__pthread_sem_wait TD_SEMAPHORE_ACQUIRE library=cpp semantic=sem-acquire traits=semaphore
__pthread_sem_post TD_SEMAPHORE_RELEASE library=cpp semantic=sem-release traits=semaphore
