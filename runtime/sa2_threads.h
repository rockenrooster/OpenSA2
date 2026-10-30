/* Guest threads and the multimedia timer, per the G1 decision's section 3.
 *
 * What G1 decided, quoted so the implementation can be read against it:
 *
 *   "Guest threads run as host threads under one guest lock, with
 *    deterministic hand-off. Each guest thread gets its own CPU-state struct
 *    and guest stack; guest memory is shared. A single runtime lock is held by
 *    whichever guest thread executes lifted code and is released only inside
 *    the runtime's sync and time entry points (WaitForSingleObject, Sleep,
 *    event and critical-section calls, the message pump), where the runtime
 *    picks the next runnable thread by a fixed rule (round-robin in creation
 *    order). Under identical inputs and virtualised time the interleaving is a
 *    function of the program, so replays are deterministic. ... The
 *    `timeSetEvent` callback runs on the timer tick under the same lock."
 *
 * So: one `pthread_mutex_t`, held for the whole time a guest thread is inside
 * lifted code, released only by `pthread_cond_wait` at a hand-off point. A
 * `g_current` index names the one thread allowed to run; every other host
 * thread sits in `while (g_current != me) pthread_cond_wait(...)`, so a
 * spurious wakeup cannot start a second guest thread and the interleaving is
 * whatever the fixed selection rule says it is. **There is no preemption and
 * no host time anywhere in the selection.**
 *
 * Virtual time (design 3.5) is the other half. The clock only moves when the
 * guest asks it to (`Sleep`, the four time stubs) or when the scheduler finds
 * that *nothing is runnable*, in which case it advances the clock to the
 * earliest pending deadline -- a timer's next tick, a sleeper's wake time, a
 * wait's timeout -- and fires it. That rule is what lets the sound worker's
 * `WaitForSingleObject(event, INFINITE)` end: the periodic `timeSetEvent`
 * timer pulses the event, and the pulse happens at a virtual instant that is a
 * function of the program rather than of how fast this machine is.
 *
 * If nothing is runnable and there is no pending deadline either, the wait has
 * no end. The scheduler says so rather than sleeping: `sa2_sched_block`
 * returns SA2_WAKE_NEVER and the caller refuses by name, which is the answer
 * the single-threaded runtime gave for a different reason.
 *
 * What this does NOT provide, stated here because the risk register asks:
 * there is no parallelism. Two guest threads never execute at the same
 * instant, so a data race the real game has is not reproduced -- a lost update
 * the shipped binary suffers on a real machine will not happen here. What is
 * reproduced is every interleaving that hand-off points can produce, and
 * exactly one of them per input.
 */
#ifndef SA2_RUNTIME_SA2_THREADS_H
#define SA2_RUNTIME_SA2_THREADS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thread 0 is the one the process starts on. The arena in guest_memory.h holds
 * SA2_GUEST_TSTACK_SLOTS stacks, so that plus thread 0 is the ceiling.
 *
 * It was 9, on the reading that the image's nine thread-creation sites (G1
 * section 3) meant nine threads. Two things are wrong with that. Thread 0 is
 * not created at a site, so nine sites need ten rows -- and the arena had
 * eight slots, so the guest was refused a ninth worker the moment the CRI file
 * system opened. And a *site* is not a thread: the "AsynDo" routine at
 * 0x008653d0 is started once per asynchronous operation and finishes, so one
 * site produced eight threads in one run and would produce hundreds in a long
 * one.
 *
 * So the thing that is scarce, the 512 KiB **stack slot**, is handed back when
 * a thread finishes. Rows are the cheap resource (SA2_TLS_SLOTS dominates one
 * at about 4.9 KiB) and slots are the dear one; a static assertion in
 * runtime/sa2_threads.c keeps rows from being the tighter of the two, because
 * then the message would name the wrong limit.
 *
 * **A row is reused once every row has been used, as Windows frees a thread
 * object**: when its thread has ended *and* the one handle CreateThread gave
 * for it has been closed (the image has no DuplicateHandle, OpenThread or
 * GetCurrentThread, so that handle is the only one). Until then a row keeps its
 * history, so every run that creates fewer than SA2_MAX_GUEST_THREADS threads
 * is the run it was before rows were reused. A reused row is a new thread in
 * every way the guest can see: a new thread id (ids are never reused), a TEB
 * initialised again, zeroed TLS and FLS slots, a stack slot chosen by the
 * same rule, and a place at the **end** of the creation order, because the
 * hand-off rule picks by creation order (`sa2_sched_seq`), not by row index.
 * A handle whose thread's row now holds another thread names nothing
 * (sa2_thread_index_of_handle). Before rows were reused, every "AsynDo"
 * operation spent one for the life of the process, and a play session ran out
 * of them a few stage loads after the main menu (2026-10-02).
 *
 * Two rules came with reuse (the 2026-10-02 review, S1 and S3). A row whose
 * thread ended in a trap of its own -- anything but returning or ExitThread,
 * an FLS callback's trap included -- is never reused, so its trap note, where
 * tools/s4/crt_run.py and tools/s4/reach_extend.py read a function-table
 * miss, stays as it ended. And a guest thread's host allocations, its signal
 * stack (sa2_trap_thread_init) and its remill State (sa2_call), are freed when
 * its host thread ends (sa2_trap_thread_fini, sa2_call_thread_fini), so
 * reusing rows does not grow the process by about 68 KiB per thread; the next
 * bound is the handle table (SA2_HANDLE_MAX). */
/* 255: every row keeps a TEB (SA2_GUEST_TEB_SLOTS, 256) and a handle row
 * stores its thread's index in a byte. It was 64, and loading Metal Harbor
 * starts the "AsynDo" routine once per asynchronous operation -- 48 threads by
 * tick 117 -- so a stage would have run out of rows where the boot never did
 * (2026-09-23). */
#define SA2_MAX_GUEST_THREADS 255

/* Why a wait ended. The caller reads this and cannot conflate the three. */
enum {
    SA2_WAKE_NEVER   = 0,  /* nothing runnable and no deadline: the wait has no end */
    SA2_WAKE_SIGNAL  = 1,  /* the object was signalled; the caller re-reads its state */
    SA2_WAKE_PULSE   = 2,  /* PulseEvent released exactly this thread; no state to re-read */
    SA2_WAKE_TIMEOUT = 3,  /* virtual time reached the caller's deadline */
    SA2_WAKE_EXIT    = 4,  /* the thread being waited for finished */
};

/* What a blocked thread is waiting for. */
enum {
    SA2_WAIT_EVENT  = 1,   /* object = a guest event HANDLE */
    SA2_WAIT_THREAD = 2,   /* object = a guest thread index */
    SA2_WAIT_CS     = 3,   /* object = a guest CRITICAL_SECTION address */
    SA2_WAIT_SLEEP  = 4,   /* object unused; deadline only */
    /* SuspendThread on the running thread. object = the thread's own index;
     * no deadline, because only ResumeThread ends it. See sa2_sched_suspend. */
    SA2_WAIT_SUSPEND = 5,
};

/* Take the guest lock for thread 0 and register it. Idempotent; every entry
 * point below calls it, so a program that never creates a thread never has to
 * know this exists. Returns the current thread index (0). */
int  sa2_sched_init(void);

/* Thread 0's guest thread id, which SA2_GUEST_TID moves. Every later thread's
 * id is derived from it, so moving it moves them all -- a probe that hard-codes
 * a worker's id therefore fails, which is the point. Must be called before the
 * first thread is created; it is a no-op afterwards and says so. */
int  sa2_sched_set_base_tid(uint32_t tid);

/* SA2_THREADS_BREAK: falsification switches, so every clause in the probe's
 * verdict can be shown failing before it is trusted. Each one breaks exactly
 * one thing this file claims to do:
 *
 *   refuse     CreateThread refuses, as the single-threaded runtime did
 *   notimer    timeSetEvent refuses
 *   nopulse    the timer ticks and its pulse releases nobody
 *   sharedtls  every thread reads thread 0's TLS slots
 *   sharedfls  every thread reads thread 0's FLS slots, which is where the
 *              MSVC CRT keeps its per-thread data: the slice 12 defect
 *   noflsexit  a thread that ends runs no FLS callbacks, so a CRT per-thread
 *              block the CRT made lazily is never freed: the 2026-09-26
 *              defect (sa2_sched_fls_exit_callbacks)
 *   sharedteb  every thread points FS at thread 0's TEB
 *   tinystack  a worker gets 4 KiB of stack instead of 1 MiB
 *   free       the token goes to whichever host thread wakes first, so the
 *              order is the host's and not the program's
 *   pollimports  the guest-poll rule's quiet test (sa2_sched_poll) compares
 *              the import-call count alone, the marker the 2026-10-02 review
 *              warned against: a poll on the drawing path, whose runtime work
 *              is COM methods and a frame wait, reads as quiet
 *   noreuse    no row is ever reused, so CreateThread refuses once every row
 *              has been used, as it did before 2026-10-02
 *   rowindex   the hand-off and a pulse walk rows by index, not by creation
 *              number, so a thread on a reused row runs ahead of older ones:
 *              the order sa2_sched_seq exists to keep. It changes nothing
 *              until a row is reused (the 2026-10-02 review, S2)
 *
 * Off by default, so no shipping verdict changes by linking this in.
 *
 * SA2_THREAD_ROWS=<n> (2..SA2_MAX_GUEST_THREADS, the default) is how many rows
 * are used before one is reused. A lower number makes a short run reuse rows,
 * so that "a reused row changes nothing the guest sees" can be checked on a
 * registered replay rather than only on a session long enough to need it. */
void        sa2_threads_config_from_env(void);

/* The virtual-time budget, in guest milliseconds. The scheduler advances the
 * clock whenever nothing is runnable, so a guest whose only remaining activity
 * is a periodic timer waking a worker that goes straight back to waiting runs
 * forever at full speed and burns no guest progress at all -- a livelock that
 * looks exactly like a hang and is not one. Reaching the budget stops the run
 * with a note naming it. SA2_VIRTUAL_BUDGET_MS moves it; 0 removes it. It is a
 * bound on the *guest's* clock, not on wall time, so it does not make a run
 * depend on how fast this machine is. */
uint64_t sa2_sched_budget_us(void);
int      sa2_sched_budget_hit(void);
const char *sa2_threads_break(void);       /* "" when nothing is broken */

int      sa2_sched_current(void);                 /* index of the running guest thread */
uint32_t sa2_sched_tid(int idx);                  /* its guest thread id, 0 if none */
uint32_t sa2_sched_teb(int idx);                  /* its TEB guest address */
/* Rows in use, thread 0 included: every thread ever created until rows are
 * reused, and never more than SA2_MAX_GUEST_THREADS. The bound of a loop over
 * rows; sa2_import_threads_created counts threads. */
int      sa2_sched_count(void);
int      sa2_sched_live(void);                    /* how many have not finished */
/* The creation order: 0 for thread 0, then 1, 2, ... for each thread created,
 * never reused. The hand-off rule's "creation order" is this number's, so a
 * reused row does not move its new thread ahead of older ones. */
uint64_t sa2_sched_seq(int idx);
/* The guest closed the thread's handle (CloseHandle). With its thread ended,
 * its row may then be reused; see SA2_MAX_GUEST_THREADS. */
void     sa2_sched_handle_closed(int idx);
uint64_t sa2_sched_rows_reused(void);             /* rows given to a later thread */

/* Create a guest thread running `start_va(param)` on its own stack and TEB.
 * `suspended` is CREATE_SUSPENDED: the host thread exists and waits, and runs
 * nothing until `sa2_sched_resume`. Returns the thread index, or -1 with
 * `*why` set to a short reason the caller reports. */
int sa2_sched_spawn(uint32_t start_va, uint32_t param, int suspended, const char **why);

/* ExitThread: record the exit code the running thread is ending with. The
 * caller then leaves its lifted code by trapping, because the runtime cannot
 * unwind a host call stack it is in the middle of. */
void     sa2_sched_exit_current(uint32_t code);

/* One dword of the thread parameter, snapshotted at CreateThread time.
 *
 * The runtime does not know what it means; the *probe* does, and sets the
 * offset. `_beginthreadex` at 0x00828ad3 passes CreateThread its own
 * `_threadstartex` and stashes the routine the guest actually asked for at
 * offset 0x54 of the block it passes as the parameter -- and
 * `_threadstartex` then copies what it needs and frees that block, so reading
 * it after the run gives whatever the heap has put there since. The first
 * version of this read it at the end and reported 0x00006e69, two bytes of a
 * string. The snapshot is taken while the block is still the guest's. */
void     sa2_sched_set_param_peek(uint32_t offset);
uint32_t sa2_sched_param_peek(int idx);

/* ------------------------------------------------------------- suspension
 *
 * **A suspend count is a count, and a thread whose count is non-zero is not
 * runnable whatever else it was doing.** That is the whole rule, and it is
 * Windows' own: `SuspendThread` increments, `ResumeThread` decrements, both
 * return the *previous* value, and only a count of zero puts a thread back in
 * the scheduler's hands. A thread that suspends *itself* does not return from
 * the call until somebody resumes it.
 *
 * The first implementation of this file wrote the rule as a state transition
 * -- `if (state == T_READY) state = T_SUSPENDED` -- which is a different
 * claim: it suspends a thread that happens to be waiting its turn and does
 * nothing at all to a thread that is running or blocked. CRI's server body at
 * 0x007C53F0 ends every pass with `SuspendThread` on **its own** handle, so
 * under that rule the count climbed past 0x1fc9b while the thread it named ran
 * on, and the main thread got seven slices in four hundred thousand calls.
 *
 * `suspend_count` is therefore orthogonal to the wait state: `pick_next`
 * refuses a thread with a non-zero count, and a self-suspend parks the caller
 * at a hand-off point exactly as `WaitForSingleObject` does.
 *
 * **One entry point G1's parenthetical list does not contain.** G1 section 3
 * names "WaitForSingleObject, Sleep, event and critical-section calls, the
 * message pump" as the places the guest lock is released. Parking inside
 * `SuspendThread` without releasing it would stop every guest thread at once,
 * so `SuspendThread` is a hand-off point here; it is recorded in the journal
 * the way `Present` was rather than folded in silently. */
int      sa2_sched_resume(int idx);       /* previous suspend count, -1 on a bad index */
int      sa2_sched_suspend(int idx);      /* previous suspend count, -1 on a bad index */
int      sa2_sched_suspend_count(int idx);
uint64_t sa2_sched_parks(int idx);        /* times this thread parked on its own suspend */

/* SA2_SUSPEND_PARK=0: the break. A self-suspend increments the count and
 * returns immediately, which is what this runtime did before, so the defect
 * can be shown producing its own symptom rather than argued about. 1 (the
 * default) parks. It scopes to parking alone -- the count still keeps a thread
 * out of `pick_next` in both arms -- so exactly one clause moves. */
int      sa2_sched_suspend_park(void);

/* The suspension census: every SuspendThread/ResumeThread the scheduler saw,
 * with *who called it*, *who it named*, and *what state that thread was in*.
 * Those three are what the previous slice guessed at and got wrong. The first
 * SA2_SUSPEND_LOG events are kept in order; the totals count them all. */
#define SA2_SUSPEND_LOG 64u
typedef struct {
    uint32_t seq;             /* 1-based, over suspends and resumes together */
    uint32_t op;              /* 0 = SuspendThread, 1 = ResumeThread */
    int32_t  by;              /* the guest thread index that made the call */
    int32_t  target;
    int32_t  count_before, count_after;
    uint32_t parked;          /* 1 when this call parked the caller */
    const char *target_state; /* the target's state BEFORE the call */
} sa2_suspend_row;
uint32_t               sa2_sched_suspend_log_count(void);
const sa2_suspend_row *sa2_sched_suspend_log_at(uint32_t i);
uint64_t sa2_sched_suspend_total(void);
uint64_t sa2_sched_resume_total(void);
uint64_t sa2_sched_suspend_self_total(void);   /* the caller named itself */
uint64_t sa2_sched_suspend_other_total(void);
uint64_t sa2_sched_parks_total(void);
uint64_t sa2_sched_suspend_no_end(void);       /* parks nothing could ever end */
/* How many SuspendThread calls found the target in each scheduler state, by
 * the state's own name. `i` walks 0..sa2_sched_state_count()-1. */
uint32_t    sa2_sched_state_count(void);
const char *sa2_sched_state_label(uint32_t i);
uint64_t    sa2_sched_suspend_on_state(uint32_t i);
int      sa2_sched_finished(int idx);
/* It ended through ExitThread, whose code is sa2_sched_exit_code's; 0 for a
 * thread that returned from its routine or trapped. */
int      sa2_sched_exit_called(int idx);
uint32_t sa2_sched_exit_code(int idx);
void     sa2_sched_set_priority(int idx, int32_t prio);
int32_t  sa2_sched_priority(int idx);

/* ----------------------------------------------------------- hand-off points
 *
 * Every one of these may run other guest threads before it returns, and none
 * of them may be called from anywhere but a runtime entry point reached from
 * lifted code -- that is the whole of the "released only inside the runtime's
 * sync and time entry points" rule.
 */

/* Run every other ready thread once, round-robin from this one. Returns the
 * number of hand-offs performed (0 when nothing else was ready). */
int sa2_sched_yield(const char *where);
/* SA2_SPIN_ADVANCE: the import-call diagnostic described in sa2_threads.c. 0
 * is off and is the only setting a verdict is taken under. */
uint32_t sa2_sched_spin_advance(void);
uint64_t sa2_sched_spin_advances(void);
void     sa2_sched_spin_tick(void);

/* **The spin rule**, which ships on, and the clock change this stage makes.
 *
 * A hand-off point that finds nobody else ready is a *spin observation*. After
 * SA2_SPIN_YIELDS of them in a row the clock advances to the earliest deadline,
 * because a thread that has passed that many hand-off points with nothing else
 * able to run and the clock standing still is busy-waiting, and a busy-wait on
 * real hardware is paid for in time. `SA2_SPIN_YIELDS=0` turns it off and is
 * the break: with it off the guest's `adxf_GetStat` poll never ends.
 *
 * `observations` counts hand-off points that found nobody ready; `clock_moves`
 * counts the ones that actually moved the clock -- two numbers rather than one,
 * because a rule that fires and changes nothing must not read as a rule that
 * worked. */
uint32_t sa2_sched_spin_yields(void);
uint64_t sa2_sched_spin_clock_moves(void);
uint64_t sa2_sched_spin_observations(void);

/* SA2_CS_HANDOFF: 1 (the default) makes `LeaveCriticalSection` a hand-off
 * point. G1's decision already lists "event and critical-section calls" among
 * the entry points where the lock is released and the next runnable thread is
 * picked; the first implementation handed off only on *contention*, and the
 * guest's streaming poll takes an uncontended lock four times per iteration and
 * nothing else, so a ready worker starved forever. Leave, not Enter: a hand-off
 * belongs where the guest has given something up. 0 is the break. */
int sa2_sched_cs_handoff(void);

/* ---------------------------------------------------- the guest's busy-wait
 *
 * **A hand-off point at a guest function, which G1's list does not contain,
 * and the first runtime behaviour keyed on a guest code address.** Every other
 * hand-off point is a runtime entry point: an import, or the frame's wait in
 * the d3d9 device. This one is the guest's own operation-done query
 * 0x00427EE0, which runtime/sa2_poll_handoff.c wraps in every build
 * (tools/s4/link_s4.py PRODUCTION_WRAPS).
 *
 * Why (2026-10-02, the NEW FILE hang): `SOC_AsynDoWithNowLoading`
 * (0x00428470) starts an "AsynDo" thread, resumes it, and then waits for a
 * done byte only that thread sets. When the byte 0x0093BC5B is 0 the wait is
 * `cmp / je / call 0x00427EE0 / test / je` and nothing else: no import, no
 * frame, no runtime call at all. The scheduler is never consulted, the worker
 * stays READY with zero slices, and the game hangs. On Windows the worker runs
 * on another core; design 3.5 asks for "the same sync semantics".
 *
 * The rule: a poll that finds the operation not done, made by a thread that
 * has passed **no runtime entry since its own previous poll**, is a busy-wait
 * by construction, and it runs `sa2_sched_yield` (Sleep(0): every other ready
 * thread once, round-robin, the clock unchanged; when nobody is ready it is a
 * spin observation, so the spin rule still moves the clock after
 * SA2_SPIN_YIELDS of them). "No runtime entry" is the measured definition the
 * experiment ran under, not a looser one: the thread's previous poll left the
 * import-call count, the switch count, the spin observations, the clock
 * advances and the frame count exactly as this poll finds them. A poll on a
 * drawing path is never quiet, because the frame's wait moves the frame count;
 * a marker of imports alone would read it as quiet (SA2_THREADS_BREAK=
 * pollimports, the review's S2a). The marker is per thread and is taken after
 * any hand-off the poll itself made.
 *
 * SA2_POLL_HANDOFF: 1 (the default) is the rule; 0 is the break, which counts
 * the polls that would hand off and hands off at none, so the hang comes back.
 * Nothing else is read: any other value refuses the run at configuration (as
 * SA2_TIME_BREAK does), so a spelling such as `on` or `off` cannot switch the
 * rule off without a record. The setting is the `rule` of sa2_poll_stats. */

/* One call of a guest poll the runtime wraps. `done` is what the poll is about
 * to answer, read the way the guest reads it: 1 done, 0 not done, -1 when the
 * container or the operation cannot be read. Only 0 can hand off: a -1 poll is
 * counted and nothing else, since the lifted function is about to fault on the
 * same read, and a hand-off before it would move where that trap lands among
 * the other threads. Returns 1 when it handed off. Called only from
 * runtime/sa2_poll_handoff.c, inside lifted code's call of the guest function,
 * which is where an import stub calls the scheduler. */
int sa2_sched_poll(int done, const char *where);

typedef struct {
    int      rule;                 /* SA2_POLL_HANDOFF */
    uint64_t polls;                /* every call of a wrapped poll */
    uint64_t not_done;             /* ... that found the operation not done (done == 0) */
    uint64_t quiet;                /* ... and no runtime entry since the thread's previous poll */
    uint64_t hand_offs;            /* quiet polls the rule handed off at (0 with the rule off) */
    uint64_t switched;             /* ... at which another thread actually ran */
    uint64_t first_poll;           /* the first hand-off: which poll, */
    uint32_t first_import_call;    /* the import-call count, */
    uint64_t first_frame;          /* and the frame count at it; all 0 when none */
} sa2_poll_stats;
void sa2_sched_poll_stats(sa2_poll_stats *out);

/* Block until `object` is signalled, until virtual time reaches `deadline_us`
 * (0 = no deadline), or until nothing can make progress. Returns SA2_WAKE_*. */
int sa2_sched_block(int reason, uint32_t object, uint64_t deadline_us, const char *where);

/* Make threads blocked on `object` runnable. `pulse` releases at most one and
 * tells it so (PulseEvent); otherwise every waiter is released with
 * SA2_WAKE_SIGNAL and re-reads the object's own state. Returns how many were
 * released. */
int sa2_sched_wake(int reason, uint32_t object, int pulse);

/* Sleep(ms), in virtual time: SA2_WAIT_SLEEP with a deadline, so other threads
 * run first and the clock only jumps if none can. */
void sa2_sched_sleep_ms(uint32_t ms);

/* **The frame boundary, which G1's list of hand-off points does not contain.**
 *
 * G1 section 3 names "WaitForSingleObject, Sleep, event and critical-section
 * calls, the message pump". The guest's loading loop contains none of them:
 * with threads on, it renders BeginScene / Clear / DrawPrimitiveUP / EndScene /
 * Present and polls a flag, and it did that **4 012 107 times in 180 wall
 * seconds** while the four worker threads it had just started sat READY with
 * zero slices between them. One guest lock plus G1's hand-off list is a
 * livelock for any loop that draws instead of sleeping.
 *
 * The d3d9 device is the honest place to fix it, and design 3.5 -- not this
 * file -- says what it should do there: "the simulation runs at exactly 60 Hz
 * per tick under host control". The frame's wait hands off and then advances
 * the virtual clock to the next frame boundary, and the guest's own frame loop
 * becomes the thing that makes time pass. SA2_FRAME_US moves the period; a run
 * that hard-codes 60 Hz somewhere else will disagree with it and say so.
 *
 * **Where in the frame the wait is taken is the native oracle's.** It was
 * `IDirect3DDevice9::Present` until 2026-09-26, on the reading that a real
 * Present blocks until the driver lets the frame go. The adopted oracle's pace
 * is the Mod Loader's frame limiter in the BeginScene wrapper, at the frame's
 * start; whether its restored Present also waits is not measured
 * (runtime/sa2_d3d9.c, `frame_wait`). The device now
 * takes the wait at BeginScene (runtime/sa2_d3d9.c, `frame_wait`, which says
 * what moved), and at a Present only when no BeginScene came before it;
 * SA2_D3D9_BREAK=presentwait puts it back at Present.
 *
 * Recorded as an addition to G1's list rather than folded in silently. */
void     sa2_sched_frame_boundary(void);
uint64_t sa2_sched_frame_us(void);
uint64_t sa2_sched_frames(void);        /* frame boundaries crossed */
/* The next frame boundary moves the clock n frame periods (1 to 16) instead of one: play's
 * low-FPS speed fix (runtime/sa2_frameskip.c) owes the N game ticks a stage frame ran. The
 * boundary still counts one frame. Nothing calls it with the fix off. */
void     sa2_sched_owe_frames(uint64_t n);

/* ------------------------------------------------------- multimedia timers */

/* timeSetEvent's five arguments. Returns the timer id (non-zero) or 0, which
 * is what Windows documents as "the timer could not be created". */
uint32_t sa2_timer_set(uint32_t delay_ms, uint32_t resolution, uint32_t callback,
                       uint32_t user, uint32_t flags);
int      sa2_timer_kill(uint32_t id);            /* 1 on success */
uint32_t sa2_timer_live(void);
uint64_t sa2_timer_fires(void);
uint64_t sa2_timer_pulses_delivered(void);   /* pulses that released a waiting thread */
uint64_t sa2_timer_pulses_lost(void);        /* pulses nobody was waiting for */

/* ------------------------------------------------------------- diagnostics
 *
 * Every one of these is a count of something that happened, not a count of
 * something that was requested: `sa2_sched_switches` is incremented after the
 * token has changed hands, not when a hand-off is asked for, because a counter
 * incremented before the thing it counts agrees with a run where that thing
 * never happened. */
uint64_t sa2_sched_switches(void);
uint64_t sa2_sched_time_advances(void);
/* SA2_CLOCK_FOLLOWS_REAL (runtime/sa2_threads.c): frame boundaries that moved the virtual clock
 * past the frame clock's to keep it up with real time, by how much in all, and the real time let
 * go past the catch-up bound. */
uint64_t sa2_sched_follow_moves(void);
uint64_t sa2_sched_follow_moved_us(void);
uint64_t sa2_sched_follow_dropped_us(void);
uint64_t sa2_sched_deadlocks(void);              /* waits that had no possible end */
uint64_t sa2_sched_slices(int idx);              /* times this thread got the token */
uint64_t sa2_sched_guest_calls(int idx);
/* The guest address each thread's FIRST and LAST import call returned to.
 *
 * `guest_start_routine` in the probe's rows is the routine `_beginthreadex`
 * was *asked* for, peeked out of its parameter block at CreateThread time. It
 * says what the guest wanted; it does not say what this thread ran. The
 * previous slice reasoned about which thread executes a loop from that field
 * plus a disassembly and got the answer backwards. These two are the thread's
 * own program counters, so they place it in the image rather than in an
 * argument. */
uint32_t sa2_sched_first_caller(int idx);
uint32_t sa2_sched_last_caller(int idx);
void     sa2_sched_note_caller(uint32_t caller);
uint32_t sa2_sched_stack_low(int idx);           /* lowest ESP this thread reached */
uint32_t sa2_sched_stack_base(int idx);          /* the guard line it must stay above */
uint32_t sa2_sched_stack_top(int idx);
uint32_t sa2_sched_start_va(int idx);
uint32_t sa2_sched_param(int idx);
const char *sa2_sched_state_name(int idx);
uint32_t sa2_sched_trap_status(int idx);
uint32_t sa2_sched_trap_pc(int idx);
const char *sa2_sched_trap_note(int idx);
/* The last hand-off point each thread passed, for a run that stops mid-wait.
 * It is a *name*, it is never cleared, and it is therefore not evidence about
 * what a thread is waiting for now: use the four below for that. */
const char *sa2_sched_where(int idx);

/* What a blocked thread is blocked on, live rather than last-seen: the wait
 * reason (SA2_WAIT_*), the object (an event HANDLE, a thread index, a guest
 * CRITICAL_SECTION address, or 0 for a pure deadline), the deadline in virtual
 * microseconds (0 = none), and how many blocking waits this thread has entered
 * and ended. All four read as "not blocked" / 0 when the thread is not blocked,
 * because the previous wait of a running thread is not an answer to "what is it
 * waiting for". */
int         sa2_sched_wait_reason(int idx);
uint32_t    sa2_sched_wait_object(int idx);
uint64_t    sa2_sched_wait_deadline(int idx);
uint64_t    sa2_sched_blocks(int idx);
uint64_t    sa2_sched_wakes(int idx);
const char *sa2_sched_wait_reason_name(int idx);

/* Per-thread TLS backing for KERNEL32!Tls*: sa2_imports.c owns the index
 * allocation, this owns the storage, because the storage is per thread and the
 * indices are not. */
uint32_t *sa2_sched_tls(int idx);

/* Per-thread FLS backing for KERNEL32!Fls*, for exactly the reason above.
 *
 * **Fiber-local storage is per fiber, and a thread with no fiber has one**, so
 * a process with no fibers has FLS values that are per thread. This runtime
 * held them in one process-wide array, on the reading that "one fiber" made
 * the semantics TLS's -- and then did not give them TLS's storage. The MSVC
 * CRT keeps its `_ptiddata` in an FLS slot, so every guest thread shared one
 * per-thread data block: `_threadstartex` writes the routine it was asked for
 * into `ptd->_initaddr` and `_callthreadstartex` reads it back, and with one
 * block the last writer won. Three CRI threads created suspended and resumed
 * together all entered the third one's routine. See the journal. */
#define SA2_FLS_SLOTS 128u
uint32_t *sa2_sched_fls(int idx);

/* **A thread that ends runs its FLS callbacks first**, as Windows ends one.
 * Returning from the start routine and ExitThread both end in
 * LdrShutdownThread, whose first act (Wine's RtlProcessFlsData) is: for each
 * FLS index in ascending order that holds a non-null value on the ending
 * thread, call the callback FlsAlloc registered for it with that value, on
 * that thread, and clear the value. Only then does the thread end and its
 * handle become signalled.
 *
 * The MSVC CRT registers `_freefls` (0x007ac4ed) that way (`_mtinit`,
 * 0x007ac7a8), and it is the only thing that frees a `_ptiddata` the CRT made
 * lazily, on the first CRT call of a thread the guest started with
 * CreateThread rather than `_beginthreadex`. The "AsynDo" routine is such a
 * thread. Without the callbacks every one of those blocks stayed allocated
 * and kept its reference on `__initialmbcinfo` (0x00917330), which rose on
 * the lifted side where native holds it (2026-09-26, the U6 finding).
 *
 * The callbacks run only for a thread that returned or called ExitThread; a
 * thread that trapped ends where it trapped. Each callback that returns is
 * counted on the thread's row; one that traps ends the thread with its own
 * note, followed by the callback it was in.
 *
 * **Known limit: Wine's FLS lock is not modelled.** Wine runs these callbacks
 * holding its FLS lock (`fls_section`, which RtlProcessFlsData and RtlFlsFree
 * take around the callbacks in dlls/ntdll/thread.c), so natively two ending
 * threads' callbacks never interleave, and FlsAlloc, FlsFree and another
 * thread's first FlsSetValue wait until they end. Here the callbacks hold no
 * such lock, and `_freefls` passes the CRT's lock hand-offs, so another thread
 * can run in the middle of a callback and do any of those. The boot and Metal
 * Harbor replays show no sign of it: each switches threads exactly as often
 * as it did without the callbacks. */
uint64_t sa2_sched_fls_exit_callbacks(int idx);

/* Threads that ran out of their own stack slot. Counted where it is detected,
 * *before* the trap that stops the run, because the low-water mark cannot fall
 * below the guard line the trap defends: a clause that compared them would
 * have been unable to fail. */
uint64_t sa2_sched_stack_escapes(void);

/* Hand-offs at which the running thread's own TEB held an SEH chain head that
 * was neither the terminator nor an address on that thread's own stack.
 *
 * This exists because breaking the per-thread TEB on purpose
 * (SA2_THREADS_BREAK=sharedteb) changed **nothing** about a run: not one import
 * call, not one trap, only the TEB addresses the probe printed. G1 says SEH
 * depends on one TEB per thread, and it does -- but nothing in this runtime
 * reads the FS:[0] chain yet (RaiseException returns normally for the only code
 * it sees, G1 section 2 step 1), so a shared chain corrupts in silence. A claim
 * whose falsification is invisible is not yet a checked claim.
 *
 * The invariant is the guest's own: a thread's SEH registration records live on
 * that thread's stack, so the chain head it sees must be its own or the
 * terminator. With one TEB for everybody it is somebody else's, and this says
 * so at the first hand-off after the second thread registers. */
uint64_t sa2_sched_seh_violations(void);
uint64_t sa2_sched_seh_checks(void);
uint32_t sa2_sched_seh_first_bad(int idx);

/* --------------------------------------------------------------- the ticks
 *
 * One row per message-pump poll: the scheduler's whole observable state at
 * that instant. Two runs of the same binary with the same environment must
 * produce byte-identical rows, and that -- not an argument about mutexes -- is
 * what says the interleaving is a function of the program.
 *
 * It is a fixed array, written as the run goes, so a run that stops still has
 * the rows up to where it stopped. */
#define SA2_TICK_MAX 512u
typedef struct {
    uint32_t seq, caller;
    uint64_t virtual_us, import_calls;
    uint64_t switches, time_advances, timer_fires, pulses_delivered;
    uint64_t worker_guest_calls;
} sa2_tick_row;
void                sa2_sched_tick_note(uint32_t caller);
uint32_t            sa2_sched_tick_count(void);      /* rows kept (<= SA2_TICK_MAX) */
uint64_t            sa2_sched_ticks_total(void);     /* polls seen, kept or not */
const sa2_tick_row *sa2_sched_tick_at(uint32_t i);

/* Sampled by the import dispatcher on every call: the guest stack pointer of
 * the running thread. Keeps the low-water mark and refuses a worker that has
 * run out of its own stack slot. */
void sa2_sched_note_esp(uint32_t esp);
#define SA2_TLS_SLOTS 1088u

/* ------------------------------------------------ what sa2_imports.c supplies
 *
 * The scheduler needs the virtual clock and the event objects, and both belong
 * to the Win32 shape rather than to scheduling. These are defined in
 * runtime/sa2_imports.c. */
uint64_t sa2_virtual_us(void);
void     sa2_virtual_advance_to_us(uint64_t us);
/* Set (or, with `pulse`, momentarily release) the guest event `handle`.
 * Returns 0 when the handle is not an event this runtime created. */
int      sa2_event_signal(uint32_t handle, int pulse);
uint32_t sa2_import_calls(void);
/* The callback FlsAlloc registered for FLS index `index`, or 0 when the index
 * is not allocated or was allocated without one. */
uint32_t sa2_fls_callback(uint32_t index);

#ifdef __cplusplus
}
#endif
#endif
