# Linux kernel concurrency API semantics.
#
# Format:
#   symbol OPERATION [key=value ...]
#
# Supported keys include match=exact|prefix, lock, mode, success, object,
# callback, secondary-callback, domain, condition, flags, value, size,
# expires, subclass, context, secondary-context, rcu-flavor, completion, order,
# synchronous, serializes-domain, raw-lock, nested-lock, interruptible,
# timeout, wake-all, wake-exclusive, deferred-reclamation,
# returns-retired-pointer,
# requires-rcu-section, managed-allocation, may-sleep, may-spawn,
# may-access-shared, explicit IRQ/BH/preemption state effects, and
# preemption-effect-non-rt.
# Exact rules take precedence over prefix rules; later files override earlier
# rules. LOTUS_CONFIG_DIR selects the authoritative default-spec directory.
# LinuxKernelConfig::api_spec_paths supplies version-, architecture-,
# subsystem-, or project-specific overlays.

# Spinlocks
spin_lock LOCK_ACQUIRE match=prefix lock=spin mode=exclusive object=0 disables-preemption=true preemption-effect-non-rt=true
raw_spin_lock LOCK_ACQUIRE match=prefix lock=raw-spin mode=exclusive object=0 raw-lock=true disables-preemption=true
_raw_spin_lock LOCK_ACQUIRE match=prefix lock=raw-spin mode=exclusive object=0 raw-lock=true disables-preemption=true
spin_unlock LOCK_RELEASE match=prefix lock=spin mode=exclusive object=0 enables-preemption=true preemption-effect-non-rt=true
raw_spin_unlock LOCK_RELEASE match=prefix lock=raw-spin mode=exclusive object=0 raw-lock=true enables-preemption=true
_raw_spin_unlock LOCK_RELEASE match=prefix lock=raw-spin mode=exclusive object=0 raw-lock=true enables-preemption=true
spin_trylock LOCK_TRY match=prefix lock=spin mode=exclusive object=0 success=nonzero
raw_spin_trylock LOCK_TRY match=prefix lock=raw-spin mode=exclusive object=0 success=nonzero raw-lock=true
_raw_spin_trylock LOCK_TRY match=prefix lock=raw-spin mode=exclusive object=0 success=nonzero raw-lock=true
spin_lock_init LOCK_INIT lock=spin object=0
raw_spin_lock_init LOCK_INIT lock=raw-spin object=0 raw-lock=true
spin_lock_irq LOCK_ACQUIRE lock=spin mode=exclusive object=0 disables-local-irq=true disables-preemption=true preemption-effect-non-rt=true
spin_unlock_irq LOCK_RELEASE lock=spin mode=exclusive object=0 enables-local-irq=true enables-preemption=true preemption-effect-non-rt=true
spin_lock_irqsave LOCK_ACQUIRE lock=spin mode=exclusive object=0 flags=1 saves-irq-state=true disables-local-irq=true disables-preemption=true preemption-effect-non-rt=true
spin_unlock_irqrestore LOCK_RELEASE lock=spin mode=exclusive object=0 flags=1 restores-irq-state=true enables-local-irq=true enables-preemption=true preemption-effect-non-rt=true
spin_lock_bh LOCK_ACQUIRE lock=spin mode=exclusive object=0 disables-bh=true disables-preemption=true preemption-effect-non-rt=true
spin_unlock_bh LOCK_RELEASE lock=spin mode=exclusive object=0 enables-bh=true enables-preemption=true preemption-effect-non-rt=true
spin_lock_nested LOCK_ACQUIRE lock=spin mode=exclusive object=0 subclass=1 nested-lock=true disables-preemption=true preemption-effect-non-rt=true
raw_spin_lock_irqsave LOCK_ACQUIRE lock=raw-spin mode=exclusive object=0 flags=1 saves-irq-state=true disables-local-irq=true disables-preemption=true raw-lock=true
raw_spin_unlock_irqrestore LOCK_RELEASE lock=raw-spin mode=exclusive object=0 flags=1 restores-irq-state=true enables-local-irq=true enables-preemption=true raw-lock=true
_raw_spin_lock_irqsave LOCK_ACQUIRE lock=raw-spin mode=exclusive object=0 flags=1 saves-irq-state=true disables-local-irq=true disables-preemption=true raw-lock=true
_raw_spin_unlock_irqrestore LOCK_RELEASE lock=raw-spin mode=exclusive object=0 flags=1 restores-irq-state=true enables-local-irq=true enables-preemption=true raw-lock=true

# Mutexes and semaphores
mutex_lock LOCK_ACQUIRE match=prefix lock=mutex mode=exclusive object=0 may-sleep=true
mutex_lock_interruptible LOCK_TRY match=prefix lock=mutex mode=exclusive object=0 success=zero interruptible=true may-sleep=true
mutex_lock_killable LOCK_TRY match=prefix lock=mutex mode=exclusive object=0 success=zero interruptible=true may-sleep=true
mutex_trylock LOCK_TRY lock=mutex mode=exclusive object=0 success=nonzero
mutex_unlock LOCK_RELEASE lock=mutex mode=exclusive object=0
mutex_init LOCK_INIT lock=mutex object=0
__mutex_init LOCK_INIT lock=mutex object=0
mutex_lock_nested LOCK_ACQUIRE lock=mutex mode=exclusive object=0 subclass=1 nested-lock=true may-sleep=true
down LOCK_ACQUIRE lock=semaphore mode=exclusive object=0 may-sleep=true
down_interruptible LOCK_TRY lock=semaphore mode=exclusive object=0 success=zero interruptible=true may-sleep=true
down_killable LOCK_TRY lock=semaphore mode=exclusive object=0 success=zero interruptible=true may-sleep=true
down_trylock LOCK_TRY lock=semaphore mode=exclusive object=0 success=zero
up LOCK_RELEASE lock=semaphore mode=exclusive object=0
sema_init LOCK_INIT lock=semaphore object=0
init_MUTEX LOCK_INIT lock=semaphore object=0
init_MUTEX_LOCKED LOCK_INIT lock=semaphore object=0

# Reader/writer locks and semaphores
read_lock LOCK_ACQUIRE match=prefix lock=rwlock mode=shared object=0
read_unlock LOCK_RELEASE match=prefix lock=rwlock mode=shared object=0
write_lock LOCK_ACQUIRE match=prefix lock=rwlock mode=exclusive object=0
write_unlock LOCK_RELEASE match=prefix lock=rwlock mode=exclusive object=0
down_read LOCK_ACQUIRE lock=rwsem mode=shared object=0 may-sleep=true
down_read_interruptible LOCK_TRY match=prefix lock=rwsem mode=shared object=0 success=zero interruptible=true may-sleep=true
down_read_killable LOCK_TRY lock=rwsem mode=shared object=0 success=zero interruptible=true may-sleep=true
down_read_trylock LOCK_TRY lock=rwsem mode=shared object=0 success=nonzero
up_read LOCK_RELEASE lock=rwsem mode=shared object=0
down_write LOCK_ACQUIRE lock=rwsem mode=exclusive object=0 may-sleep=true
down_write_interruptible LOCK_TRY lock=rwsem mode=exclusive object=0 success=zero interruptible=true may-sleep=true
down_write_killable LOCK_TRY match=prefix lock=rwsem mode=exclusive object=0 success=zero interruptible=true may-sleep=true
down_write_trylock LOCK_TRY lock=rwsem mode=exclusive object=0 success=nonzero
up_write LOCK_RELEASE lock=rwsem mode=exclusive object=0
init_rwsem LOCK_INIT lock=rwsem object=0
read_lock_irqsave LOCK_ACQUIRE lock=rwlock mode=shared object=0 flags=1 saves-irq-state=true disables-local-irq=true
read_unlock_irqrestore LOCK_RELEASE lock=rwlock mode=shared object=0 flags=1 restores-irq-state=true enables-local-irq=true
write_lock_irqsave LOCK_ACQUIRE lock=rwlock mode=exclusive object=0 flags=1 saves-irq-state=true disables-local-irq=true
write_unlock_irqrestore LOCK_RELEASE lock=rwlock mode=exclusive object=0 flags=1 restores-irq-state=true enables-local-irq=true
read_lock_nested LOCK_ACQUIRE lock=rwlock mode=shared object=0 subclass=1 nested-lock=true
write_lock_nested LOCK_ACQUIRE lock=rwlock mode=exclusive object=0 subclass=1 nested-lock=true

# RCU and SRCU
rcu_read_lock RCU_READ_LOCK match=prefix rcu-flavor=classic
rcu_read_unlock RCU_READ_UNLOCK match=prefix rcu-flavor=classic
__rcu_read_lock RCU_READ_LOCK rcu-flavor=classic
__rcu_read_unlock RCU_READ_UNLOCK rcu-flavor=classic
rcu_read_lock_bh RCU_READ_LOCK rcu-flavor=bh
rcu_read_unlock_bh RCU_READ_UNLOCK rcu-flavor=bh
rcu_read_lock_sched RCU_READ_LOCK rcu-flavor=sched
rcu_read_unlock_sched RCU_READ_UNLOCK rcu-flavor=sched
rcu_read_lock_trace RCU_READ_LOCK rcu-flavor=tasks-trace
rcu_read_unlock_trace RCU_READ_UNLOCK rcu-flavor=tasks-trace
srcu_read_lock RCU_READ_LOCK match=prefix domain=0 rcu-flavor=srcu
srcu_read_unlock RCU_READ_UNLOCK match=prefix domain=0 rcu-flavor=srcu
synchronize_rcu RCU_SYNC match=prefix synchronous=true rcu-flavor=classic
synchronize_srcu RCU_SYNC domain=0 synchronous=true rcu-flavor=srcu
synchronize_rcu_tasks RCU_SYNC synchronous=true rcu-flavor=tasks
synchronize_rcu_tasks_trace RCU_SYNC synchronous=true rcu-flavor=tasks-trace
call_rcu RCU_CALL object=0 callback=1 context=rcu may-spawn=true rcu-flavor=classic
call_srcu RCU_CALL domain=0 object=1 callback=2 context=rcu may-spawn=true rcu-flavor=srcu
call_rcu_tasks RCU_CALL object=0 callback=1 context=rcu may-spawn=true rcu-flavor=tasks
call_rcu_tasks_trace RCU_CALL object=0 callback=1 context=rcu may-spawn=true rcu-flavor=tasks-trace
kfree_rcu RCU_CALL object=0 deferred-reclamation=true may-spawn=true rcu-flavor=classic
kvfree_rcu RCU_CALL object=0 deferred-reclamation=true may-spawn=true rcu-flavor=classic
rcu_barrier RCU_BARRIER synchronous=true rcu-flavor=classic
srcu_barrier RCU_BARRIER domain=0 synchronous=true rcu-flavor=srcu
rcu_assign_pointer RCU_ASSIGN object=0 rcu-flavor=classic
rcu_replace_pointer RCU_ASSIGN object=0 rcu-flavor=classic returns-retired-pointer=true
rcu_dereference RCU_DEREFERENCE match=prefix object=0 rcu-flavor=classic
rcu_dereference_protected RCU_DEREFERENCE object=0 rcu-flavor=classic requires-rcu-section=false

# Sequence locks
seqlock_init SEQLOCK_INIT lock=seqlock object=0
read_seqbegin SEQ_READ_BEGIN match=prefix lock=seqlock object=0
read_seqretry SEQ_READ_RETRY match=prefix lock=seqlock object=0
write_seqlock SEQ_WRITE_LOCK match=prefix lock=seqlock mode=exclusive object=0
write_sequnlock SEQ_WRITE_UNLOCK match=prefix lock=seqlock mode=exclusive object=0

# Wait queues and completions
init_waitqueue_head WAITQUEUE_INIT object=0
wait_event WAIT_EVENT match=prefix object=0 condition=1 may-sleep=true
wait_event_interruptible WAIT_EVENT match=prefix object=0 condition=1 may-sleep=true interruptible=true
wait_event_killable WAIT_EVENT match=prefix object=0 condition=1 may-sleep=true interruptible=true
wait_event_timeout WAIT_EVENT object=0 condition=1 may-sleep=true timeout=true
wait_event_interruptible_timeout WAIT_EVENT object=0 condition=1 may-sleep=true interruptible=true timeout=true
wake_up WAKE_UP match=prefix object=0
__wake_up WAKE_UP object=0
__wake_up_sync_key WAKE_UP object=0
wake_up_all WAKE_UP object=0 wake-all=true
wake_up_one WAKE_UP object=0 wake-exclusive=true
wake_up_nr WAKE_UP object=0 wake-exclusive=true
prepare_to_wait PREPARE_WAIT match=prefix object=0
finish_wait FINISH_WAIT object=0
init_completion COMPLETION_INIT object=0
reinit_completion COMPLETION_REINIT object=0
wait_for_completion COMPLETION_WAIT match=prefix object=0 may-sleep=true
wait_for_completion_interruptible COMPLETION_WAIT object=0 may-sleep=true interruptible=true
wait_for_completion_killable COMPLETION_WAIT object=0 may-sleep=true interruptible=true
wait_for_completion_timeout COMPLETION_WAIT object=0 may-sleep=true timeout=true
complete COMPLETION_SIGNAL object=0 completion=one
complete_all COMPLETION_SIGNAL object=0 completion=all wake-all=true

# Timers
timer_setup TIMER_SETUP object=0 callback=1 domain=0 context=timer serializes-domain=true
setup_timer TIMER_SETUP object=0 callback=1 domain=0 context=timer serializes-domain=true
init_timer TIMER_SETUP match=prefix object=0 domain=0 context=timer serializes-domain=true
__timer_init TIMER_SETUP object=0 callback=1 domain=0 context=timer serializes-domain=true
timer_setup_key TIMER_SETUP object=0 callback=1 domain=0 context=timer serializes-domain=true
mod_timer TIMER_MOD object=0 domain=0 expires=1 context=timer serializes-domain=true may-spawn=true
add_timer TIMER_MOD match=prefix object=0 domain=0 context=timer serializes-domain=true may-spawn=true
del_timer TIMER_DELETE match=prefix object=0 domain=0 serializes-domain=true
del_timer_sync TIMER_DELETE object=0 domain=0 synchronous=true serializes-domain=true
timer_delete TIMER_DELETE match=prefix object=0 domain=0 serializes-domain=true
timer_delete_sync TIMER_DELETE object=0 domain=0 synchronous=true serializes-domain=true
timer_shutdown TIMER_SHUTDOWN match=prefix object=0 domain=0 serializes-domain=true
timer_shutdown_sync TIMER_SHUTDOWN object=0 domain=0 synchronous=true serializes-domain=true

# Kthreads and workqueues
kthread_create KTHREAD_CREATE match=prefix object=result callback=0 context=kthread may-spawn=true
kthread_run KTHREAD_RUN object=result callback=0 context=kthread may-spawn=true
wake_up_process KTHREAD_START object=0 may-spawn=true
kthread_stop KTHREAD_STOP object=0 synchronous=true
kthread_should_stop KTHREAD_SHOULD_STOP
INIT_WORK WORKqueue object=0 callback=1 context=workqueue
__init_work WORKqueue object=0 callback=1 context=workqueue
init_work WORKqueue object=0 callback=1 context=workqueue
queue_work WORKqueue_SUBMIT match=prefix object=1 domain=0 context=workqueue may-spawn=true
queue_work_on WORKqueue_SUBMIT object=2 domain=1 context=workqueue may-spawn=true
schedule_work WORKqueue_SUBMIT match=prefix object=0 context=workqueue may-spawn=true
schedule_work_on WORKqueue_SUBMIT object=1 context=workqueue may-spawn=true
flush_work WORKqueue_FLUSH match=prefix object=0 synchronous=true
drain_workqueue WORKqueue_FLUSH domain=0 synchronous=true
cancel_work WORKqueue_CANCEL match=prefix object=0
cancel_work_sync WORKqueue_CANCEL object=0 synchronous=true
destroy_workqueue WORKqueue_DESTROY domain=0 synchronous=true
flush_delayed_work WORKqueue_FLUSH object=0 synchronous=true
flush_workqueue WORKqueue_FLUSH domain=0 synchronous=true
cancel_delayed_work WORKqueue_CANCEL match=prefix object=0
cancel_delayed_work_sync WORKqueue_CANCEL object=0 synchronous=true
create_workqueue WORKqueue_CREATE object=result domain=result
create_singlethread_workqueue WORKqueue_CREATE object=result domain=result serializes-domain=true
alloc_workqueue WORKqueue_CREATE object=result domain=result
alloc_ordered_workqueue WORKqueue_CREATE object=result domain=result serializes-domain=true

# IRQ, bottom halves, tasklets, NAPI, and softirqs
request_irq IRQ_REQUEST object=0 callback=1 domain=0 context=hardirq may-spawn=true
request_threaded_irq IRQ_REQUEST object=0 callback=1 secondary-callback=2 domain=0 context=hardirq secondary-context=threaded-irq may-spawn=true
free_irq IRQ_FREE object=0 synchronous=true
local_irq_disable IRQ_DISABLE disables-local-irq=true
arch_local_irq_disable IRQ_DISABLE disables-local-irq=true
local_irq_save IRQ_DISABLE flags=0 saves-irq-state=true disables-local-irq=true
arch_local_irq_save IRQ_DISABLE flags=0 saves-irq-state=true disables-local-irq=true
local_irq_enable IRQ_ENABLE enables-local-irq=true
arch_local_irq_enable IRQ_ENABLE enables-local-irq=true
local_irq_restore IRQ_ENABLE flags=0 restores-irq-state=true enables-local-irq=true
arch_local_irq_restore IRQ_ENABLE flags=0 restores-irq-state=true enables-local-irq=true
disable_irq IRQ_LINE_DISABLE object=0
disable_irq_nosync IRQ_LINE_DISABLE object=0
enable_irq IRQ_LINE_ENABLE object=0
local_bh_disable BH_DISABLE disables-bh=true
__local_bh_disable_ip BH_DISABLE disables-bh=true
local_bh_enable BH_ENABLE enables-bh=true
__local_bh_enable_ip BH_ENABLE enables-bh=true
preempt_disable PREEMPT_DISABLE match=prefix disables-preemption=true
preempt_enable PREEMPT_ENABLE match=prefix enables-preemption=true
tasklet_setup TASKLET_SETUP object=0 callback=1 domain=0 context=tasklet serializes-domain=true
tasklet_init TASKLET_SETUP object=0 callback=1 domain=0 context=tasklet serializes-domain=true
tasklet_schedule TASKLET_SCHEDULE match=prefix object=0 domain=0 context=tasklet serializes-domain=true may-spawn=true
tasklet_kill TASKLET_KILL match=prefix object=0 domain=0 synchronous=true serializes-domain=true
netif_napi_add NAPI_REGISTER match=prefix object=1 callback=2 domain=1 context=napi serializes-domain=true
napi_schedule NAPI_SCHEDULE match=prefix object=0 domain=0 context=napi serializes-domain=true may-spawn=true
__napi_schedule NAPI_SCHEDULE match=prefix object=0 domain=0 context=napi serializes-domain=true may-spawn=true
napi_disable NAPI_DISABLE object=0 domain=0 synchronous=true serializes-domain=true
napi_synchronize NAPI_DISABLE object=0 domain=0 synchronous=true serializes-domain=true
open_softirq SOFTIRQ_REGISTER object=0 callback=1 domain=0 context=softirq
raise_softirq SOFTIRQ_RAISE match=prefix object=0 domain=0 context=softirq may-spawn=true

# Memory ordering and atomics
smp_mb MEMORY_BARRIER match=prefix order=full
smp_rmb MEMORY_BARRIER match=prefix order=acquire
smp_wmb MEMORY_BARRIER match=prefix order=release
mb MEMORY_BARRIER order=full
rmb MEMORY_BARRIER order=acquire
wmb MEMORY_BARRIER order=release
barrier MEMORY_BARRIER order=compiler
atomic_read ATOMIC_READ match=prefix object=0 order=relaxed
atomic64_read ATOMIC_READ match=prefix object=0 order=relaxed
atomic_read_acquire ATOMIC_READ object=0 order=acquire
atomic64_read_acquire ATOMIC_READ object=0 order=acquire
atomic_set ATOMIC_WRITE match=prefix object=0 value=1 order=relaxed
atomic64_set ATOMIC_WRITE match=prefix object=0 value=1 order=relaxed
atomic_set_release ATOMIC_WRITE object=0 value=1 order=release
atomic64_set_release ATOMIC_WRITE object=0 value=1 order=release
atomic_add ATOMIC_RMW match=prefix object=1 value=0 order=full
atomic64_add ATOMIC_RMW match=prefix object=1 value=0 order=full
atomic_sub ATOMIC_RMW match=prefix object=1 value=0 order=full
atomic64_sub ATOMIC_RMW match=prefix object=1 value=0 order=full
atomic_inc ATOMIC_RMW match=prefix object=0 order=full
atomic64_inc ATOMIC_RMW match=prefix object=0 order=full
atomic_dec ATOMIC_RMW match=prefix object=0 order=full
atomic64_dec ATOMIC_RMW match=prefix object=0 order=full
atomic_cmpxchg ATOMIC_RMW match=prefix object=0 value=1 order=full
atomic64_cmpxchg ATOMIC_RMW match=prefix object=0 value=1 order=full
atomic_xchg ATOMIC_RMW match=prefix object=0 value=1 order=full
atomic64_xchg ATOMIC_RMW match=prefix object=0 value=1 order=full
set_bit ATOMIC_RMW match=prefix object=1 value=0 order=relaxed
clear_bit ATOMIC_RMW match=prefix object=1 value=0 order=relaxed
change_bit ATOMIC_RMW match=prefix object=1 value=0 order=relaxed
test_bit ATOMIC_READ match=prefix object=1 value=0 order=relaxed
test_and_set_bit ATOMIC_RMW match=prefix object=1 value=0 order=full
test_and_clear_bit ATOMIC_RMW match=prefix object=1 value=0 order=full
test_and_change_bit ATOMIC_RMW match=prefix object=1 value=0 order=full

# Allocation and reclamation
kmalloc_array KMALLOC object=result
kmalloc KMALLOC match=prefix object=result size=0
kzalloc KMALLOC match=prefix object=result size=0
kcalloc KMALLOC object=result
kcalloc_node KMALLOC object=result
krealloc KMALLOC object=result size=1
kmem_cache_alloc KMALLOC object=result
devm_kmalloc KMALLOC object=result size=1 managed-allocation=true
devm_kzalloc KMALLOC object=result size=1 managed-allocation=true
vmalloc VMALLOC object=result size=0
vzalloc VMALLOC object=result size=0
devm_vmalloc VMALLOC object=result size=1 managed-allocation=true
kfree RCU_RECLAIM object=0
kvfree RCU_RECLAIM object=0
vfree RCU_RECLAIM object=0
devm_kfree RCU_RECLAIM object=1 managed-allocation=true
kmem_cache_free RCU_RECLAIM object=1
