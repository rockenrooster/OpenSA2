/* Guest threads, one guest lock, deterministic hand-off. See sa2_threads.h for
 * the decision this implements and for what it does not provide. */
#define _GNU_SOURCE      /* sched_setaffinity and the CPU_SET macros, for SA2_GUEST_HOST_CPUS */
#include "sa2_threads.h"

#include <pthread.h>
#if defined(__linux__)
#include <errno.h>
#include <sched.h>
#include <sys/syscall.h>
/* <unistd.h>'s, declared alone: that header's brk() would meet this file's brk below. */
long syscall(long number, ...);
#endif
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cpu_state.h"
#include "guest_memory.h"
#include "sa2_call.h"
#include "sa2_stall.h"
#include "sa2_trap.h"

/* ------------------------------------------------------------------ state */

/* T_SUSPENDED is gone on purpose. Suspension used to be one of these states,
 * which made it exclusive with running and with blocked -- and a thread that
 * is suspended *while* blocked, or suspended while it is the one running, is
 * exactly the case CRI's server body takes. It is a count on the row now, and
 * these are only about where the thread is in the scheduler. */
enum { T_UNUSED = 0, T_RUN, T_READY, T_BLOCKED, T_DONE, T_STATE_N };

static const char *k_state_name[] = {"unused", "running", "ready", "blocked",
                                     "finished"};

typedef struct {
    int             used;
    int             index;
    int             state;
    uint64_t        seq;                    /* creation order: 0 for thread 0, never reused */
    int             handle_open;            /* CreateThread's handle not yet closed */
    uint32_t        tid;
    uint32_t        start_va, param;
    uint32_t        stack_low, stack_top;   /* guard line and initial ESP source */
    int             stack_slot;             /* index into the arena; -1 for thread 0 */
    uint32_t        stack_low_water;
    uint32_t        teb;
    int32_t         priority;               /* recorded; the hand-off rule ignores it (see below) */
    int             suspend_count;

    int             wait_reason;
    uint32_t        wait_object;
    uint64_t        deadline_us;            /* 0 = no deadline */
    int             wake_reason;
    const char     *where;

    uint32_t        exit_code;
    int             exit_called;            /* ended through ExitThread, whose code is exit_code */
    uint64_t        fls_exit_callbacks;     /* FLS callbacks run as it ended */
    uint32_t        trap_status, trap_pc;
    char            trap_note[SA2_TRAP_NOTE_MAX];
    int             trap_kept;              /* ended in a trap of its own: the row is never reused */

    uint64_t        slices, guest_calls;
    uint64_t        blocks, wakes;          /* blocking waits entered / ended */
    uint64_t        parks;                  /* times it parked on its own suspend */
    uint32_t        first_caller, last_caller;
    uint32_t        param_peek;
    uint32_t        seh_first_bad;
    /* The guest-poll marker (sa2_sched_poll): the scheduler's counters as this
     * thread's previous poll left them. `poll_marked` is 0 until its first
     * poll, so a thread's first poll is never quiet. */
    int             poll_marked;
    uint32_t        poll_imports;
    uint64_t        poll_switches, poll_spins, poll_advances, poll_frames;
    uint32_t        tls[SA2_TLS_SLOTS];
    uint32_t        fls[SA2_FLS_SLOTS];

    pthread_t       host;
    pthread_cond_t  cv;
    int             host_started;
    uint32_t        host_tid;               /* the host thread's id (Linux gettid), 0 elsewhere */
} thread_row;

/* The ceiling is the stack arena's, plus thread 0 which has its own pinned
 * stack. Asserted rather than commented, because the two constants live in
 * different headers and the last time they disagreed the guest lost a thread
 * silently. */
_Static_assert(SA2_MAX_GUEST_THREADS <= (int)SA2_GUEST_TEB_SLOTS && SA2_MAX_GUEST_THREADS <= 255,
               "every thread row needs a TEB, and a handle row stores its index in a byte");
_Static_assert(SA2_MAX_GUEST_THREADS > (int)SA2_GUEST_TSTACK_SLOTS,
               "rows must not be scarcer than stack slots, or a refusal names the wrong limit");

static thread_row  g_t[SA2_MAX_GUEST_THREADS];
static int         g_count;
static int         g_current = -1;
static int         g_ready;               /* sa2_sched_init has run */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Row reuse (see SA2_MAX_GUEST_THREADS in the header). `g_spawned` numbers the
 * threads in creation order; `g_rows_cap` is how many rows are used before one
 * is reused (SA2_THREAD_ROWS); `g_retired_guest_calls` keeps the guest calls of
 * threads whose rows were reused, so the tick rows' total does not fall. */
static uint64_t g_spawned, g_rows_reused, g_retired_guest_calls;
static int      g_rows_cap = SA2_MAX_GUEST_THREADS;

static uint64_t g_switches, g_time_advances, g_deadlocks;
static uint32_t g_param_peek_off;      /* 0 = do not peek */
static int      g_param_peek_set;

/* SA2_THREADS_BREAK, see the header. One string, compared once per use. */
static char g_break[32];
static int brk(const char *what) { return g_break[0] && !strcmp(g_break, what); }
const char *sa2_threads_break(void) { return g_break; }

/* 30 virtual seconds by default: long enough for a title screen to settle at
 * 60 Hz, short enough that a livelock is reported rather than waited out. */
static uint64_t g_budget_us = 30ull * 1000ull * 1000ull;
/* 16 666 us: 60 Hz, design 3.5's tick. */
static uint64_t g_frame_us = 16666;
static uint64_t g_frames;
uint64_t sa2_sched_frame_us(void) { return g_frame_us; }
uint64_t sa2_sched_frames(void)   { return g_frames; }
static int      g_budget_hit;
uint64_t sa2_sched_budget_us(void) { return g_budget_us; }
int      sa2_sched_budget_hit(void) { return g_budget_hit; }

/* SA2_CLOCK_FOLLOWS_REAL=1, live play's (host/launch, host/android): a frame boundary moves the
 * virtual clock to no less than the real time elapsed since the run's first frame boundary
 * (follow_real). The guest's sound devices answer every cursor and timer from the virtual clock
 * and the player's mixer renders the stream's sound by it (host/player/mixer.h), so a game drawing
 * below 60 frames a second handed the player less than a second of sound a second: at 2400x1080
 * on a Moto One 5G Ace (about 52 fps) 30% of the audio device's requests found its queue short
 * (2026-10-29), heard as stutter. Kept up with real time, the sound plays at its own rate and a
 * slow frame slows only the frame-locked game, as on a PC. One boundary catches up at most
 * kFollowCatchup periods past its own next one; a longer gap (the app in the background, a long
 * load) is let go rather than caught up all at once. A game at or above 60 fps never moves it.
 * Unset or any other value, the clock is the frame clock alone: replays, the selftest, every
 * receipt's run. */
#define kFollowCatchup 4u
static int      g_follow_real;
static uint64_t g_follow_base_ns, g_follow_base_us;
static uint64_t g_follow_moves, g_follow_moved_us, g_follow_dropped_us;
uint64_t sa2_sched_follow_moves(void)      { return g_follow_moves; }
uint64_t sa2_sched_follow_moved_us(void)   { return g_follow_moved_us; }
uint64_t sa2_sched_follow_dropped_us(void) { return g_follow_dropped_us; }

static uint32_t g_spin_advance;        /* SA2_SPIN_ADVANCE; 0 = off */
/* SA2_CS_HANDOFF: 1 = LeaveCriticalSection hands off, which is G1's own list
 * ("event and critical-section calls"). 0 is the break. */
static int      g_cs_handoff = 1;
/* SA2_SPIN_YIELDS: the spin rule's threshold. Defined here with the rest of
 * the environment-settable state; the rule itself is below sa2_sched_yield.
 *
 * 256 was measured, not chosen. The rule advances the virtual clock, and the
 * frame loop computes each frame boundary from *now*, so a rule that fires
 * often stretches frames: at 16 the guest's eight frames spanned 249 992 us
 * instead of 8 x 16 666, which destroys the one observable slice 8 and slice 9
 * both rest on. At 256 the twelfth tick row reads 133 330 us -- the same value
 * the rule-off run produces -- and the CRI server threads still go from 9
 * blocking waits to 194. At 4096 the invariant holds too and the workers drop
 * to 21, which is the livelock breaking slowly for no gain. */
static uint32_t g_spin_yields = 256;

/* SA2_SUSPEND_PARK: 1 (the default) makes SuspendThread on the running thread
 * park it until the count returns to zero, which is what Windows does. 0 is
 * the break and is what this runtime did before: the count climbs and the
 * thread runs on. See the header. */
static int      g_suspend_park = 1;

/* SA2_POLL_HANDOFF: 1 (the default) makes a quiet guest poll a hand-off point
 * (sa2_sched_poll, see the header). 0 is the break: the polls are counted and
 * nothing hands off, so the NEW FILE hang comes back. No other value is read. */
static int      g_poll_handoff = 1;

/* SA2_GUEST_HOST_CPUS=<list> (Linux and Android; "6,7" or "4-7"): every guest thread's host thread
 * runs only on those CPUs. Off by default. The guest runs one thread at a time, so the threads
 * lose no parallelism to a short list, and a hand-off between two of them on one CPU is a
 * switch on that CPU rather than a wake of another one, which on a phone may first have to
 * leave a sleep state. A measurement's knob (the thermal unit, 2026-10-12): it changes where the
 * guest runs, never what it computes or in what order, so a replay is the same with it set. */
#if defined(__linux__)
static uint32_t host_tid_now(void) { return (uint32_t)syscall(SYS_gettid); }

static cpu_set_t g_guest_cpus;
static int       g_guest_cpus_on;

/* A CPU set as a list ("0-3,7") into buf[n]; "?" for none. */
static void cpu_list(const cpu_set_t *set, char *buf, size_t n) {
    size_t len = 0;
    buf[0] = 0;
    for (int c = 0; set && c < CPU_SETSIZE && len + 16 < n; c++) {
        if (!CPU_ISSET(c, set)) continue;
        int e = c;
        while (e + 1 < CPU_SETSIZE && CPU_ISSET(e + 1, set)) e++;
        len += (size_t)snprintf(buf + len, n - len, e > c ? "%s%d-%d" : "%s%d", len ? "," : "", c, e);
        c = e;
    }
    if (!buf[0]) snprintf(buf, n, "?");
}

/* SA2_GUEST_HOST_CPUS on the calling host thread, guest thread `index` (starting at `start_va`).
 * Says nothing when the thread then runs on exactly the CPUs asked. Otherwise one line, for the
 * first 16 such threads: the call's error, or the CPUs the kernel left it (a cpuset narrower than
 * the list, or a CPU not active). 0 when it holds, 1 when the CPUs differ, -1 when the call failed.
 * Called before the thread takes the guest lock, so the count is atomic. */
static int guest_cpus_apply(int index, uint32_t start_va) {
    if (!g_guest_cpus_on) return 0;
    const int rc = sched_setaffinity(0, sizeof g_guest_cpus, &g_guest_cpus);
    const int err = rc ? errno : 0;
    cpu_set_t got;
    CPU_ZERO(&got);
    const int read_back = sched_getaffinity(0, sizeof got, &got) == 0;
    if (!rc && read_back && CPU_EQUAL(&got, &g_guest_cpus)) return 0;
    /* The first 16 are said; a game that makes many threads does not fill its log with them. */
    static int said;
    const int nth = __atomic_add_fetch(&said, 1, __ATOMIC_RELAXED);
    if (nth > 16) return rc ? -1 : 1;
    if (nth == 16)
        fprintf(stderr, "sa2: SA2_GUEST_HOST_CPUS: the 16th thread that does not run where asked; later ones are "
                "not said\n");
    char asked[256], now[256];
    cpu_list(&g_guest_cpus, asked, sizeof asked);
    cpu_list(read_back ? &got : NULL, now, sizeof now);
    if (rc)
        fprintf(stderr, "sa2: SA2_GUEST_HOST_CPUS: guest thread %d (start 0x%08x, host tid %u): sched_setaffinity "
                "to CPUs %s failed: %s (errno %d); it runs on CPUs %s\n", index, start_va, host_tid_now(), asked,
                strerror(err), err, now);
    else
        fprintf(stderr, "sa2: SA2_GUEST_HOST_CPUS: guest thread %d (start 0x%08x, host tid %u) asked for CPUs %s "
                "and runs on CPUs %s\n", index, start_va, host_tid_now(), asked, now);
    return rc ? -1 : 1;
}
static void guest_cpus_from_env(void) {
    const char *v = getenv("SA2_GUEST_HOST_CPUS");
    if (!v || !*v) return;
    CPU_ZERO(&g_guest_cpus);
    const char *p = v;
    int ok = 1;
    while (*p && ok) {
        char *end = NULL;
        const long a = strtol(p, &end, 10);
        long b = a;
        if (end == p || a < 0 || a >= CPU_SETSIZE) { ok = 0; break; }
        p = end;
        if (*p == '-') {
            b = strtol(p + 1, &end, 10);
            if (end == p + 1 || b < a || b >= CPU_SETSIZE) { ok = 0; break; }
            p = end;
        }
        for (long c = a; c <= b; c++) CPU_SET((int)c, &g_guest_cpus);
        if (*p == ',') p++;
        else if (*p) ok = 0;
    }
    g_guest_cpus_on = ok && CPU_COUNT(&g_guest_cpus) > 0;
    if (!g_guest_cpus_on) {
        fprintf(stderr, "sa2: SA2_GUEST_HOST_CPUS=%s: not a CPU list; not used\n", v);
        return;
    }
    /* The calling thread is guest thread 0's; said once the call has been made. */
    const int got = guest_cpus_apply(0, 0);
    fprintf(stderr, "sa2: SA2_GUEST_HOST_CPUS=%s: %s\n", v,
            got == 0 ? "guest threads run on those CPUs"
                     : got > 0 ? "guest threads are asked onto those CPUs; guest thread 0 runs on a different set (above)"
                               : "guest threads are asked onto those CPUs; guest thread 0's call failed (above)");
}

/* SIGURG, the relay's nudge (host/launch/sa2_relay.h, "The stall watch"): taken as a no-op with
 * SA_RESTART, so a system call it interrupts starts again, or returns what it had done, as a
 * write the kernel left asleep on a pipe does. A process-directed signal lands on any thread that
 * does not block it; the one that matters is the token's holder, inside the recorder's write or
 * the pace pipe's read, so a thread that is not passes it on to that one. */
static volatile sig_atomic_t g_nudges;
static void on_nudge(int sig) {
    (void)sig;
    const int saved = errno;
    g_nudges++;
    const int cur = g_current;
    if (cur >= 0 && cur < SA2_MAX_GUEST_THREADS) {
        const uint32_t tid = g_t[cur].host_tid;
        if (tid && tid != host_tid_now()) syscall(SYS_tgkill, syscall(SYS_getpid), (long)tid, (long)SIGURG);
    }
    errno = saved;
}
static void nudge_handler_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_nudge;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGURG, &sa, NULL))
        fprintf(stderr, "sa2: SIGURG, the relay's nudge, cannot be caught: %s\n", strerror(errno));
}
static uint64_t nudges_taken(void) { return (uint64_t)g_nudges; }
#else
static uint32_t host_tid_now(void) { return 0; }
static int guest_cpus_apply(int index, uint32_t start_va) { (void)index; (void)start_va; return 0; }
static void guest_cpus_from_env(void) {}
static void nudge_handler_install(void) {}
static uint64_t nudges_taken(void) { return 0; }
#endif

void sa2_threads_config_from_env(void) {
    guest_cpus_from_env();
    nudge_handler_install();
    const char *v = getenv("SA2_THREADS_BREAK");
    snprintf(g_break, sizeof g_break, "%s", v ? v : "");
    v = getenv("SA2_VIRTUAL_BUDGET_MS");
    if (v && *v) g_budget_us = strtoull(v, NULL, 0) * 1000ull;
    v = getenv("SA2_FRAME_US");
    if (v && *v) g_frame_us = strtoull(v, NULL, 0);
    v = getenv("SA2_CLOCK_FOLLOWS_REAL");
    g_follow_real = v && !strcmp(v, "1");
    if (g_follow_real)
        fprintf(stderr, "sa2 clock: SA2_CLOCK_FOLLOWS_REAL=1: a frame boundary keeps the virtual clock up "
                        "with real time, at most %u frames at once\n", kFollowCatchup);
    v = getenv("SA2_SPIN_ADVANCE");
    if (v && *v) g_spin_advance = (uint32_t)strtoul(v, NULL, 0);
    v = getenv("SA2_SPIN_YIELDS");
    if (v && *v) g_spin_yields = (uint32_t)strtoul(v, NULL, 0);
    v = getenv("SA2_CS_HANDOFF");
    if (v && *v) g_cs_handoff = (int)strtol(v, NULL, 0);
    v = getenv("SA2_SUSPEND_PARK");
    if (v && *v) g_suspend_park = (int)strtol(v, NULL, 0);
    /* Read strictly, unlike the switches above: strtol reads `on`, `true` or
     * `yes` as 0, which would switch the rule off with nothing to say so but
     * the record's `rule` (the 2026-10-02 review). The refusal is
     * SA2_TIME_BREAK's (sa2_imports.c), at configuration, before the guest runs;
     * its address is the guest function the rule is keyed on. */
    v = getenv("SA2_POLL_HANDOFF");
    if (v && *v) {
        if (!strcmp(v, "0")) {
            g_poll_handoff = 0;
        } else if (!strcmp(v, "1")) {
            g_poll_handoff = 1;
        } else {
            sa2_trap_note("SA2_POLL_HANDOFF=%s: 1 is the guest-poll rule and 0 its break, and "
                          "no other value is read", v);
            sa2_trap_raise(SA2_TRAP_CONTROL_LEFT, 0x00427EE0u);
        }
    }
    v = getenv("SA2_THREAD_ROWS");
    if (v && *v) {
        const long n = strtol(v, NULL, 0);
        g_rows_cap = n < 2 ? 2 : n > SA2_MAX_GUEST_THREADS ? SA2_MAX_GUEST_THREADS : (int)n;
    }
}
int sa2_sched_suspend_park(void) { return g_suspend_park; }
int sa2_sched_cs_handoff(void) { return g_cs_handoff; }
static uint32_t g_next_tid = 0x00000f48u;   /* thread 0's id in sa2_imports.c */

/* ------------------------------------------------------------- the timers */

typedef struct {
    int      used;
    uint32_t id, delay_ms, resolution, callback, user, flags;
    uint64_t next_due_us;
    uint64_t fires;
} timer_row;

#define SA2_MAX_TIMERS 8u
#define TIME_PERIODIC            0x0001u
#define TIME_CALLBACK_MASK       0x0030u
#define TIME_CALLBACK_FUNCTION   0x0000u
#define TIME_CALLBACK_EVENT_SET  0x0010u
#define TIME_CALLBACK_EVENT_PULSE 0x0020u

static timer_row g_timer[SA2_MAX_TIMERS];
static uint32_t  g_timer_next_id = 1;
static uint64_t  g_timer_fires;
static uint64_t  g_pulses_lost;
static uint64_t  g_pulses_delivered;

/* How many times the virtual clock has been moved forward with nothing becoming
 * runnable. A periodic timer whose event nobody waits on would otherwise walk
 * the clock forward one tick at a time forever, which reads exactly like a hang
 * and is not one. The bound turns it into a refusal with a note. */
#define SA2_SCHED_EMPTY_ADVANCE_MAX 4096u
static uint32_t g_empty_advances;

uint32_t sa2_timer_live(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SA2_MAX_TIMERS; i++) if (g_timer[i].used) n++;
    return n;
}
uint64_t sa2_timer_fires(void) { return g_timer_fires; }
uint64_t sa2_timer_pulses_lost(void) { return g_pulses_lost; }
uint64_t sa2_timer_pulses_delivered(void) { return g_pulses_delivered; }

/* ------------------------------------------------------------ bookkeeping */

int sa2_sched_current(void) { return g_current < 0 ? 0 : g_current; }
int sa2_sched_count(void)   { return g_count; }

int sa2_sched_live(void) {
    int n = 0;
    for (int i = 0; i < g_count; i++) if (g_t[i].used && g_t[i].state != T_DONE) n++;
    return n;
}

static thread_row *row(int idx) {
    return (idx >= 0 && idx < SA2_MAX_GUEST_THREADS && g_t[idx].used) ? &g_t[idx] : NULL;
}

uint32_t sa2_sched_tid(int idx)        { thread_row *t = row(idx); return t ? t->tid : 0; }
uint32_t sa2_sched_teb(int idx)        { thread_row *t = row(idx); return t ? t->teb : 0; }
uint64_t sa2_sched_seq(int idx)        { thread_row *t = row(idx); return t ? t->seq : 0; }
uint64_t sa2_sched_rows_reused(void)   { return g_rows_reused; }
void sa2_sched_handle_closed(int idx) {
    thread_row *t = row(idx);
    if (t) t->handle_open = 0;
}
int      sa2_sched_finished(int idx)   { thread_row *t = row(idx); return t && t->state == T_DONE; }
int      sa2_sched_exit_called(int idx){ thread_row *t = row(idx); return t && t->exit_called; }
uint32_t sa2_sched_exit_code(int idx)  { thread_row *t = row(idx); return t ? t->exit_code : 0; }
int32_t  sa2_sched_priority(int idx)   { thread_row *t = row(idx); return t ? t->priority : 0; }
uint64_t sa2_sched_switches(void)      { return g_switches; }
uint64_t sa2_sched_time_advances(void) { return g_time_advances; }
uint64_t sa2_sched_deadlocks(void)     { return g_deadlocks; }
uint64_t sa2_sched_slices(int idx)     { thread_row *t = row(idx); return t ? t->slices : 0; }
uint64_t sa2_sched_guest_calls(int idx){ thread_row *t = row(idx); return t ? t->guest_calls : 0; }
uint32_t sa2_sched_first_caller(int idx){ thread_row *t = row(idx); return t ? t->first_caller : 0; }
uint32_t sa2_sched_last_caller(int idx) { thread_row *t = row(idx); return t ? t->last_caller : 0; }
void sa2_sched_note_caller(uint32_t caller) {
    thread_row *t = row(g_current);
    if (!t) return;
    if (!t->first_caller) t->first_caller = caller;
    t->last_caller = caller;
}
uint32_t sa2_sched_stack_low(int idx)  { thread_row *t = row(idx); return t ? t->stack_low_water : 0; }
uint32_t sa2_sched_stack_base(int idx) { thread_row *t = row(idx); return t ? t->stack_low : 0; }
uint32_t sa2_sched_stack_top(int idx)  { thread_row *t = row(idx); return t ? t->stack_top : 0; }
uint32_t sa2_sched_start_va(int idx)   { thread_row *t = row(idx); return t ? t->start_va : 0; }
uint32_t sa2_sched_param(int idx)      { thread_row *t = row(idx); return t ? t->param : 0; }
uint32_t sa2_sched_trap_status(int idx){ thread_row *t = row(idx); return t ? t->trap_status : 0; }
uint32_t sa2_sched_trap_pc(int idx)    { thread_row *t = row(idx); return t ? t->trap_pc : 0; }
const char *sa2_sched_trap_note(int idx){ thread_row *t = row(idx); return t ? t->trap_note : ""; }
const char *sa2_sched_where(int idx)   { thread_row *t = row(idx); return t && t->where ? t->where : ""; }

/* What a thread is blocked ON, which `where` does not say.
 *
 * `where` is the name of the last hand-off point the thread passed and it is
 * never cleared, so a worker that called `Sleep` once and has been waiting on
 * an event ever since still reports "Sleep". Slice 10 read exactly that and
 * spent a run believing a CRI server thread was sleeping when it was waiting
 * for an event nobody signals -- a label that can be stale is not evidence.
 * These report the scheduler's own live fields, and they read as "not blocked"
 * for a thread that is not blocked rather than returning the last wait it had.
 */
int sa2_sched_wait_reason(int idx) {
    thread_row *t = row(idx);
    return (t && t->state == T_BLOCKED) ? t->wait_reason : 0;
}
uint32_t sa2_sched_wait_object(int idx) {
    thread_row *t = row(idx);
    return (t && t->state == T_BLOCKED) ? t->wait_object : 0;
}
uint64_t sa2_sched_wait_deadline(int idx) {
    thread_row *t = row(idx);
    return (t && t->state == T_BLOCKED) ? t->deadline_us : 0;
}
uint64_t sa2_sched_blocks(int idx)     { thread_row *t = row(idx); return t ? t->blocks : 0; }
uint64_t sa2_sched_wakes(int idx)      { thread_row *t = row(idx); return t ? t->wakes : 0; }
const char *sa2_sched_wait_reason_name(int idx) {
    switch (sa2_sched_wait_reason(idx)) {
    case SA2_WAIT_EVENT:  return "event";
    case SA2_WAIT_THREAD: return "thread";
    case SA2_WAIT_CS:     return "critical-section";
    case SA2_WAIT_SLEEP:  return "deadline";
    case SA2_WAIT_SUSPEND: return "suspend";
    default:              return "not-blocked";
    }
}
/* The stall watch's copy (sa2_stall.h). Read without the guest lock, on the watch's thread. */
int sa2_sched_stall_rows(sa2_stall_row *out, int max, int *current, int *lock_held, uint64_t *nudges) {
    *current = g_current;
    if (pthread_mutex_trylock(&g_lock) == 0) {
        *lock_held = 0;
        pthread_mutex_unlock(&g_lock);
    } else {
        *lock_held = 1;
    }
    *nudges = nudges_taken();
    int n = 0;
    const int count = g_count;
    for (int i = 0; i < count && i < SA2_MAX_GUEST_THREADS && n < max; i++) {
        const thread_row *t = &g_t[i];
        if (!t->used) continue;
        sa2_stall_row *r = &out[n++];
        memset(r, 0, sizeof *r);
        r->index = t->index;
        r->host_tid = t->host_tid;
        r->start_va = t->start_va;
        r->state = t->state >= 0 && t->state < T_STATE_N ? k_state_name[t->state] : "?";
        r->suspend_count = t->suspend_count;
        r->wait = sa2_sched_wait_reason_name(i);
        r->wait_object = sa2_sched_wait_object(i);
        r->deadline_us = sa2_sched_wait_deadline(i);
        r->where = t->where ? t->where : "";
        r->last_caller = t->last_caller;
        r->guest_calls = t->guest_calls;
        r->blocks = t->blocks;
        r->wakes = t->wakes;
        r->kstate = '?';
        r->last_cpu = -1;
    }
    return n;
}

uint32_t *sa2_sched_tls(int idx) {
    if (g_break[0] && !strcmp(g_break, "sharedtls")) return g_t[0].tls;
    thread_row *t = row(idx);
    return t ? t->tls : g_t[0].tls;
}

uint32_t *sa2_sched_fls(int idx) {
    if (g_break[0] && !strcmp(g_break, "sharedfls")) return g_t[0].fls;
    thread_row *t = row(idx);
    return t ? t->fls : g_t[0].fls;
}
uint64_t sa2_sched_fls_exit_callbacks(int idx) {
    thread_row *t = row(idx);
    return t ? t->fls_exit_callbacks : 0;
}

const char *sa2_sched_state_name(int idx) {
    thread_row *t = row(idx);
    return t ? k_state_name[t->state] : "unused";
}

void sa2_sched_set_priority(int idx, int32_t prio) {
    thread_row *t = row(idx);
    if (t) t->priority = prio;
}

/* The lowest ESP each thread has been observed at. Sampled by the import
 * dispatcher, which is the one place the runtime sees the guest's stack
 * pointer on every call, and used to refuse a thread that has left its own
 * stack slot before it can scribble on the slot below. */
static uint64_t g_stack_escapes;
uint64_t sa2_sched_stack_escapes(void) { return g_stack_escapes; }

void sa2_sched_note_esp(uint32_t esp) {
    thread_row *t = row(g_current);
    if (!t) return;
    t->guest_calls++;
    if (esp && esp < t->stack_low_water) t->stack_low_water = esp;
    if (t->index != 0 && esp && esp < t->stack_low) {
        /* Counted here, not inferred from the low-water mark afterwards: the
         * trap below stops the run, so the mark can never be seen below the
         * guard and a check that compared the two could not have failed. */
        g_stack_escapes++;
        sa2_trap_note("guest thread %d (start %#010x) has run its stack down to %#010x, "
                      "below its own slot's guard line %#010x: it is one page from the "
                      "thread below it", t->index, t->start_va, esp, t->stack_low);
        sa2_trap_raise(SA2_TRAP_FAULT, esp);
    }
}

/* --------------------------------------------------------------- the ticks */

static sa2_tick_row g_tick[SA2_TICK_MAX];
static uint32_t     g_tick_used;
static uint64_t     g_tick_total;

uint32_t sa2_sched_tick_count(void)  { return g_tick_used; }
uint64_t sa2_sched_ticks_total(void) { return g_tick_total; }
const sa2_tick_row *sa2_sched_tick_at(uint32_t i) {
    return i < g_tick_used ? &g_tick[i] : NULL;
}

void sa2_sched_tick_note(uint32_t caller) {
    g_tick_total++;
    if (g_tick_used >= SA2_TICK_MAX) return;
    sa2_tick_row *r = &g_tick[g_tick_used++];
    r->seq = (uint32_t)g_tick_total;
    r->caller = caller;
    r->virtual_us = sa2_virtual_us();
    r->import_calls = sa2_import_calls();
    r->switches = g_switches;
    r->time_advances = g_time_advances;
    r->timer_fires = g_timer_fires;
    r->pulses_delivered = g_pulses_delivered;
    r->worker_guest_calls = g_retired_guest_calls;
    for (int i = 1; i < g_count; i++) r->worker_guest_calls += g_t[i].guest_calls;
}

/* ---------------------------------------------------------- the SEH chain */

static uint64_t g_seh_checks, g_seh_violations;
uint64_t sa2_sched_seh_checks(void)     { return g_seh_checks; }
uint64_t sa2_sched_seh_violations(void) { return g_seh_violations; }
uint32_t sa2_sched_seh_first_bad(int idx) { thread_row *t = row(idx); return t ? t->seh_first_bad : 0; }

/* The chain head in `t`'s TEB must be the terminator or an address on `t`'s own
 * stack. Checked at every hand-off, which is every point at which a shared TEB
 * could hand one thread another's registration records. */
static void check_seh(thread_row *t) {
    if (!t || !t->teb) return;
    uint32_t head = 0;
    if (sa2_peek(t->teb + SA2_TEB_EXCEPTION_LIST, 4, &head) != SA2_OK) return;
    g_seh_checks++;
    if (head == SA2_SEH_END_OF_CHAIN) return;
    if (head >= t->stack_low && head < t->stack_top) return;
    g_seh_violations++;
    if (!t->seh_first_bad) t->seh_first_bad = head;
}

/* -------------------------------------------------------------- the token */

/* SA2_THREADS_BREAK=free: hand the token to *whichever* host thread the host's
 * own scheduler wakes first, instead of to the one the fixed rule names. Still
 * one guest thread at a time -- the mutex sees to that -- but the order is the
 * host's and not the program's. It exists so that "two runs are identical" can
 * be shown failing; a determinism check nobody has watched fail is a
 * determinism check nobody should believe. */
#define SCHED_ANYONE (-2)

static void take_token(thread_row *me) {
    for (;;) {
        if (g_current == me->index) return;
        if (g_current == SCHED_ANYONE) { g_current = me->index; return; }
        pthread_cond_wait(&me->cv, &g_lock);
    }
}

/* Hand the guest lock to `next` and wait until it comes back. The counter is
 * incremented *after* g_current has moved, so a run where no hand-off ever
 * happened cannot report one. */
static void switch_to(thread_row *me, int next) {
    check_seh(me);
    if (brk("free")) {
        g_current = SCHED_ANYONE;
        g_switches++;
        for (int i = 0; i < g_count; i++) pthread_cond_broadcast(&g_t[i].cv);
        /* Give the host a real chance to pick somebody else. */
        pthread_cond_wait(&me->cv, &g_lock);
        take_token(me);
        me->slices++;
        return;
    }
    g_current = next;
    g_switches++;
    pthread_cond_broadcast(&g_t[next].cv);
    take_token(me);
    me->slices++;
    check_seh(me);
}

/* The key the walk orders by: the creation number, or under the break
 * SA2_THREADS_BREAK=rowindex the row index, which is the walk before rows were
 * reused and what it would become if a reused row kept its place (the
 * 2026-10-02 review, S2: no run can show the order mattering until two live
 * rows hold threads out of creation order, so the break is what shows it).
 * Without reuse the two keys rise together and the break changes nothing. */
static uint64_t order_key(int i) { return brk("rowindex") ? (uint64_t)i : g_t[i].seq; }

/* Round-robin in creation order, starting after `from`. This is the "fixed
 * rule" of the decision, and it deliberately ignores `priority`: SetThreadPriority
 * is recorded because the guest calls it, but honouring it would make the
 * interleaving a function of a number the runtime cannot check against
 * anything. Named here rather than left as an omission.
 *
 * Creation order is `seq`, not the row index: a reused row holds a thread newer
 * than every other, and walking rows by index would put it ahead of older
 * threads. The eligible thread with the smallest `seq` after `from`'s, else the
 * smallest `seq` of all (which can be `from` itself, last, as before). Until a
 * row is reused, `seq` rises with the index and this is the index walk exactly. */
static int ring_first(int from, int (*eligible)(const thread_row *)) {
    const uint64_t s0 = order_key(from);
    int after = -1, wrap = -1;
    for (int i = 0; i < g_count; i++) {
        const thread_row *t = &g_t[i];
        if (!eligible(t)) continue;
        const uint64_t s = order_key(i);
        if (s > s0) { if (after < 0 || s < order_key(after)) after = i; }
        else        { if (wrap < 0 || s < order_key(wrap)) wrap = i; }
    }
    return after >= 0 ? after : wrap;
}

/* A non-zero suspend count is refused here and nowhere else: this is the one
 * place that decides who runs, so it is the one place that has to know a
 * suspended thread does not. */
static int ready_to_run(const thread_row *t) {
    return t->used && t->state == T_READY && t->suspend_count == 0;
}
static int pick_next(int from) { return ring_first(from, ready_to_run); }

/* Any thread that is not finished, for the case where the holder is leaving
 * and nothing is ready: somebody has to be given the chance to decide it is
 * stuck.
 *
 * A suspended thread is not that somebody. Handing it the token would *run*
 * it -- `take_token` returns as soon as `g_current` names you -- and "the
 * thread the guest suspended started executing because everybody else
 * finished" is the same defect this file exists to fix, reached by a
 * different door. */
static int live_unsuspended(const thread_row *t) {
    return t->used && t->state != T_DONE && t->state != T_UNUSED && t->suspend_count == 0;
}
static int pick_any_live(int from) { return ring_first(from, live_unsuspended); }

/* --------------------------------------------------------- virtual time */

static uint64_t earliest_deadline(void) {
    uint64_t best = 0;
    for (uint32_t i = 0; i < SA2_MAX_TIMERS; i++)
        if (g_timer[i].used && (!best || g_timer[i].next_due_us < best))
            best = g_timer[i].next_due_us;
    for (int i = 0; i < g_count; i++)
        if (g_t[i].used && g_t[i].state == T_BLOCKED && g_t[i].deadline_us
            && (!best || g_t[i].deadline_us < best))
            best = g_t[i].deadline_us;
    return best;
}

static int wake_matching(int reason, uint32_t object, int pulse);

/* Fire every timer due at or before `now`, and release every blocked thread
 * whose deadline has passed. Returns how many threads became ready. */
static int fire_due(uint64_t now) {
    int woke = 0;
    for (uint32_t i = 0; i < SA2_MAX_TIMERS; i++) {
        timer_row *tm = &g_timer[i];
        while (tm->used && tm->next_due_us <= now) {
            tm->fires++;
            g_timer_fires++;
            const uint32_t kind = tm->flags & TIME_CALLBACK_MASK;
            if (kind == TIME_CALLBACK_EVENT_SET) {
                sa2_event_signal(tm->callback, 0);
                woke += wake_matching(SA2_WAIT_EVENT, tm->callback, 0);
            } else if (kind == TIME_CALLBACK_EVENT_PULSE) {
                /* PulseEvent: release one waiter and leave the event clear. When
                 * nobody waits, the pulse is lost -- which is what Windows does,
                 * and is why this is not "set the flag and hope somebody sees it".
                 * The losses are counted, because a worker that never ran and a
                 * worker that ran on every tick would otherwise both be "the timer
                 * fired N times". */
                int n = brk("nopulse") ? 0 : wake_matching(SA2_WAIT_EVENT, tm->callback, 1);
                if (n) g_pulses_delivered += (uint64_t)n; else g_pulses_lost++;
                woke += n;
            } else {
                sa2_trap_note("timer %u fires a TIME_CALLBACK_FUNCTION callback at %#010x, "
                              "which this runtime does not run: no call site in this image "
                              "asks for one, so implementing it would be a guess with no "
                              "oracle", tm->id, tm->callback);
                sa2_trap_raise(SA2_TRAP_CONTROL_LEFT, tm->callback);
            }
            if (tm->flags & TIME_PERIODIC) {
                uint32_t d = tm->delay_ms ? tm->delay_ms : 1u;
                tm->next_due_us += (uint64_t)d * 1000ull;
            } else {
                tm->used = 0;
            }
        }
    }
    for (int i = 0; i < g_count; i++) {
        thread_row *t = &g_t[i];
        if (t->used && t->state == T_BLOCKED && t->deadline_us && t->deadline_us <= now) {
            t->state = T_READY;
            t->wake_reason = SA2_WAKE_TIMEOUT;
            woke++;
        }
    }
    return woke;
}

/* Nothing is runnable. If any deadline is pending, move the virtual clock to
 * it and fire. Returns 1 when something changed. */
static int advance_time(void) {
    uint64_t due = earliest_deadline();
    if (!due) return 0;
    if (g_budget_us && due > g_budget_us) { g_budget_hit = 1; return 0; }
    uint64_t now = sa2_virtual_us();
    if (due > now) {
        sa2_virtual_advance_to_us(due);
        now = due;
    }
    g_time_advances++;
    if (fire_due(now) > 0) { g_empty_advances = 0; return 1; }
    /* The clock moved and nothing woke. That is legitimate once -- a timer can
     * tick while its waiter is elsewhere -- but not without limit. */
    return ++g_empty_advances < SA2_SCHED_EMPTY_ADVANCE_MAX;
}

/* ------------------------------------------------------------- hand-offs */

/* A pulse releases the waiter first in creation order (`seq`), which until a
 * row is reused is the first by index. */
static int wake_matching(int reason, uint32_t object, int pulse) {
    int n = 0, first = -1;
    for (int i = 0; i < g_count; i++) {
        thread_row *t = &g_t[i];
        if (!t->used || t->state != T_BLOCKED) continue;
        if (t->wait_reason != reason || t->wait_object != object) continue;
        if (pulse) {
            if (first < 0 || order_key(i) < order_key(first)) first = i;
            continue;
        }
        t->state = T_READY;
        t->wake_reason = SA2_WAKE_SIGNAL;
        n++;
    }
    if (pulse && first >= 0) {
        g_t[first].state = T_READY;
        g_t[first].wake_reason = SA2_WAKE_PULSE;
        n = 1;
    }
    return n;
}

int sa2_sched_wake(int reason, uint32_t object, int pulse) {
    sa2_sched_init();
    return wake_matching(reason, object, pulse);
}

/* SA2_SPIN_ADVANCE=<n>: a diagnostic, off by default (0), and never on in a
 * run a verdict is taken from.
 *
 * The rule this runtime ships is G1's: the virtual clock moves only when the
 * guest asks or when *nothing* is runnable. A guest that polls without ever
 * blocking is therefore runnable forever, the clock stops, and any other
 * thread sleeping to a future time never wakes. That is what the guest does
 * after the Steam wall comes down: it polls `adxf_GetStat` in a
 * CRICRS_Enter/Leave pair and 3.6 million further import calls buy exactly
 * zero frames, zero switches and zero microseconds.
 *
 * With `n` set, `n` consecutive hand-offs that found nobody else ready move
 * the clock to the earliest deadline instead. It is deterministic (a function
 * of the same state everything else here reads), it is counted, and it exists
 * to answer one question -- *is the sleeping CRI worker what the poll is
 * waiting for?* -- not to change what the runtime does. Turning it on is
 * recorded in the run's own JSON. */
/* defined above sa2_sched_yield; declared here so the env reader can set it */
static uint64_t g_spin_advances, g_spin_calls;
static int spin_clock_move(thread_row *me);
uint32_t sa2_sched_spin_advance(void)   { return g_spin_advance; }
uint64_t sa2_sched_spin_advances(void)  { return g_spin_advances; }

/* Called once per import call. The counter resets at every hand-off, so it
 * measures exactly the thing the first version of this diagnostic could not
 * see: import calls made with no hand-off at all.
 *
 * **Two defects slice 10 found by breaking it.** Both made it report large
 * numbers for a run in which it did nothing:
 *
 *   * `if (pick_next(...) >= 0) return;` read "somebody else is ready" as
 *     "this is not a spin". A thread that is READY and never scheduled is
 *     *exactly* the starvation this diagnostic exists to find, and the guest's
 *     `adxf_GetStat` poll left the sound server READY -- so the diagnostic
 *     returned early on every single call of the run it was turned on for. A
 *     diagnostic placed where the thing it diagnoses never passes fires zero
 *     times and means nothing. It now hands off first.
 *   * `me->state = T_BLOCKED` made `earliest_deadline()` read the *running*
 *     thread's stale `deadline_us` -- left over from its last frame wait -- and
 *     a deadline in the past makes `advance_time()` move the clock nowhere and
 *     wake nobody, which it then counts as an advance because its empty-advance
 *     allowance returns true. 742 "advances" moved the clock 0 us. The field is
 *     now cleared, since a running thread is not waiting for a time. */
void sa2_sched_spin_tick(void) {
    if (!g_spin_advance) { g_spin_calls = 0; return; }
    if (++g_spin_calls < g_spin_advance) return;
    g_spin_calls = 0;
    sa2_sched_init();
    if (sa2_sched_yield("spin")) return;      /* somebody else ran; not a spin */
    if (spin_clock_move(&g_t[g_current])) g_spin_advances++;
}

/* The spin rule, and the one change to the clock this slice makes.
 *
 * G1 says the clock moves "when nothing is runnable". The implementation read
 * `runnable` as `state == T_READY`, and that is not the same as *able to make
 * progress*: the guest's `adxf_GetStat` loop is runnable forever while the
 * thread that would end it sleeps 16 ms, so the clock stops and the sleep never
 * expires. The runtime cannot know in general that a loop makes no progress --
 * but it can observe that this thread has passed `n` hand-off points in a row
 * with **no other thread ready and the clock not moving**, and that is a
 * busy-wait by construction. Time then advances to the earliest deadline, which
 * is what a busy-wait on real hardware gets for free.
 *
 * It is deterministic: every input is scheduler state, and two runs of the same
 * binary produce the same sequence. It is bounded by the virtual budget, which
 * `advance_time` already refuses to cross. SA2_SPIN_YIELDS moves `n`; 0 turns
 * the rule off, which is the break that shows the livelock is still there. */
static uint64_t g_spin_clock_moves, g_spin_observations;
static uint32_t g_spin_run;
uint32_t sa2_sched_spin_yields(void)        { return g_spin_yields; }
uint64_t sa2_sched_spin_clock_moves(void)   { return g_spin_clock_moves; }
uint64_t sa2_sched_spin_observations(void)  { return g_spin_observations; }

/* Move the clock on behalf of a thread that is spinning. `me` is RUNNING, so
 * its `deadline_us` is a leftover from its last wait and must not be what the
 * clock advances to; `advance_time` reads "nobody runnable", which is why the
 * state is borrowed for the length of the call. Returns 1 if the clock moved. */
static int spin_clock_move(thread_row *me) {
    const uint64_t before = sa2_virtual_us();
    const int prev = me->state;
    const uint64_t prev_deadline = me->deadline_us;
    me->state = T_BLOCKED;
    me->deadline_us = 0;
    advance_time();
    me->state = prev;
    me->deadline_us = prev_deadline;
    return sa2_virtual_us() > before;
}

int sa2_sched_yield(const char *where) {
    sa2_sched_init();
    thread_row *me = &g_t[g_current];
    me->where = where;
    int n = pick_next(me->index);
    if (n < 0) {
        /* Nobody else is ready. Count it, and after enough of them in a row
         * with the clock standing still, move the clock: see above. */
        g_spin_observations++;
        if (!g_spin_yields || ++g_spin_run < g_spin_yields) return 0;
        g_spin_run = 0;
        if (spin_clock_move(me)) g_spin_clock_moves++;
        n = pick_next(me->index);
        if (n < 0) return 0;
    }
    g_spin_run = 0;
    g_spin_calls = 0;
    me->state = T_READY;
    switch_to(me, n);
    me->state = T_RUN;
    return 1;
}

/* The guest's busy-wait (see the header). The quiet test is the measured one:
 * every counter below is moved by every runtime entry that can run another
 * thread or move the clock -- an import call, a switch, a hand-off point that
 * found nobody ready, a clock advance, a frame boundary -- and a poll whose
 * thread moved none of them since its own previous poll is a pure spin. The
 * rule fires only there, so a wait that already passes a hand-off point is
 * left exactly as it was. */
static uint64_t g_polls, g_polls_not_done, g_polls_quiet, g_poll_handoffs, g_poll_switched;
static uint64_t g_poll_first, g_poll_first_frame;
static uint32_t g_poll_first_import;

static void poll_mark(thread_row *t) {
    t->poll_marked = 1;
    t->poll_imports = sa2_import_calls();
    t->poll_switches = g_switches;
    t->poll_spins = g_spin_observations;
    t->poll_advances = g_time_advances;
    t->poll_frames = g_frames;
}

static int poll_quiet(const thread_row *t) {
    if (!t->poll_marked) return 0;
    if (t->poll_imports != sa2_import_calls()) return 0;
    if (brk("pollimports")) return 1;
    return t->poll_switches == g_switches && t->poll_spins == g_spin_observations
        && t->poll_advances == g_time_advances && t->poll_frames == g_frames;
}

int sa2_sched_poll(int done, const char *where) {
    sa2_sched_init();
    thread_row *me = &g_t[g_current];
    int handed = 0;
    g_polls++;
    /* Only a poll that read "not done" is a wait. -1, an operation the wrapper
     * could not read, is the lifted function's fault a moment later, and it
     * lands where it always did (the header). */
    if (done == 0) {
        g_polls_not_done++;
        if (poll_quiet(me)) {
            g_polls_quiet++;
            if (g_poll_handoff) {
                if (!g_poll_handoffs) {
                    g_poll_first = g_polls;
                    g_poll_first_import = sa2_import_calls();
                    g_poll_first_frame = g_frames;
                }
                g_poll_handoffs++;
                if (sa2_sched_yield(where)) g_poll_switched++;
                handed = 1;
            }
        }
    }
    /* Taken after any hand-off this poll made, so the next poll asks whether
     * anything ran between this one's return and itself. */
    poll_mark(me);
    return handed;
}

void sa2_sched_poll_stats(sa2_poll_stats *out) {
    out->rule = g_poll_handoff;
    out->polls = g_polls;
    out->not_done = g_polls_not_done;
    out->quiet = g_polls_quiet;
    out->hand_offs = g_poll_handoffs;
    out->switched = g_poll_switched;
    out->first_poll = g_poll_first;
    out->first_import_call = g_poll_first_import;
    out->first_frame = g_poll_first_frame;
}

int sa2_sched_block(int reason, uint32_t object, uint64_t deadline_us, const char *where) {
    sa2_sched_init();
    thread_row *me = &g_t[g_current];
    me->where = where;
    me->state = T_BLOCKED;
    me->wait_reason = reason;
    me->wait_object = object;
    me->deadline_us = deadline_us;
    me->wake_reason = SA2_WAKE_NEVER;
    me->blocks++;

    /* A deadline that has already passed is a timeout, not a wait. */
    if (deadline_us && deadline_us <= sa2_virtual_us()) {
        me->state = T_RUN;
        me->wakes++;
        return SA2_WAKE_TIMEOUT;
    }

    for (;;) {
        if (me->state == T_READY) {          /* somebody released me */
            me->state = T_RUN;
            me->wakes++;
            return me->wake_reason;
        }
        int n = pick_next(me->index);
        if (n >= 0) { switch_to(me, n); continue; }
        if (advance_time()) continue;
        /* Nothing is runnable and either no clock can move or moving it has
         * stopped waking anybody. The wait has no end. */
        me->state = T_RUN;
        g_empty_advances = 0;
        g_deadlocks++;
        return SA2_WAKE_NEVER;
    }
}

/* The frame periods the next boundary moves the clock: 1, or the N game ticks the frame just ran
 * under play's low-FPS speed fix (runtime/sa2_frameskip.c, which owes them when its level loop
 * returns; with the fix off nothing ever owes, so every boundary moves one period). */
static uint64_t g_frames_owed = 1;

void sa2_sched_owe_frames(uint64_t n) {
    if (n >= 1 && n <= 16) g_frames_owed = n;
}

/* SA2_CLOCK_FOLLOWS_REAL (above): the boundary `next` the frame clock asks for, or the real time
 * elapsed since the first boundary when that is later, at most kFollowCatchup periods past
 * `next`; a later real time moves the base, so the excess is let go. */
static uint64_t follow_real(uint64_t now, uint64_t next) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return next;
    const uint64_t real_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!g_follow_base_ns) {
        g_follow_base_ns = real_ns;
        g_follow_base_us = now;
        return next;
    }
    uint64_t target = g_follow_base_us + (real_ns - g_follow_base_ns) / 1000ull;
    const uint64_t cap = next + kFollowCatchup * g_frame_us;
    if (target > cap) {
        g_follow_dropped_us += target - cap;
        g_follow_base_ns = real_ns;
        g_follow_base_us = cap;
        target = cap;
    }
    if (target <= next) return next;
    g_follow_moves++;
    g_follow_moved_us += target - next;
    return target;
}

void sa2_sched_frame_boundary(void) {
    sa2_sched_init();
    const uint64_t periods = g_frames_owed;
    g_frames_owed = 1;
    if (!g_frame_us) { sa2_sched_yield("frame"); return; }
    const uint64_t now = sa2_virtual_us();
    uint64_t next = ((now / g_frame_us) + periods) * g_frame_us;
    if (g_follow_real) next = follow_real(now, next);
    g_frames++;
    /* One tick row per frame, taken before the hand-off: this is the
     * "per-tick state" two runs are compared on. */
    sa2_sched_tick_note(0xF2A3E000u | (uint32_t)(g_frames & 0xFFFu));
    /* A sleep to the next frame boundary: every ready thread runs first, and
     * only when none can does the clock move -- which is the same rule every
     * other hand-off point uses, so the frame loop does not get a special one. */
    sa2_sched_block(SA2_WAIT_SLEEP, 0, next, "frame");
    g_t[g_current].state = T_RUN;
}

void sa2_sched_sleep_ms(uint32_t ms) {
    sa2_sched_init();
    uint64_t deadline = sa2_virtual_us() + (uint64_t)ms * 1000ull;
    thread_row *me = &g_t[g_current];
    /* Sleep(0) on Windows yields to an equal-priority runnable thread and
     * returns; it does not advance the clock. Anything else is a real wait. */
    if (ms == 0) { sa2_sched_yield("Sleep(0)"); return; }
    int w = sa2_sched_block(SA2_WAIT_SLEEP, 0, deadline, "Sleep");
    if (w == SA2_WAKE_NEVER && !g_budget_hit) {
        /* Nobody else can run and no deadline is nearer: the sleep itself is
         * the deadline, so honour it by moving the clock. This is exactly
         * single-threaded Sleep.
         *
         * **Not when the budget is what refused the advance.** The first
         * version of this had no such guard, and a guest polling in a
         * `Sleep(10)` loop for a worker that had already died walked the
         * virtual clock forward 3 777 006 668 times in ten wall minutes --
         * every single one of them past the budget that was supposed to stop
         * exactly this. A fallback that ignores the bound it falls back from
         * is not a fallback. */
        if (sa2_virtual_us() < deadline) sa2_virtual_advance_to_us(deadline);
        g_time_advances++;
        fire_due(sa2_virtual_us());
    }
    me->state = T_RUN;
}

/* ----------------------------------------------------------- thread bodies */

/* Call the guest's `fn(arg)`, a one-argument __stdcall routine, as the top of
 * thread `t`'s own stack, on its own TEB. */
static void call_on_own_stack(thread_row *t, uint32_t fn, uint32_t arg,
                              sa2_cpu_state_t *st, sa2_call_result_t *r) {
    sa2_cpu_state_reset(st);
    /* A new thread starts at control word 0x027F on Windows and under Wine.
     * sa2_cpu_state_reset's 0x007F is Direct3D's per-thread setting, which only
     * the thread that creates the device gets, so the loading threads ran at
     * single precision here and double natively (2026-09-23 review). */
    st->x87_cw = 0x027F;
    st->fs_base = t->teb;
    /* __stdcall DWORD WINAPI f(LPVOID): [esp] return address, [esp+4] parameter. */
    uint32_t esp = (t->stack_top - 16u) & ~0xFu;
    esp -= 8u;
    sa2_poke(esp + 4u, 4, &arg);
    uint32_t sentinel = SA2_CALL_SENTINEL;
    sa2_poke(esp, 4, &sentinel);
    st->gpr[SA2_ESP] = esp;
    sa2_call(fn, st, 0, r);
}

static void run_guest_thread(thread_row *t) {
    sa2_cpu_state_t st;
    sa2_call_result_t r;
    t->stack_low_water = ((t->stack_top - 16u) & ~0xFu) - 8u;
    call_on_own_stack(t, t->start_va, t->param, &st, &r);
    t->trap_status = r.status;
    t->trap_pc = r.exit_pc;
    snprintf(t->trap_note, sizeof t->trap_note, "%s", r.note);
    /* ExitThread leaves through a trap too; any other trap is the thread's own
     * end, and its note is evidence (reusable_row). */
    t->trap_kept = r.status != SA2_TRAP_NONE && !t->exit_called;
    /* A thread that returned ends with its routine's EAX. One that called
     * ExitThread ends with the code it passed, which sa2_sched_exit_current
     * recorded before the call trapped out: EAX at that trap is whatever the
     * lifted code left there, and reading it made every CRI thread that ended
     * in `_endthreadex` -> ExitThread(0) answer GetExitCodeThread with 1. */
    if (!t->exit_called) t->exit_code = st.gpr[SA2_EAX];
}

/* The FLS half of Windows' thread exit (see sa2_sched_fls_exit_callbacks in
 * the header): each index with a value on this thread, in ascending order, gets the
 * callback FlsAlloc registered for it, on this thread, and the value is then
 * cleared. The callbacks run guest code and may pass hand-off points (the
 * CRT's `_freefls` takes and leaves its locks), so other threads can run
 * meanwhile; this thread is still running and its handle not yet signalled,
 * which is what a waiter on it sees natively too. A callback that traps ends
 * the thread there, with its own note. Wine's FLS lock is not modelled: see
 * the header.
 *
 * Each callback's frame is built where the start routine's was, at the top of
 * the thread's stack. Natively the callbacks run below the frames the thread
 * ended in, which matters only to a thread that ended inside them -- through
 * ExitThread, with FS:[0] still naming its dead SEH records -- and that also
 * holds an FLS value. The CRT's `_endthreadex` clears its value before it
 * calls ExitThread (0x008289ff), so in this image such a thread runs no
 * callback: the probe's rows show `fls_exit_callbacks` 0 on every thread that
 * ended in ExitThread. */
static void run_fls_exit(thread_row *t) {
    if (brk("noflsexit")) return;
    /* The row's own slots, not sa2_sched_fls(): under the `sharedfls` break
     * that answers thread 0's, and an ending worker would then run `_freefls`
     * on the main thread's block and clear it. Under that break nothing ever
     * writes a worker's own slots, so no callback runs, as when that defect
     * was live. */
    uint32_t *slots = t->fls;
    for (uint32_t i = 0; i < SA2_FLS_SLOTS; i++) {
        const uint32_t value = slots[i];
        if (!value) continue;
        const uint32_t fn = sa2_fls_callback(i);
        if (fn) {
            sa2_cpu_state_t st;
            sa2_call_result_t r;
            call_on_own_stack(t, fn, value, &st, &r);
            if (r.status != SA2_TRAP_NONE) {
                t->trap_status = r.status;
                t->trap_pc = r.exit_pc;
                /* The callback's own note first: tools/s4/reach_extend.py
                 * and thread_run.py read a function-table miss from the start
                 * of a thread's note, and a prefix hid it from them. */
                snprintf(t->trap_note, sizeof t->trap_note,
                         "%s (in the FLS callback %#010x(%#010x) for index %u, run as "
                         "the thread ended)", r.note, fn, value, i);
                t->trap_kept = 1;
                return;
            }
            /* Counted once it has returned; a callback that trapped is in the
             * row's note instead. */
            t->fls_exit_callbacks++;
        }
        slots[i] = 0;
    }
}

static void *thread_body(void *arg) {
    thread_row *t = (thread_row *)arg;
    /* SIGALRM is sa2_call's wall-clock bound and belongs to whichever thread
     * armed it. Blocking it here keeps it on the main thread instead of
     * unwinding a worker at an arbitrary instruction, which would be the one
     * genuinely non-deterministic event in this design. */
#if !defined(_WIN32)
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
#endif      /* Windows: no SIGALRM; sa2_call's bound is the thread that armed it (sa2_trap.c) */
    sa2_trap_thread_init();
    t->host_tid = host_tid_now();
    (void)guest_cpus_apply(t->index, t->start_va);

    pthread_mutex_lock(&g_lock);
    t->host_started = 1;
    take_token(t);
    t->slices++;
    t->state = T_RUN;
    check_seh(t);

    run_guest_thread(t);
    /* Returned, or called ExitThread: Windows' thread exit runs the FLS
     * callbacks before the thread ends. A thread that trapped ends there. */
    if (t->trap_status == SA2_TRAP_NONE || t->exit_called) run_fls_exit(t);
    /* A worker that ended on its own trap is said at once: the run's record
     * names it only when the main call returns, and a game left waiting on
     * what that thread held (a movie's decoder dying with CRI's lock) is
     * killed before that, with no record at all. */
    if (t->trap_kept)
        fprintf(stderr, "sa2 trap: guest thread %d (start %#010x) ended on trap status %u at %#010x: %s\n",
                t->index, t->start_va, t->trap_status, t->trap_pc, t->trap_note);

    t->state = T_DONE;
    /* Anything waiting for this thread to end can run now. */
    wake_matching(SA2_WAIT_THREAD, (uint32_t)t->index, 0);
    int n = pick_next(t->index);
    if (n < 0) n = pick_any_live(t->index);
    if (n >= 0) {
        g_current = n;
        g_switches++;
        pthread_cond_broadcast(&g_t[n].cv);
    }
    pthread_mutex_unlock(&g_lock);
    /* This host thread runs no more guest code: free its signal stack and its
     * remill State, about 68 KiB, which outlived it until rows were reused and
     * would now grow the process with every thread (the 2026-10-02 review,
     * S3). Neither is guest-visible, and neither is the row's. */
    sa2_call_thread_fini();
    sa2_trap_thread_fini();
    return NULL;
}

/* ----------------------------------------------------------------- create */

int sa2_sched_init(void) {
    if (g_ready) return g_current;
    /* The calling host thread is guest thread 0 and takes the guest lock. From
     * here on it holds it for as long as it runs lifted code. */
    pthread_mutex_lock(&g_lock);
    thread_row *t = &g_t[0];
    t->used = 1;
    t->index = 0;
    t->state = T_RUN;
    t->tid = g_next_tid;
    t->teb = sa2_guest_teb(0);
    t->stack_low = SA2_GUEST_STACK_BASE;
    t->stack_top = SA2_GUEST_STACK_TOP;
    t->stack_slot = -1;                 /* thread 0 keeps the pinned stack */
    t->stack_low_water = SA2_GUEST_STACK_TOP;
    t->host = pthread_self();
    t->host_tid = host_tid_now();
    pthread_cond_init(&t->cv, NULL);
    t->slices = 1;
    t->seq = 0;
    g_count = 1;
    g_current = 0;
    g_ready = 1;
    return 0;
}

int sa2_sched_set_base_tid(uint32_t tid) {
    if (g_count > 1) return 0;          /* too late to renumber the living */
    g_next_tid = tid;
    if (g_ready) g_t[0].tid = tid;
    return 1;
}

/* The row a new thread gets once every row has been used: the lowest-numbered
 * one whose thread has ended and whose handle the guest has closed, which is
 * when Windows frees a thread object. -1 when there is none.
 *
 * Never the row of a thread that ended in a trap of its own (anything but
 * returning or ExitThread, an FLS callback's trap included): its trap note is
 * where tools/s4/crt_run.py and tools/s4/reach_extend.py read a function-table
 * miss and the probe's per-row clauses read how it ended, and reuse would wipe
 * it (the 2026-10-02 review, S1). Such a row stays as it ended. */
static int reusable_row(void) {
    if (brk("noreuse")) return -1;
    for (int i = 1; i < g_count; i++) {
        const thread_row *t = &g_t[i];
        if (t->used && t->state == T_DONE && !t->handle_open && !t->trap_kept) return i;
    }
    return -1;
}

int sa2_sched_spawn(uint32_t start_va, uint32_t param, int suspended, const char **why) {
    sa2_sched_init();
    static const char *no_slot = "every thread row holds a thread that is running, whose handle is "
                                 "still open, or that ended in a trap of its own";
    static const char *no_reuse = "SA2_THREADS_BREAK=noreuse: every thread row has been used";
    static const char *no_stack = "the guest thread stack arena has no slot left";
    static const char *no_teb = "the TEB region has no slot for this thread";
    static const char *no_host = "the host refused to create a thread";
    static const char *broken = "SA2_THREADS_BREAK=refuse";
    if (why) *why = NULL;
    if (brk("refuse")) { if (why) *why = broken; return -1; }

    int idx = g_count < g_rows_cap ? g_count : reusable_row();
    if (idx < 0) { if (why) *why = brk("noreuse") ? no_reuse : no_slot; return -1; }
    const int reused = idx < g_count;
    /* The first stack slot no living thread is using. A finished thread's
     * stack is nobody's -- its host thread has returned and the row keeps only
     * the bounds, for the report -- so the slot goes back. Without this one
     * creation site, the "AsynDo" routine at 0x008653d0, exhausted the arena
     * on its own: eight threads that ran 59 import calls each and finished,
     * and then a refusal that read as "the guest wants nine threads at once"
     * when it wanted two. */
    unsigned slot = SA2_GUEST_TSTACK_SLOTS;
    for (unsigned c = 0; c < SA2_GUEST_TSTACK_SLOTS; c++) {
        int taken = 0;
        for (int i = 1; i < g_count; i++)
            if (g_t[i].used && g_t[i].state != T_DONE && g_t[i].stack_slot == (int)c) { taken = 1; break; }
        if (!taken) { slot = c; break; }
    }
    uint32_t low = 0, top = 0;
    if (sa2_guest_tstack(slot, &low, &top) != SA2_OK) { if (why) *why = no_stack; return -1; }
    if (brk("tinystack")) low = top - 0x1000u;   /* 4 KiB: the guard must trip */

    thread_row *t = &g_t[idx];
    if (reused) {
        /* Its old thread ended and nobody holds it: thread_body's last act was
         * to hand the token on and unlock, so its host thread touches the row
         * no more, and no other thread waits on this row's condition. Only the
         * total of its guest calls is kept, for the tick rows. */
        g_retired_guest_calls += t->guest_calls;
        pthread_cond_destroy(&t->cv);
        g_rows_reused++;
    }
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->index = idx;
    t->start_va = start_va;
    t->param = param;
    t->stack_low = low;
    t->stack_top = top;
    t->stack_low_water = top;
    t->stack_slot = (int)slot;
    t->tid = (g_next_tid += 4u);
    t->seq = ++g_spawned;
    t->handle_open = 1;
    if (g_param_peek_set && param) sa2_peek(param + g_param_peek_off, 4, &t->param_peek);
    t->priority = 0;
    /* CREATE_SUSPENDED is the same rule as every other suspension: the thread
     * is READY and its count keeps it out of `pick_next` until ResumeThread
     * brings the count to zero. */
    t->suspend_count = suspended ? 1 : 0;
    t->state = T_READY;
    pthread_cond_init(&t->cv, NULL);

    /* Its own TEB, with its own stack bounds in it. */
    t->teb = sa2_guest_teb(brk("sharedteb") ? 0u : (unsigned)idx);
    if (!t->teb || sa2_guest_teb_init_stack(brk("sharedteb") ? 0u : (unsigned)idx,
                                            low, top) != SA2_OK) {
        t->used = 0;
        if (why) *why = no_teb;
        return -1;
    }
    /* Lay the stack down so the poison tier does not see a read from a page
     * nothing has written, and so two runs start from identical bytes. */
    sa2_guest_fill(low, 0, top - low);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* Lifted code recurses on the *host* stack; musl's 128 KiB default is far
     * too little for a call chain that reached 14 624 imports on the main
     * thread's 8 MiB. */
    pthread_attr_setstacksize(&attr, 64u << 20);
    int rc = pthread_create(&t->host, &attr, thread_body, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        t->used = 0;
        if (why) *why = no_host;
        return -1;
    }
    /* Nothing joins a guest thread's host thread, so a finished one would keep
     * its 64 MiB stack mapped for the life of the process: 255 rows, 16 GiB. */
    pthread_detach(t->host);
    if (!reused) g_count = idx + 1;
    return idx;
}

void sa2_sched_set_param_peek(uint32_t offset) {
    g_param_peek_off = offset;
    g_param_peek_set = 1;
}
uint32_t sa2_sched_param_peek(int idx) { thread_row *t = row(idx); return t ? t->param_peek : 0; }

void sa2_sched_exit_current(uint32_t code) {
    thread_row *t = row(g_current);
    if (t) { t->exit_code = code; t->exit_called = 1; t->where = "ExitThread"; }
}

/* ------------------------------------------------------------- suspension */

/* The census. It records what the previous slice inferred: who called, who was
 * named, and what state the named thread was in at that instant. */
static sa2_suspend_row g_susp[SA2_SUSPEND_LOG];
static uint32_t g_susp_used, g_susp_seq;
static uint64_t g_susp_total, g_resume_total, g_susp_self, g_susp_other;
static uint64_t g_parks_total, g_susp_no_end;
static uint64_t g_susp_on_state[T_STATE_N];

uint32_t sa2_sched_suspend_log_count(void) { return g_susp_used; }
const sa2_suspend_row *sa2_sched_suspend_log_at(uint32_t i) {
    return i < g_susp_used ? &g_susp[i] : NULL;
}
uint64_t sa2_sched_suspend_total(void)       { return g_susp_total; }
uint64_t sa2_sched_resume_total(void)        { return g_resume_total; }
uint64_t sa2_sched_suspend_self_total(void)  { return g_susp_self; }
uint64_t sa2_sched_suspend_other_total(void) { return g_susp_other; }
uint64_t sa2_sched_parks_total(void)         { return g_parks_total; }
uint64_t sa2_sched_suspend_no_end(void)      { return g_susp_no_end; }
uint32_t    sa2_sched_state_count(void)      { return (uint32_t)T_STATE_N; }
const char *sa2_sched_state_label(uint32_t i) {
    return i < T_STATE_N ? k_state_name[i] : "?";
}
uint64_t sa2_sched_suspend_on_state(uint32_t i) {
    return i < T_STATE_N ? g_susp_on_state[i] : 0;
}
int      sa2_sched_suspend_count(int idx) { thread_row *t = row(idx); return t ? t->suspend_count : 0; }
uint64_t sa2_sched_parks(int idx)         { thread_row *t = row(idx); return t ? t->parks : 0; }

static sa2_suspend_row *susp_note(int op, int by, const thread_row *t) {
    g_susp_seq++;
    if (g_susp_used >= SA2_SUSPEND_LOG) return NULL;
    sa2_suspend_row *r = &g_susp[g_susp_used++];
    r->seq = g_susp_seq;
    r->op = (uint32_t)op;
    r->by = by;
    r->target = t->index;
    r->count_before = t->suspend_count;
    r->count_after = t->suspend_count;
    r->parked = 0;
    r->target_state = k_state_name[t->state];
    return r;
}

int sa2_sched_resume(int idx) {
    sa2_sched_init();
    thread_row *t = row(idx);
    if (!t || t->state == T_DONE) return -1;
    sa2_suspend_row *r = susp_note(1, g_current, t);
    g_resume_total++;
    int prev = t->suspend_count;
    if (t->suspend_count > 0) t->suspend_count--;
    /* A thread that parked on its own suspend is blocked on nothing but the
     * count, so zero is what makes it ready again. A thread that was merely
     * kept out of `pick_next` needs no transition: its state never changed. */
    if (t->suspend_count == 0 && t->state == T_BLOCKED
        && t->wait_reason == SA2_WAIT_SUSPEND) {
        t->state = T_READY;
        t->wake_reason = SA2_WAKE_SIGNAL;
    }
    if (r) r->count_after = t->suspend_count;
    return prev;
}

/* Park the running thread until its suspend count returns to zero. This is
 * `sa2_sched_block` with the count as the condition instead of an object, and
 * it is written out rather than folded into that function because its wake
 * condition is not "somebody set my state" -- a resume to a non-zero count
 * must leave the thread exactly where it is. */
static void park_self(thread_row *me) {
    me->where = "SuspendThread(self)";
    me->state = T_BLOCKED;
    me->wait_reason = SA2_WAIT_SUSPEND;
    me->wait_object = (uint32_t)me->index;
    me->deadline_us = 0;
    me->wake_reason = SA2_WAKE_NEVER;
    me->blocks++;
    me->parks++;
    g_parks_total++;
    for (;;) {
        if (me->suspend_count == 0) {
            me->state = T_RUN;
            me->wakes++;
            return;
        }
        int n = pick_next(me->index);
        if (n >= 0) { switch_to(me, n); continue; }
        if (advance_time()) continue;
        /* Nothing can run and no clock can move, so nothing will ever call
         * ResumeThread on this thread. Windows would leave it suspended
         * forever; this runtime says which thread and which count rather than
         * hanging, because a hang is the one answer a replay cannot read. */
        g_susp_no_end++;
        g_deadlocks++;
        me->state = T_RUN;
        sa2_trap_note("guest thread %d (start %#010x) suspended itself and its suspend "
                      "count is %d: no other guest thread can run and no deadline is "
                      "pending, so no ResumeThread will ever arrive",
                      me->index, me->start_va, me->suspend_count);
        sa2_trap_raise(SA2_TRAP_CONTROL_LEFT, me->start_va);
    }
}

/* SuspendThread.
 *
 * The count is the whole of it; see the header for the rule and for what this
 * used to do instead. The two cases the old state-transition form could not
 * express are both live in the CRI run: the caller naming **itself** (which
 * must park), and the caller naming a thread that is **blocked** (whose count
 * must outlive the wait, so that satisfying the wait does not make it run). */
int sa2_sched_suspend(int idx) {
    sa2_sched_init();
    thread_row *t = row(idx);
    if (!t || t->state == T_DONE) return -1;
    sa2_suspend_row *r = susp_note(0, g_current, t);
    g_susp_total++;
    g_susp_on_state[t->state < T_STATE_N ? t->state : 0]++;
    const int self = (t->index == g_current);
    if (self) g_susp_self++; else g_susp_other++;
    int prev = t->suspend_count;
    t->suspend_count++;
    if (r) r->count_after = t->suspend_count;
    if (self && g_suspend_park) {
        if (r) r->parked = 1;
        park_self(t);
    }
    return prev;
}

/* ---------------------------------------------------------------- timers */

uint32_t sa2_timer_set(uint32_t delay_ms, uint32_t resolution, uint32_t callback,
                       uint32_t user, uint32_t flags) {
    sa2_sched_init();
    if (brk("notimer")) return 0;
    for (uint32_t i = 0; i < SA2_MAX_TIMERS; i++) {
        if (g_timer[i].used) continue;
        timer_row *tm = &g_timer[i];
        tm->used = 1;
        tm->id = g_timer_next_id++;
        tm->delay_ms = delay_ms ? delay_ms : 1u;
        tm->resolution = resolution;
        tm->callback = callback;
        tm->user = user;
        tm->flags = flags;
        tm->fires = 0;
        tm->next_due_us = sa2_virtual_us() + (uint64_t)tm->delay_ms * 1000ull;
        return tm->id;
    }
    return 0;
}

int sa2_timer_kill(uint32_t id) {
    for (uint32_t i = 0; i < SA2_MAX_TIMERS; i++)
        if (g_timer[i].used && g_timer[i].id == id) { g_timer[i].used = 0; return 1; }
    return 0;
}
