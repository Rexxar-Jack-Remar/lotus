# === Resource & API Misuse Patterns ===
#
# Detects resource leaks (file handles, sockets, locks) and API pairing
# violations using the built-in ResourceFlowQuery and Cypher scans.
#
# Prerequisites:
#   --build-pdg (default on)
#   -g for source location info

# ---------------------------------------------------------------------------
# 1. Resource API scan — discover all known acquire/release calls in the
#    program. The built-in API table covers:
#
#   Heap:       malloc/calloc/realloc/reallocf/valloc/aligned_alloc/
#               posix_memalign/memalign/pvalloc (acquire)
#               free/cfree (release)
#   mmap:       mmap/mmap64 (acquire), munmap/munmap64 (release)
#   File:       fopen/fopen64/freopen/freopen64/tmpfile/tmpfile64/
#               fdopen/popen (acquire), fclose/pclose (release)
#   FD:         open/open64/creat/creat64/socket/socketpair/accept/accept4/
#               dup/dup2/dup3/epoll_create/eventfd/signalfd/timerfd_create/
#               inotify_init/memfd_create/shm_open (acquire)
#               close/closefrom/shutdown (release)
#   Directory:  opendir/fdopendir (acquire), closedir (release)
#   Lock:       pthread_mutex_lock/trylock, pthread_spin_lock/trylock,
#               pthread_rwlock_rdlock/tryrdlock/wrlock/trywrlock,
#               mtx_lock/trylock (acquire)
#               pthread_mutex_unlock, pthread_spin_unlock,
#               pthread_rwlock_unlock, mtx_unlock (release)
# ---------------------------------------------------------------------------
MATCH (c:INST_FUNCALL)
WHERE c.callee IN ["malloc", "calloc", "realloc", "free",
                    "fopen", "fclose", "popen", "pclose",
                    "open", "close", "socket", "accept",
                    "mmap", "munmap",
                    "shm_open",
                    "opendir", "closedir",
                    "pthread_mutex_lock", "pthread_mutex_unlock",
                    "pthread_spin_lock", "pthread_spin_unlock"]
RETURN c.callee AS callee, c.func AS function, c.src AS location, c.llvm AS ir
ORDER BY c.callee
LIMIT 200

# ---------------------------------------------------------------------------
# 2. File-handle leak detection via resource-flow analysis
#
# The built-in file API table covers fopen/fopen64/freopen/freopen64,
# tmpfile/tmpfile64/fdopen/popen for acquire, fclose/pclose for release.
# File-descriptor APIs cover open/creat/socket/accept/dup/epoll_create/eventfd
# etc. for acquire, close/closefrom/shutdown for release.
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['fopen','fopen64','freopen','tmpfile',
#                           'open','open64','creat','socket']
#       RETURN c" \
#     --resource-kind file \
#     --format json
#
# Or check file descriptors separately:
#   --resource-kind fd
#
# orphaned_resources: opened but never closed (LEAK)
# double_release_candidates: closed multiple times
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 3. Lock/unlock pairing check (POSIX threads, C11 threads)
#
# The built-in lock API table covers:
#   Acquire: pthread_mutex_lock/trylock, pthread_spin_lock/trylock,
#            pthread_rwlock_rdlock/tryrdlock, pthread_rwlock_wrlock/trywrlock,
#            mtx_lock/trylock
#   Release: pthread_mutex_unlock, pthread_spin_unlock,
#            pthread_rwlock_unlock, mtx_unlock
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['pthread_mutex_lock','pthread_mutex_trylock',
#                           'pthread_spin_lock','pthread_spin_trylock',
#                           'pthread_rwlock_rdlock','pthread_rwlock_tryrdlock',
#                           'pthread_rwlock_wrlock','pthread_rwlock_trywrlock',
#                           'mtx_lock','mtx_trylock']
#       RETURN c" \
#     --resource-kind lock \
#     --format json
#
# orphaned_resources: acquired lock never released (potential deadlock)
# double_release_candidates: lock released multiple times
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 4. Memory-mapped I/O leak (mmap without munmap)
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['mmap','mmap64']
#       RETURN c" \
#     --resource-kind heap \
#     --format json
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 5. Dynamic memory: new/delete pairing analysis
#
#   lotus-ir-pdg-query input.bc \
#     --analysis resource-flow \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['new','_Znwm','_Znam']
#       RETURN c" \
#     --resource-kind heap \
#     --format json
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 6. Resource acquire/release summary per function
#
#   lotus-ir-pdg-query input.bc \
#     --analysis summary \
#     --criteria-query "
#       MATCH (c:INST_FUNCALL)
#       WHERE c.callee IN ['fopen','malloc','socket']
#       RETURN c" \
#     --summary-kind resource-kinds \
#     --format json
#
# Shows which functions may allocate or release specific resource types.
# Useful for auditing which functions are responsible for cleanup.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 7. Manual Cypher: find all calls paired by function (potential API mispair)
#    — shows fopen/fclose within the same function
# ---------------------------------------------------------------------------
MATCH (open:INST_FUNCALL {callee:"fopen"})
MATCH (close:INST_FUNCALL {callee:"fclose"})
WHERE open.func = close.func
RETURN open.func AS function,
       open.src AS open_location,
       close.src AS close_location
LIMIT 100
