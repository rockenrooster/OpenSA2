/* Does the image's own CRT start up, and how far does it get?
 *
 *   usage: crt_probe <sonic2app-unpacked.exe> [--va 0x007a801c]
 *                    [--stop-at 0x00401b80] [--timeout S]
 *                    [--json PATH] [--dump-imports PATH] [--trace PATH]
 *
 *   The shipped sonic2app.exe instead, with SA2_EXE_KIND=shipped in the environment and a
 *   build that links the overlay table (tools/s4/link_s4.py --exe-overlay). The run record's
 *   `exe` says which file the loader mapped (runtime/sa2_exe_overlay.h).
 *
 * What this asks that `harness/import_probe.c` did not
 * ----------------------------------------------------
 * The previous slice drove `FUN_00406520`, an interior game function, straight
 * into `_malloc`. `_malloc` at 0x007a84a5 reads the CRT heap handle at
 * 0x01751ff4, finds 0, and takes MSVC's uninitialised-runtime path
 * (__FF_MSGBANNER -> __NMSG_WRITE(0x1e) -> ___crtMessageBoxA ->
 * __crtExitProcess(255)). Nothing about that is an import problem: that entry
 * point never ran CRT init.
 *
 * So this drives the *PE entry point*, 0x007a801c:
 *
 *     007a801c  call 007B5195h        ; __security_init_cookie
 *     007a8021  jmp  007A7E9Eh        ; __tmainCRTStartup
 *
 * and __tmainCRTStartup is the real thing -- GetStartupInfoA, _heap_init
 * (HeapCreate, storing the handle at 0x01751ff4), _mtinit, _RTC_Initialize,
 * _ioinit, GetCommandLineA, __crtGetEnvironmentStringsA, _setargv, _setenvp,
 * _cinit, and then
 *
 *     007a7fa7  push 400000h          ; hInstance
 *     007a7fac  call 00401B80h        ; WinMain
 *
 * The evidence is guest arithmetic, not a program counter
 * ------------------------------------------------------
 * Three observables, each a value only the guest's own instructions could have
 * put where it is, and each recomputed independently on the host:
 *
 *  1. **The security cookie.** `__security_init_cookie` XORs five import
 *     results together -- GetSystemTimeAsFileTime's two dwords,
 *     GetCurrentProcessId, GetCurrentThreadId, GetTickCount, and
 *     QueryPerformanceCounter's two dwords -- applies two corrections, and
 *     stores the result at 0x00917300 with its complement at 0x00917304. The
 *     probe reads the five values *out of the runtime's own call log* and
 *     replays the guest's arithmetic in C. Both sides move: `SA2_VIRTUAL_EPOCH`
 *     changes the time sources, so a probe carrying a hard-coded answer fails.
 *
 *  2. **The heap handle.** [0x01751ff4] must hold the handle HeapCreate
 *     returned in this run, and [0x01d18a54] must hold 1 (__active_heap =
 *     __SYSTEM_HEAP), which only `_heap_init`'s tail writes.
 *
 *  3. **The mutex name, and the flag WinMain computes from the command line.**
 *     WinMain's first act is CreateMutexW on a wide string in the image's own
 *     .rdata; the runtime never supplies it and the probe reports what the
 *     guest passed. WinMain then compares its own parse of the command line
 *     against "-r" and stores the result at 0x01933f6c, and SA2_GUEST_CMDLINE
 *     moves both sides of that comparison.
 *
 *     What this is NOT is a reading of WinMain's four arguments. `--stop-at`
 *     registers a native over an address, and WinMain is entered by a *direct*
 *     call at 0x007a7fac, which lifted code does not route through the function
 *     table -- so the native never fires and `stop.reached` stays false. The
 *     one value that would check `GetStartupInfoA`, nCmdShow = 10, is therefore
 *     not checked by anything, and that is a residue rather than a claim.
 *
 * Any run that derails is not allowed to pass
 * -------------------------------------------
 * The previous slice's probe printed `"ok": true` over a run whose guest stack
 * had been destroyed, because every assertion read state produced *before* the
 * derailment. The same hole is possible here and the same fix applies, plus
 * one more: `--require-stop` makes "the run ended at the address I asked it to
 * stop at" an assertion rather than a report, so a run that stops earlier at an
 * unimplemented import fails loudly instead of being read as progress.
 *
 * Output is one JSON document on stdout and, with --json, to that path as well,
 * written whether or not the assertions held: the failing run is the one whose
 * numbers a later slice will want and the one that always gets deleted.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/cpu_state.h"
#include "../runtime/function_table.h"
#include "../runtime/guest_memory.h"
#include "../runtime/remill_runtime.h"
#include "../runtime/sa2_call.h"
#include "../runtime/sa2_files.h"
#include "../runtime/sa2_d3d9.h"
#include "../runtime/sa2_dialog.h"
#include "../runtime/sa2_gdi.h"
#include "../runtime/sa2_dsound.h"
#include "../runtime/sa2_ddraw.h"
#include "../runtime/sa2_xaudio2.h"
#include "../runtime/sa2_devprofile.h"
#include "../runtime/sa2_dinput.h"
#include "../runtime/sa2_guest_call.h"
#include "../runtime/sa2_wbem.h"
#include "../runtime/sa2_iface.h"
#include "../runtime/sa2_imports.h"
#include "../runtime/sa2_emulate.h"
#include "../runtime/sa2_exe_overlay.h"
#include "../runtime/sa2_module.h"
#include "../runtime/sa2_threads.h"
#include "../runtime/sa2_window.h"
#include "../runtime/sa2_stall.h"
#include "../runtime/sa2_trap.h"

/* The device's closed world (runtime/sa2_d3d9.c; the user's ruling of 2026-10-09, item 1)
 * is opened only by a unit build's flag. This file is the game's main, so a game build
 * compiled with that flag is refused here rather than run with every method answering. */
#ifdef SA2_D3D9_OPEN_WORLD_UNIT_BUILD
#error "SA2_D3D9_OPEN_WORLD_UNIT_BUILD opens the d3d9 closed world for unit probes only; a game build (harness/crt_probe.c) must not define it"
#endif

#define DEFAULT_VA      0x007a801cu   /* AddressOfEntryPoint + ImageBase */
#define WINMAIN_VA      0x00401b80u   /* called at 0x007a7fac with 4 args */
#define STACK_TOP       0x03eefffcu   /* the arm probe's convention */
#define STACK_WORDS     8

/* CRT globals, each named by the instruction that writes it. */
#define VA_COOKIE       0x00917300u   /* 007b5220 mov [917300h],esi   __security_cookie */
#define VA_COOKIE_COMPL 0x00917304u   /* 007b5225 mov [917304h],eax   ~cookie */
#define VA_CRTHEAP      0x01751ff4u   /* 007b5180 mov [1751FF4h],eax  _crtheap */
#define VA_ACTIVE_HEAP  0x01d18a54u   /* 007b518e mov [1D18A54h],eax  __active_heap */
#define VA_CMDLINE      0x01d19bbcu   /* 007a7f4e mov [1D19BBCh],eax  _acmdln */
#define VA_ENVIRON      0x017517d0u   /* 007a7f58 mov [17517D0h],eax  _aenvptr */
#define COOKIE_DEFAULT  0xbb40e64eu   /* 007b51ac mov edi,0BB40E64Eh */
#define WINMAIN_END     0x00401e50u   /* WinMain's own `ret 10h` region */
#define VA_DASH_R_FLAG  0x01933f6cu   /* 00401be2 mov [1933F6Ch],eax: lstrcmpA(lpCmdLine,"-r")==0 */
/* The sound subsystem's worker start routine, pushed by the guest at
 * 0x00785298 and handed to _beginthreadex. The probe knows it in order to
 * *check* that the guest supplied it; the runtime never does. */
#define SOUND_WORKER_VA 0x00785500u
#define CRT_THREADSTARTEX_VA 0x00828a50u  /* what CreateThread is actually given */
#define CRT_PTD_START_ROUTINE 0x54u       /* 00828b3a mov [esi+54h],edi */
#define VA_SSE2_AVAIL   0x01d19ba4u   /* 007ae786 mov [1D19BA4h],eax: __sse2_available, from CPUID */
/* `__flsindex`: the FLS slot the MSVC CRT keeps its `_ptiddata` in. 007AC464
 * and 007AC48E push this dword, and 00828A0F reads the routine to call out of
 * the block that slot names. `guest_start_routine` says what the guest ASKED
 * for; the routine in this block is the one the thread RAN, and the two
 * disagreeing is the whole of the slice 12 defect.
 *
 * **This read was wrong first, and said so.** It pointed at [0x0091730C] and
 * TLS, because `_getptd_noexit` at 007AC2E5 does call `TlsGetValue` with that
 * dword -- but that slot caches the *decoded FlsGetValue thunk*, not the ptd.
 * The tell was that it answered 0x00201010 for all six threads, and 0x00201010
 * is an import thunk address: a per-thread block that six independent arrays
 * agree on, at an address no allocator returns. A probe that had answered six
 * equal *heap* pointers would have read as "TLS is shared" and confirmed the
 * wrong cause. */
#define VA_CRT_FLS_INDEX 0x00917308u

static uint32_t peek32(uint32_t a) {
    uint32_t v = 0;
    return sa2_peek(a, 4, &v) == SA2_OK ? v : 0xDEADBEEFu;
}

/* SA2_PEEK: see the capture site below. Kept as a fixed array so nothing in
 * the probe allocates while reporting a run that has already trapped. */
#define PEEK_MAX 32
typedef struct { char name[48]; uint32_t va; uint32_t value; int ok; } peek_row;
static peek_row g_peek[PEEK_MAX];
static unsigned g_peek_n;

static void peek_capture(void) {
    const char *spec = getenv("SA2_PEEK");
    if (!spec || !*spec) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok && g_peek_n < PEEK_MAX; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        const char *name = eq ? tok : tok;
        const char *addr = eq ? eq + 1 : tok;
        if (eq) *eq = 0;
        peek_row *r = &g_peek[g_peek_n];
        snprintf(r->name, sizeof r->name, "%s", name);
        /* `0xA>0x10>0x4` = read the dword at 0xA, add 0x10 and read that, add
         * 0x4 and read that. One syntax for "the global" and "the field of the
         * object the global points at", which is the whole question here. */
        char chain[128];
        snprintf(chain, sizeof chain, "%s", addr);
        char *save = NULL;
        char *part = strtok_r(chain, ">", &save);
        uint32_t a = part ? (uint32_t)strtoul(part, NULL, 0) : 0;
        uint32_t v = 0;
        int ok = sa2_peek(a, 4, &v) == SA2_OK;
        while (ok && (part = strtok_r(NULL, ">", &save)) != NULL) {
            a = v + (uint32_t)strtoul(part, NULL, 0);
            ok = sa2_peek(a, 4, &v) == SA2_OK;
        }
        r->va = a;
        r->ok = ok;
        r->value = ok ? v : 0;
        g_peek_n++;
    }
}

/* SA2_PEEK_FIND=0xVALUE[,0xVALUE...]: every heap dword equal to that value.
 * "Was this object ever built?" is answered by looking for its vtable pointer
 * in the heap, not by trusting the pointer a global happens to hold. Bounded
 * at FIND_MAX hits per value; the total is reported so a truncated list cannot
 * read as a complete one. */
#define FIND_MAX 24
#define FIND_VALUES 4
typedef struct { uint32_t value; uint32_t total; uint32_t n; uint32_t at[FIND_MAX]; } find_row;
static find_row g_find[FIND_VALUES];
static unsigned g_find_n;
/* The scan's own witness. A search that reports "not present" is worth nothing
 * unless the same pass can show it read something: `scanned` is how many
 * dwords the scan actually read, `nonzero` how many were not zero, and
 * `first_nonzero`/`last_nonzero` where the written part of the heap is. A run
 * that allocated megabytes and reports nonzero = 0 is a broken scan, not an
 * empty heap, and the JSON says which. */
static uint32_t g_find_scanned, g_find_nonzero, g_find_first_nz, g_find_last_nz, g_find_refused;
static uint32_t g_find_lo, g_find_hi;

static void peek_find(void) {
    const char *spec = getenv("SA2_PEEK_FIND");
    if (!spec || !*spec) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok && g_find_n < FIND_VALUES; tok = strtok(NULL, ","))
        g_find[g_find_n++].value = (uint32_t)strtoul(tok, NULL, 0);
    const char *range = getenv("SA2_PEEK_FIND_RANGE");
    g_find_lo = SA2_GUEST_HEAP_BASE;
    g_find_hi = SA2_GUEST_HEAP_END;
    if (range && *range) {
        char rb[64];
        snprintf(rb, sizeof rb, "%s", range);
        char *dash = strchr(rb, ':');
        if (dash) { *dash = 0; g_find_hi = (uint32_t)strtoul(dash + 1, NULL, 0); }
        g_find_lo = (uint32_t)strtoul(rb, NULL, 0);
    }
    for (uint32_t a = g_find_lo; a + 4u <= g_find_hi; a += 4u) {
        uint32_t v;
        if (sa2_peek(a, 4, &v) != SA2_OK) { g_find_refused++; continue; }
        g_find_scanned++;
        if (v) {
            if (!g_find_nonzero) g_find_first_nz = a;
            g_find_last_nz = a;
            g_find_nonzero++;
        }
        for (unsigned i = 0; i < g_find_n; i++) {
            if (v != g_find[i].value) continue;
            if (g_find[i].n < FIND_MAX) g_find[i].at[g_find[i].n++] = a;
            g_find[i].total++;
        }
    }
}

/* SA2_PEEK_DUMP=0xADDR:N -- N dwords from ADDR, for reading an object out. */
#define DUMP_MAX 96
static uint32_t g_dump_at, g_dump_n, g_dump[DUMP_MAX];
static int g_dump_ok[DUMP_MAX];

static void peek_dump(void) {
    const char *spec = getenv("SA2_PEEK_DUMP");
    if (!spec || !*spec) return;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    char *colon = strchr(buf, ':');
    if (colon) *colon = 0;
    g_dump_at = (uint32_t)strtoul(buf, NULL, 0);
    g_dump_n = colon ? (uint32_t)strtoul(colon + 1, NULL, 0) : 16u;
    if (g_dump_n > DUMP_MAX) g_dump_n = DUMP_MAX;
    for (uint32_t i = 0; i < g_dump_n; i++) {
        uint32_t v = 0;
        g_dump_ok[i] = sa2_peek(g_dump_at + 4u * i, 4, &v) == SA2_OK;
        g_dump[i] = g_dump_ok[i] ? v : 0;
    }
}

static int gstr(uint32_t a, char *out, size_t cap) {
    if (!a) { out[0] = 0; return 0; }
    for (size_t i = 0; i + 1 < cap; i++) {
        uint8_t b;
        if (sa2_peek(a + (uint32_t)i, 1, &b) != SA2_OK) { out[i] = 0; return 0; }
        out[i] = (char)b;
        if (!b) return 1;
    }
    out[cap - 1] = 0;
    return 0;
}

/* The answer classes of runtime/sa2_d3d9.h, as the words a reader of the JSON
 * needs: an invented capability and a value the guest itself supplied must
 * never read the same. */
static const char *answer_name(uint8_t a) {
    switch (a) {
    case SA2_D3D9_ANS_REFUSED: return "refused";
    case SA2_D3D9_ANS_NOOP:    return "no-op-success";
    case SA2_D3D9_ANS_GUEST:   return "from-guest";
    case SA2_D3D9_ANS_INVENT:  return "invented";
    case SA2_D3D9_ANS_OBJECT:  return "fabricated-object";
    case SA2_D3D9_ANS_UNSET:   return "d3d9-default-never-set";
    default:                   return "unknown";
    }
}

/* The answer classes of runtime/sa2_dsound.h. Same words as the d3d9 ones
 * wherever they mean the same thing, plus the one class d3d9 did not need:
 * `clock-derived`, a value computed from the virtual clock and a buffer's own
 * declared format. It is neither invented nor supplied by the guest, and a
 * reader who could not tell it from either would have no way to check it. */
static const char *ds_answer_name(uint8_t a) {
    switch (a) {
    case SA2_DS_ANS_REFUSED: return "refused";
    case SA2_DS_ANS_NOOP:    return "no-op-success";
    case SA2_DS_ANS_GUEST:   return "from-guest";
    case SA2_DS_ANS_INVENT:  return "invented";
    case SA2_DS_ANS_OBJECT:  return "fabricated-object";
    case SA2_DS_ANS_UNSET:   return "dsound-default-never-set";
    case SA2_DS_ANS_CLOCK:   return "clock-derived";
    default:                 return "unknown";
    }
}

/* The answer classes of runtime/sa2_xaudio2.h, in dsound's words wherever
 * they mean the same thing. */
static const char *xa2_answer_name(uint8_t a) {
    switch (a) {
    case SA2_XA2_ANS_REFUSED: return "refused";
    case SA2_XA2_ANS_NOOP:    return "no-op-success";
    case SA2_XA2_ANS_GUEST:   return "from-guest";
    case SA2_XA2_ANS_PROFILE: return "from-profile";
    case SA2_XA2_ANS_OBJECT:  return "fabricated-object";
    case SA2_XA2_ANS_CLOCK:   return "clock-derived";
    default:                  return "unknown";
    }
}

/* The answer classes of runtime/sa2_dinput.h and runtime/sa2_wbem.h. The same
 * words as the other devices' wherever they mean the same thing, and two of
 * their own: `from-device-profile`, an answer the capture's device profile
 * recorded natively, and `native-error`, the error native gives a call made in
 * the wrong state. */
static const char *di_answer_name(uint8_t a) {
    switch (a) {
    case SA2_DI_ANS_REFUSED: return "refused";
    case SA2_DI_ANS_NOOP:    return "no-op-success";
    case SA2_DI_ANS_GUEST:   return "from-guest";
    case SA2_DI_ANS_OBJECT:  return "fabricated-object";
    case SA2_DI_ANS_IDLE:    return "idle-device-state";
    case SA2_DI_ANS_INVALID: return "native-error";
    case SA2_DI_ANS_PROFILE: return "from-device-profile";
    case SA2_DI_ANS_BREAK:   return "named-break";
    case SA2_DI_ANS_HOST:    return "from-host-input";
    default:                 return "unknown";
    }
}
static const char *wbem_answer_name(uint8_t a) {
    switch (a) {
    case SA2_WBEM_ANS_REFUSED: return "refused";
    case SA2_WBEM_ANS_NOOP:    return "no-op-success";
    case SA2_WBEM_ANS_GUEST:   return "from-guest";
    case SA2_WBEM_ANS_OBJECT:  return "fabricated-object";
    case SA2_WBEM_ANS_PROFILE: return "from-device-profile";
    default:                   return "unknown";
    }
}

/* The answer classes of runtime/sa2_ddraw.h: the dsound words where they mean
 * the same thing, and `from-profile`, an answer the capture's device profile
 * recorded natively. */
static const char *dd_answer_name(uint8_t a) {
    switch (a) {
    case SA2_DD_ANS_REFUSED: return "refused";
    case SA2_DD_ANS_NOOP:    return "no-op-success";
    case SA2_DD_ANS_GUEST:   return "from-guest";
    case SA2_DD_ANS_PROFILE: return "from-profile";
    case SA2_DD_ANS_OBJECT:  return "fabricated-object";
    default:                 return "unknown";
    }
}

/* The total GetAvailableVidMem leaves in the game's local under the active
 * profile: the recorded one after a success, none (0 in the module's records)
 * after a failure. Compared, never printed: under a capture's profile it is a
 * host measurement. */
static uint32_t dd_profile_total(void) {
    return (sa2_dp_ddraw_vidmem_hr() & 0x80000000u) ? 0u : sa2_dp_ddraw_vidmem_total();
}

/* The Steam/COM answer classes. "no-op-success" and "no-such-stat" are both
 * things this runtime says without implementing anything, and they are printed
 * apart because they are not the same claim: one reports that a call did
 * nothing, the other reports that there is nothing to report. "invented" is
 * the label on the single Steam value this runtime does make up. */
static const char *iface_answer_name(uint8_t a) {
    switch (a) {
    case SA2_IFACE_ANS_REFUSED:  return "refused";
    case SA2_IFACE_ANS_IMPL:     return "no-op-success";
    case SA2_IFACE_ANS_SURVEY:   return "survey-placeholder";
    case SA2_IFACE_ANS_DECLINED: return "no-such-stat";
    case SA2_IFACE_ANS_RECORDED: return "recorded-and-discarded";
    case SA2_IFACE_ANS_NONE:     return "none-known";
    case SA2_IFACE_ANS_GUEST:    return "from-guest";
    case SA2_IFACE_ANS_INVENT:   return "invented";
    default:                     return "unknown";
    }
}

static void jstr(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c < 0x20 || c >= 0x7F) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* Generated by tools/s4/lift_extra.py; absent unless that file is linked. */
extern int sa2_s4_extra_register(void) __attribute__((weak));

/* ------------------------------------------------------------ stop-at */

static uint32_t g_stop_va;
static int      g_stopped;
static uint32_t g_stop_esp, g_stop_args[4];

static void *stop_native(void *s, uint32_t pc, void *m) {
    (void)m;
    g_stopped = 1;
    g_stop_esp = sa2_rt_get_gpr(s, SA2_ESP);
    for (int i = 0; i < 4; i++) g_stop_args[i] = peek32(g_stop_esp + 4u + 4u * (uint32_t)i);
    sa2_trap_note("reached the requested stop address %#010x; the run is ended here on purpose",
                  pc);
    sa2_trap_raise(SA2_TRAP_CONTROL_LEFT, pc);
}

/* --------------------------- what the guest parsed out of its own config
 *
 * The evidence for this slice cannot be a program counter. It has to be a
 * value the guest computed from bytes it read off the user's disk, and the
 * configuration object is exactly that: `FUN_00405310` loads
 * `C:\\SA2\\Config\\UserConfig.cfg`, walks the XML, and `sscanf("%d")`s eight
 * attributes into fixed offsets of its `this`.
 *
 * Finding `this` without instrumenting the guest: the same object is passed to
 * `FUN_004064B0`, which does `mov esi,eax; add esi,15Ch` and hands that to
 * `GetCurrentDirectoryA`. So the object's address is *in the import call log*,
 * as the second argument of the call whose return address is 0x004064c5. The
 * guest published its own `this` pointer and the probe reads it back.
 *
 * The host side of the comparison is an independent scan of the same file,
 * through the same file system, in this file. Neither side is a constant in
 * any source: change the file and both move. */
#define CFG_SELF_FROM_CWD_CALLER 0x004064c5u   /* the ret addr of 004064bf call [8730BCh] */
#define CFG_SELF_FIELD_OFFSET    0x15cu        /* 004064b3 add esi,15Ch */

typedef struct { const char *key; uint32_t offset; } cfg_field;
static const cfg_field CFG_FIELDS[] = {
    {"FullScreen",  0x04u},   /* 004053a4 "FullScreen" -> lea ecx,[ebx+4]    */
    {"Width",       0x08u},   /* 00405416 "Width"      -> lea edx,[ebx+8]    */
    {"Height",      0x0cu},   /* 0040543c "Height"     -> lea ecx,[ebx+0Ch]  */
    {"RefreshRate", 0x10u},   /* 00405462 "RefreshRate"-> lea edx,[ebx+10h]  */
    {"Language",    0x14u},   /* 00405488 "Language"   -> lea ecx,[ebx+14h]  */
    {"Display",     0x18u},   /* 004053ca "Display"    -> lea edx,[ebx+18h]  */
    {"Res",         0x1cu},   /* 004053f0 "Res"        -> lea ecx,[ebx+1Ch]  */
    {"Analytics",   0x20u},   /* 004054ae "Analytics"  -> lea edx,[ebx+20h]  */
};
#define CFG_FIELD_COUNT ((int)(sizeof CFG_FIELDS / sizeof CFG_FIELDS[0]))

/* `Key="123"` out of the same file, read through the same guest file system.
 * Returns 1 and the value, or 0 when the attribute is not there. */
static int cfg_from_file(const char *text, const char *key, long *out) {
    char pat[64];
    snprintf(pat, sizeof pat, "%s=\"", key);
    const char *p = strstr(text, pat);
    if (!p) return 0;
    p += strlen(pat);
    if (*p < '0' || *p > '9') {
        if (!(*p == '-' && p[1] >= '0' && p[1] <= '9')) return 0;
    }
    *out = strtol(p, NULL, 10);
    return 1;
}

/* The file, as bytes, into a caller-supplied buffer. Nothing here keeps them:
 * the buffer is local to `main`, the values extracted are integers, and no
 * byte of the installation reaches the JSON. */
static uint32_t read_config_text(const char *guest_path, char *buf, uint32_t cap) {
    int st = 0, existed = 0;
    uint32_t h = sa2_vfs_open(guest_path, SA2_VFS_ACCESS_READ, SA2_VFS_OPEN_EXISTING,
                              &st, &existed);
    if (!h) { buf[0] = 0; return 0; }
    uint32_t got = 0;
    if (sa2_vfs_read(h, buf, cap - 1, &got) != SA2_VFS_OK) got = 0;
    buf[got] = 0;
    sa2_vfs_close(h);
    return got;
}

/* ----------------------------- the escape, at the guest's own call site
 *
 * `harness/vfs_probe.c` drives the path mapper directly and shows that
 * `C:\\SA2\\..\\..\\etc\\passwd` classifies as an escape. That is the mapper's
 * answer, not the guest's: what matters is what happens when *lifted code*
 * calls an import with such a path, and the only way to know is to make the
 * call the way the guest makes it.
 *
 * So the string goes into guest memory, a guest stack is built with the
 * return address and the argument where `import_native` reads them, and
 * `sa2_call` dispatches the bound `SHLWAPI!PathFileExistsA` thunk. The run is
 * expected to end in a trap naming the path.
 *
 * It is run as a *pair*. The same machinery with a path inside the
 * installation must return 1 without trapping. Without the second half this
 * would be a check that passes for a runtime which refuses every path, which
 * is exactly the shape of check this project keeps finding it cannot fail. */
typedef struct {
    int      ran;
    int      refused;      /* the stub raised: the run ended somewhere else */
    int      answered;     /* the stub returned a value and cleaned the stack */
    uint32_t status;
    uint32_t exit_pc;
    uint32_t returned;
    uint32_t esp_delta;
    char     note[SA2_TRAP_NOTE_MAX];
} escape_result;

/* A native is not lifted code: it never writes EIP, so a *clean* return from
 * an import thunk leaves EIP at the thunk and `sa2_call` reports
 * SA2_TRAP_CONTROL_LEFT with `exit_pc` equal to the thunk. That is what
 * "answered" means here, and it is told apart from a refusal by two facts at
 * once: where the run ended, and whether stdcall's cleanup happened --
 * `import_native` only adds `4 + 4 * argc` to ESP after the stub returns, so a
 * stub that raised leaves ESP exactly where the caller set it. */

static void call_pathexists(const char *path, escape_result *out) {
    memset(out, 0, sizeof *out);
    const sa2_import_info *in = sa2_import_by_name("SHLWAPI.dll", "PathFileExistsA");
    if (!in || !in->thunk) return;
    /* A scratch page well clear of the guest's own stack and heap. */
    const uint32_t str = SA2_GUEST_SCRATCH_BASE + 0x2000u;
    const uint32_t len = (uint32_t)strlen(path) + 1u;
    if (sa2_poke(str, len, path) != SA2_OK) return;
    sa2_cpu_state_t st;
    sa2_cpu_state_reset(&st);
    const uint32_t esp = STACK_TOP - 0x2000u;
    st.gpr[SA2_ESP] = esp;
    sa2_call_place_return(&st, SA2_CALL_SENTINEL);   /* [esp]: the "caller" */
    if (sa2_poke(esp + 4u, 4, &str) != SA2_OK) return;  /* [esp+4]: the path argument */
    sa2_call_result_t r;
    sa2_call(in->thunk, &st, 30, &r);
    out->ran = 1;
    out->status = r.status;
    out->exit_pc = r.exit_pc;
    out->returned = st.gpr[SA2_EAX];
    out->esp_delta = st.gpr[SA2_ESP] - esp;
    out->answered = r.status == SA2_TRAP_CONTROL_LEFT && r.exit_pc == in->thunk
                    && out->esp_delta == 8u;     /* the return slot plus one argument */
    out->refused = !out->answered;
    snprintf(out->note, sizeof out->note, "%s", r.note);
}

/* The pair's paths, and the configuration file the probe reads beside the
 * guest, from the directory the guest answers (sa2_import_cwd): C:\SA2, or a
 * capture's own through SA2_GUEST_CWD, which is also the guest-side name of the
 * installation (runtime/sa2_files.h, Decision 1). They were literals under
 * C:\SA2, and a lifted run given a capture's directory read
 * `escape_refused_and_control_served` false for it (2026-09-24): its control,
 * C:\SA2\Config\UserConfig.cfg, is a path on another drive there, and the
 * file system refuses it as an escape, as it must.
 *
 * The control is the directory's Config\UserConfig.cfg. The absolute escape
 * climbs one level more than the directory is deep, so it always leaves the
 * drive's root: under C:\SA2 it is exactly the literal it replaced,
 * C:\SA2\..\..\etc\passwd, and under Z:\home\...\oracle\game it climbs eight.
 * Either way the '..' underflow refuses it, and with the underflow clamped
 * (tools/s4/fs_break.py) the root-prefix comparison does: the same two
 * barriers in both directories, where a fixed C:\SA2 path under a capture's
 * directory would meet only a third, the drive letter. The relative escape
 * needs no directory. */
static void self_test_paths(const char *cwd, char *escape, size_t escape_cap,
                            char *config, size_t config_cap) {
    char dir[SA2_VFS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", cwd);
    size_t n = strlen(dir);
    while (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) dir[--n] = 0;
    /* Components after the drive, as sa2_vfs_normalise splits them. */
    const char *p = (dir[0] && dir[1] == ':') ? dir + 2 : dir;
    int depth = 0;
    for (;;) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '\\' && *p != '/') p++;
        if (!(p - s == 1 && s[0] == '.')) depth++;
    }
    snprintf(config, config_cap, "%s\\Config\\UserConfig.cfg", dir);
    size_t used = (size_t)snprintf(escape, escape_cap, "%s", dir);
    for (int i = 0; i <= depth && used + 4 < escape_cap; i++)
        used += (size_t)snprintf(escape + used, escape_cap - used, "\\..");
    if (used < escape_cap) snprintf(escape + used, escape_cap - used, "\\etc\\passwd");
}

/* ------------------------------- the Steam stats surface, called directly
 *
 * The user's instruction for this slice is that Steam achievements and stats
 * are not worth implementing *and not worth lying about*. The first half is
 * easy to check -- the guest walks past the wall or it does not. The second
 * half is the one that kept going unchecked, and slice 8's eleventh defect
 * says why: a claim whose falsification is invisible is not yet a checked
 * claim. `SA2_STEAM_BREAK=fabricate` makes the declining queries invent an
 * answer, and on the guest's own path *nothing changes*, because the guest's
 * per-frame routine never reaches a query. A property that cannot be observed
 * to fail is not being checked.
 *
 * So the probe asks the questions the guest does not, the way the guest would:
 * it reads the fabricated object's vtable out of guest memory, builds a guest
 * stack, puts `this` in ECX, and calls the slot. The observable is a sentinel
 * the *probe* wrote into the out-parameter before the call. A declining query
 * leaves it alone; a fabricating one overwrites it. The sentinel is a value no
 * part of the runtime knows, so "the out-parameter still reads 0xA5A5A5A5" is
 * not a runtime counter agreeing with itself.
 *
 * It runs after the guest has stopped, so it cannot perturb the run it
 * measures, and every row it adds to the interface tally carries
 * SA2_CALL_SENTINEL as its caller. */
#define STEAM_PROBE_SENTINEL 0xA5A5A5A5u

typedef struct {
    int      ran;
    uint32_t obj, vtable;
    /* GetAchievement(name, &out) */
    int      get_ran;
    uint32_t get_returned, get_out_after, get_esp_delta;
    /* SetAchievement(name) */
    int      set_ran;
    uint32_t set_returned, set_esp_delta;
    /* StoreStats() */
    int      store_ran;
    uint32_t store_returned, store_esp_delta;
    char     note[SA2_TRAP_NOTE_MAX];
} steam_probe_result;

/* One __thiscall vtable call. `argv`/`argc` are the stack arguments; the
 * callee pops them, so `esp_delta` is 4 + 4*argc when the convention held. */
static int call_userstats(uint32_t obj, uint32_t slot, const uint32_t *argv, uint32_t argc,
                          uint32_t *returned, uint32_t *esp_delta, char *note, size_t nnote) {
    uint32_t vt = 0, thunk = 0;
    if (sa2_peek(obj, 4, &vt) != SA2_OK || !vt) return 0;
    if (sa2_peek(vt + 4u * slot, 4, &thunk) != SA2_OK || !thunk) return 0;
    sa2_cpu_state_t st;
    sa2_cpu_state_reset(&st);
    const uint32_t esp = STACK_TOP - 0x4000u;
    st.gpr[SA2_ESP] = esp;
    st.gpr[SA2_ECX] = obj;                      /* __thiscall: `this` in ECX */
    sa2_call_place_return(&st, SA2_CALL_SENTINEL);
    for (uint32_t k = 0; k < argc; k++)
        if (sa2_poke(esp + 4u + 4u * k, 4, &argv[k]) != SA2_OK) return 0;
    sa2_call_result_t r;
    sa2_call(thunk, &st, 30, &r);
    *returned = st.gpr[SA2_EAX];
    *esp_delta = st.gpr[SA2_ESP] - esp;
    if (note) snprintf(note, nnote, "%s", r.note);
    /* A native never writes EIP, so a clean return leaves the run at the thunk
     * with SA2_TRAP_CONTROL_LEFT -- the same reading `call_pathexists` uses.
     *
     * That reading alone is not enough here, and the difference matters: a
     * REFUSED slot also raises SA2_TRAP_CONTROL_LEFT *with the thunk as its
     * address*, so "ended at the thunk" is true of an answer and of a refusal
     * alike. The second fact tells them apart without depending on the
     * argument count: `iface_native` only moves ESP after the method body
     * returns, so a refusal leaves ESP exactly where this probe set it. */
    return r.status == SA2_TRAP_CONTROL_LEFT && r.exit_pc == thunk && *esp_delta != 0u;
}

static void run_steam_probe(steam_probe_result *out) {
    memset(out, 0, sizeof *out);
    out->obj = sa2_iface_get("ISteamUserStats");
    if (!out->obj) return;
    if (sa2_peek(out->obj, 4, &out->vtable) != SA2_OK) return;
    /* A name no achievement in any game has, placed in guest scratch, so the
     * name the runtime records can only have come through the guest pointer. */
    static const char probe_name[] = "SA2_RECOMP_PROBE_NO_SUCH_ACHIEVEMENT";
    const uint32_t name_va = SA2_GUEST_SCRATCH_BASE + 0x4000u;
    const uint32_t out_va  = SA2_GUEST_SCRATCH_BASE + 0x4100u;
    if (sa2_poke(name_va, sizeof probe_name, probe_name) != SA2_OK) return;
    const uint32_t sentinel = STEAM_PROBE_SENTINEL;
    if (sa2_poke(out_va, 4, &sentinel) != SA2_OK) return;
    out->ran = 1;

    uint32_t a2[2] = {name_va, out_va};
    out->get_ran = call_userstats(out->obj, 6, a2, 2, &out->get_returned,
                                  &out->get_esp_delta, out->note, sizeof out->note);
    out->get_out_after = peek32(out_va);

    uint32_t a1[1] = {name_va};
    out->set_ran = call_userstats(out->obj, 7, a1, 1, &out->set_returned,
                                  &out->set_esp_delta, NULL, 0);
    out->store_ran = call_userstats(out->obj, 10, NULL, 0, &out->store_returned,
                                    &out->store_esp_delta, NULL, 0);
}

/* -------------------------------------------- the cookie, recomputed */

/* Find the n'th logged call of an import by name; NULL when it never happened. */
static const sa2_import_call *nth_call(const char *name, uint32_t n) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < sa2_import_calls_logged(); i++) {
        const sa2_import_call *c = sa2_import_call_at(i);
        const sa2_import_info *in = sa2_import_of_index(c->index);
        if (in && !strcmp(in->name, name)) {
            if (seen++ == n) return c;
        }
    }
    return NULL;
}

/* __security_init_cookie, 0x007b5195, replayed in C over the values the
 * runtime handed the guest. Returns 1 when every input was available. */
static int cookie_expected(uint32_t *out, uint32_t *why_missing) {
    const sa2_import_call *ft  = nth_call("GetSystemTimeAsFileTime", 0);
    const sa2_import_call *pid = nth_call("GetCurrentProcessId", 0);
    const sa2_import_call *tid = nth_call("GetCurrentThreadId", 0);
    const sa2_import_call *tick = nth_call("GetTickCount", 0);
    const sa2_import_call *qpc = nth_call("QueryPerformanceCounter", 0);
    *why_missing = (!ft) | (!pid << 1) | (!tid << 2) | (!tick << 3) | (!qpc << 4);
    if (*why_missing) return 0;
    /* The values the stubs wrote, taken from the call record rather than from
     * the guest stack slots they were written into: by the end of the run the
     * CRT has reused those slots, so reading them afterwards recomputes the
     * right arithmetic over the wrong inputs. */
    if (ft->outc < 2 || qpc->outc < 2) { *why_missing = 0x20u; return 0; }
    /* 007b51cc GetSystemTimeAsFileTime(&ebp-8): [ebp-8] low, [ebp-4] high.
     *          mov esi,[ebp-4]; xor esi,[ebp-8]  ->  high ^ low  */
    uint32_t esi = ft->out[1] ^ ft->out[0];
    esi ^= pid->ret;                    /* 007b51de xor esi,eax */
    esi ^= tid->ret;                    /* 007b51e6 */
    esi ^= tick->ret;                   /* 007b51ee */
    /* 007b51f4 QueryPerformanceCounter(&ebp-0x10); [ebp-0x10] low, [ebp-0xc] high
     *          mov eax,[ebp-0Ch]; xor eax,[ebp-10h]; xor esi,eax  */
    esi ^= qpc->out[1] ^ qpc->out[0];
    if (esi == COOKIE_DEFAULT) esi = COOKIE_DEFAULT + 1u;      /* 007b5206 */
    else if (!(esi & 0xffff0000u)) esi |= esi << 16;           /* 007b520d..007b5216 */
    *out = esi;
    return 1;
}


/* ------------------------------------------- driving the guest's main loop
 *
 * WinMain's message loop is at 0x00401da4 and WinMain does not reach it in
 * this build. The path is
 *
 *     00401d40  call 004019D0h   ; register the class, create the window
 *     00401d49  call 004010D0h   ; initialise -- Steam, then Direct3D
 *     00401d53  je   00401E48h   ; on false, WinMain exits without pumping
 *
 * and `FUN_004010D0` reaches `IDirect3D9::CreateDevice`, which is S5's work.
 * So the loop is behind the presentation boundary, not in front of it.
 *
 * What is still measurable is the loop itself, because the loop is guest bytes
 * and this project already lifts an address as its own module when no map
 * entry reaches it (`tools/s4/lift_extra.py`). 0x00401da4 is lifted that way
 * and driven here with the two registers WinMain loads immediately before it:
 *
 *     00401d98  mov ebx,[873354h]   ; PeekMessageW
 *     00401d9e  mov edi,[873328h]   ; GetMessageW
 *
 * This is a **mid-function window entry** and is labelled as one everywhere it
 * is reported. It is not WinMain running. What it does establish is what a
 * pump can establish: how many times the guest's own loop body executes, what
 * it does in each, and that the loop leaves the way a program leaves rather
 * than the way a trap leaves.
 *
 * The frame is WinMain's, rebuilt:
 *
 *     push ebp / mov ebp,esp / and esp,~7 / sub esp,24h / push ebx,esi,edi
 *
 * so the loop's ESP is (aligned ESP - 0x24 - 0xC), its MSG lives at ESP+0x14,
 * its mutex handle at ESP+0x10, and the exit path's `mov esp,ebp / pop ebp /
 * ret 10h` lands on the sentinel at [EBP+4]. */
#define LOOP_HEAD_VA 0x00401da4u

typedef struct {
    int      ran, lifted;
    uint32_t status, exit_pc, esp, ebp;
    uint32_t polls, dispatches, wndproc_calls, gets;
    uint32_t quit_posted, quit_by_host, quit_by_guest, quit_delivered;
    uint32_t postquit_caller;          /* the guest address that called PostQuitMessage */
    char     note[SA2_TRAP_NOTE_MAX];
} loop_result;

static void drive_loop(loop_result *out, uint32_t timeout, uint32_t mutex_handle,
                       uint32_t hinstance) {
    memset(out, 0, sizeof *out);
    out->lifted = sa2_ft_find(LOOP_HEAD_VA) != NULL;
    if (!out->lifted) return;

    const uint32_t ebp = STACK_TOP - 0x8000u;
    const uint32_t aligned = (ebp - 0x40u) & ~7u;
    const uint32_t esp = aligned - 0x30u;
    uint32_t zero = 0, sentinel = SA2_CALL_SENTINEL;
    /* WinMain's own frame, as its prologue left it. */
    sa2_poke(ebp, 4, &zero);                     /* the saved EBP */
    sa2_poke(ebp + 4u, 4, &sentinel);            /* the return address */
    sa2_poke(ebp + 8u, 4, &hinstance);           /* WinMain's hInstance */
    sa2_poke(ebp + 0x0cu, 4, &zero);
    sa2_poke(ebp + 0x10u, 4, &zero);             /* lpCmdLine */
    sa2_poke(ebp + 0x14u, 4, &zero);             /* nCmdShow */
    for (uint32_t a = esp; a < aligned + 0x30u; a += 4u) sa2_poke(a, 4, &zero);
    /* [esp+10h] is the single-instance mutex the shutdown path closes; giving
     * it the handle the guest's own CreateMutexW returned is what makes that
     * path a real close rather than a CloseHandle(0) the runtime would refuse. */
    sa2_poke(esp + 0x10u, 4, &mutex_handle);

    const sa2_import_info *peek = sa2_import_by_name("USER32.dll", "PeekMessageW");
    const sa2_import_info *get  = sa2_import_by_name("USER32.dll", "GetMessageW");
    if (!peek || !get) return;

    sa2_cpu_state_t st;
    sa2_cpu_state_reset(&st);
    st.gpr[SA2_ESP] = esp;
    st.gpr[SA2_EBP] = ebp;
    st.gpr[SA2_EBX] = peek->thunk;      /* 00401d98 mov ebx,[873354h] */
    st.gpr[SA2_EDI] = get->thunk;       /* 00401d9e mov edi,[873328h] */

    sa2_call_result_t r;
    sa2_call(LOOP_HEAD_VA, &st, timeout, &r);
    out->ran = 1;
    out->status = r.status;
    out->exit_pc = r.exit_pc;
    out->esp = st.gpr[SA2_ESP];
    out->ebp = ebp;
    snprintf(out->note, sizeof out->note, "%s", r.note);

    sa2_wnd_stats w;
    sa2_window_stats(&w);
    out->polls = w.polls;
    out->gets = w.gets;
    out->dispatches = w.dispatches;
    out->wndproc_calls = w.wndproc_calls;
    out->quit_posted = w.quit_posted;
    out->quit_by_host = w.quit_by_host;
    out->quit_by_guest = w.quit_by_guest;
    out->quit_delivered = w.quit_delivered;
    for (uint32_t i = 0; i < sa2_window_log_count(); i++) {
        const sa2_wnd_event *e = sa2_window_log_at(i);
        if (e && !strcmp(e->what, "PostQuitMessage")) { out->postquit_caller = e->caller; break; }
    }
}

/* ------------------------------- the blocking GetMessageW, at the call site
 *
 * `sa2_window_get` refuses an empty queue rather than waiting, and the
 * counter that says so was reported by every run and incremented by none --
 * which by this project's own rule is not a check. It is exercised here the
 * way `call_pathexists` exercises the escape: the bound USER32!GetMessageW
 * thunk is dispatched from a synthetic guest frame, once with an empty queue
 * and once with a message in it. One must refuse and the other must answer,
 * because a runtime that refused every GetMessageW would satisfy either half
 * alone. */
typedef struct {
    int      ran_empty, refused_empty, ran_ready, answered_ready;
    uint32_t status_empty, status_ready, returned_ready, esp_delta_ready;
    uint32_t refusals_counted;
    char     note[SA2_TRAP_NOTE_MAX];
} block_result;

static void call_getmessage(uint32_t msg_buf, escape_result *out) {
    memset(out, 0, sizeof *out);
    const sa2_import_info *in = sa2_import_by_name("USER32.dll", "GetMessageW");
    if (!in || !in->thunk) return;
    sa2_cpu_state_t st;
    sa2_cpu_state_reset(&st);
    const uint32_t esp = STACK_TOP - 0x3000u;
    st.gpr[SA2_ESP] = esp;
    sa2_call_place_return(&st, SA2_CALL_SENTINEL);
    uint32_t z = 0;
    if (sa2_poke(esp + 4u, 4, &msg_buf) != SA2_OK) return;     /* lpMsg */
    for (uint32_t k = 2; k <= 4; k++)
        if (sa2_poke(esp + 4u * k, 4, &z) != SA2_OK) return;    /* hWnd, min, max */
    sa2_call_result_t r;
    sa2_call(in->thunk, &st, 30, &r);
    out->ran = 1;
    out->status = r.status;
    out->exit_pc = r.exit_pc;
    out->returned = st.gpr[SA2_EAX];
    out->esp_delta = st.gpr[SA2_ESP] - esp;
    out->answered = r.status == SA2_TRAP_CONTROL_LEFT && r.exit_pc == in->thunk
                    && out->esp_delta == 20u;    /* the return slot plus four arguments */
    out->refused = !out->answered;
    snprintf(out->note, sizeof out->note, "%s", r.note);
}

int main(int argc, char **argv) {
    const char *pe = NULL, *json_path = NULL, *dump_path = NULL;
    uint32_t va = DEFAULT_VA, timeout = 120;
    int require_stop = 0, require_files = 0, mb_policy = SA2_MB_REACHED;
    int drive = 0, require_loop = 0, require_winmain_loop = 0, require_threads = 0;
    /* Two escapes, not one. They are refused by *different* barriers: the
     * absolute form by the '..' underflow first and, when the underflow is
     * clamped, by the root-prefix comparison after normalisation; the relative
     * form only by the underflow (runtime/sa2_files.c). A single
     * path cannot tell which barrier is holding, and `tools/s4/fs_break.py`
     * found exactly that: with the underflow clamped -- the classic traversal
     * bug -- the absolute path was still refused by the other check, and an
     * assertion written around it alone reported no change. */
    /* The absolute escape, the control and the configuration file default to
     * the guest's own directory's (self_test_paths, once the environment is
     * read); under C:\SA2 they are C:\SA2\..\..\etc\passwd and
     * C:\SA2\Config\UserConfig.cfg, the literals they replaced. */
    const char *escape_path = NULL;
    const char *escape_path2 = "..\\..\\etc\\passwd";
    const char *control_path = NULL;
    const char *config_path = NULL;
    const char *stop_import_name = NULL;
    uint32_t stop_import_after = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--va") && i + 1 < argc) va = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--stop-at") && i + 1 < argc) g_stop_va = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--require-stop")) require_stop = 1;
        else if (!strcmp(argv[i], "--require-files")) require_files = 1;
        else if (!strcmp(argv[i], "--drive-loop")) drive = 1;
        else if (!strcmp(argv[i], "--require-loop")) { drive = 1; require_loop = 1; }
        /* WinMain reaching its *own* loop. Mutually exclusive with the
         * mid-function drive above by construction: that one runs after the
         * entry-point call returns, and this one is a property of that call. */
        else if (!strcmp(argv[i], "--require-winmain-loop")) require_winmain_loop = 1;
        else if (!strcmp(argv[i], "--require-threads")) require_threads = 1;
        else if (!strcmp(argv[i], "--escape-path") && i + 1 < argc) escape_path = argv[++i];
        else if (!strcmp(argv[i], "--escape-path2") && i + 1 < argc) escape_path2 = argv[++i];
        else if (!strcmp(argv[i], "--control-path") && i + 1 < argc) control_path = argv[++i];
        else if (!strcmp(argv[i], "--config-path") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--stop-import") && i + 1 < argc) stop_import_name = argv[++i];
        else if (!strcmp(argv[i], "--stop-import-after") && i + 1 < argc)
            stop_import_after = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--mb-policy") && i + 1 < argc) mb_policy = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-imports") && i + 1 < argc) dump_path = argv[++i];
        else if (!pe) pe = argv[i];
    }
    if (!pe) { fprintf(stderr, "usage: crt_probe <sonic2app-unpacked.exe> [--va VA]\n"); return 2; }

    /* Room for the 251 imports, the GetProcAddress rows, and every block
     * `tools/s4/lift_extra.py` lifted: dispatch targets the whole-image lift
     * produced no module for, a few hundred to a couple of thousand of them
     * depending on which sweeps ran. A table that refuses a registration says
     * so rather than the run failing somewhere else. */
    /* The offset the CRT stashes the caller's thread routine at, declared by
     * the probe because it is knowledge about this image and not about
     * scheduling (see sa2_sched_set_param_peek). */
    sa2_sched_set_param_peek(CRT_PTD_START_ROUTINE);
    if (sa2_runtime_init(pe, 4096) != SA2_OK) {
        fprintf(stderr, "crt_probe: sa2_runtime_init(%s) failed\n", pe);
        return 2;
    }
    /* The poison tier (runtime/guest_memory.h): SA2_GUEST_POISON=1 traps on a read nothing
     * wrote, =2 counts such reads. Read after init, as harness/plan_driver.c does; with the
     * variable unset nothing changes. Which executable init loaded, and the shipped-executable
     * overlay's identity and counts, go into the run record (`exe`). */
    sa2_guest_poison_from_env();
    /* The missing-block edge table, and the policy that reads it.
     *
     * Only the S2 corpus build ever emitted this table, so at image scale it is
     * empty and every control-flow edge that leaves a module's lifted range is
     * SA2_MB_EDGE_UNKNOWN -- which traps under every policy. The image's own
     * entry point ends in `jmp 007A7E9Eh`, a tail call into __tmainCRTStartup,
     * so with no table the run stops on the instruction after the cookie is
     * computed. `tools/s4/mb_edges.py` emits the table for the whole image and
     * `--mb-policy` chooses what the runtime does with it; SA2_MB_REACHED (2),
     * "dispatch only a jmp edge", is the only policy a whole program can run
     * under and it is the one with evidence behind it (remill_runtime.h).
     * SA2_MISSING_BLOCK_DISPATCH in the environment still wins, so the before
     * column is one variable and not a rebuild. */
    /* Blocks the whole-image lift could not reach from any map entry, lifted
     * separately by `tools/s4/lift_extra.py` and registered here -- after the
     * generated table, so the row wins the address. Weak, so a driver linked
     * without that generated file still builds and reports 0. */
    const int extra_rows = sa2_s4_extra_register ? sa2_s4_extra_register() : 0;
    if (stop_import_name) sa2_import_stop_on(stop_import_name, stop_import_after);
    sa2_cpu_model_from_env();
    sa2_mb_register_generated();
    sa2_missing_block_dispatch = mb_policy;
    sa2_mb_policy_from_env();
    const uint32_t mb_edges = sa2_mb_edge_count();

    /* Counted after every other registration, so the binding's own arithmetic is
     * the only thing this compares. */
    const uint32_t rows_before = sa2_ft_count();
    sa2_imports_config_from_env();
    static char escape_default[SA2_VFS_PATH_MAX], config_default[SA2_VFS_PATH_MAX];
    self_test_paths(sa2_import_cwd(), escape_default, sizeof escape_default,
                    config_default, sizeof config_default);
    if (!escape_path) escape_path = escape_default;
    if (!control_path) control_path = config_default;
    if (!config_path) config_path = config_default;
    const int bind_rc = sa2_imports_bind();
    const uint32_t bound = sa2_import_count();
    const uint32_t rows_after = sa2_ft_count();

    /* The stop native is registered *after* the generated table, so it wins the
     * address. `sa2_ft_add` documents that a later registration wins; if that
     * ever changes, `stop_registered_over_lifted` below turns false. */
    int stop_registered = 0, stop_was_lifted = 0;
    if (g_stop_va) {
        const sa2_ft_entry *prev = sa2_ft_find(g_stop_va);
        stop_was_lifted = prev && prev->kind == SA2_FN_LIFTED;
        stop_registered = sa2_ft_add(g_stop_va, stop_native, "stop", SA2_FN_NATIVE) >= 0;
        const sa2_ft_entry *now = sa2_ft_find(g_stop_va);
        stop_registered = stop_registered && now && now->fn == stop_native;
    }

    uint32_t slots_holding_thunk = 0, slots_still_unbound = 0;
    for (uint32_t i = 0; i < bound; i++) {
        const sa2_import_info *in = sa2_import_at(i);
        if (peek32(in->slot) == in->thunk) slots_holding_thunk++; else slots_still_unbound++;
    }

    /* Before: the CRT globals as the file has them, so "the guest wrote this"
     * is a difference and not a reading. */
    const uint32_t cookie0 = peek32(VA_COOKIE), heap0 = peek32(VA_CRTHEAP),
                   active0 = peek32(VA_ACTIVE_HEAP);

    sa2_cpu_state_t st;
    sa2_cpu_state_reset(&st);
    /* The process starts where Windows and Wine start every thread: control
     * word 0x027F, double precision. sa2_cpu_state_reset's 0x007F is Direct3D's
     * setting, which the recording device now applies when the game creates its
     * device, as the native game gets it (2026-09-23). */
    st.x87_cw = 0x027F;
    st.gpr[SA2_ESP] = STACK_TOP;
    sa2_call_place_return(&st, SA2_CALL_SENTINEL);

    sa2_call_result_t res;
    const int rc = sa2_call(va, &st, timeout, &res);
    /* The guest's run has ended: no frame comes after it, so the stall watch has nothing left to
     * watch, and its thread is ended before the process's exit starts (sa2_stall.h). */
    sa2_stall_stop();
    /* The guest has stopped. Every import call from here on is this harness's
     * own control probe, and SA2_IMPORT_BUDGET exists to stop the *guest* at
     * the same call in every run, not to refuse the checks that run after it.
     * Without this, every budgeted run reported
     * `escape_refused_and_control_served` false with `control_esp_delta` 0 --
     * a check reading as broken because its control could not run. */
    sa2_import_budget_release();

    const sa2_import_info *stop_import = sa2_import_for_thunk(res.exit_pc);
    uint32_t stop_stack[STACK_WORDS];
    for (int i = 0; i < STACK_WORDS; i++) stop_stack[i] = peek32(st.gpr[SA2_ESP] + 4u * (uint32_t)i);

    /* --- SA2_PEEK: guest dwords read out of the *stopped* image ----------
     *
     * A bounds-check fault names the address the access was to and nothing
     * about the object the guest was walking. The dispatch ring says which
     * guest entry points ran; this says what the globals and objects they
     * built actually hold when the run stops. It is read-only, off unless the
     * variable is set, and it is the run's own memory rather than a static
     * read of the file, so a zero here means the guest left a zero.
     *
     * SA2_PEEK is a comma-separated list of `name=0xADDR` or bare `0xADDR`.
     * `sa2_peek` refuses an address outside the guest space, and a refusal is
     * reported as `null` rather than as a value, because 0xDEADBEEF is a value
     * a guest could have written. */
    peek_capture();
    peek_find();
    peek_dump();

    /* --- the observables ------------------------------------------------ */
    const uint32_t cookie = peek32(VA_COOKIE), cookie_c = peek32(VA_COOKIE_COMPL);
    uint32_t cookie_want = 0, cookie_missing = 0;
    const int cookie_computable = cookie_expected(&cookie_want, &cookie_missing);
    const int cookie_matches = cookie_computable && cookie == cookie_want
                               && cookie_c == ~cookie_want;

    const uint32_t crtheap = peek32(VA_CRTHEAP), active_heap = peek32(VA_ACTIVE_HEAP);
    const sa2_import_call *hc = nth_call("HeapCreate", 0);
    const int heap_initialised = hc && hc->ret && crtheap == hc->ret && active_heap == 1u;

    /* WinMain is reached by a *direct* call at 0x007a7fac, which the lift turns
     * into a direct LLVM call to sub_401b80. Direct calls do not go through the
     * function table (S3), so the --stop-at native registered over the address
     * never sees it and "reached" stays false. The evidence that WinMain ran is
     * therefore guest-produced rather than instrumented: the return address the
     * guest's own `call dword [873078h]` pushed, read off the guest stack when
     * the import dispatched, is inside WinMain's body.
     *
     * The guest then computes something from a value this run chose:
     *
     *     00401bc8  mov eax,[ebp+10h]        ; WinMain's lpCmdLine
     *     00401bcb  push 89DDE8h             ; "-r"
     *     00401bd1  call dword [87307Ch]     ; lstrcmpA
     *     00401bdd  neg eax / sbb eax,eax / inc eax
     *     00401be2  mov [1933F6Ch],eax       ; 1 when the tail is exactly "-r"
     *
     * lpCmdLine is the guest's own parse of the command line the runtime
     * published, so SA2_GUEST_CMDLINE moves both sides of that comparison. */
    uint32_t winmain_calls = 0, winmain_first_caller = 0;
    for (uint32_t i = 0; i < sa2_import_calls_logged(); i++) {
        const sa2_import_call *c = sa2_import_call_at(i);
        if (c->caller >= WINMAIN_VA && c->caller < WINMAIN_END) {
            if (!winmain_calls) winmain_first_caller = c->caller;
            winmain_calls++;
        }
    }
    const int reached_winmain = winmain_calls > 0;
    const uint32_t dash_r = peek32(VA_DASH_R_FLAG);
    const uint32_t dash_r_want = strcmp(sa2_import_cmdline_tail(), "-r") == 0 ? 1u : 0u;
    const sa2_import_call *mutex = nth_call("CreateMutexW", 0);
    const int guest_parsed_the_command_line = reached_winmain && nth_call("lstrcmpA", 0)
                                              && dash_r == dash_r_want;

    /* CPUID leaf 1 EDX bit 26 reaches the guest: __sse2_available_init at
     * 0x007ae71f tests it, runs its own OS-support check, and stores the
     * result. The guest's value must agree with what the model reported, which
     * is how the processor model is shown to be load bearing rather than
     * merely answered. */
    const uint32_t sse2_avail = peek32(VA_SSE2_AVAIL);
    const uint32_t sse2_want = (sa2_cpuid_edx1() & (1u << 26)) ? 1u : 0u;
    const int guest_saw_the_cpu_model = sse2_avail == sse2_want;

    sa2_heap_stats_t hs;
    sa2_heap_stats(&hs);
    /* The allocator's own counters against a walk of the arena the guest can
     * see. They disagree only if a guest write went through a block header. */
    /* `ready` is in this and the byte totals are compared, not only the block
     * counts. Without `ready` the assertion was `0 == 0` for every run that
     * never created a heap -- three of the published rows -- which made it the
     * one member of the ok conjunction with no demonstrated falsifier. The
     * byte comparison is what catches a header whose size changed while its
     * in-use bit stayed set, which a count comparison cannot see. */
    const int heap_consistent = hs.ready && hs.walk_blocks != 0xffffffffu
                                && hs.walk_inuse == hs.live_blocks
                                && hs.walk_inuse_bytes == hs.live_bytes;

    char cmdline[512], environ0[512];
    const uint32_t cmdline_p = peek32(VA_CMDLINE), environ_p = peek32(VA_ENVIRON);
    gstr(cmdline_p, cmdline, sizeof cmdline);
    gstr(environ_p, environ0, sizeof environ0);

    /* --- the guest's file system ---------------------------------------- */
    sa2_vfs_stats_t vfs;
    sa2_vfs_stats(&vfs);
    const uint32_t vfs_log_main = sa2_vfs_log_count();

    /* The guest's own seek-to-end, against the size the host reported for the
     * same descriptor at the moment it was opened. The guest asks the file how
     * long it is; the host asks the file system; a runtime that fabricated a
     * length would have to fabricate the same one twice. Opens and their seeks
     * are strictly interleaved on this path, so the k-th FILE_END seek belongs
     * to the k-th open -- and the count check below is what notices if that
     * ever stops being true. */
    uint32_t seek_end_n = 0, seek_end_agree = 0, seek_end_nonzero = 0;
    uint32_t seek_end_got[8], seek_end_want[8];
    memset(seek_end_got, 0, sizeof seek_end_got);
    memset(seek_end_want, 0, sizeof seek_end_want);
    for (uint32_t i = 0; i < sa2_import_calls_logged() && seek_end_n < 8; i++) {
        const sa2_import_call *c = sa2_import_call_at(i);
        const sa2_import_info *in = sa2_import_of_index(c->index);
        if (!in || strcmp(in->name, "SetFilePointer") || c->argc < 4 || c->arg[3] != 2u)
            continue;                                   /* FILE_END only */
        const sa2_vfs_open_record *r = sa2_vfs_opened_at(seek_end_n);
        seek_end_got[seek_end_n] = c->ret;
        seek_end_want[seek_end_n] = r ? (uint32_t)r->size : 0u;
        if (r && c->ret == (uint32_t)r->size) seek_end_agree++;
        if (c->ret) seek_end_nonzero++;
        seek_end_n++;
    }

    /* The configuration object: `this` out of the guest's own import argument,
     * the values out of guest memory, the expected values out of the file. */
    uint32_t cfg_self = 0;
    for (uint32_t i = 0; i < sa2_import_calls_logged() && !cfg_self; i++) {
        const sa2_import_call *c = sa2_import_call_at(i);
        const sa2_import_info *in = sa2_import_of_index(c->index);
        if (in && !strcmp(in->name, "GetCurrentDirectoryA")
            && c->caller == CFG_SELF_FROM_CWD_CALLER && c->arg[1] > CFG_SELF_FIELD_OFFSET)
            cfg_self = c->arg[1] - CFG_SELF_FIELD_OFFSET;
    }
    /* The probe's own read below is an open too, so the guest's list is frozen
     * here: a report that included the prober's file would be reporting itself. */
    const uint32_t vfs_opened_main = sa2_vfs_opened_count();
    char cfg_text[8192];
    const uint32_t cfg_bytes = read_config_text(config_path, cfg_text, sizeof cfg_text);
    uint32_t cfg_guest[CFG_FIELD_COUNT];
    long     cfg_host[CFG_FIELD_COUNT];
    int      cfg_have[CFG_FIELD_COUNT];
    int cfg_compared = 0, cfg_agree = 0, cfg_nonzero = 0;
    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        cfg_guest[i] = cfg_self ? peek32(cfg_self + CFG_FIELDS[i].offset) : 0u;
        cfg_host[i] = 0;
        cfg_have[i] = cfg_from_file(cfg_text, CFG_FIELDS[i].key, &cfg_host[i]);
        if (!cfg_self || !cfg_have[i]) continue;
        cfg_compared++;
        if ((long)cfg_guest[i] == cfg_host[i]) cfg_agree++;
        /* A pair that is 0 == 0 proves nothing: it is counted, but the verdict
         * below also requires several pairs that are not. */
        if (cfg_host[i] != 0) cfg_nonzero++;
    }
    const int guest_parsed_its_config = cfg_self != 0 && cfg_bytes > 0
                                        && cfg_compared >= CFG_FIELD_COUNT
                                        && cfg_agree == cfg_compared
                                        && cfg_nonzero >= 3;
    const int guest_sized_its_files = seek_end_n >= 2 && seek_end_agree == seek_end_n
                                      && seek_end_nonzero == seek_end_n;

    /* Everything above belongs to the guest's own run. The escape pair below
     * makes two more import calls, so the counts the run produced are frozen
     * here rather than read again at print time. */
    const uint32_t calls_main = sa2_import_calls();
    const uint32_t logged_main = sa2_import_calls_logged();

    escape_result esc, esc2, esc_ctl;
    call_pathexists(escape_path, &esc);
    call_pathexists(escape_path2, &esc2);
    call_pathexists(control_path, &esc_ctl);
    /* One side refuses, the other is served, through the same thunk and the
     * same dispatch. Either half alone would pass for a runtime that answered
     * every path the same way. */
    const int escape_refused = esc.ran && esc.refused && esc.status == SA2_TRAP_FAULT
                               && strstr(esc.note, "refuses this path") != NULL
                               && esc2.ran && esc2.refused && esc2.status == SA2_TRAP_FAULT
                               && strstr(esc2.note, "refuses this path") != NULL
                               && esc_ctl.ran && esc_ctl.answered && esc_ctl.returned == 1u;

    /* The window the guest built, frozen before the loop driver runs so the
     * two are never confused. */
    sa2_wnd_stats wnd_before;
    sa2_window_stats(&wnd_before);
    const uint32_t wnd_log_before = sa2_window_log_count();
    const sa2_wnd_class  *cls0 = sa2_window_class_at(0);
    const sa2_wnd_window *win0 = sa2_window_at(0);
    /* The window's extents, recomputed from two inputs the runtime did not
     * choose and one it did, exactly as FUN_004019D0 computes them:
     *
     *   00401a9a  lea ebx,[ebx+eax*2]    ; width  += 2 * SM_CXFRAME
     *   00401aa1  add eax,[esp+40h]      ; height += SM_CYCAPTION
     *   00401aa5  lea ecx,[eax+edi*2]    ;         += 2 * SM_CYFRAME
     *   00401ac1  sub edi,eax            ; x = (SM_CXSCREEN>>1) - (width>>1)
     *   00401ad7  sub ecx,eax            ; y = (SM_CYSCREEN>>1) - (height>>1)
     *   00401ad9  jns .. / xor ecx,ecx   ; ... clamped at zero, and x is not
     *
     * Width, Height and the full-screen flag come from the guest's own parse
     * of its configuration file; the five metrics come from the runtime and
     * are read back out of the call log rather than from the source that
     * produced them. Three things move it independently: the overlay's
     * Width/Height, the overlay's FullScreen, and SA2_WND_SCREEN. A runtime
     * that recorded a window it invented cannot satisfy it. */
    uint32_t sm[64];
    memset(sm, 0, sizeof sm);
    for (uint32_t i = 0; i < sa2_window_log_count(); i++) {
        const sa2_wnd_event *e = sa2_window_log_at(i);
        if (e && !strcmp(e->what, "GetSystemMetrics") && e->a[0] < 64u) sm[e->a[0]] = e->a[1];
    }
    const int cfg_fullscreen = cfg_self ? (int)peek32(cfg_self + 0x04u) != 0 : 0;
    const long cfg_w = cfg_host[1], cfg_h = cfg_host[2];    /* Width, Height, from the file */
    long want_w = cfg_w, want_h = cfg_h;
    if (!cfg_fullscreen) {
        want_w = cfg_w + 2 * (long)sm[32];
        want_h = cfg_h + 2 * (long)sm[33] + (long)sm[4];
    }
    long want_x = ((long)sm[0] >> 1) - (want_w >> 1);
    long want_y = ((long)sm[1] >> 1) - (want_h >> 1);
    if (want_y < 0) want_y = 0;
    const int guest_sized_its_window =
        win0 && cfg_w > 0 && cfg_h > 0 && sm[0] > 0 && sm[1] > 0
        && (long)win0->w == want_w && (long)win0->h == want_h
        && (long)win0->x == want_x && (long)win0->y == want_y
        /* WS_POPUP when the configuration says full screen, WS_CAPTION|
         * WS_SYSMENU when it does not: 0x00401a7a..0x00401a86's arithmetic. */
        && win0->style == (cfg_fullscreen ? 0x80000000u : 0x00c80000u);
    /* The window procedure is a lifted guest function and not a runtime
     * native, which is what makes DispatchMessageW's callback guest code. */
    const sa2_ft_entry *wp = cls0 ? sa2_ft_find(cls0->wndproc) : NULL;
    const int wndproc_is_lifted = wp && wp->kind == SA2_FN_LIFTED;
    const int guest_built_its_window =
        sa2_window_class_count() == 1u && sa2_window_count() == 1u
        && cls0 && cls0->wndproc != 0u && cls0->atom >= SA2_WND_ATOM_FIRST
        && win0 && win0->hwnd != 0u && win0->w > 0 && win0->h > 0
        && win0->visible != 0u && win0->wndproc == cls0->wndproc
        && !strcmp(win0->class_name, cls0->name)
        && wndproc_is_lifted && guest_sized_its_window;

    loop_result loop;
    if (drive) drive_loop(&loop, timeout, mutex ? mutex->ret : 0u, SA2_GUEST_IMAGE_BASE);
    else memset(&loop, 0, sizeof loop);

    /* The blocking guard, refused then served. Run after the loop so the
     * queue is empty and nothing the loop measured can be disturbed: these
     * two calls are counted separately below. */
    const uint32_t msg_buf = SA2_GUEST_SCRATCH_BASE + 0x3000u;
    escape_result get_empty, get_ready;
    memset(&get_empty, 0, sizeof get_empty);
    memset(&get_ready, 0, sizeof get_ready);
    uint32_t block_refusals_before = 0, block_refusals_after = 0;
    if (drive) {
        sa2_wnd_stats t;
        sa2_window_stats(&t);
        block_refusals_before = t.blocking_get_refused;
        call_getmessage(msg_buf, &get_empty);
        sa2_window_stats(&t);
        block_refusals_after = t.blocking_get_refused;
        sa2_window_post(sa2_window_main(), SA2_WM_KEYDOWN, 0x20u, 0);
        call_getmessage(msg_buf, &get_ready);
    }
    const int blocking_get_refused_and_ready_served =
        get_empty.ran && get_empty.refused && get_empty.status == SA2_TRAP_CONTROL_LEFT
        && strstr(get_empty.note, "block forever") != NULL
        && block_refusals_after == block_refusals_before + 1u
        && get_ready.ran && get_ready.answered && get_ready.returned == 1u
        && peek32(msg_buf + 4u) == SA2_WM_KEYDOWN;

    sa2_wnd_stats pump;
    sa2_window_stats(&pump);
    /* The guest's own Steam calls, snapshotted before the honesty probe adds
     * any of its own. The probe's rows are still reported; they are told apart
     * by their caller, which is SA2_CALL_SENTINEL and cannot be a guest
     * address. */
    sa2_iface_stats ifs;
    sa2_iface_stats_get(&ifs);
    steam_probe_result steam;
    run_steam_probe(&steam);
    sa2_iface_stats ifs_after;
    sa2_iface_stats_get(&ifs_after);
    sa2_d3d9_stats d3d;
    sa2_d3d9_stats_get(&d3d);

    /* ------------------------------- WinMain running its own message loop
     *
     * WinMain's path to its loop is
     *
     *     00401d49  call 004010D0h            ; init
     *     00401d51  test al,al
     *     00401d53  je   00401E48h            ; on false, WinMain exits
     *     00401d5e  call [873080h]            ; SetThreadExecutionState(80000003h)
     *     00401d77  call [873350h]            ; SystemParametersInfoW(10h, ...)
     *     00401da4  ...                       ; the loop head
     *
     * so three things distinguish "WinMain ran its own loop" from the previous
     * slice's synthetic entry at 0x00401da4, and none of them is a counter the
     * runtime keeps for its own sake:
     *
     *  - `SetThreadExecutionState` was called, with 0x80000003, returning to
     *    0x00401d64. That instruction is *after* the branch on init's result,
     *    so the call cannot have happened unless FUN_004010D0 returned true --
     *    which it cannot do without a device.
     *  - `SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE)` returning to
     *    0x00401d79, likewise between the branch and the loop.
     *  - every poll came back to 0x00401db3, the address after `call ebx` at
     *    0x00401db1. The mid-function drive produces polls from the same
     *    address, so this clause alone would not separate them -- which is why
     *    it is conjoined with `!drive` and with the two calls above, and why
     *    the count is kept by `sa2_window_note_poll_caller` rather than read
     *    out of the 4096-entry import log a long run overflows.
     */
    /* Read from the import row, not from the call log: with threads on these
     * calls happen at call 15 4xx and the log stops at 4096, so the log-based
     * version reported "init did not return true" for a run whose loop was
     * demonstrably turning. See `sa2_import_info.first_caller`. */
    const sa2_import_info *stes_i = sa2_import_by_name("KERNEL32.dll", "SetThreadExecutionState");
    const sa2_import_info *spi_i  = sa2_import_by_name("USER32.dll", "SystemParametersInfoW");
    const int init_returned_true = stes_i != NULL && stes_i->calls > 0
                                   && stes_i->first_caller == 0x00401d64u
                                   && stes_i->first_arg0 == 0x80000003u;
    const int winmain_reached_its_loop = init_returned_true
                                         && spi_i != NULL && spi_i->calls > 0
                                         && spi_i->first_caller == 0x00401d79u;
    const uint32_t winmain_iterations = sa2_window_polls_from(0x00401db3u);
    const uint32_t polls_elsewhere = pump.polls > winmain_iterations
                                     ? pump.polls - winmain_iterations : 0u;
    /* A device had to have been built for any of this to be reachable, and the
     * guest had to have asked it for something. Both are conjoined so that a
     * run with the device switched off cannot satisfy this by another route. */
    /* Reported, and deliberately NOT conjoined.
     *
     * It was written as an assertion -- "every method call landed on an object
     * this runtime built, so no declared argument count is wrong" -- and
     * breaking the argument count with SA2_D3D9_BREAK=argc left it at zero,
     * because the guest loads `this` from a register and not from the drifted
     * stack. A check that does not move when the thing it checks is broken is
     * the eighth of its kind in this project, and the honest thing is to
     * demote it rather than to keep it and feel covered. The drift is caught
     * instead by `IDirect3D9::CreateDevice` refusing a DeviceType outside
     * 1..3 (runtime/sa2_d3d9.c), which does stop the run under that break. */
    const int d3d9_every_this_is_an_object =
        !sa2_d3d9_device_enabled() || d3d.unknown_this == 0;
    /* The closed world's mode (runtime/sa2_d3d9.c) is decided by the device from the
     * keyboard's environment, apart from runtime/sa2_dinput.c: log when the keyboard
     * source is the live FIFO in a session with a person at it, trap otherwise (an
     * automated session on the FIFO, play_live --synthetic-keys, traps), and never a
     * unit build's open world (which the #error above keeps out of this probe). This
     * holds the device's reading of the keyboard source to dinput's on every run. */
    const int dinput_live = !strcmp(sa2_dinput_input_source(), "fifo");
    const int d3d9_world_is_the_keyboard_sources =
        sa2_d3d9_world_live_source() == dinput_live &&
        !strcmp(sa2_d3d9_world_mode_name(),
                dinput_live && !sa2_d3d9_world_automated() ? "log" : "trap");

    /* ------------------------ textures the guest built from its own blobs
     *
     * Two clauses, and each has a break that fails it, because a check whose
     * break moves nothing is the defect this project keeps finding:
     *
     *  - every texture d3dx9 answered was sized from its own blob's header --
     *    the row records which field each number came from, and nothing was
     *    fabricated. SA2_D3D9_BREAK=texguess invents 256x256 instead, and the
     *    clause goes false with `dimensions_from` reading "invented".
     *  - no texture surface holds a byte this runtime wrote. This is counted
     *    out of guest memory rather than read from a flag: a texture the guest
     *    never locked must still be entirely zero, because nothing in this
     *    runtime decodes an image. SA2_D3D9_BREAK=fakepixels fills them and
     *    the clause goes false, with `unlocked_surfaces_holding_bytes` saying
     *    how many. A texture released before this reads -- at shutdown, nearly
     *    all of them -- has had its store freed, so its count is the one
     *    sa2_d3d9.c took at that release (`released`), not zero for want of
     *    bytes to read. Two kinds of row are left out, each for a reason: a
     *    texture the guest locked, whose bytes it may have written itself; and
     *    a texture a verbatim D3DXLoadSurfaceFromSurface copied into
     *    (`guest_copied`), which moves the guest's bytes from another surface
     *    into the texture's store without a Lock. The rows left out for the
     *    second reason alone, copied into and never locked, are counted in
     *    `guest_copied_rows_left_out`, and the rows the count did read in
     *    `rows_counted`, so a run says how many rows the clause stands on. A
     *    counted row may have no store at all (never locked, never released),
     *    and then it reads no guest byte: `rows_counted` is the rows the clause
     *    looked at, not the rows whose memory it scanned.
     *    The clause reads the fakepixels flag before any byte, so under that
     *    break it fails whatever the count says; tools/s4/d3d9_life_check.py
     *    runs fakepixels and reads the count alone.
     *
     * Both require evidence rather than passing vacuously: a run that loaded no
     * texture reads false, and `textures_from_files.answered` beside it says
     * that is why. An unexercised clause that reads true is how slice 11
     * reverted a correct fix. */
    const uint32_t tex_rows = sa2_d3d9_texsrc_count();
    uint32_t tex_unlocked_nonzero = 0, tex_answered = 0, tex_guest_copied = 0, tex_counted = 0;
    int tex_sized_from_headers = 1, tex_no_runtime_pixels = 1;
    for (uint32_t i = 0; i < tex_rows; i++) {
        const sa2_d3d9_texsrc *t = sa2_d3d9_texsrc_at(i);
        if (!t) break;
        if (t->refused) continue;          /* a refusal answered nothing */
        tex_answered++;
        if (!t->parsed ||
            (t->dim_from != SA2_TEXP_STATED && t->dim_from != SA2_TEXP_CALLER))
            tex_sized_from_headers = 0;
        if (t->pixels_decoded || t->runtime_wrote_pixels) tex_no_runtime_pixels = 0;
        if (t->locks) continue;            /* the guest may have written these itself */
        if (t->guest_copied) { tex_guest_copied++; continue; }
        tex_counted++;
        if (sa2_d3d9_texsrc_nonzero(i) > 0u) {
            tex_unlocked_nonzero++;
            tex_no_runtime_pixels = 0;
        }
    }
    /* The hash every texture row carries is worth nothing if the function that
     * computes it is wrong, and a wrong one is invisible: plausible constants,
     * reproducible across runs, unfalsifiable from inside. The runtime hashes
     * the published NIST vectors at every run instead. */
    const int sha256_matches_vectors = sa2_d3d9_sha256_selftest();
    const int textures_sized_from_their_own_headers = tex_answered > 0 && tex_sized_from_headers;
    const int no_texture_surface_holds_runtime_bytes = tex_answered > 0 && tex_no_runtime_pixels;
    const int winmain_loop_ran = !drive && winmain_reached_its_loop
                                 && winmain_iterations > 0u && polls_elsewhere == 0u
                                 && d3d.created > 0u && d3d.method_calls > 0u;

    /* The loop's verdict, and every clause of it can fail.
     *
     *  - it ran at all, and ended through the guest's own `ret 10h` on the
     *    sentinel rather than through a trap;
     *  - the guest's loop body executed at least once;
     *  - GetMessageW was reached, which only happens when PeekMessageW
     *    reported a message, which only happens when the host put one there;
     *  - the guest's *own* window procedure ran and called PostQuitMessage
     *    from inside itself -- 0x0040185d is an address in the guest's WM_CLOSE
     *    handler and nothing in this runtime can push it;
     *  - the quit came from the guest, not from the host's budget.
     *
     * The last two are what make this a round trip rather than a count: the
     * host injects a message, guest code decides to end the program, and the
     * pump reports the decision back to the same guest loop. */
    const int loop_iterated = loop.ran && loop.polls > 0;
    const int loop_saw_a_message = loop.gets > 0 && pump.polls_message > 0;
    const int guest_wndproc_ran = loop.wndproc_calls > 0 && loop.postquit_caller != 0u
                                  && loop.postquit_caller >= 0x00401810u
                                  && loop.postquit_caller < 0x004018b8u;
    const int guest_ended_the_loop = loop.quit_by_guest && !loop.quit_by_host;
    /* The loop left its own cycle: GetMessageW pulled WM_QUIT and answered
     * FALSE, which is the only branch out of 0x00401da4..0x00401dde that is
     * not a trap. Without this clause "the guest ended the loop" would be
     * satisfied by a PostQuitMessage that nothing ever collected. */
    const int loop_left_the_pump = loop.quit_delivered > 0;
    const int loop_ok = loop_iterated && loop_saw_a_message
                        && guest_wndproc_ran && guest_ended_the_loop && loop_left_the_pump
                        && blocking_get_refused_and_ready_served;

    /* --- what the threads did (G1 section 3, runtime/sa2_threads.c) ------
     *
     * The observables here are guest arithmetic, not runtime bookkeeping:
     *
     *  - the worker's start address. Nothing in this runtime knows 0x00785500;
     *    it arrives as the third argument of a CreateThread the CRT's
     *    _beginthreadex made on the guest's behalf, and the guest computed it.
     *  - the worker's own import calls. Every record in the call log carries
     *    the guest thread that made it, and a record with thread != 0 can only
     *    have come from lifted code running on a second guest stack.
     *  - the pulses the periodic timer delivered, against the pulses it lost.
     *    A timer that ticks into an empty room and a timer whose worker is
     *    waiting for it are the same tick count and a different program.
     */
    const int sched_threads = sa2_sched_count();
    uint32_t worker_import_calls = 0, worker_first_caller = 0;
    uint8_t  threads_seen_in_log = 0;
    for (uint32_t i = 0; i < logged_main; i++) {
        const sa2_import_call *c = sa2_import_call_at(i);
        if (!c || c->thread == 0) continue;
        if (!worker_import_calls) worker_first_caller = c->caller;
        worker_import_calls++;
        if (c->thread > threads_seen_in_log) threads_seen_in_log = c->thread;
    }
    /* The recent ring too: the log fills at 4096 calls and the worker runs
     * after that, so counting only the log would report zero for a worker that
     * ran thousands of times. */
    uint32_t worker_recent_calls = 0;
    for (uint32_t i = 0; i < sa2_import_recent_count(); i++) {
        const sa2_import_recent *r = sa2_import_recent_at(i);
        if (r && r->thread) worker_recent_calls++;
    }
    uint64_t worker_guest_calls = 0, worker_slices = 0;
    for (int i = 1; i < sched_threads; i++) {
        worker_guest_calls += sa2_sched_guest_calls(i);
        worker_slices += sa2_sched_slices(i);
    }
    const int thread_created = sa2_import_threads_created() > 0;
    /* The routine the *guest* asked for, which is not the address CreateThread
     * was given. `_beginthreadex` at 0x00828ad3 hands CreateThread its own
     * `_threadstartex` (0x00828a50) and stashes the caller's routine at offset
     * 0x54 of the per-thread block it passes as the parameter:
     *
     *     00828b3a  mov [esi+54h],edi        ; edi = the routine the guest passed
     *     00828b49  push 828A50h             ; _threadstartex
     *     00828b54  call [873058h]           ; CreateThread
     *
     * So this reads the guest's own structure to recover it. The runtime does
     * not know that layout and must not: it is the probe's oracle, not the
     * runtime's behaviour. The first version of this clause compared
     * `start_va` against the sound worker and was false for the right reason
     * in a way that looked like the wrong one. */
    int sound_worker = 0;
    uint32_t user_start[SA2_MAX_GUEST_THREADS];
    memset(user_start, 0, sizeof user_start);
    for (int i = 1; i < sched_threads && i < SA2_MAX_GUEST_THREADS; i++) {
        user_start[i] = sa2_sched_start_va(i) == CRT_THREADSTARTEX_VA
                      ? sa2_sched_param_peek(i)
                      : sa2_sched_start_va(i);
        if (user_start[i] == SOUND_WORKER_VA) sound_worker = 1;
    }
    const int timer_armed = sa2_import_timers_created() > 0;
    const int timer_fired = sa2_timer_fires() > 0;
    const int pulses_landed = sa2_timer_pulses_delivered() > 0;
    const int worker_ran_lifted_code = worker_guest_calls > 0 && worker_slices > 1;
    /* No thread left its own stack slot, and no wait ended because the
     * scheduler gave up. Both are the shape of failure this design has, so
     * both are clauses rather than remarks. */
    const int stacks_kept = sa2_sched_stack_escapes() == 0;
    const int no_endless_wait = sa2_sched_deadlocks() == 0;
    /* Every hand-off found the running thread looking at its own SEH chain.
     * The clause is conjoined with "something was checked", because a
     * violation count of zero over zero checks is not a result. */
    const int seh_private = sa2_sched_seh_checks() > 0 && sa2_sched_seh_violations() == 0;
    /* Every worker that ended the way a thread ends -- it returned, or called
     * ExitThread -- let go of the CRT's per-thread block: Windows' thread exit
     * runs the FLS callbacks (the CRT's `_freefls`) and clears the value, so
     * no ended thread still names a block in the CRT's FLS slot. A thread that
     * trapped ended where it trapped and is not asked. Reported and not
     * conjoined; `noflsexit` in tools/s4/thread_run.py's matrix is the break
     * that shows it can fail, against a control where it holds. */
    int ended_released = 1;
    {
        const uint32_t idx = peek32(VA_CRT_FLS_INDEX);
        for (int i = 1; idx < SA2_FLS_SLOTS && i < sched_threads; i++)
            if (sa2_sched_finished(i)
                && (sa2_sched_trap_status(i) == SA2_TRAP_NONE || sa2_sched_exit_called(i))
                && sa2_sched_fls(i)[idx]) { ended_released = 0; break; }
    }

    /* --- the assertions -------------------------------------------------- */
    const int imports_bound  = (bind_rc > 0) && (bound > 0) && (slots_still_unbound == 0);
    const int table_grew     = (rows_after == rows_before + bound);
    /* A run "ended where intended" only if it ended the way a *program* ends:
     * through the entry's own `ret`, by the guest calling ExitProcess or
     * TerminateProcess, or at the address this probe asked it to stop at.
     *
     * Stopping at an import is NOT that. The previous slice's probe counted it
     * as one, which was right while the question was "does the import table
     * dispatch" and is wrong now that the question is "does CRT init finish":
     * an unimplemented import, and an implemented one that refuses because a
     * value has no defensible answer, are both work items, and a probe that
     * prints `ok` over them reports a wall as progress. It happened once in
     * this session -- `ok: true` over a run that had stopped inside
     * GetStringTypeW -- which is what bought the distinction. `stopped_at`
     * below still reports which import it was, because that is the work list. */
    const int ended_where_intended = (res.status == SA2_TRAP_NONE)
                                  || (g_stopped && res.exit_pc == g_stop_va)
                                  || sa2_import_stopped_on_request()
                                  || sa2_import_exited();
    const int stop_reached   = (g_stopped && res.exit_pc == g_stop_va)
                               || sa2_import_stopped_on_request();
    /* A headless run that put up a message box has almost always just printed
     * the reason it failed, so this is an assertion and not a remark. */
    const int no_message_box = sa2_msgbox_count() == 0;
    /* `reached_winmain` and `guest_parsed_the_command_line` were reported here
     * and left out of this conjunction, which made them assertions that could
     * not fail. Breaking lstrcmpA to report every string equal found it: the
     * guest's own -r flag came out 1 where the probe expected 0, the assertion
     * said so, and the probe still printed `"ok": true`. That is the fifth
     * check in this project that turned out unable to fail and the second that
     * a probe found about itself. */
    /* The file system's own three. `no_install_write_attempt` is a guard, not
     * the evidence: the evidence that the installation is untouched is
     * `tools/s4/install_guard.py`'s before/after manifest, taken outside this
     * process. `guest_read_a_real_file` is here rather than in the report
     * because a run in which no file was ever opened is not a run of this
     * slice, and it would otherwise be the one fact nobody checked. */
    const int no_install_write_attempt = vfs.install_write_attempts == 0;
    const int guest_read_a_real_file = vfs.install_opens > 0 && vfs.bytes_read > 0
                                       && guest_sized_its_files && guest_parsed_its_config;

    /* --------- Steam: declined, and observably not fabricated -----------
     *
     * Four clauses, one per break, and each names the thing it watches.
     *
     *  - the wall is gone. The observable is *not* "the runtime implemented a
     *    slot": it is that a second `PeekMessageW` came back to 0x00401db3.
     *    That address is the instruction after `call ebx` in WinMain's own
     *    loop head, so a second one can only happen if the guest returned from
     *    its per-frame routine and went round. Under the previous slice this
     *    was 1 and the run stopped inside the first frame.
     *  - a declined query left the guest's out-parameter alone. The witness is
     *    a sentinel the probe wrote, not a counter the runtime kept.
     *  - the no-ops reported success.
     *  - every method popped exactly what it declared.
     *
     * `guest_asked_steam_for_a_stat` is reported and not conjoined: it is the
     * precondition of the first clause and would otherwise let a run that
     * never reached Steam at all pass for one that walked past it. */
    const sa2_iface_slot_stat *us0 = NULL;
    for (uint32_t i = 0; i < sa2_iface_slot_count(); i++) {
        const sa2_iface_slot_stat *t = sa2_iface_slot_at(i);
        if (t && !strcmp(t->iface, "ISteamUserStats") && t->slot == 0
            && t->first_caller == 0x0040e8c5u) { us0 = t; break; }
    }
    const int guest_asked_steam_for_a_stat = us0 != NULL && us0->calls > 0;
    const int steam_wall_gone = guest_asked_steam_for_a_stat
                                && ifs.methods_refused == 0u
                                && winmain_iterations > 1u;
    const int steam_declined_without_writing =
        steam.ran && steam.get_ran && steam.get_returned == 0u
        && steam.get_out_after == STEAM_PROBE_SENTINEL;
    const int steam_noop_reported_success =
        steam.ran && steam.set_ran && steam.set_returned == 1u
        && steam.store_ran && steam.store_returned == 1u;
    const int steam_methods_popped_their_arguments =
        steam.ran && steam.get_esp_delta == 12u && steam.set_esp_delta == 8u
        && steam.store_esp_delta == 4u;
    /* The name the runtime recorded came through the guest pointer the probe
     * placed, and was recorded once as a query and once as a write. */
    int steam_recorded_the_name = 0;
    for (uint32_t i = 0; i < sa2_steam_name_count(); i++) {
        const sa2_steam_name *n = sa2_steam_name_at(i);
        if (n && !strcmp(n->name, "SA2_RECOMP_PROBE_NO_SUCH_ACHIEVEMENT")
            && n->asks > 0 && n->sets > 0) steam_recorded_the_name = 1;
    }

    /* The clause table.
     *
     * Slice 7's fourth finding was that a conjunction with many false clauses
     * hides a newly-false one as effectively as no check at all: `ok` had been
     * false for six reasons at once and nothing said which one was new. The
     * conjunction below is unchanged -- the fix is that every clause is also
     * emitted by name, so `tools/s4/thread_run.py` can diff this run's clauses
     * against a baseline's and name the ones that *changed*. A verdict that
     * says "false" is not the same artefact as one that says "false, and
     * `heap_walk_agrees` is the clause that stopped being true".
     *
     * `in_verdict` is whether the clause is conjoined into `ok` in this run,
     * so a reported-but-not-conjoined clause cannot be mistaken for a check. */
    const int ok = imports_bound && table_grew && ended_where_intended
                   && cookie_matches && heap_initialised && heap_consistent
                   && no_message_box && reached_winmain
                   && guest_parsed_the_command_line && guest_saw_the_cpu_model
                   && escape_refused && no_install_write_attempt
                   && (!require_files || guest_read_a_real_file)
                   && (!require_stop || stop_reached)
                   && (!require_loop || (loop_ok && guest_built_its_window))
                   && (!require_winmain_loop || winmain_loop_ran)
                   && (!require_threads || (thread_created && sound_worker && timer_armed
                                            && timer_fired && pulses_landed
                                            && worker_ran_lifted_code && stacks_kept
                                            && no_endless_wait && seh_private))
                   && steam_declined_without_writing && steam_noop_reported_success
                   && steam_methods_popped_their_arguments && steam_recorded_the_name
                   && (!require_winmain_loop || steam_wall_gone);

    const struct { const char *name; int value; int in_verdict; } clause[] = {
        {"imports_bound", imports_bound, 1},
        {"table_grew_by_the_import_count", table_grew, 1},
        {"ended_where_intended", ended_where_intended, 1},
        {"guest_computed_the_cookie", cookie_matches, 1},
        {"guest_created_its_heap", heap_initialised, 1},
        {"heap_walk_agrees", heap_consistent, 1},
        {"no_message_box", no_message_box, 1},
        {"guest_saw_the_cpu_model", guest_saw_the_cpu_model, 1},
        {"reached_winmain", reached_winmain, 1},
        {"guest_parsed_the_command_line", guest_parsed_the_command_line, 1},
        {"escape_refused_and_control_served", escape_refused, 1},
        {"no_install_write_attempt", no_install_write_attempt, 1},
        {"reached_the_stop_address", stop_reached, require_stop},
        {"guest_read_a_real_file", guest_read_a_real_file, require_files},
        {"guest_sized_its_files", guest_sized_its_files, 0},
        {"guest_parsed_its_config", guest_parsed_its_config, 0},
        {"guest_built_its_window", guest_built_its_window, require_loop},
        {"guest_sized_its_window", guest_sized_its_window, 0},
        {"wndproc_is_lifted_guest_code", wndproc_is_lifted, 0},
        {"loop_ended_after_the_guest_asked_to_quit", loop_ok, require_loop},
        {"loop_iterated", loop_iterated, 0},
        {"loop_saw_a_message", loop_saw_a_message, 0},
        {"guest_wndproc_ran_and_posted_quit", guest_wndproc_ran, 0},
        {"guest_ended_the_loop", guest_ended_the_loop, 0},
        {"d3d9_every_this_is_an_object", d3d9_every_this_is_an_object, 0},
        {"d3d9_world_is_the_keyboard_sources", d3d9_world_is_the_keyboard_sources, 0},
        {"texture_hashes_use_a_sha256_that_matches_its_test_vectors",
         sha256_matches_vectors, 0},
        {"every_texture_was_sized_from_its_own_header",
         textures_sized_from_their_own_headers, 0},
        {"no_texture_surface_holds_bytes_this_runtime_wrote",
         no_texture_surface_holds_runtime_bytes, 0},
        {"winmain_ran_its_own_loop", winmain_loop_ran, require_winmain_loop},
        {"loop_left_the_pump_on_wm_quit", loop_left_the_pump, 0},
        {"blocking_getmessage_refused_and_ready_served",
         blocking_get_refused_and_ready_served, 0},
        {"guest_created_a_thread", thread_created, require_threads},
        {"the_thread_is_the_guests_sound_worker", sound_worker, require_threads},
        {"guest_armed_the_periodic_timer", timer_armed, require_threads},
        {"the_timer_fired", timer_fired, require_threads},
        {"a_pulse_released_a_waiting_thread", pulses_landed, require_threads},
        {"the_worker_ran_lifted_code", worker_ran_lifted_code, require_threads},
        {"every_thread_stayed_in_its_stack_slot", stacks_kept, require_threads},
        {"no_wait_ended_because_nothing_could_run", no_endless_wait, require_threads},
        {"every_thread_saw_only_its_own_seh_chain", seh_private, require_threads},
        {"every_ended_thread_released_its_crt_block", ended_released, 0},
        {"guest_asked_steam_for_a_stat", guest_asked_steam_for_a_stat, 0},
        {"guest_walked_past_the_steam_stats_wall", steam_wall_gone, require_winmain_loop},
        {"steam_declined_without_writing_the_out_parameter",
         steam_declined_without_writing, 1},
        {"steam_noop_reported_success", steam_noop_reported_success, 1},
        {"steam_methods_popped_their_own_arguments",
         steam_methods_popped_their_arguments, 1},
        {"steam_recorded_the_guests_own_name", steam_recorded_the_name, 1},
    };

    FILE *out = stdout;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (!json_path) break;
            out = fopen(json_path, "w");
            if (!out) { fprintf(stderr, "crt_probe: cannot write %s\n", json_path); break; }
        }
        fprintf(out, "{\n \"va\": \"0x%08x\", \"rc\": %d, \"bind_enabled\": %s,\n",
                va, rc, sa2_imports_bound() ? "true" : "false");
        fprintf(out, " \"extra_blocks_registered\": %d,\n", extra_rows);
        /* Which executable this run loaded, said by the loader rather than read from the
         * path: "shipped" only when init mapped the shipped file through the overlay
         * (SA2_EXE_KIND=shipped, runtime/sa2_exe_overlay.h), and the overlay's report, its
         * table's digests, the load's refusal line and the poison tier's .text and header
         * counts. A run on the unpacked executable says "unpacked" with the report beside. */
        {
            const char *kind_env = getenv("SA2_EXE_KIND");
            fprintf(out, " \"exe\": {\"path\": ");
            jstr(out, pe);
            fprintf(out, ", \"kind\": \"%s\", \"kind_env\": ",
                    sa2_exe_overlay_loaded() ? "shipped" : "unpacked");
            if (kind_env) jstr(out, kind_env); else fputs("null", out);
            fprintf(out, ", \"poison_mode\": %d, \"overlay\": ", sa2_guest_poison);
            sa2_exe_overlay_report(out);
            fprintf(out, " },\n");
        }
        fprintf(out, " \"cpu\": {\"cpuid_calls\": %llu, \"rdtsc_calls\": %llu, "
                     "\"cpuid_leaf1_edx\": \"0x%08x\", \"sse2_reported\": %s,\n"
                     "   \"guest_sse2_available_va\": \"0x%08x\", \"guest_sse2_available\": %u, "
                     "\"guest_saw_the_cpu_model\": %s},\n",
                (unsigned long long)sa2_cpuid_calls(), (unsigned long long)sa2_rdtsc_calls(),
                sa2_cpuid_edx1(), (sa2_cpuid_edx1() & (1u << 26)) ? "true" : "false",
                VA_SSE2_AVAIL, sse2_avail, guest_saw_the_cpu_model ? "true" : "false");
        fprintf(out, " \"missing_block\": {\"policy\": %d, \"policy_name\": ",
                sa2_missing_block_dispatch);
        jstr(out, sa2_mb_policy_name(sa2_missing_block_dispatch));
        fprintf(out, ", \"edges\": %u, \"census\": [", mb_edges);
        for (uint32_t i = 0; i < sa2_mb_census_count(); i++) {
            const sa2_mb_census_entry *e = sa2_mb_census_at(i);
            fprintf(out, "%s{\"target\": \"0x%08x\", \"source\": \"0x%08x\", \"kind\": ",
                    i ? ", " : "", e->target, e->source);
            jstr(out, sa2_mb_edge_name(e->kind));
            fprintf(out, ", \"hits\": %u, \"dispatches\": %u, \"traps\": %u}",
                    e->hits, e->dispatches, e->traps);
        }
        fprintf(out, "]},\n");
        fprintf(out, " \"bind\": {\"rc\": %d, \"imports\": %u, \"table_rows_before\": %u, "
                     "\"table_rows_after\": %u, \"slots_holding_thunk\": %u, "
                     "\"slots_still_unbound\": %u},\n",
                bind_rc, bound, rows_before, rows_after, slots_holding_thunk, slots_still_unbound);
        fprintf(out, " \"stop\": {\"va\": \"0x%08x\", \"registered\": %s, "
                     "\"replaced_a_lifted_row\": %s, \"reached\": %s, \"esp\": \"0x%08x\",\n"
                     "   \"args\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"]},\n",
                g_stop_va, stop_registered ? "true" : "false",
                stop_was_lifted ? "true" : "false", g_stopped ? "true" : "false",
                g_stop_esp, g_stop_args[0], g_stop_args[1], g_stop_args[2], g_stop_args[3]);
        fprintf(out, " \"crt\": {\n");
        fprintf(out, "   \"cookie\": {\"va\": \"0x%08x\", \"before\": \"0x%08x\", "
                     "\"after\": \"0x%08x\", \"complement\": \"0x%08x\", "
                     "\"expected\": \"0x%08x\", \"computable\": %s, \"missing_mask\": %u, "
                     "\"matches\": %s},\n",
                VA_COOKIE, cookie0, cookie, cookie_c, cookie_want,
                cookie_computable ? "true" : "false", cookie_missing,
                cookie_matches ? "true" : "false");
        fprintf(out, "   \"heap\": {\"crtheap_va\": \"0x%08x\", \"before\": \"0x%08x\", "
                     "\"after\": \"0x%08x\", \"heapcreate_returned\": \"0x%08x\", "
                     "\"active_heap_before\": %u, \"active_heap_after\": %u, "
                     "\"initialised\": %s},\n",
                VA_CRTHEAP, heap0, crtheap, hc ? hc->ret : 0u, active0, active_heap,
                heap_initialised ? "true" : "false");
        fprintf(out, "   \"acmdln\": {\"ptr\": \"0x%08x\", \"text\": ", cmdline_p);
        jstr(out, cmdline);
        fprintf(out, "},\n   \"aenvptr\": {\"ptr\": \"0x%08x\", \"first\": ", environ_p);
        jstr(out, environ0);
        fprintf(out, "}\n },\n");
        fprintf(out, " \"winmain\": {\"va\": \"0x%08x\", \"reached\": %s, "
                     "\"import_calls_from_it\": %u, \"first_caller\": \"0x%08x\",\n"
                     "   \"mutex_name\": ", WINMAIN_VA, reached_winmain ? "true" : "false",
                winmain_calls, winmain_first_caller);
        jstr(out, sa2_named_object_count() && sa2_named_object_at(0)
                  ? sa2_named_object_at(0) : "");
        fprintf(out, ", \"mutex_name_va\": \"0x%08x\",\n   \"cmdline_tail\": ",
                mutex ? mutex->arg[2] : 0u);
        jstr(out, sa2_import_cmdline_tail());
        fprintf(out, ", \"dash_r_flag_va\": \"0x%08x\", \"dash_r_flag\": %u, "
                     "\"dash_r_expected\": %u, \"guest_parsed_the_command_line\": %s},\n",
                VA_DASH_R_FLAG, dash_r, dash_r_want,
                guest_parsed_the_command_line ? "true" : "false");
        fprintf(out, " \"heap\": {\"arena\": [\"0x%08x\", \"0x%08x\"], \"ready\": %u, "
                     "\"allocs\": %u, \"frees\": %u, \"failures\": %u,\n"
                     "   \"live_blocks\": %u, \"live_bytes\": %u, \"peak_bytes\": %u, "
                     "\"walk_blocks\": %u, \"walk_inuse\": %u, \"walk_largest_free\": %u, "
                     "\"consistent\": %s,\n"
                     "   \"fail_last_bytes\": %u, \"fail_max_bytes\": %u,\n"
                     "   \"runtime_arena\": {\"base\": \"0x%08x\", \"end\": \"0x%08x\", "
                     "\"used\": %u, \"overlaps_guest_heap\": %s}},\n",
                hs.arena_base, hs.arena_end, hs.ready, hs.allocs, hs.frees, hs.failures,
                hs.live_blocks, hs.live_bytes, hs.peak_bytes, hs.walk_blocks, hs.walk_inuse,
                hs.walk_largest_free, heap_consistent ? "true" : "false",
                hs.fail_last_bytes, hs.fail_max_bytes,
                SA2_GUEST_RT_ARENA_BASE, SA2_GUEST_RT_ARENA_END, sa2_guest_rt_used(),
                (SA2_GUEST_RT_ARENA_BASE < SA2_GUEST_HEAP_END
                 && SA2_GUEST_RT_ARENA_END > SA2_GUEST_HEAP_BASE) ? "true" : "false");
        fprintf(out, " \"getprocaddress\": {\"resolved\": %u, \"rows\": [",
                sa2_dynimport_count());
        for (uint32_t i = 0; i < sa2_dynimport_count(); i++) {
            const sa2_import_info *in = sa2_dynimport_at(i);
            fprintf(out, "%s{\"module\": ", i ? ", " : "");
            jstr(out, in->dll);
            fprintf(out, ", \"name\": ");
            jstr(out, in->name);
            fprintf(out, ", \"thunk\": \"0x%08x\", \"implemented\": %s, \"calls\": %u}",
                    in->thunk, in->implemented ? "true" : "false", in->calls);
        }
        fprintf(out, "],\n   \"unresolved\": %u, \"unresolved_names\": [",
                sa2_dynimport_missing_count());
        for (uint32_t i = 0; i < sa2_dynimport_missing_count(); i++) {
            const char *n = sa2_dynimport_missing_at(i);
            if (!n) break;
            if (i) fprintf(out, ", ");
            jstr(out, n);
        }
        fprintf(out, "]},\n");
        fprintf(out, " \"loadlibrary_refused\": [");
        for (uint32_t i = 0; i < sa2_loadlibrary_refused_count(); i++) {
            const char *n = sa2_loadlibrary_refused_at(i);
            if (!n) break;
            if (i) fprintf(out, ", ");
            jstr(out, n);
        }
        /* The rule that refused each one, index for index with the list above,
         * which keeps its shape for the tools that read it. */
        fprintf(out, "],\n \"loadlibrary_refused_why\": [");
        for (uint32_t i = 0; i < sa2_loadlibrary_refused_count(); i++) {
            const char *w = sa2_loadlibrary_refused_why(i);
            if (!w) break;
            if (i) fprintf(out, ", ");
            jstr(out, w);
        }
        /* The modules LoadLibrary mapped as data (runtime/sa2_module.c). */
        fprintf(out, "],\n \"modules\": [");
        for (uint32_t i = 0; i < sa2_module_count(); i++) {
            const sa2_module *md = sa2_module_at(i);
            fprintf(out, "%s{\"asked\": ", i ? ", " : "");
            jstr(out, md->asked);
            fprintf(out, ", \"backing\": ");
            jstr(out, md->backing);
            fprintf(out, ", \"self_name\": ");
            jstr(out, md->self_name);
            fprintf(out, ", \"substituted\": %s, \"base\": \"0x%08x\", \"size\": %u, "
                    "\"image_base\": \"0x%08x\", \"file_bytes\": %u, \"relocations\": %u, "
                    "\"exports\": %u, \"code_exports\": %u, \"code_natives\": [",
                    md->substituted ? "true" : "false", md->base, md->size, md->image_base,
                    md->file_bytes, md->relocations, md->exports, md->code_exports);
            for (uint32_t k = 0; k < md->code_count; k++)
                fprintf(out, "%s\"0x%08x\"", k ? ", " : "", md->code_va[k]);
            fprintf(out, "], \"code_calls\": %u, \"imports_unresolved\": %u, "
                    "\"entry_rva_never_called\": \"0x%08x\", \"refs\": %u, \"mappings\": %u, "
                    "\"lookups\": %u, \"misses\": %u}",
                    md->code_calls, md->imports_unresolved, md->entry_rva, md->refs, md->mappings,
                    md->lookups, md->misses);
        }
        fprintf(out, "],\n \"message_boxes\": {\"count\": %u, \"shown\": [", sa2_msgbox_count());
        for (uint32_t i = 0; i < sa2_msgbox_count() && i < 8u; i++) {
            fprintf(out, "%s{\"caption\": ", i ? ", " : "");
            jstr(out, sa2_msgbox_caption(i));
            fprintf(out, ", \"text\": ");
            jstr(out, sa2_msgbox_text(i));
            fprintf(out, "}");
        }
        fprintf(out, "]},\n");
        /* What the guest said through OutputDebugString, kept by the runtime:
         * the pointer in a call record is a reused stack buffer by the end. */
        fprintf(out, " \"debug_strings\": {\"count\": %u, \"kept\": [", sa2_debug_string_count());
        for (uint32_t i = 0; i < sa2_debug_strings_kept(); i++) {
            uint32_t ordinal = 0;
            int truncated = 0;
            const char *t = sa2_debug_string_at(i, &ordinal, &truncated);
            if (!t) break;
            fprintf(out, "%s{\"ordinal\": %u, \"truncated\": %s, \"text\": ", i ? ", " : "",
                    ordinal, truncated ? "true" : "false");
            jstr(out, t);
            fprintf(out, "}");
        }
        fprintf(out, "]},\n");
        /* Instructions remill could not lift, executed by runtime/sa2_emulate.cpp,
         * and how many continued at the next instruction's lifted entry. */
        fprintf(out, " \"emulated\": {\"total\": %llu, \"continued\": %llu, \"by_kind\": {",
                (unsigned long long)sa2_rt_emulated(), (unsigned long long)sa2_rt_emulated_continued());
        for (uint32_t i = 0; i < sa2_emulate_kinds(); i++)
            fprintf(out, "%s\"%s\": %llu", i ? ", " : "", sa2_emulate_kind_name(i),
                    (unsigned long long)sa2_emulate_kind_count(i));
        fprintf(out, "}},\n");
        fprintf(out, " \"virtual\": {\"cwd\": ");
        jstr(out, sa2_import_cwd());
        fprintf(out, ", \"exe_path\": ");
        jstr(out, sa2_import_exe_path());
        fprintf(out, ", \"sleep_ms\": %llu, \"last_error\": %u, "
                     "\"guest_called_exitprocess\": %s, \"exit_code\": %u},\n",
                (unsigned long long)sa2_import_virtual_ms(), sa2_import_last_error(),
                sa2_import_exited() ? "true" : "false", sa2_import_exit_code());
        fprintf(out, " \"trap\": {\"status\": %u, \"exit_pc\": \"0x%08x\", \"note\": ",
                res.status, res.exit_pc);
        jstr(out, res.note);
        fprintf(out, ",\n   \"import\": ");
        if (stop_import) {
            fprintf(out, "{\"dll\": ");
            jstr(out, stop_import->dll);
            fprintf(out, ", \"name\": ");
            jstr(out, stop_import->name);
            fprintf(out, ", \"slot\": \"0x%08x\", \"implemented\": %s}",
                    stop_import->slot, stop_import->implemented ? "true" : "false");
        } else {
            fprintf(out, "null");
        }
        fprintf(out, ",\n   \"esp\": \"0x%08x\", \"stack\": [", st.gpr[SA2_ESP]);
        for (int i = 0; i < STACK_WORDS; i++)
            fprintf(out, "%s\"0x%08x\"", i ? ", " : "", stop_stack[i]);
        fprintf(out, "]},\n");
        /* What the runtime refused because it has one guest thread, and which
         * COM classes it has no server for. Counters nothing prints are
         * counters nobody reads, so these are reported beside the run they
         * belong to rather than living in the runtime's statics. */
        fprintf(out, " \"refused_for_one_thread\": {\"threads\": %u, \"mm_timers\": %u,\n"
                     "   \"cocreateinstance\": [",
                sa2_import_threads_refused(), sa2_import_timers_refused());
        for (uint32_t i = 0; i < sa2_import_cocreate_count(); i++) {
            const uint8_t *cl = sa2_import_cocreate_clsid(i), *ii = sa2_import_cocreate_iid(i);
            if (!cl || !ii) break;
            fprintf(out, "%s\n     {\"clsid\": \"", i ? "," : "");
            for (int k = 0; k < 16; k++) fprintf(out, "%02x", cl[k]);
            fprintf(out, "\", \"iid\": \"");
            for (int k = 0; k < 16; k++) fprintf(out, "%02x", ii[k]);
            fprintf(out, "\", \"caller\": \"0x%08x\"}", sa2_import_cocreate_caller(i));
        }
        fprintf(out, "],\n   \"cocreateinstance_total\": %u},\n", sa2_import_cocreate_count());
        fprintf(out, " \"recent_imports\": [");
        for (uint32_t i = 0; i < sa2_import_recent_count(); i++) {
            const sa2_import_recent *r = sa2_import_recent_at(i);
            const sa2_import_info *ii = sa2_import_at_index(r->index);
            fprintf(out, "%s\n   {\"seq\": %u, \"dll\": ", i ? "," : "", r->seq);
            jstr(out, ii ? ii->dll : "?");
            fprintf(out, ", \"name\": ");
            jstr(out, ii ? ii->name : "?");
            /* The thread index has been in this ring since slice 8 and was
             * never printed, so "which thread makes these calls" was a
             * question the run could answer and the report could not. The
             * previous slice answered it by reading the image instead, and
             * got it wrong. */
            fprintf(out, ", \"thread\": %u, \"caller\": \"0x%08x\", \"arg0\": \"0x%08x\", \"ret\": ",
                    (unsigned)r->thread, r->caller, r->arg0);
            if (r->ret_valid) fprintf(out, "\"0x%08x\"}", r->ret);
            else fprintf(out, "null}");      /* the call never returned */
        }
        fprintf(out, "],\n");
        fprintf(out, " \"peek\": {");
        for (unsigned i = 0; i < g_peek_n; i++) {
            fprintf(out, "%s\n   ", i ? "," : "");
            jstr(out, g_peek[i].name);
            if (g_peek[i].ok) fprintf(out, ": {\"va\": \"0x%08x\", \"value\": \"0x%08x\"}",
                                      g_peek[i].va, g_peek[i].value);
            else fprintf(out, ": {\"va\": \"0x%08x\", \"value\": null}", g_peek[i].va);
        }
        fprintf(out, "},\n");
        fprintf(out, " \"peek_find_scan\": {\"from\": \"0x%08x\", \"to\": \"0x%08x\", "
                     "\"dwords_read\": %u, \"refused\": %u, \"nonzero\": %u, "
                     "\"first_nonzero\": \"0x%08x\", \"last_nonzero\": \"0x%08x\"},\n",
                g_find_lo, g_find_hi, g_find_scanned, g_find_refused, g_find_nonzero,
                g_find_first_nz, g_find_last_nz);
        fprintf(out, " \"peek_find\": [");
        for (unsigned i = 0; i < g_find_n; i++) {
            fprintf(out, "%s\n   {\"value\": \"0x%08x\", \"total\": %u, \"listed\": %u, \"at\": [",
                    i ? "," : "", g_find[i].value, g_find[i].total, g_find[i].n);
            for (uint32_t k = 0; k < g_find[i].n; k++)
                fprintf(out, "%s\"0x%08x\"", k ? ", " : "", g_find[i].at[k]);
            fprintf(out, "]}");
        }
        fprintf(out, "],\n");
        fprintf(out, " \"peek_dump\": {\"at\": \"0x%08x\", \"words\": [", g_dump_at);
        for (uint32_t i = 0; i < g_dump_n; i++) {
            if (g_dump_ok[i]) fprintf(out, "%s\"0x%08x\"", i ? ", " : "", g_dump[i]);
            else fprintf(out, "%snull", i ? ", " : "");
        }
        fprintf(out, "]},\n");
        fprintf(out, " \"files\": {\"guest_root\": ");
        jstr(out, sa2_vfs_guest_root());
        fprintf(out, ", \"read_root\": ");
        jstr(out, sa2_vfs_read_root());
        fprintf(out, ", \"write_root\": ");
        jstr(out, sa2_vfs_write_root());
        fprintf(out, ",\n   \"ready\": %s, \"case_fold\": %s,\n",
                sa2_vfs_ready() ? "true" : "false",
                sa2_vfs_case_fold_enabled() ? "true" : "false");
        fprintf(out, "   \"stats\": {\"resolves\": %u, \"escapes_refused\": %u, "
                     "\"bad_refused\": %u, \"ambiguous_refused\": %u, \"not_found\": %u, "
                     "\"case_folded_components\": %u,\n"
                     "     \"opens\": %u, \"install_opens\": %u, \"overlay_opens\": %u, "
                     "\"copy_ups\": %u, \"creates\": %u, \"dirs_created\": %u, "
                     "\"deletes\": %u,\n"
                     "     \"reads\": %u, \"writes\": %u, \"seeks\": %u, \"closes\": %u, "
                     "\"finds\": %u, \"bytes_read\": %llu, \"bytes_written\": %llu, "
                     "\"install_write_attempts\": %u},\n",
                vfs.resolves, vfs.escapes_refused, vfs.bad_refused, vfs.ambiguous_refused,
                vfs.not_found, vfs.case_folded_components, vfs.opens, vfs.install_opens,
                vfs.overlay_opens, vfs.copy_ups, vfs.creates, vfs.dirs_created, vfs.deletes,
                vfs.reads, vfs.writes, vfs.seeks, vfs.closes, vfs.finds,
                (unsigned long long)vfs.bytes_read, (unsigned long long)vfs.bytes_written,
                vfs.install_write_attempts);
        fprintf(out, "   \"escape_test\": {\"path\": ");
        jstr(out, escape_path);
        fprintf(out, ", \"ran\": %s, \"refused\": %s, \"status\": %u, "
                     "\"exit_pc\": \"0x%08x\", \"esp_delta\": %u, \"note\": ",
                esc.ran ? "true" : "false", esc.refused ? "true" : "false", esc.status,
                esc.exit_pc, esc.esp_delta);
        jstr(out, esc.note);
        fprintf(out, ",\n     \"path2\": ");
        jstr(out, escape_path2);
        fprintf(out, ", \"ran2\": %s, \"refused2\": %s, \"status2\": %u, \"note2\": ",
                esc2.ran ? "true" : "false", esc2.refused ? "true" : "false", esc2.status);
        jstr(out, esc2.note);
        fprintf(out, ",\n     \"control_path\": ");
        jstr(out, control_path);
        fprintf(out, ", \"control_ran\": %s, \"control_answered\": %s, "
                     "\"control_status\": %u, \"control_esp_delta\": %u, "
                     "\"control_returned\": %u, \"refused_and_served\": %s},\n",
                esc_ctl.ran ? "true" : "false", esc_ctl.answered ? "true" : "false",
                esc_ctl.status, esc_ctl.esp_delta,
                esc_ctl.returned, escape_refused ? "true" : "false");
        fprintf(out, "   \"opened\": [");
        for (uint32_t i = 0; i < vfs_opened_main; i++) {
            const sa2_vfs_open_record *r = sa2_vfs_opened_at(i);
            fprintf(out, "%s\n     {\"guest\": ", i ? "," : "");
            jstr(out, r->guest);
            fprintf(out, ", \"rel\": ");
            jstr(out, r->rel);
            fprintf(out, ", \"host_size\": %llu, \"layer\": %d, \"write\": %s}",
                    (unsigned long long)r->size, r->layer, r->write ? "true" : "false");
        }
        fprintf(out, "],\n   \"guest_seek_to_end\": {\"pairs\": %u, \"agree\": %u, "
                     "\"nonzero\": %u, \"guest\": [", seek_end_n, seek_end_agree,
                seek_end_nonzero);
        for (uint32_t i = 0; i < seek_end_n; i++)
            fprintf(out, "%s%u", i ? ", " : "", seek_end_got[i]);
        fprintf(out, "], \"host\": [");
        for (uint32_t i = 0; i < seek_end_n; i++)
            fprintf(out, "%s%u", i ? ", " : "", seek_end_want[i]);
        fprintf(out, "], \"agrees\": %s},\n", guest_sized_its_files ? "true" : "false");
        fprintf(out, "   \"config\": {\"path\": ");
        jstr(out, config_path);
        fprintf(out, ", \"object_va\": \"0x%08x\", \"object_from\": "
                     "\"the second argument of GetCurrentDirectoryA at 0x%08x, minus 0x%x\",\n"
                     "     \"file_bytes\": %u, \"compared\": %d, \"agree\": %d, "
                     "\"nonzero\": %d, \"fields\": [",
                cfg_self, CFG_SELF_FROM_CWD_CALLER, CFG_SELF_FIELD_OFFSET,
                cfg_bytes, cfg_compared, cfg_agree, cfg_nonzero);
        for (int i = 0; i < CFG_FIELD_COUNT; i++) {
            fprintf(out, "%s\n       {\"key\": ", i ? "," : "");
            jstr(out, CFG_FIELDS[i].key);
            fprintf(out, ", \"va\": \"0x%08x\", \"guest\": %u, \"file\": %ld, "
                         "\"in_file\": %s, \"agree\": %s}",
                    cfg_self ? cfg_self + CFG_FIELDS[i].offset : 0u, cfg_guest[i],
                    cfg_host[i], cfg_have[i] ? "true" : "false",
                    (cfg_have[i] && (long)cfg_guest[i] == cfg_host[i]) ? "true" : "false");
        }
        fprintf(out, "],\n     \"guest_parsed_its_config\": %s},\n",
                guest_parsed_its_config ? "true" : "false");
        fprintf(out, "   \"resolutions\": [");
        for (uint32_t i = 0; i < vfs_log_main; i++) {
            const sa2_vfs_event *e = sa2_vfs_log_at(i);
            if (!e) break;
            fprintf(out, "%s\n     {\"seq\": %u, \"guest\": ", i ? "," : "", e->seq);
            jstr(out, e->guest);
            fprintf(out, ", \"rel\": ");
            jstr(out, e->rel);
            fprintf(out, ", \"status\": ");
            jstr(out, sa2_vfs_status_name(e->status));
            fprintf(out, ", \"layer\": %d, \"write\": %s}", e->layer,
                    e->write ? "true" : "false");
        }
        fprintf(out, "]},\n");
        fprintf(out, " \"calls\": {\"total\": %u, \"logged\": %u, \"distinct_imports\": ",
                calls_main, logged_main);
        uint32_t distinct = 0;
        for (uint32_t i = 0; i < bound; i++) distinct += sa2_import_at(i)->calls ? 1u : 0u;
        fprintf(out, "%u, \"records\": [\n", distinct);
        for (uint32_t i = 0; i < logged_main; i++) {
            const sa2_import_call *c = sa2_import_call_at(i);
            const sa2_import_info *in = sa2_import_of_index(c->index);
            fprintf(out, "   {\"seq\": %u, \"dll\": ", c->seq);
            jstr(out, in ? in->dll : "?");
            fprintf(out, ", \"name\": ");
            jstr(out, in ? in->name : "?");
            fprintf(out, ", \"implemented\": %s, \"caller\": \"0x%08x\", \"args\": [",
                    c->implemented ? "true" : "false", c->caller);
            for (uint32_t k = 0; k < c->argc; k++)
                fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
            fprintf(out, "], \"ret\": \"0x%08x\"}%s\n",
                    c->ret, i + 1 < logged_main ? "," : "");
        }
        fprintf(out, " ]},\n");
        /* ---- the window, the pump, and the presentation boundary ---- */
        fprintf(out, " \"window\": {\"classes\": %u, \"windows\": %u, \"icons\": %u, "
                     "\"cursors\": %u, \"screen\": [%u, %u], \"built\": %s,\n",
                wnd_before.classes, wnd_before.windows, wnd_before.icons, wnd_before.cursors,
                sa2_window_screen_w(), sa2_window_screen_h(),
                guest_built_its_window ? "true" : "false");
        fprintf(out, "   \"class\": ");
        if (cls0) {
            fprintf(out, "{\"name\": ");
            jstr(out, cls0->name);
            fprintf(out, ", \"atom\": %u, \"style\": \"0x%08x\", \"wndproc\": \"0x%08x\", "
                         "\"hinstance\": \"0x%08x\", \"hicon\": \"0x%08x\", "
                         "\"hcursor\": \"0x%08x\", \"hbrush\": \"0x%08x\", "
                         "\"cls_extra\": %u, \"wnd_extra\": %u}",
                    cls0->atom, cls0->style, cls0->wndproc, cls0->hinstance, cls0->hicon,
                    cls0->hcursor, cls0->hbrush, cls0->cls_extra, cls0->wnd_extra);
        } else fprintf(out, "null");
        fprintf(out, ",\n   \"expected\": {\"from\": \"the guest's own config "
                     "Width/Height and the five metrics the runtime reported\", "
                     "\"fullscreen\": %s, \"w\": %ld, \"h\": %ld, \"x\": %ld, \"y\": %ld, "
                     "\"sm\": {\"cxscreen\": %u, \"cyscreen\": %u, \"cycaption\": %u, "
                     "\"cxframe\": %u, \"cyframe\": %u}, \"agrees\": %s},"
                     "\n   \"window\": ",
                cfg_fullscreen ? "true" : "false", want_w, want_h, want_x, want_y,
                sm[0], sm[1], sm[4], sm[32], sm[33],
                guest_sized_its_window ? "true" : "false");
        if (win0) {
            fprintf(out, "{\"hwnd\": \"0x%08x\", \"kind\": ", win0->hwnd);
            jstr(out, sa2_window_token_kind(win0->hwnd) ? sa2_window_token_kind(win0->hwnd) : "?");
            fprintf(out, ", \"class\": ");
            jstr(out, win0->class_name);
            fprintf(out, ", \"title\": ");
            jstr(out, win0->title);
            fprintf(out, ", \"style\": \"0x%08x\", \"exstyle\": \"0x%08x\", "
                         "\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d, "
                         "\"hinstance\": \"0x%08x\", \"parent\": \"0x%08x\", "
                         "\"menu\": \"0x%08x\", \"param\": \"0x%08x\", "
                         "\"wndproc\": \"0x%08x\", \"show\": %u, \"shows\": %u, "
                         "\"focuses\": %u, \"foregrounds\": %u}",
                    win0->style, win0->exstyle, win0->x, win0->y, win0->w, win0->h,
                    win0->hinstance, win0->parent, win0->menu, win0->param, win0->wndproc,
                    win0->visible, win0->shows, win0->focuses, win0->foregrounds);
        } else fprintf(out, "null");
        fprintf(out, ",\n   \"calls\": [");
        for (uint32_t i = 0; i < sa2_window_log_count(); i++) {
            const sa2_wnd_event *e = sa2_window_log_at(i);
            if (!e) break;
            fprintf(out, "%s\n     {\"seq\": %u, \"what\": ", i ? "," : "", e->seq);
            jstr(out, e->what);
            fprintf(out, ", \"caller\": \"0x%08x\", \"a\": [\"0x%08x\", \"0x%08x\", "
                         "\"0x%08x\", \"0x%08x\"], \"before_loop\": %s}",
                    e->caller, e->a[0], e->a[1], e->a[2], e->a[3],
                    i < wnd_log_before ? "true" : "false");
        }
        fprintf(out, "]},\n");
        fprintf(out, " \"pump\": {\"driven\": %s, \"lifted\": %s, \"head_va\": \"0x%08x\",\n"
                     "   \"how\": \"a mid-function window entry: WinMain does not reach its own "
                     "loop in this build because FUN_004010D0 returns false at "
                     "IDirect3D9::CreateDevice, so 0x00401da4 is lifted as its own module and "
                     "driven with the two registers WinMain loads at 0x00401d98 and 0x00401d9e\",\n"
                     "   \"poll_limit\": %u, \"status\": %u, \"exit_pc\": \"0x%08x\", "
                     "\"esp\": \"0x%08x\", \"ebp\": \"0x%08x\", \"note\": ",
                drive ? "true" : "false", loop.lifted ? "true" : "false", LOOP_HEAD_VA,
                sa2_window_poll_limit(), loop.status, loop.exit_pc, loop.esp, loop.ebp);
        jstr(out, loop.note);
        fprintf(out, ",\n   \"iterations\": %u, \"empty_polls\": %u, \"message_polls\": %u, "
                     "\"gets\": %u, \"translates\": %u, \"dispatches\": %u,\n"
                     "   \"guest_wndproc_calls\": %u, \"defwndproc_calls\": %u, "
                     "\"posted\": %u, \"delivered\": %u, \"injected\": %u, "
                     "\"queue_peak\": %u, \"blocking_get_refused\": %u,\n"
                     "   \"quit_posted\": %u, \"quit_by_host\": %u, \"quit_by_guest\": %u, "
                     "\"quit_delivered\": %u, "
                     "\"blocking_get\": {\"empty_refused\": %s, \"empty_status\": %u, "
                     "\"refusals_counted\": %u, \"ready_answered\": %s, "
                     "\"ready_returned\": %u, \"ready_message\": \"0x%04x\", "
                     "\"note\": \"%s\"}, "
                     "\"postquit_caller\": \"0x%08x\", \"cursor_show\": %d},\n",
                pump.polls, pump.polls_empty, pump.polls_message, pump.gets, pump.translates,
                pump.dispatches, pump.wndproc_calls, pump.defwndproc_calls, pump.posted,
                pump.delivered, pump.injected, pump.queue_peak, pump.blocking_get_refused,
                pump.quit_posted, pump.quit_by_host, pump.quit_by_guest,
                pump.quit_delivered,
                get_empty.refused ? "true" : "false", get_empty.status,
                block_refusals_after - block_refusals_before,
                get_ready.answered ? "true" : "false", get_ready.returned,
                peek32(msg_buf + 4u),
                get_empty.note[0] ? "refused" : "not run",
                loop.postquit_caller, pump.cursor_show);
        fprintf(out, " \"interfaces\": {\"built\": %u, \"method_calls\": %u, "
                     "\"implemented\": %u, \"surveyed\": %u, \"refused\": %u,\n"
                     "   \"steam_init\": %u, \"steam_id\": \"0x%08x%08x\", "
                     "\"d3d9_creates\": %u, \"d3d9_survey_depth\": %u,\n"
                     "   \"presentation_survey_on\": %s, \"presentation_surveyed\": %u,\n"
                     "   \"calls\": [",
                ifs.interfaces_built, ifs.method_calls, ifs.methods_implemented,
                ifs.methods_surveyed, ifs.methods_refused, ifs.init_calls,
                ifs.steam_id_hi, ifs.steam_id_lo, ifs.d3d9_creates, ifs.d3d9_survey_depth,
                sa2_presentation_survey_on() ? "true" : "false", sa2_presentation_surveyed());
        for (uint32_t i = 0; i < sa2_iface_log_count(); i++) {
            const sa2_iface_call *c = sa2_iface_log_at(i);
            if (!c) break;
            fprintf(out, "%s\n     {\"seq\": %u, \"iface\": ", i ? "," : "", c->seq);
            jstr(out, c->iface);
            fprintf(out, ", \"method\": ");
            jstr(out, c->name);
            fprintf(out, ", \"slot\": %u, \"caller\": \"0x%08x\", \"this\": \"0x%08x\", "
                         "\"args\": [", c->slot, c->caller, c->self);
            for (uint32_t k = 0; k < c->argc; k++)
                fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
            fprintf(out, "], \"ret\": \"0x%08x\", \"answered\": ", c->ret);
            jstr(out, iface_answer_name(c->answered));
            fprintf(out, "}");
        }
        fprintf(out, "],\n");
        /* The Steam surface, kept apart from the call log because the log is
         * capped at 256 entries and this must not be. One row per (interface,
         * slot) ever called, with the class of answer it got. */
        fprintf(out, "   \"slots\": [");
        for (uint32_t i = 0; i < sa2_iface_slot_count(); i++) {
            const sa2_iface_slot_stat *t = sa2_iface_slot_at(i);
            if (!t) break;
            fprintf(out, "%s\n     {\"interface\": ", i ? "," : "");
            jstr(out, t->iface);
            fprintf(out, ", \"method\": ");
            jstr(out, t->method);
            fprintf(out, ", \"slot\": %u, \"argc\": %d, \"calls\": %u, "
                         "\"first_seq\": %u, \"first_caller\": \"0x%08x\", "
                         "\"last_caller\": \"0x%08x\", \"answered\": ",
                    t->slot, t->argc, t->calls, t->first_seq, t->first_caller,
                    t->last_caller);
            jstr(out, iface_answer_name(t->answered));
            fprintf(out, "}");
        }
        fprintf(out, "],\n   \"names\": [");
        for (uint32_t i = 0; i < sa2_steam_name_count(); i++) {
            const sa2_steam_name *n = sa2_steam_name_at(i);
            if (!n) break;
            fprintf(out, "%s\n     {\"name\": ", i ? "," : "");
            jstr(out, n->name);
            fprintf(out, ", \"asks\": %u, \"sets\": %u, \"first_caller\": \"0x%08x\"}",
                   n->asks, n->sets, n->first_caller);
        }
        fprintf(out, "],\n   \"names_dropped\": %u,\n", sa2_steam_names_dropped());
        /* What this runtime declares for ISteamUserStats, whether or not the
         * guest called it -- so "refused" is a named decision and not an
         * absence. `tools/s4/steam_surface.py --argc` reads these counts back
         * and compares them against the pushes the image makes. */
        fprintf(out, "   \"declared\": [");
        {
            uint32_t n = 0;
            for (uint32_t k = 0; k < SA2_IFACE_SLOTS; k++) {
                const char *nm = sa2_iface_declared_name("ISteamUserStats", k);
                if (!nm) continue;
                fprintf(out, "%s\n     {\"slot\": %u, \"method\": ", n++ ? "," : "", k);
                jstr(out, nm);
                fprintf(out, ", \"argc\": %d}", sa2_iface_declared_argc("ISteamUserStats", k));
            }
        }
        fprintf(out, "],\n   \"declined\": %u, \"recorded\": %u, \"invented\": %u,\n"
                     "   \"out_params_unwritten\": %u, \"callbacks_never_delivered\": %u,\n"
                     "   \"break\": ",
                ifs.methods_declined, ifs.methods_recorded, ifs.methods_invented,
                ifs.out_params_unwritten, ifs.callbacks_never_delivered);
        jstr(out, sa2_steam_break_name());
        /* SteamMetricsSDK and XInput. They are here because a review found
         * `sa2_iface.h` claiming that `pad_absent`/`pad_present` is "how a
         * receipt says which policy the run was taken under" while no receipt
         * carried either number. A counter nothing reads is not a record, and
         * a comment asserting that it is makes the gap harder to see.
         *
         * `xinput_policy` is a word rather than something to infer from which
         * counter is non-zero: in a run that never calls XInput both are zero
         * and the policy is still a fact about the run.
         *
         * A live session (a FIFO input source, runtime/sa2_dinput.h, "The host
         * gamepads") also carries `pad_live` and the source's gamepad counters,
         * and its policy is "live-pads" once a call was answered from a pad the
         * source held connected. A replay's source is a script or none, so its
         * record is byte for byte what it was. */
        fprintf(out, ",\n   \"metrics\": {\"getinstance\": %u, \"initialize\": %u, "
                     "\"logevent\": %u, \"cleaninstance\": %u},\n"
                     "   \"xinput\": {\"getstate\": %u, \"setstate\": %u, "
                     "\"getcaps\": %u, \"absent\": %u, \"present\": %u, "
                     "\"policy\": \"%s\"",
                ifs.metrics_getinstance, ifs.metrics_initialize,
                ifs.metrics_logevent, ifs.metrics_cleaninstance,
                ifs.xinput_getstate, ifs.xinput_setstate, ifs.xinput_getcaps,
                ifs.pad_absent, ifs.pad_present,
                ifs.pad_live              ? "live-pads"
                : sa2_xinput_pad_present() ? "fabricated-pad-at-rest"
                                           : "no-controller-connected");
        if (!strcmp(sa2_dinput_input_source(), "fifo")) {
            sa2_di_input_stats pin;
            sa2_dinput_input_stats_get(&pin);
            fprintf(out, ", \"pad_live\": %u, \"pad_records\": %u, \"pad_last_seq\": %u, "
                         "\"pad_seq_gaps\": %u, \"pad_changes\": %u, \"pad_wait_ms\": %u, "
                         "\"pad_waited_ms\": %u, \"pad_wait_found\": %u, \"pad_answers\": %u",
                    ifs.pad_live, pin.pad_records, pin.pad_last_seq, pin.pad_seq_gaps,
                    pin.pad_changes, pin.pad_wait_ms, pin.pad_waited_ms, pin.pad_wait_found,
                    pin.pad_answers);
            /* Hot-plug's stand-ins (runtime/sa2_dinput.h, "Pads that connect later"): the slots
             * the start-up scan was told were connected though no pad was, and the answers it
             * took; pad_live counts real pads only. */
            fprintf(out, ", \"pad_hotplug\": %u, \"pad_objects\": \"0x%x\", \"pad_stand_in_slots\": %u, "
                         "\"pad_stand_ins\": %u, \"pad_hidden\": %u",
                    pin.pad_hotplug, pin.pad_objects, pin.pad_stand_in_slots, pin.pad_stand_ins,
                    pin.pad_hidden);
        }
        fprintf(out, "},\n   \"spi_invented\": %u", sa2_spi_invented());
        fprintf(out, ",\n   \"probe\": {\"ran\": %s, \"object\": \"0x%08x\", "
                     "\"vtable\": \"0x%08x\", \"sentinel\": \"0x%08x\",\n"
                     "     \"GetAchievement\": {\"answered\": %s, \"returned\": \"0x%08x\", "
                     "\"out_after\": \"0x%08x\", \"esp_delta\": %u, \"expect_esp_delta\": 12},\n"
                     "     \"SetAchievement\": {\"answered\": %s, \"returned\": \"0x%08x\", "
                     "\"esp_delta\": %u, \"expect_esp_delta\": 8},\n"
                     "     \"StoreStats\": {\"answered\": %s, \"returned\": \"0x%08x\", "
                     "\"esp_delta\": %u, \"expect_esp_delta\": 4},\n     \"note\": ",
                steam.ran ? "true" : "false", steam.obj, steam.vtable,
                STEAM_PROBE_SENTINEL,
                steam.get_ran ? "true" : "false", steam.get_returned,
                steam.get_out_after, steam.get_esp_delta,
                steam.set_ran ? "true" : "false", steam.set_returned, steam.set_esp_delta,
                steam.store_ran ? "true" : "false", steam.store_returned,
                steam.store_esp_delta);
        jstr(out, steam.note);
        fprintf(out, "},\n   \"after_probe\": {\"method_calls\": %u, \"declined\": %u, "
                     "\"recorded\": %u, \"refused\": %u, \"invented\": %u}\n",
                ifs_after.method_calls, ifs_after.methods_declined,
                ifs_after.methods_recorded, ifs_after.methods_refused,
                ifs_after.methods_invented);
        fprintf(out, " },\n");
        /* ---- dialogs, the clipboard, and GDI ---- */
        /* Both modules record every call and neither was read by anything
         * until a review pointed out that an unread recording is not
         * evidence. The shape mirrors the interfaces block above: the counts
         * that say what *kind* of answer the guest got, then the log.
         *
         * `invented` is kept apart from `noop` in both, because "the guest
         * called n presentation stubs" and "this runtime told the guest n
         * things it does not know" are different numbers and reading them off
         * one counter is how a fabrication stops being visible. */
        {
            sa2_dlg_stats dlg;
            sa2_gdi_stats gdi;
            sa2_dialog_stats(&dlg);
            sa2_gdi_stats_get(&gdi);
            fprintf(out, " \"dialog\": {\"calls\": %u, \"measured\": %u, \"noop\": %u, "
                         "\"invented\": %u, \"refused\": %u,\n"
                         "   \"clipboard\": {\"opens\": %u, \"empties\": %u, \"sets\": %u, "
                         "\"closes\": %u, \"nested_opens\": %u, \"out_of_order\": %u,\n"
                         "     \"handles_kept\": %u, \"handles_dropped\": %u, \"open_at_exit\": %u},\n"
                         "   \"dialogs_refused\": %u, \"dlg_items_absent\": %u, "
                         "\"buttons_checked\": %u, \"beeps\": %u, \"helps\": %u,\n"
                         "   \"strings\": {\"asked\": %u, \"found\": %u, \"absent\": %u, "
                         "\"chars\": %u},\n   \"last_refusal\": ",
                    dlg.calls, dlg.measured, dlg.noop_success, dlg.invented, dlg.refused,
                    dlg.clipboard_opens, dlg.clipboard_empties, dlg.clipboard_sets,
                    dlg.clipboard_closes, dlg.clipboard_open_nested,
                    dlg.clipboard_out_of_order, dlg.clipboard_handles_kept,
                    dlg.clipboard_handles_dropped, dlg.clipboard_open,
                    dlg.dialogs_refused, dlg.dlg_items_absent, dlg.buttons_checked,
                    dlg.beeps, dlg.helps, dlg.strings_asked, dlg.strings_found,
                    dlg.strings_absent, dlg.string_chars);
            jstr(out, sa2_dialog_refusal());
            fprintf(out, ",\n   \"calls_log\": [");
            for (uint32_t i = 0; i < sa2_dialog_log_count(); i++) {
                const sa2_dlg_event *e = sa2_dialog_log_at(i);
                if (!e) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"what\": ", i ? "," : "", e->seq);
                jstr(out, e->what);
                fprintf(out, ", \"caller\": \"0x%08x\", \"args\": [\"0x%08x\", \"0x%08x\", "
                             "\"0x%08x\", \"0x%08x\"], \"ret\": \"0x%08x\", \"kind\": %u}",
                        e->caller, e->a[0], e->a[1], e->a[2], e->a[3], e->ret, e->kind);
            }
            fprintf(out, "],\n   \"clipboard_handles\": [");
            for (uint32_t i = 0; i < sa2_dialog_clipboard_count(); i++) {
                const sa2_dlg_clip *c = sa2_dialog_clipboard_at(i);
                if (!c) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"format\": %u, \"handle\": \"0x%08x\"}",
                        i ? "," : "", c->seq, c->format, c->handle);
            }
            fprintf(out, "]\n },\n");

            fprintf(out, " \"gdi\": {\"calls\": %u, \"refused\": %u, \"noop\": %u, "
                         "\"token\": %u, \"state\": %u, \"default\": %u, \"fail\": %u, "
                         "\"invented\": %u,\n"
                         "   \"dcs_created\": %u, \"dcs_adopted\": %u, \"dcs_deleted\": %u,\n"
                         "   \"fonts_created\": %u, \"brushes_created\": %u, "
                         "\"objects_deleted\": %u,\n"
                         "   \"selects\": %u, \"set_bk\": %u, \"set_text\": %u,\n"
                         "   \"text_extents\": %u, \"text_extent_chars\": %u,\n"
                         "   \"unknown_handle\": %u, \"unknown_dc\": %u, "
                         "\"deleted_while_selected\": %u, \"tokens_exhausted\": %u,\n"
                         "   \"strict\": %u, \"advance_px\": %u, \"line_px\": %u,\n"
                         "   \"calls_log\": [",
                    gdi.calls, gdi.refused, gdi.noop, gdi.token, gdi.state,
                    gdi.deflt, gdi.fail, gdi.invented,
                    gdi.dcs_created, gdi.dcs_adopted, gdi.dcs_deleted,
                    gdi.fonts_created, gdi.brushes_created, gdi.objects_deleted,
                    gdi.selects, gdi.set_bk, gdi.set_text,
                    gdi.text_extents, gdi.text_extent_chars,
                    gdi.unknown_handle, gdi.unknown_dc, gdi.deleted_while_selected,
                    gdi.tokens_exhausted, gdi.strict, gdi.advance_px, gdi.line_px);
            for (uint32_t i = 0; i < sa2_gdi_log_count(); i++) {
                const sa2_gdi_event *e = sa2_gdi_log_at(i);
                if (!e) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"what\": ", i ? "," : "", e->seq);
                jstr(out, e->what);
                fprintf(out, ", \"caller\": \"0x%08x\", \"args\": [\"0x%08x\", \"0x%08x\", "
                             "\"0x%08x\", \"0x%08x\"], \"ret\": \"0x%08x\", "
                             "\"answered\": %u, \"outc\": %u, \"out\": [%u, %u]}",
                        e->caller, e->a[0], e->a[1], e->a[2], e->a[3], e->ret,
                        e->answered, e->outc, e->out0, e->out1);
            }
            fprintf(out, "]\n },\n");
        }
        /* ---- WinMain's own loop, and the recording d3d9 device ---- */
        /* Where the guest was, not only what it touched. */
        fprintf(out, " \"recent_dispatch\": {\"total\": %u, \"newest_first\": [",
                sa2_ft_recent_count());
        for (uint32_t i = 0; i < sa2_ft_recent_count(); i++)
            fprintf(out, "%s\"0x%08x\"", i ? ", " : "", sa2_ft_recent_at(i));
        fprintf(out, "]},\n");
        fprintf(out, " \"winmain_loop\": {\n"
                     "   \"how\": \"WinMain's own path: FUN_004010D0 returned true at "
                     "0x00401d51, so WinMain fell through to its loop head at 0x00401da4 "
                     "inside the same entry-point call. This is not a mid-function entry.\",\n"
                     "   \"ran\": %s, \"init_returned_true\": %s, \"reached_its_loop\": %s,\n"
                     "   \"iterations\": %u, \"polls_total\": %u, \"polls_elsewhere\": %u,\n"
                     "   \"setthreadexecutionstate\": {\"seen\": %s, \"caller\": \"0x%08x\", "
                     "\"arg0\": \"0x%08x\"},\n"
                     "   \"systemparametersinfow\": {\"seen\": %s, \"caller\": \"0x%08x\", "
                     "\"arg0\": \"0x%08x\"},\n   \"poll_callers\": [",
                winmain_loop_ran ? "true" : "false",
                init_returned_true ? "true" : "false",
                winmain_reached_its_loop ? "true" : "false",
                winmain_iterations, pump.polls, polls_elsewhere,
                (stes_i && stes_i->calls) ? "true" : "false",
                stes_i ? stes_i->first_caller : 0u, stes_i ? stes_i->first_arg0 : 0u,
                (spi_i && spi_i->calls) ? "true" : "false",
                spi_i ? spi_i->first_caller : 0u, spi_i ? spi_i->first_arg0 : 0u);
        for (uint32_t i = 0; i < sa2_window_poll_caller_count(); i++) {
            const sa2_wnd_caller *pc = sa2_window_poll_caller_at(i);
            fprintf(out, "%s{\"caller\": \"0x%08x\", \"polls\": %u}", i ? ", " : "",
                    pc->caller, pc->polls);
        }
        fprintf(out, "]},\n");

        fprintf(out, " \"d3d9\": {\"device_enabled\": %s, \"strict_caps\": %s,\n"
                     "   \"creates\": %u, \"method_calls\": %u, \"distinct_methods\": %u,\n"
                     "   \"objects_built\": %u, \"object_stride\": %u, \"object_fill\": %u, "
                     "\"guest_bytes_allocated\": %u, \"calls_on_unknown_this\": %u,\n"
                     "   \"lifetime\": {\"objects_destroyed\": %u, \"guest_bytes_freed\": %u, "
                     "\"level_surfaces\": %u, \"calls_on_destroyed\": %u, "
                     "\"calls_on_destroyed_first_caller\": \"0x%08x\", "
                     "\"releases_past_zero\": %u, "
                     "\"releases_past_zero_first_caller\": \"0x%08x\"},\n"
                     "   \"answers\": {\"refused\": %u, \"noop\": %u, \"from_guest\": %u, "
                     "\"invented\": %u, \"object\": %u, \"unset_default\": %u},\n"
                     "   \"frames\": {\"present\": %u, \"begin_scene\": %u, \"end_scene\": %u, "
                     "\"clear\": %u, \"draw\": %u, \"waits_at_begin_scene\": %u, "
                     "\"waits_at_present\": %u},\n",
                sa2_d3d9_device_enabled() ? "true" : "false", d3d.strict_caps ? "true" : "false",
                d3d.created, d3d.method_calls, d3d.distinct_methods,
                d3d.objects_built, d3d.object_stride, d3d.object_fill,
                d3d.guest_bytes_allocated, d3d.unknown_this,
                d3d.objects_destroyed, d3d.guest_bytes_freed, d3d.level_surfaces,
                d3d.calls_on_destroyed, d3d.calls_on_destroyed_first_caller,
                d3d.releases_past_zero, d3d.releases_past_zero_first_caller,
                d3d.refused, d3d.noop, d3d.from_guest, d3d.invented, d3d.objects, d3d.unset,
                d3d.present_calls, d3d.beginscene_calls, d3d.endscene_calls,
                d3d.clear_calls, d3d.draw_calls, d3d.frame_waits_beginscene,
                d3d.frame_waits_present);
        /* Every ID3DXConstantTable D3DXGetShaderConstantTable answered with
         * (SA2_D3DX_CONSTANT_TABLE), in the order it built them: the header
         * numbers the oracle's d3dx9 trace prints for the same call, the count
         * each table stands at, and who made its final Release and when, in
         * the device's method sequence, beside the sequence number of the
         * run's last Present. Its method calls are in the method surface
         * below, like every interface's. */
        fprintf(out, "   \"constant_tables\": {\"switch\": %s, \"built\": %u, \"destroyed\": %u, "
                     "\"qi_answered\": %u, \"qi_declined\": %u, \"rows_dropped\": %u, "
                     "\"last_present_seq\": %u,\n"
                     "     \"rows\": [",
                d3d.constant_table ? "true" : "false", d3d.constant_tables_built,
                d3d.constant_tables_destroyed, d3d.constant_table_qi_answered,
                d3d.constant_table_qi_declined, sa2_d3d9_ctab_rows_dropped(),
                d3d.last_present_seq);
        for (uint32_t i = 0; i < sa2_d3d9_ctab_count(); i++) {
            const sa2_d3d9_ctab *r = sa2_d3d9_ctab_at(i);
            if (!r) break;
            fprintf(out, "%s\n      {\"seq\": %u, \"caller\": \"0x%08x\", \"bytecode\": \"0x%08x\", "
                         "\"table\": \"0x%08x\", \"version\": \"0x%08x\", \"constants\": %u, "
                         "\"ctab_bytes\": %u, \"refs\": %u, \"released_by\": \"0x%08x\", "
                         "\"released_seq\": %u}",
                    i ? "," : "", r->seq, r->caller, r->bytecode, r->table, r->version,
                    r->constants, r->ctab_bytes, r->refs, r->released_by, r->released_seq);
        }
        fprintf(out, "]},\n");
        /* Every texture the guest built from a blob it handed to d3dx9, by
         * hash, with the header fields each number came from and the explicit
         * record that no pixel of the blob was decoded into the surface. The
         * refused rows are here too: a refusal is this stage's work item and
         * the run that ends in one is the run whose numbers matter most.
         *
         * `bytes` and `sha256` are the blob's; no byte of it is written here.
         * `surface_nonzero_bytes` is counted out of guest memory now, so "the
         * surface holds no decoded image" is checked rather than declared. */
        fprintf(out, "   \"textures_from_files\": {\"answered\": %u, \"refused\": %u,\n"
                     "     \"source_bytes_hashed\": %u, \"with_decoded_pixels\": %u, "
                     "\"rows_dropped\": %u,\n"
                     "     \"rows_counted\": %u, \"unlocked_surfaces_holding_bytes\": %u, "
                     "\"guest_copied_rows_left_out\": %u,\n"
                     "     \"rows\": [",
                d3d.tex_from_file, d3d.tex_from_file_refused, d3d.tex_bytes_hashed,
                d3d.tex_with_decoded_pixels, d3d.tex_rows_dropped, tex_counted,
                tex_unlocked_nonzero, tex_guest_copied);
        for (uint32_t i = 0; i < sa2_d3d9_texsrc_count(); i++) {
            const sa2_d3d9_texsrc *t = sa2_d3d9_texsrc_at(i);
            if (!t) break;
            char sha[65], mag[17];
            for (unsigned k = 0; k < 32; k++) snprintf(sha + 2 * k, 3, "%02x", t->sha256[k]);
            for (unsigned k = 0; k < 8; k++)  snprintf(mag + 2 * k, 3, "%02x", t->magic[k]);
            fprintf(out, "%s\n      {\"seq\": %u, \"caller\": \"0x%08x\", \"container\": ",
                    i ? "," : "", t->seq, t->caller);
            jstr(out, sa2_d3d9_container_name(t->container));
            fprintf(out, ", \"magic\": \"%s\",\n"
                         "       \"bytes\": %u, \"sha256\": \"%s\", \"hashed_bytes\": %u,\n"
                         "       \"parsed\": %s, \"refused\": %s, \"texture\": \"0x%08x\",\n"
                         "       \"file\": {\"w\": %u, \"h\": %u, \"depth\": %u, \"levels\": %u, "
                         "\"format\": %u},\n"
                         "       \"png\": {\"bit_depth\": %u, \"colour_type\": %u, "
                         "\"interlace\": %u, \"trns\": %u},\n"
                         "       \"asked\": {\"w\": \"0x%08x\", \"h\": \"0x%08x\", "
                         "\"levels\": \"0x%08x\", \"format\": \"0x%08x\"},\n"
                         "       \"created\": {\"w\": %u, \"h\": %u, \"levels\": %u, "
                         "\"format\": %u, \"level0_bytes\": %u, \"chain_bytes\": %u},\n"
                         "       \"surface\": \"0x%08x\", \"surface_bytes_reserved\": %u,\n"
                         "       \"surface_nonzero_bytes\": %u, \"locks\": %u, "
                         "\"released\": %s, \"guest_copied\": %s,\n"
                         "       \"pixels_decoded\": %s, \"runtime_wrote_pixels\": %s,\n"
                         "       \"dimensions_from\": ",
                    mag, t->src_len, sha, t->hashed_bytes,
                    t->parsed ? "true" : "false", t->refused ? "true" : "false", t->obj,
                    t->file_w, t->file_h, t->file_depth, t->file_levels, t->file_fmt,
                    t->png_bit_depth, t->png_colour_type, t->png_interlace, t->png_trns,
                    t->want_w, t->want_h, t->want_levels, t->want_fmt,
                    t->w, t->h, t->levels, t->fmt, t->level0_bytes, t->chain_bytes,
                    t->surface, t->reserved, sa2_d3d9_texsrc_nonzero(i), t->locks,
                    t->released ? "true" : "false", t->guest_copied ? "true" : "false",
                    t->pixels_decoded ? "true" : "false",
                    t->runtime_wrote_pixels ? "true" : "false");
            jstr(out, sa2_d3d9_provenance_name(t->dim_from));
            fprintf(out, ", \"format_from\": ");
            jstr(out, sa2_d3d9_provenance_name(t->fmt_from));
            fprintf(out, ", \"levels_from\": ");
            jstr(out, sa2_d3d9_provenance_name(t->levels_from));
            fprintf(out, "}");
        }
        fprintf(out, "]},\n");
        /* Every D3DCAPS9 field this runtime wrote, by name. A capability the
         * guest branches on has to be readable in the run's own record, or the
         * only place the renderer tier is written down is the source. */
        fprintf(out, "   \"shader_model\": %u, \"caps_fields_written\": [", d3d.shader_model);
        for (uint32_t i = 0; ; i++) {
            uint32_t off = 0, val = 0; const char *nm = NULL; int fg = 0;
            if (!sa2_d3d9_caps_field_at(i, &off, &nm, &val, &fg)) break;
            fprintf(out, "%s\n     {\"offset\": \"0x%02x\", \"field\": ", i ? "," : "", off);
            jstr(out, nm ? nm : "?");
            fprintf(out, ", \"value\": \"0x%08x\", \"source\": ", val);
            jstr(out, fg ? "from-guest" : "invented");
            fprintf(out, "}");
        }
        fprintf(out, "],\n");
        /* The dynamic method surface: one row per (interface, slot) the guest
         * actually called, in the order it first called each, with an exact
         * count. This is the half of M7's question a static scan cannot see. */
        fprintf(out, "   \"dynamic_surface\": [");
        {
            /* Ordered by the sequence number of each method's first call, by
             * repeatedly taking the smallest not yet emitted. Bounded by the
             * table (15 interfaces x 128 slots), not by the call count, so a
             * long run costs nothing here. */
            uint32_t emitted = 0, last = 0;
            for (;;) {
                uint32_t bt = 0, bk = 0, best = 0xffffffffu;
                for (uint32_t t = 0; t < sa2_d3d9_type_count(); t++)
                    for (uint32_t k = 0; k < SA2_D3D9_MAX_SLOTS; k++) {
                        const sa2_d3d9_slot_stat *st = sa2_d3d9_slot_stat_at(t, k);
                        if (!st || !st->used) continue;
                        if (st->first_seq <= last || st->first_seq >= best) continue;
                        best = st->first_seq; bt = t; bk = k;
                    }
                if (best == 0xffffffffu) break;
                last = best;
                const sa2_d3d9_slot_stat *st = sa2_d3d9_slot_stat_at(bt, bk);
                const char *nm = sa2_d3d9_method_name(bt, bk);
                /* Two statements: `emitted ? ... : ...` beside `++emitted` in one
                 * argument list is unsequenced (the P3 integration review). */
                fprintf(out, "%s\n     {\"order\": %u, \"interface\": ",
                        emitted ? "," : "", emitted + 1u);
                emitted++;
                jstr(out, sa2_d3d9_type_name(bt));
                fprintf(out, ", \"method\": ");
                jstr(out, nm ? nm : "<unnamed slot>");
                fprintf(out, ", \"slot\": %u, \"argc\": %d, \"calls\": %u, "
                             "\"first_caller\": \"0x%08x\", \"first_answer\": ",
                        bk, sa2_d3d9_method_argc(bt, bk), st->calls, st->first_caller);
                jstr(out, answer_name(st->answered));
                fprintf(out, "}");
            }
        }
        fprintf(out, "],\n   \"calls\": [");
        for (uint32_t i = 0; i < sa2_d3d9_log_count(); i++) {
            const sa2_d3d9_call *c = sa2_d3d9_log_at(i);
            if (!c) break;
            fprintf(out, "%s\n     {\"seq\": %u, \"interface\": ", i ? "," : "", c->seq);
            jstr(out, sa2_d3d9_type_name(c->type));
            fprintf(out, ", \"method\": ");
            {
                const char *nm = sa2_d3d9_method_name(c->type, c->slot);
                jstr(out, nm ? nm : "<unnamed slot>");
            }
            fprintf(out, ", \"slot\": %u, \"caller\": \"0x%08x\", \"this\": \"0x%08x\", "
                         "\"args\": [", c->slot, c->caller, c->self);
            for (uint32_t k = 0; k < c->argc; k++)
                fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
            fprintf(out, "], \"ret\": \"0x%08x\", \"answered\": ", c->ret);
            jstr(out, answer_name(c->answered));
            fprintf(out, "}");
        }
        fprintf(out, "],\n   \"implemented\": [");
        {
            uint32_t n = 0;
            for (uint32_t t = 0; t < sa2_d3d9_type_count(); t++)
                for (uint32_t k = 0; k < SA2_D3D9_MAX_SLOTS; k++) {
                    const char *nm = sa2_d3d9_method_name(t, k);
                    if (!nm) continue;
                    const sa2_d3d9_slot_stat *st = sa2_d3d9_slot_stat_at(t, k);
                    fprintf(out, "%s\n     {\"interface\": ", n++ ? "," : "");
                    jstr(out, sa2_d3d9_type_name(t));
                    fprintf(out, ", \"method\": ");
                    jstr(out, nm);
                    fprintf(out, ", \"slot\": %u, \"argc\": %d, \"implemented\": %s, "
                                 "\"calls\": %u, \"flags\": ",
                            k, sa2_d3d9_method_argc(t, k),
                            sa2_d3d9_method_implemented(t, k) ? "true" : "false",
                            st ? st->calls : 0u);
                    jstr(out, sa2_d3d9_method_flags(t, k));
                    fprintf(out, "}");
                }
        }
        /* The closed world (runtime/sa2_d3d9.c; the user's ruling of 2026-10-09, item 1):
         * the mode, the M7 list the device held its calls to, and every method off it
         * the guest called, in the order of its first call, with its count. In trap mode
         * the first such call ended the run, and the trap above names it too. */
        fprintf(out, "],\n   ");
        sa2_d3d9_closed_world_json(out);
        fprintf(out, "},\n");
        /* ---- the recording DirectSound device ---- */
        {
            sa2_ds_stats ds;
            sa2_dsound_stats_get(&ds);
            fprintf(out, " \"dsound\": {\"device_enabled\": %s, \"strict_caps\": %s,\n"
                         "   \"creates\": %u, \"method_calls\": %u, \"distinct_methods\": %u,\n"
                         "   \"objects_built\": %u, \"guest_bytes_allocated\": %u, "
                         "\"calls_on_unknown_this\": %u,\n"
                         "   \"answers\": {\"refused\": %u, \"noop\": %u, \"from_guest\": %u, "
                         "\"invented\": %u, \"object\": %u, \"unset_default\": %u, "
                         "\"clock_derived\": %u},\n"
                         "   \"buffers\": %u, \"primary_buffers\": %u, "
                         "\"cooperative_level\": %u,\n"
                         "   \"query_interface\": {\"answered\": %u, \"declined\": %u},\n"
                         "   \"cursor\": {\"reads\": %u, \"wraps\": %u, \"went_backwards\": %u, "
                         "\"write_lead_us\": %u},\n"
                         "   \"reuse\": {\"stores_reused\": %u, \"objects_reused\": %u, "
                         "\"stores_kept_locked\": %u, \"calls_on_released\": %u, "
                         "\"stores_reused_dirty\": %u},\n",
                    sa2_dsound_device_enabled() ? "true" : "false",
                    ds.strict_caps ? "true" : "false",
                    ds.created, ds.method_calls, ds.distinct_methods,
                    ds.objects_built, ds.guest_bytes_allocated, ds.unknown_this,
                    ds.refused, ds.noop, ds.from_guest, ds.invented, ds.objects,
                    ds.unset, ds.clock,
                    ds.buffers, ds.primary_buffers, ds.cooperative_level,
                    ds.qi_answered, ds.qi_declined,
                    ds.cursor_reads, ds.cursor_wraps, ds.cursor_went_backwards,
                    ds.write_lead_us,
                    ds.stores_reused, ds.objects_reused, ds.stores_kept_locked,
                    ds.calls_on_released, ds.stores_reused_dirty);
            /* One row per buffer record the device keeps (sa2_dsound.h,
             * sa2_ds_buffer): one per sound buffer for the first 511 made. Past
             * that a new buffer takes the row of the released buffer whose slot
             * it is handed, so a row shows the latest buffer in its slot and
             * there are fewer rows than `buffers` above. Several rows can name
             * one backing_store: a buffer and its duplicates, and a released
             * buffer and the later buffer its block was handed to.
             *
             * Each row has the cursor arithmetic's own inputs beside its
             * outputs: a reader can recompute
             * `elapsed * avg_bytes_per_sec / 1000000` from this row and get
             * the same byte, which is the only way "the cursor advances at the
             * declared rate" is a checkable claim rather than a sentence. */
            fprintf(out, "   \"sound_buffers\": [");
            for (uint32_t i = 0; i < sa2_dsound_buffer_count(); i++) {
                const sa2_ds_buffer *b = sa2_dsound_buffer_at(i);
                uint32_t play = 0, write = 0;
                sa2_dsound_buffer_cursors(i, &play, &write);
                if (!b) break;
                fprintf(out, "%s\n     {\"index\": %u, \"this\": \"0x%08x\", "
                             "\"created_from\": \"0x%08x\", \"primary\": %s,\n"
                             "      \"flags\": \"0x%08x\", \"bytes\": %u, "
                             "\"format\": {\"tag\": %u, \"channels\": %u, "
                             "\"samples_per_sec\": %u, \"avg_bytes_per_sec\": %u, "
                             "\"block_align\": %u, \"bits\": %u, \"from_guest\": %s},\n"
                             "      \"write_lead_bytes\": %u, \"playing\": %s, "
                             "\"looping\": %s, \"halted_at_end\": %s, \"released\": %s,\n"
                             "      \"anchor_us\": %llu, \"anchor_pos\": %u, "
                             "\"now_us\": %llu, \"play_cursor\": %u, \"write_cursor\": %u,\n"
                             "      \"calls\": {\"play\": %u, \"stop\": %u, "
                             "\"get_position\": %u, \"set_position\": %u, \"lock\": %u, "
                             "\"unlock\": %u}, \"bytes_locked\": %llu, "
                             "\"backing_store\": \"0x%08x\", \"backing_bytes\": %u}",
                        i ? "," : "", i, b->addr, b->create_caller,
                        b->primary ? "true" : "false",
                        b->flags, b->bytes, b->format_tag, b->channels,
                        b->samples_per_sec, b->avg_bytes_per_sec, b->block_align,
                        b->bits_per_sample, b->format_from_guest ? "true" : "false",
                        b->write_lead, b->playing ? "true" : "false",
                        b->looping ? "true" : "false", b->halted ? "true" : "false",
                        b->released ? "true" : "false",
                        (unsigned long long)b->anchor_us, b->anchor_pos,
                        (unsigned long long)sa2_virtual_us(), play, write,
                        b->play_calls, b->stop_calls, b->getpos_calls,
                        b->setpos_calls, b->lock_calls, b->unlock_calls,
                        (unsigned long long)b->bytes_locked, b->mem, b->mem_len);
            }
            fprintf(out, "],\n   \"calls\": [");
            for (uint32_t i = 0; i < sa2_dsound_log_count(); i++) {
                const sa2_ds_call *c = sa2_dsound_log_at(i);
                if (!c) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"virtual_us\": %llu, \"interface\": ",
                        i ? "," : "", c->seq, (unsigned long long)c->virtual_us);
                jstr(out, sa2_dsound_type_name(c->type));
                fprintf(out, ", \"method\": ");
                {
                    const char *nm = sa2_dsound_method_name(c->type, c->slot);
                    jstr(out, nm ? nm : "<unnamed slot>");
                }
                fprintf(out, ", \"slot\": %u, \"caller\": \"0x%08x\", \"this\": \"0x%08x\", "
                             "\"args\": [", c->slot, c->caller, c->self);
                for (uint32_t k = 0; k < c->argc; k++)
                    fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
                fprintf(out, "], \"ret\": \"0x%08x\", \"out\": [\"0x%08x\", \"0x%08x\"], "
                             "\"answered\": ", c->ret, c->out0, c->out1);
                jstr(out, ds_answer_name(c->answered));
                fprintf(out, "}");
            }
            /* Every named slot with its declared argument count, which is what
             * `tools/s4/dsound_surface.py --argc` compares against the dwords
             * the image pushes. The runtime publishes the counts rather than
             * the tool re-deriving them from the source, so the thing checked
             * is the thing that runs. */
            fprintf(out, "],\n   \"methods\": [");
            {
                uint32_t n = 0;
                for (uint32_t t = 0; t < sa2_dsound_type_count(); t++)
                    for (uint32_t k = 0; k < SA2_DS_MAX_SLOTS; k++) {
                        const char *nm = sa2_dsound_method_name(t, k);
                        if (!nm) continue;
                        const sa2_ds_slot_stat *st = sa2_dsound_slot_stat_at(t, k);
                        fprintf(out, "%s\n     {\"interface\": ", n++ ? "," : "");
                        jstr(out, sa2_dsound_type_name(t));
                        fprintf(out, ", \"name\": ");
                        jstr(out, nm);
                        fprintf(out, ", \"slot\": %u, \"argc\": %d, \"implemented\": %s, "
                                     "\"calls\": %u, \"first_caller\": \"0x%08x\", "
                                     "\"flags\": ",
                                k, sa2_dsound_method_argc(t, k),
                                sa2_dsound_method_implemented(t, k) ? "true" : "false",
                                st ? st->calls : 0u, st ? st->first_caller : 0u);
                        jstr(out, sa2_dsound_method_flags(t, k));
                        fprintf(out, "}");
                    }
            }
            fprintf(out, "]},\n");
        }
        /* ---- the recording XAudio2 engine ---- */
        {
            sa2_xa2_stats xs;
            sa2_xaudio2_stats_get(&xs);
            fprintf(out, " \"xaudio2\": {\"device_enabled\": %s, \"break\": ",
                    sa2_xaudio2_device_enabled() ? "true" : "false");
            jstr(out, sa2_xaudio2_break_name());
            fprintf(out, ",\n   \"cocreate_calls\": %u, \"cocreate_refused\": %u, "
                         "\"engines\": %u,\n"
                         "   \"method_calls\": %u, \"distinct_methods\": %u, "
                         "\"objects_built\": %u, \"calls_on_unknown_this\": %u, "
                         "\"calls_on_dead_this\": %u,\n"
                         "   \"answers\": {\"refused\": %u, \"noop\": %u, \"from_guest\": %u, "
                         "\"from_profile\": %u, \"object\": %u, \"clock_derived\": %u},\n"
                         "   \"mastering_voices\": %u, \"source_voices\": %u, "
                         "\"voices_destroyed\": %u,\n"
                         "   \"submits\": %u, \"submitted_bytes\": %llu, "
                         "\"buffers_completed\": %u, \"getstate_calls\": %u, "
                         "\"starvations\": %u, \"frames_played\": %llu, \"log_dropped\": %u,\n",
                    xs.cocreate_calls, xs.cocreate_refused, xs.engines,
                    xs.method_calls, xs.distinct_methods, xs.objects_built,
                    xs.unknown_this, xs.dead_this,
                    xs.refused, xs.noop, xs.from_guest, xs.from_profile, xs.objects, xs.clock,
                    xs.mastering_voices, xs.source_voices, xs.voices_destroyed,
                    xs.submits, (unsigned long long)xs.submitted_bytes,
                    xs.buffers_completed, xs.getstate_calls, xs.starvations,
                    (unsigned long long)xs.frames_played, xs.log_dropped);
            fprintf(out, "   \"engine_objects\": [");
            for (uint32_t i = 0; i < sa2_xaudio2_engine_count(); i++) {
                const sa2_xa2_engine *e = sa2_xaudio2_engine_at(i);
                if (!e) break;
                fprintf(out, "%s\n     {\"this\": \"0x%08x\", \"created_from\": \"0x%08x\", "
                             "\"initialized\": %s, \"init_flags\": \"0x%08x\", "
                             "\"processor\": \"0x%08x\", \"refs\": %u, \"released\": %s, "
                             "\"release_calls\": %u, \"master\": \"0x%08x\", "
                             "\"source_voices\": %u}",
                        i ? "," : "", e->addr, e->create_caller,
                        e->initialized ? "true" : "false", e->init_flags, e->init_processor,
                        e->refs, e->released ? "true" : "false", e->release_calls, e->master,
                        e->source_voices);
            }
            /* One row per voice, with the clock arithmetic's inputs beside its
             * outputs: `anchor_frame + elapsed_us * samples_per_sec * ratio /
             * 1e6` can be redone from the row. */
            fprintf(out, "],\n   \"voices\": [");
            for (uint32_t i = 0; i < sa2_xaudio2_voice_count(); i++) {
                const sa2_xa2_voice *v = sa2_xaudio2_voice_at(i);
                if (!v) break;
                uint32_t q_now = 0;
                uint64_t played_now = 0;
                const int is_source = sa2_xaudio2_voice_now(i, &q_now, &played_now);
                fprintf(out, "%s\n     {\"index\": %u, \"this\": \"0x%08x\", \"kind\": \"%s\", "
                             "\"engine\": \"0x%08x\", \"created_from\": \"0x%08x\", "
                             "\"flags\": \"0x%08x\", \"destroyed\": %s, "
                             "\"destroyed_from\": \"0x%08x\",\n"
                             "      \"details_flags\": \"0x%08x\", \"input_channels\": %u, "
                             "\"input_sample_rate\": %u, \"details_calls\": %u,\n"
                             "      \"format\": {\"tag\": \"0x%04x\", \"channels\": %u, "
                             "\"samples_per_sec\": %u, \"avg_bytes_per_sec\": %u, "
                             "\"block_align\": %u, \"bits\": %u, \"cb_size\": %u, "
                             "\"valid_bits\": %u, \"channel_mask\": \"0x%08x\"},\n"
                             "      \"max_ratio_bits\": \"0x%08x\", \"ratio_bits\": \"0x%08x\", "
                             "\"output\": \"0x%08x\", \"matrix\": {\"src\": %u, \"dst\": %u, "
                             "\"calls\": %u, \"levels\": [",
                        i ? "," : "", i, v->addr,
                        v->kind == SA2_XA2_VOICE_MASTER ? "mastering" : "source",
                        v->engine, v->create_caller, v->flags,
                        v->destroyed ? "true" : "false", v->destroy_caller,
                        v->details_flags, v->input_channels, v->input_sample_rate,
                        v->details_calls,
                        v->format_tag, v->channels, v->samples_per_sec, v->avg_bytes_per_sec,
                        v->block_align, v->bits_per_sample, v->cb_size, v->valid_bits,
                        v->channel_mask, v->max_ratio_bits, v->ratio_bits, v->output,
                        v->matrix_src, v->matrix_dst, v->matrix_calls);
                for (uint32_t k = 0; k < v->matrix_src * v->matrix_dst && k < 12u; k++)
                    fprintf(out, "%s\"0x%08x\"", k ? ", " : "", v->matrix[k]);
                fprintf(out, "]},\n"
                             "      \"running\": %s, \"anchor_us\": %llu, \"anchor_frame\": %llu, "
                             "\"now_us\": %llu, \"queued_now\": %u, \"played_now\": %llu, "
                             "\"queued_end\": %llu,\n"
                             "      \"submits\": %u, \"submitted_bytes\": %llu, "
                             "\"completed\": %u, \"flushed\": %u, \"dropped_at_destroy\": %u, "
                             "\"max_queued\": %u, \"starvations\": %u,\n"
                             "      \"calls\": {\"start\": %u, \"stop\": %u, \"flush\": %u, "
                             "\"get_state\": %u, \"set_frequency_ratio\": %u},\n"
                             "      \"last_state\": {\"queued\": %u, \"samples_played\": %llu, "
                             "\"at_us\": %llu}}",
                        v->running ? "true" : "false", (unsigned long long)v->anchor_us,
                        (unsigned long long)v->anchor_frame,
                        (unsigned long long)sa2_virtual_us(), is_source ? q_now : 0u,
                        (unsigned long long)(is_source ? played_now : 0u),
                        (unsigned long long)v->queued_end, v->submits,
                        (unsigned long long)v->submitted_bytes, v->completed, v->flushed,
                        v->dropped_at_destroy, v->max_queued, v->starvations,
                        v->start_calls, v->stop_calls, v->flush_calls, v->getstate_calls,
                        v->ratio_calls, v->last_queued, (unsigned long long)v->last_samples,
                        (unsigned long long)v->last_state_us);
            }
            fprintf(out, "],\n   \"calls\": [");
            for (uint32_t i = 0; i < sa2_xaudio2_log_count(); i++) {
                const sa2_xa2_call *c = sa2_xaudio2_log_at(i);
                if (!c) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"virtual_us\": %llu, \"thread\": %u, "
                             "\"interface\": ", i ? "," : "", c->seq,
                        (unsigned long long)c->virtual_us, c->thread);
                jstr(out, sa2_xaudio2_type_name(c->type));
                fprintf(out, ", \"method\": ");
                {
                    const char *nm = sa2_xaudio2_method_name(c->type, c->slot);
                    jstr(out, nm ? nm : "<unnamed slot>");
                }
                fprintf(out, ", \"slot\": %u, \"caller\": \"0x%08x\", \"this\": \"0x%08x\", "
                             "\"args\": [", c->slot, c->caller, c->self);
                for (uint32_t k = 0; k < c->argc; k++)
                    fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
                fprintf(out, "], \"ret\": \"0x%08x\", \"out\": [\"0x%08x\", \"0x%08x\"], "
                             "\"answered\": ", c->ret, c->out0, c->out1);
                jstr(out, xa2_answer_name(c->answered));
                fprintf(out, "}");
            }
            /* Every named slot with its declared argument count, which
             * `tools/s4/xaudio2_surface.py --argc` compares against the dwords
             * the image pushes. The runtime publishes the counts, so the thing
             * checked is the thing that ran, under whatever break it set. */
            fprintf(out, "],\n   \"methods\": [");
            {
                uint32_t n = 0;
                for (uint32_t t = 0; t < sa2_xaudio2_type_count(); t++)
                    for (uint32_t k = 0; k < SA2_XA2_MAX_SLOTS; k++) {
                        const char *nm = sa2_xaudio2_method_name(t, k);
                        if (!nm) continue;
                        const sa2_xa2_slot_stat *st = sa2_xaudio2_slot_stat_at(t, k);
                        fprintf(out, "%s\n     {\"interface\": ", n++ ? "," : "");
                        jstr(out, sa2_xaudio2_type_name(t));
                        fprintf(out, ", \"name\": ");
                        jstr(out, nm);
                        fprintf(out, ", \"slot\": %u, \"argc\": %d, \"implemented\": %s, "
                                     "\"calls\": %u, \"first_caller\": \"0x%08x\", \"flags\": ",
                                k, sa2_xaudio2_method_argc(t, k),
                                sa2_xaudio2_method_implemented(t, k) ? "true" : "false",
                                st ? st->calls : 0u, st ? st->first_caller : 0u);
                        jstr(out, sa2_xaudio2_method_flags(t, k));
                        fprintf(out, "}");
                    }
            }
            fprintf(out, "]},\n");
        }
        /* ---- the device profile the run read (runtime/sa2_devprofile.h) ----
         * The source and the sha256 of the bytes the runtime parsed, so a
         * replay tool holds the run to the capture it stands for
         * (tools/s4/device_profile.py check_run), not to what it meant to pass. */
        {
            uint32_t connected = 0;
            for (uint32_t s = 0; s < SA2_DP_XINPUT_SLOTS; s++)
                connected += sa2_dp_xinput_caps_hr(s) == 0u;
            fprintf(out, " \"device_profile\": {\"source\": ");
            jstr(out, sa2_devprofile_source());
            fprintf(out, ", \"default\": %s, \"sha256\": ",
                    sa2_devprofile_is_default() ? "true" : "false");
            jstr(out, sa2_devprofile_sha256());
            fprintf(out, ",\n   \"processors\": %u, \"process_mask\": \"0x%08x\", "
                         "\"system_mask\": \"0x%08x\",\n   \"counts\": {\"gamectrl\": %u, "
                         "\"xinput_connected\": %u, \"wmi_pnp\": %u, \"ddraw\": %u}},\n",
                    sa2_dp_processors(), sa2_dp_process_mask(), sa2_dp_system_mask(),
                    sa2_dp_gamectrl_count(), connected, sa2_dp_wmi_pnp_count(),
                    sa2_dp_ddraw_count());
        }
        /* ---- the recording DirectInput device, and the WMI objects behind it ----
         *
         * An enumerated controller is named by its index in the device profile
         * and never by its GUID, and no DeviceID is printed: both are the
         * recording host's inventory, which lives only in the private capture's
         * profile, and a run record's fields end up in receipts. */
        {
            sa2_di_stats di;
            sa2_dinput_stats_get(&di);
            fprintf(out, " \"dinput\": {\"device_enabled\": %s, \"break\": ",
                    sa2_dinput_device_enabled() ? "true" : "false");
            jstr(out, sa2_dinput_break_name());
            fprintf(out, ", \"profile\": ");
            jstr(out, sa2_devprofile_source());
            fprintf(out, ", \"profile_sha256\": ");
            jstr(out, sa2_devprofile_sha256());
            fprintf(out, ",\n   \"create\": {\"calls\": %u, \"answered_with_object\": %u, "
                         "\"hinst\": \"0x%08x\", \"version\": \"0x%08x\", \"caller\": \"0x%08x\", "
                         "\"ret\": \"0x%08x\"},\n"
                         "   \"method_calls\": %u, \"distinct_methods\": %u, \"objects_built\": %u, "
                         "\"calls_on_unknown_this\": %u, \"log_dropped\": %u,\n"
                         "   \"answers\": {\"refused\": %u, \"noop\": %u, \"from_guest\": %u, "
                         "\"object\": %u, \"idle\": %u, \"native_error\": %u, "
                         "\"from_profile\": %u, \"named_break\": %u},\n"
                         "   \"profile_gamectrl\": %u, \"enumerations\": %u, "
                         "\"devices_enumerated\": %u, \"guest_stdcalls\": %u, "
                         "\"guest_stdcall_max_depth\": %u,\n"
                         "   \"state_bytes_written\": %llu, \"state_answers_nonidle\": %u,\n",
                    di.create_calls, di.created, di.create_last_hinst, di.create_last_version,
                    di.create_last_caller, di.create_last_ret,
                    di.method_calls, di.distinct_methods, di.objects_built, di.unknown_this,
                    di.log_dropped,
                    di.refused, di.noop, di.from_guest, di.object, di.idle, di.invalid,
                    di.profile, di.brk,
                    di.profile_gamectrl, di.enumerations, di.devices_enumerated,
                    sa2_guest_stdcall_count(), sa2_guest_stdcall_max_depth(),
                    (unsigned long long)di.state_bytes_written, di.state_answers_nonidle);
            fprintf(out, "   \"devices\": [");
            for (uint32_t i = 0; i < sa2_dinput_device_count(); i++) {
                const sa2_di_device *d = sa2_dinput_device_at(i);
                if (!d) break;
                fprintf(out, "%s\n     {\"index\": %u, \"this\": \"0x%08x\", \"kind\": \"%s\", "
                             "\"profile_index\": %d, \"created_from\": \"0x%08x\", "
                             "\"create_hr\": \"0x%08x\",\n"
                             "      \"format\": {\"ptr\": \"0x%08x\", \"size\": %u, "
                             "\"obj_size\": %u, \"flags\": \"0x%08x\", \"data_size\": %u, "
                             "\"objects\": %u, \"rgodf\": \"0x%08x\"},\n"
                             "      \"coop\": {\"hwnd\": \"0x%08x\", \"flags\": \"0x%08x\", "
                             "\"hwnd_is_a_window\": %s}, \"acquired\": %s,\n"
                             "      \"calls\": {\"set_format\": %u, \"coop\": %u, \"acquire\": %u, "
                             "\"unacquire\": %u, \"state\": %u, \"state_ok\": %u}, "
                             "\"state_bytes_written\": %llu, \"refs\": %u, \"released\": %s}",
                        i ? "," : "", i, d->addr,
                        d->kind == SA2_DI_KIND_KEYBOARD ? "keyboard"
                        : d->kind == SA2_DI_KIND_MOUSE ? "mouse" : "enumerated",
                        d->profile_index == 0xFFFFFFFFu ? -1 : (int)d->profile_index,
                        d->create_caller, d->create_hr,
                        d->format_ptr, d->format_size, d->format_obj_size, d->format_flags,
                        d->format_data_size, d->format_objects, d->format_rgodf,
                        d->coop_hwnd, d->coop_flags, d->coop_hwnd_is_a_window ? "true" : "false",
                        d->acquired ? "true" : "false",
                        d->set_format_calls, d->coop_calls, d->acquire_calls,
                        d->unacquire_calls, d->state_calls, d->state_ok,
                        (unsigned long long)d->state_bytes_written, d->refs,
                        d->released ? "true" : "false");
            }
            fprintf(out, "],\n   \"enumerations_made\": [");
            for (uint32_t i = 0; i < sa2_dinput_enum_count(); i++) {
                const sa2_di_enum *e = sa2_dinput_enum_at(i);
                if (!e) break;
                fprintf(out, "%s\n     {\"caller\": \"0x%08x\", \"class\": %u, "
                             "\"flags\": \"0x%08x\", \"callback\": \"0x%08x\", "
                             "\"ref\": \"0x%08x\", \"offered\": %u, \"stopped\": %s, "
                             "\"last_callback_ret\": \"0x%08x\"}",
                        i ? "," : "", e->caller, e->dev_type, e->flags, e->callback, e->ref,
                        e->offered, e->stopped ? "true" : "false", e->last_callback_ret);
            }
            fprintf(out, "],\n   \"calls\": [");
            for (uint32_t i = 0; i < sa2_dinput_log_count(); i++) {
                const sa2_di_call *c = sa2_dinput_log_at(i);
                if (!c) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"depth\": %u, \"interface\": ",
                        i ? "," : "", c->seq, c->depth);
                jstr(out, sa2_dinput_type_name(c->type));
                fprintf(out, ", \"method\": ");
                {
                    const char *nm = sa2_dinput_method_name(c->type, c->slot);
                    jstr(out, nm ? nm : "<unnamed slot>");
                }
                fprintf(out, ", \"slot\": %u, \"caller\": \"0x%08x\", \"this\": \"0x%08x\", "
                             "\"args\": [", c->slot, c->caller, c->self);
                for (uint32_t k = 0; k < c->argc; k++)
                    fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
                fprintf(out, "], \"ret\": \"0x%08x\", \"out\": [\"0x%08x\", \"0x%08x\"], "
                             "\"answered\": ", c->ret, c->out0, c->out1);
                jstr(out, di_answer_name(c->answered));
                fprintf(out, "}");
            }
            /* Every named slot with its published argument count, which is what
             * tools/s4/dinput_surface.py --argc compares with the image. */
            fprintf(out, "],\n   \"methods\": [");
            {
                uint32_t n = 0;
                for (uint32_t t = 0; t < sa2_dinput_type_count(); t++)
                    for (uint32_t k = 0; k < SA2_DI_MAX_SLOTS; k++) {
                        const char *nm = sa2_dinput_method_name(t, k);
                        if (!nm) continue;
                        const sa2_di_slot_stat *st = sa2_dinput_slot_stat_at(t, k);
                        fprintf(out, "%s\n     {\"interface\": ", n++ ? "," : "");
                        jstr(out, sa2_dinput_type_name(t));
                        fprintf(out, ", \"name\": ");
                        jstr(out, nm);
                        fprintf(out, ", \"slot\": %u, \"argc\": %d, \"implemented\": %s, "
                                     "\"calls\": %u, \"first_caller\": \"0x%08x\", \"flags\": ",
                                k, sa2_dinput_method_argc(t, k),
                                sa2_dinput_method_implemented(t, k) ? "true" : "false",
                                st ? st->calls : 0u, st ? st->first_caller : 0u);
                        jstr(out, sa2_dinput_method_flags(t, k));
                        fprintf(out, "}");
                    }
            }
            fprintf(out, "]");
            /* The host keyboard (runtime/sa2_dinput.h), only when a source is set,
             * so a run with none prints the record it always printed. */
            if (sa2_dinput_input_source()[0]) {
                sa2_di_input_stats in;
                sa2_dinput_input_stats_get(&in);
                fprintf(out, ",\n   \"host_input\": {\"source\": ");
                jstr(out, sa2_dinput_input_source());
                fprintf(out, ", \"answers_from_host\": %u, \"polls\": %u, \"polls_held\": %u, "
                             "\"keys_written\": %llu, \"script_lines\": %u, \"records\": %u, "
                             "\"last_seq\": %u, \"seq_gaps\": %u, \"polls_without_writer\": %u, "
                             "\"partial_dropped\": %u, \"log_lines\": %u, \"stop_requested\": %u, "
                             "\"quit_posted\": %u, \"quit_poll\": %u}",
                        di.host, in.polls, in.polls_held, (unsigned long long)in.keys_written,
                        in.script_lines, in.records, in.last_seq, in.seq_gaps,
                        in.polls_without_writer, in.partial_dropped, in.log_lines,
                        in.stop_requested, in.quit_posted, in.quit_poll);
            }
            fprintf(out, "},\n");
        }
        {
            sa2_wbem_stats wb;
            sa2_wbem_stats_get(&wb);
            fprintf(out, " \"wbem\": {\"profile_entities\": %u, \"cocreate_calls\": %u, "
                         "\"cocreate_last_caller\": \"0x%08x\",\n"
                         "   \"method_calls\": %u, \"distinct_methods\": %u, "
                         "\"objects_built\": %u, \"revivals\": %u, "
                         "\"calls_on_unknown_this\": %u, \"log_dropped\": %u,\n"
                         "   \"answers\": {\"refused\": %u, \"noop\": %u, \"from_guest\": %u, "
                         "\"object\": %u, \"from_profile\": %u},\n"
                         "   \"connects\": %u, \"enumerators\": %u, \"next_calls\": %u, "
                         "\"objects_returned\": %u, \"gets\": %u, \"bstrs_allocated_by_get\": %u, "
                         "\"bstr_bytes_by_get\": %u,\n"
                         "   \"bstr\": {\"allocated\": %u, \"freed\": %u},\n"
                         "   \"proxy_blanket\": {\"calls\": %u, \"caller\": \"0x%08x\", "
                         "\"qi_client_security\": %u, \"set_blanket\": %u, "
                         "\"client_security_release\": %u, \"args\": [",
                    wb.profile_entities, wb.cocreate_calls, wb.cocreate_last_caller,
                    wb.method_calls, wb.distinct_methods, wb.objects_built, wb.revivals,
                    wb.unknown_this, wb.log_dropped,
                    wb.refused, wb.noop, wb.from_guest, wb.object, wb.profile,
                    wb.connects, wb.enumerators, wb.next_calls, wb.objects_returned, wb.gets,
                    wb.bstrs_allocated, wb.bstr_bytes,
                    sa2_bstr_allocs(), sa2_bstr_frees(),
                    wb.blanket_calls, wb.blanket_caller, wb.qi_client_security,
                    wb.client_security_set_blanket, wb.client_security_release);
            for (uint32_t k = 0; k < 8u; k++)
                fprintf(out, "%s\"0x%08x\"", k ? ", " : "", wb.blanket_args[k]);
            fprintf(out, "]},\n   \"calls\": [");
            for (uint32_t i = 0; i < sa2_wbem_log_count(); i++) {
                const sa2_wbem_call *c = sa2_wbem_log_at(i);
                if (!c) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"interface\": ", i ? "," : "", c->seq);
                jstr(out, sa2_wbem_type_name(c->type));
                fprintf(out, ", \"method\": ");
                {
                    const char *nm = sa2_wbem_method_name(c->type, c->slot);
                    jstr(out, nm ? nm : "<unnamed slot>");
                }
                fprintf(out, ", \"slot\": %u, \"caller\": \"0x%08x\", \"this\": \"0x%08x\", "
                             "\"args\": [", c->slot, c->caller, c->self);
                for (uint32_t k = 0; k < c->argc; k++)
                    fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
                fprintf(out, "], \"ret\": \"0x%08x\", \"out\": [\"0x%08x\", \"0x%08x\"], "
                             "\"answered\": ", c->ret, c->out0, c->out1);
                jstr(out, wbem_answer_name(c->answered));
                fprintf(out, "}");
            }
            fprintf(out, "],\n   \"methods\": [");
            {
                uint32_t n = 0;
                for (uint32_t t = 0; t < sa2_wbem_type_count(); t++)
                    for (uint32_t k = 0; k < SA2_WBEM_MAX_SLOTS; k++) {
                        const char *nm = sa2_wbem_method_name(t, k);
                        if (!nm) continue;
                        const sa2_wbem_slot_stat *st = sa2_wbem_slot_stat_at(t, k);
                        fprintf(out, "%s\n     {\"interface\": ", n++ ? "," : "");
                        jstr(out, sa2_wbem_type_name(t));
                        fprintf(out, ", \"name\": ");
                        jstr(out, nm);
                        fprintf(out, ", \"slot\": %u, \"argc\": %d, \"implemented\": %s, "
                                     "\"calls\": %u, \"first_caller\": \"0x%08x\", \"flags\": ",
                                k, sa2_wbem_method_argc(t, k),
                                sa2_wbem_method_implemented(t, k) ? "true" : "false",
                                st ? st->calls : 0u, st ? st->first_caller : 0u);
                        jstr(out, sa2_wbem_method_flags(t, k));
                        fprintf(out, "}");
                    }
            }
            fprintf(out, "]},\n");
        }
        /* ---- the recording DirectDraw ---- */
        {
            sa2_dd_stats dd;
            sa2_ddraw_stats_get(&dd);
            fprintf(out, " \"ddraw\": {\"device_enabled\": %s, \"break\": ",
                    sa2_ddraw_device_enabled() ? "true" : "false");
            jstr(out, sa2_ddraw_break_name());
            fprintf(out, ", \"profile\": ");
            jstr(out, sa2_devprofile_source());
            fprintf(out, ",\n   \"enumerations\": %u, \"callbacks\": %u, \"creates\": %u, "
                         "\"method_calls\": %u, \"distinct_methods\": %u, "
                         "\"objects_built\": %u, \"calls_on_unknown_this\": %u,\n"
                         "   \"answers\": {\"refused\": %u, \"noop\": %u, \"from_guest\": %u, "
                         "\"from_profile\": %u, \"object\": %u},\n"
                         "   \"log_dropped\": %u, \"enumerations_dropped\": %u,\n",
                    dd.enumerations, dd.callbacks, dd.creates, dd.method_calls,
                    dd.distinct_methods, dd.objects_built, dd.unknown_this, dd.refused,
                    dd.noop, dd.from_guest, dd.from_profile, dd.objects, dd.log_dropped,
                    dd.enums_dropped);
            /* What each enumeration handed its callback, by profile entry: whether
             * lpGUID was NULL, the HMONITOR token and what the callback returned.
             * No GUID, description or name is written, so nothing built from this
             * run can copy the host's inventory. */
            fprintf(out, "   \"enumeration_calls\": [");
            for (uint32_t i = 0; i < sa2_ddraw_enum_count(); i++) {
                const sa2_dd_enum *e = sa2_ddraw_enum_at(i);
                if (!e) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"caller\": \"0x%08x\", "
                             "\"callback\": \"0x%08x\", \"context\": \"0x%08x\", "
                             "\"flags\": %u, \"offered\": %u, \"callbacks\": %u, "
                             "\"stopped_by\": %u, \"ret\": \"0x%08x\", \"passed\": [",
                        i ? "," : "", e->seq, e->caller, e->callback, e->context, e->flags,
                        e->offered, e->callbacks, e->stopped_by, e->hr);
                for (uint32_t k = 0; k < e->callbacks && k < SA2_DD_MAX_DEVICES; k++)
                    fprintf(out, "%s{\"entry\": %u, \"guid_null\": %s, "
                                 "\"hmonitor\": \"0x%08x\", \"returned\": %u}",
                            k ? ", " : "", k, e->guid_ptr[k] ? "false" : "true",
                            e->hmonitor[k], e->ret[k]);
                fprintf(out, "]}");
            }
            /* Each DirectDrawCreate, with both interfaces' counts: native ends
             * the boot with IDirectDraw7's at 0 and IDirectDraw's at 1
             * (runtime/sa2_ddraw.h). The video memory is written as whether it
             * is the profile's, never as a figure. */
            fprintf(out, "],\n   \"instances\": [");
            for (uint32_t i = 0; i < sa2_ddraw_instance_count(); i++) {
                const sa2_dd_instance *in = sa2_ddraw_instance_at(i);
                if (!in) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"created_from\": \"0x%08x\", "
                             "\"profile_entry\": %d, \"idirectdraw\": \"0x%08x\", "
                             "\"idirectdraw7\": \"0x%08x\", \"refs\": {\"idirectdraw\": %u, "
                             "\"idirectdraw7\": %u}, \"vidmem_answers\": %u, "
                             "\"total_matches_profile\": %s}",
                        i ? "," : "", in->seq, in->caller, (int)in->device, in->dd, in->dd7,
                        in->ref1, in->ref7, in->vidmem_answers,
                        !in->vidmem_answers ? "null"
                        : in->last_total == dd_profile_total() ? "true" : "false");
            }
            fprintf(out, "],\n   \"calls\": [");
            for (uint32_t i = 0; i < sa2_ddraw_log_count(); i++) {
                const sa2_dd_call *c = sa2_ddraw_log_at(i);
                if (!c) break;
                fprintf(out, "%s\n     {\"seq\": %u, \"interface\": ", i ? "," : "", c->seq);
                jstr(out, sa2_ddraw_type_name(c->type));
                fprintf(out, ", \"method\": ");
                {
                    const char *nm = sa2_ddraw_method_name(c->type, c->slot);
                    jstr(out, nm ? nm : "<unnamed slot>");
                }
                fprintf(out, ", \"slot\": %u, \"caller\": \"0x%08x\", \"this\": \"0x%08x\", "
                             "\"args\": [", c->slot, c->caller, c->self);
                for (uint32_t k = 0; k < c->argc; k++)
                    fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
                fprintf(out, "], ");
                /* An answer from the profile is a host measurement under a
                 * capture's profile, its HRESULT as well as its total: whether
                 * each is the profile's, not what it is. */
                if (c->answered == SA2_DD_ANS_PROFILE)
                    fprintf(out, "\"ret_matches_profile\": %s, \"out_matches_profile\": %s",
                            c->ret == sa2_dp_ddraw_vidmem_hr() ? "true" : "false",
                            c->out0 == dd_profile_total() ? "true" : "false");
                else
                    fprintf(out, "\"ret\": \"0x%08x\", \"out\": \"0x%08x\"", c->ret, c->out0);
                fprintf(out, ", \"answered\": ");
                jstr(out, dd_answer_name(c->answered));
                fprintf(out, "}");
            }
            /* Every named slot with its declared count and whether it has a
             * handler: what `tools/s4/ddraw_surface.py --argc` holds to the image. */
            fprintf(out, "],\n   \"methods\": [");
            {
                uint32_t named = 0;
                for (uint32_t t = 0; t < sa2_ddraw_type_count(); t++)
                    for (uint32_t k = 0; k < SA2_DD_MAX_SLOTS; k++) {
                        const char *nm = sa2_ddraw_method_name(t, k);
                        if (!nm) continue;
                        const sa2_dd_slot_stat *ss = sa2_ddraw_slot_stat_at(t, k);
                        fprintf(out, "%s\n     {\"interface\": ", named++ ? "," : "");
                        jstr(out, sa2_ddraw_type_name(t));
                        fprintf(out, ", \"name\": ");
                        jstr(out, nm);
                        fprintf(out, ", \"slot\": %u, \"argc\": %d, \"implemented\": %s, "
                                     "\"calls\": %u, \"first_caller\": \"0x%08x\", "
                                     "\"flags\": ",
                                k, sa2_ddraw_method_argc(t, k),
                                sa2_ddraw_method_implemented(t, k) ? "true" : "false",
                                ss ? ss->calls : 0u, ss ? ss->first_caller : 0u);
                        jstr(out, sa2_ddraw_method_flags(t, k));
                        fprintf(out, "}");
                    }
            }
            fprintf(out, "]},\n");
        }
        fprintf(out, " \"presentation_survey_calls\": [");
        {
            int first = 1;
            for (uint32_t i = 0; i < logged_main; i++) {
                const sa2_import_call *c = sa2_import_call_at(i);
                if (!c || !c->surveyed) continue;
                const sa2_import_info *in = sa2_import_of_index(c->index);
                fprintf(out, "%s\n   {\"seq\": %u, \"dll\": ", first ? "" : ",", c->seq);
                first = 0;
                jstr(out, in ? in->dll : "?");
                fprintf(out, ", \"name\": ");
                jstr(out, in ? in->name : "?");
                fprintf(out, ", \"caller\": \"0x%08x\", \"args\": [", c->caller);
                for (uint32_t k = 0; k < c->argc; k++)
                    fprintf(out, "%s\"0x%08x\"", k ? ", " : "", c->arg[k]);
                fprintf(out, "]}");
            }
        }
        fprintf(out, "],\n");
        fprintf(out, " \"thread_refusal\": {\"why\": ");
        jstr(out, sa2_import_thread_refused_why());
        fprintf(out, ", \"start\": \"0x%08x\"},\n", sa2_import_thread_refused_start());
        fprintf(out, " \"threads\": {\"created\": %u, \"refused\": %u, \"scheduler_rows\": %d, ",
                sa2_import_threads_created(), sa2_import_threads_refused(), sched_threads);
        /* Thread rows given to a later thread (runtime/sa2_threads.h
         * SA2_MAX_GUEST_THREADS), printed only once a row has been reused: no
         * registered run creates 255 threads, so every one of their records
         * stays byte for byte what it was, which the guest-poll acceptance
         * (tools/s5/poll_handoff_check.py record_bytes) and the registered pins
         * hold it to. A record without the field reused no row. */
        if (sa2_sched_rows_reused())
            fprintf(out, "\"rows_reused\": %llu, ", (unsigned long long)sa2_sched_rows_reused());
        /* SA2_CLOCK_FOLLOWS_REAL (runtime/sa2_threads.c): only a run whose clock it moved has
         * the field, so every replay's record stays as it was. */
        if (sa2_sched_follow_moves())
            fprintf(out, "\"clock_follows_real\": {\"moves\": %llu, \"moved_ms\": %llu, \"let_go_ms\": %llu}, ",
                    (unsigned long long)sa2_sched_follow_moves(),
                    (unsigned long long)(sa2_sched_follow_moved_us() / 1000ull),
                    (unsigned long long)(sa2_sched_follow_dropped_us() / 1000ull));
        fprintf(out, "\"live\": %d, \"switches\": %llu, \"time_advances\": %llu, "
                     "\"waits_without_end\": %llu, \"waits_that_blocked\": %llu, "
                     "\"virtual_budget_ms\": %llu, \"budget_hit\": %s, "
                     "\"guest_named_threads\": %u, \"frames\": %llu, \"frame_us\": %llu, "
                     "\"steam_survey\": %d, \"import_budget\": %u, "
                     "\"critical_section_contentions\": %llu,\n"
                     "   \"spin_advance\": %u, \"spin_advances\": %llu,\n"
                     "   \"spin_yields\": %u, \"spin_observations\": %llu, "
                     "\"spin_clock_moves\": %llu, \"cs_handoff\": %d,\n",
                sa2_sched_live(), (unsigned long long)sa2_sched_switches(),
                (unsigned long long)sa2_sched_time_advances(),
                (unsigned long long)sa2_sched_deadlocks(),
                (unsigned long long)sa2_import_waits_blocked(),
                (unsigned long long)(sa2_sched_budget_us() / 1000ull),
                sa2_sched_budget_hit() ? "true" : "false",
                sa2_import_setthreadname_calls(),
                (unsigned long long)sa2_sched_frames(),
                (unsigned long long)sa2_sched_frame_us(),
                sa2_iface_steam_survey(),
                sa2_import_budget(),
                (unsigned long long)sa2_import_cs_contentions(),
                sa2_sched_spin_advance(), (unsigned long long)sa2_sched_spin_advances(),
                sa2_sched_spin_yields(),
                (unsigned long long)sa2_sched_spin_observations(),
                (unsigned long long)sa2_sched_spin_clock_moves(),
                sa2_sched_cs_handoff());
        /* The guest-poll rule (runtime/sa2_threads.h sa2_sched_poll), beside the
         * spin rule's counters. Printed whenever a wrapped poll ran or the rule
         * is switched off: that is every run that loads, so every registered
         * record gained this one line with the rule. That was chosen, not
         * forced (agents-journals/s5/2026-10-02-newfile-hang.md): the record is
         * where Documentation/scripts/check_poll_handoff.py reads that the
         * wrapper ran, holds each registered replay's counters to what its
         * registration recorded, and holds the runs the rule was accepted on to
         * no hand-off. A run that never polls, with the rule on, prints the
         * record it always printed. */
        {
            sa2_poll_stats ps;
            sa2_sched_poll_stats(&ps);
            if (ps.polls || !ps.rule)
                fprintf(out, "   \"poll_handoff\": {\"rule\": %d, \"polls\": %llu, "
                             "\"not_done\": %llu, \"quiet\": %llu, \"hand_offs\": %llu, "
                             "\"switched\": %llu, \"first_hand_off\": {\"poll\": %llu, "
                             "\"import_call\": %u, \"frame\": %llu}},\n",
                        ps.rule, (unsigned long long)ps.polls,
                        (unsigned long long)ps.not_done, (unsigned long long)ps.quiet,
                        (unsigned long long)ps.hand_offs, (unsigned long long)ps.switched,
                        (unsigned long long)ps.first_poll, ps.first_import_call,
                        (unsigned long long)ps.first_frame);
        }
        fprintf(out, "   \"timers\": {\"created\": %u, \"refused\": %u, \"live\": %u, "
                     "\"fires\": %llu, \"pulses_delivered\": %llu, \"pulses_lost\": %llu},\n"
                     "   \"worker_import_calls_in_log\": %u, \"worker_import_calls_in_recent\": %u, "
                     "\"worker_first_caller\": \"0x%08x\", \"highest_thread_in_log\": %u,\n"
                     "   \"rows\": [",
                sa2_import_timers_created(), sa2_import_timers_refused(), sa2_timer_live(),
                (unsigned long long)sa2_timer_fires(),
                (unsigned long long)sa2_timer_pulses_delivered(),
                (unsigned long long)sa2_timer_pulses_lost(),
                worker_import_calls, worker_recent_calls, worker_first_caller,
                threads_seen_in_log);
        /* The CRT's own answer to "what did this thread run", read out of the
         * per-thread block whose address is in the thread's OWN TLS slot. */
        uint32_t crt_ptd[SA2_MAX_GUEST_THREADS] = {0}, crt_ptd_routine[SA2_MAX_GUEST_THREADS] = {0};
        {
            const uint32_t idx = peek32(VA_CRT_FLS_INDEX);
            for (int i = 0; i < sched_threads && i < (int)SA2_MAX_GUEST_THREADS; i++) {
                if (idx >= SA2_FLS_SLOTS) break;
                const uint32_t *slots = sa2_sched_fls(i);
                crt_ptd[i] = slots ? slots[idx] : 0u;
                crt_ptd_routine[i] = crt_ptd[i] ? peek32(crt_ptd[i] + CRT_PTD_START_ROUTINE) : 0u;
            }
        }
        for (int i = 0; i < sched_threads; i++) {
            fprintf(out, "%s\n    {\"index\": %d, \"tid\": \"0x%08x\", \"state\": ", i ? "," : "",
                    i, sa2_sched_tid(i));
            jstr(out, sa2_sched_state_name(i));
            fprintf(out, ", \"start_va\": \"0x%08x\", \"param\": \"0x%08x\", "
                         "\"teb\": \"0x%08x\", \"stack\": [\"0x%08x\", \"0x%08x\"], "
                         "\"stack_low_water\": \"0x%08x\", \"stack_used\": %u, "
                         "\"slices\": %llu, \"guest_calls\": %llu, \"priority\": %d, "
                         "\"exit_code\": %u, \"trap_status\": %u, \"trap_pc\": \"0x%08x\", "
                         "\"guest_start_routine\": \"0x%08x\", \"guest_name\": \"%s\", "
                         "\"first_import_caller\": \"0x%08x\", "
                         "\"last_import_caller\": \"0x%08x\", "
                         "\"crt_ptd\": \"0x%08x\", \"crt_ptd_start_routine\": \"0x%08x\", "
                         "\"seh_first_bad\": \"0x%08x\", "
                         "\"blocks\": %llu, \"wakes\": %llu, "
                         "\"suspend_count\": %d, \"parks\": %llu, "
                         "\"wait_object\": \"0x%08x\", \"wait_deadline_us\": %llu, "
                         "\"waiting_on\": \"%s\", "
                         "\"where\": ",
                    sa2_sched_start_va(i), sa2_sched_param(i), sa2_sched_teb(i),
                    sa2_sched_stack_base(i), sa2_sched_stack_top(i), sa2_sched_stack_low(i),
                    sa2_sched_stack_top(i) > sa2_sched_stack_low(i)
                        ? sa2_sched_stack_top(i) - sa2_sched_stack_low(i) : 0u,
                    (unsigned long long)sa2_sched_slices(i),
                    (unsigned long long)sa2_sched_guest_calls(i), sa2_sched_priority(i),
                    sa2_sched_exit_code(i), sa2_sched_trap_status(i), sa2_sched_trap_pc(i),
                    i < SA2_MAX_GUEST_THREADS ? user_start[i] : 0u,
                    sa2_import_thread_name(i),
                    sa2_sched_first_caller(i), sa2_sched_last_caller(i),
                    crt_ptd[i], crt_ptd_routine[i],
                    sa2_sched_seh_first_bad(i),
                    (unsigned long long)sa2_sched_blocks(i),
                    (unsigned long long)sa2_sched_wakes(i),
                    sa2_sched_suspend_count(i),
                    (unsigned long long)sa2_sched_parks(i),
                    sa2_sched_wait_object(i),
                    (unsigned long long)sa2_sched_wait_deadline(i),
                    sa2_sched_wait_reason_name(i));
            jstr(out, sa2_sched_where(i));
            /* The FLS callbacks this thread ran as it ended (the CRT's
             * `_freefls` on its per-thread block), counted where each is made. */
            fprintf(out, ", \"fls_exit_callbacks\": %llu",
                    (unsigned long long)sa2_sched_fls_exit_callbacks(i));
            fprintf(out, ", \"trap_note\": ");
            jstr(out, sa2_sched_trap_note(i));
            fprintf(out, "}");
        }
        /* The event census, joined with the scheduler: `waiting_now` is how
         * many guest threads are blocked on this exact handle at the moment
         * the run stopped. An event with waiters and `sets = 0` is a worker
         * nothing will ever release, and it reads differently from an event
         * that is set every tick -- which is the distinction slice 10 could
         * not make from a thread row alone. */
        fprintf(out, "],\n   \"events\": [");
        for (unsigned e = 0; ; e++) {
            sa2_event_row ev;
            if (!sa2_import_event_at(e, &ev)) break;
            unsigned waiting = 0;
            for (int i = 0; i < sched_threads; i++)
                if (sa2_sched_wait_reason(i) == SA2_WAIT_EVENT
                    && sa2_sched_wait_object(i) == ev.handle) waiting++;
            fprintf(out, "%s\n    {\"handle\": \"0x%08x\", \"create_caller\": \"0x%08x\", "
                         "\"manual_reset\": %s, \"signalled\": %s, \"sets\": %u, "
                         "\"resets\": %u, \"waits\": %u, \"waits_blocked\": %u, "
                         "\"waiting_now\": %u}", e ? "," : "",
                    ev.handle, ev.create_caller, ev.manual_reset ? "true" : "false",
                    ev.signalled ? "true" : "false", ev.sets, ev.resets,
                    ev.waits, ev.waits_blocked, waiting);
        }
        /* The suspension census. `by` and `target` are the two the previous
         * slice inferred from a disassembly and got the wrong way round, and
         * `target_state` is the one it could not have inferred at all. */
        fprintf(out, "],\n   \"suspension\": {\"park\": %d, \"suspends\": %llu, "
                     "\"resumes\": %llu, \"self_suspends\": %llu, "
                     "\"suspends_naming_another_thread\": %llu, \"parks\": %llu, "
                     "\"parks_with_no_possible_end\": %llu,\n    \"suspends_by_target_state\": {",
                sa2_sched_suspend_park(),
                (unsigned long long)sa2_sched_suspend_total(),
                (unsigned long long)sa2_sched_resume_total(),
                (unsigned long long)sa2_sched_suspend_self_total(),
                (unsigned long long)sa2_sched_suspend_other_total(),
                (unsigned long long)sa2_sched_parks_total(),
                (unsigned long long)sa2_sched_suspend_no_end());
        for (uint32_t s = 0; s < sa2_sched_state_count(); s++) {
            fprintf(out, "%s", s ? ", " : "");
            jstr(out, sa2_sched_state_label(s));
            fprintf(out, ": %llu", (unsigned long long)sa2_sched_suspend_on_state(s));
        }
        fprintf(out, "},\n    \"log\": [");
        for (uint32_t s = 0; s < sa2_sched_suspend_log_count(); s++) {
            const sa2_suspend_row *sr = sa2_sched_suspend_log_at(s);
            fprintf(out, "%s\n     {\"seq\": %u, \"op\": \"%s\", \"by\": %d, \"target\": %d, "
                         "\"target_state_before\": \"%s\", \"count_before\": %d, "
                         "\"count_after\": %d, \"parked\": %s}", s ? "," : "",
                    sr->seq, sr->op ? "ResumeThread" : "SuspendThread", sr->by, sr->target,
                    sr->target_state ? sr->target_state : "?",
                    sr->count_before, sr->count_after, sr->parked ? "true" : "false");
        }
        fprintf(out, "]},\n   \"seh_checks\": %llu, \"seh_violations\": %llu,\n"
                     "   \"stack_escapes\": %llu, \"ticks_total\": %llu, \"ticks_kept\": %u,\n"
                     "   \"ticks\": [",
                (unsigned long long)sa2_sched_seh_checks(),
                (unsigned long long)sa2_sched_seh_violations(),
                (unsigned long long)sa2_sched_stack_escapes(),
                (unsigned long long)sa2_sched_ticks_total(), sa2_sched_tick_count());
        for (uint32_t i = 0; i < sa2_sched_tick_count(); i++) {
            const sa2_tick_row *t = sa2_sched_tick_at(i);
            fprintf(out, "%s\n    {\"seq\": %u, \"caller\": \"0x%08x\", \"virtual_us\": %llu, "
                         "\"import_calls\": %llu, \"switches\": %llu, \"time_advances\": %llu, "
                         "\"timer_fires\": %llu, \"pulses_delivered\": %llu, "
                         "\"worker_guest_calls\": %llu}", i ? "," : "",
                    t->seq, t->caller, (unsigned long long)t->virtual_us,
                    (unsigned long long)t->import_calls, (unsigned long long)t->switches,
                    (unsigned long long)t->time_advances, (unsigned long long)t->timer_fires,
                    (unsigned long long)t->pulses_delivered,
                    (unsigned long long)t->worker_guest_calls);
        }
        fprintf(out, "]},\n");
        fprintf(out, " \"clauses\": [");
        for (unsigned i = 0; i < sizeof clause / sizeof clause[0]; i++) {
            fprintf(out, "%s\n   {\"name\": ", i ? "," : "");
            jstr(out, clause[i].name);
            fprintf(out, ", \"value\": %s, \"in_verdict\": %s}",
                    clause[i].value ? "true" : "false",
                    clause[i].in_verdict ? "true" : "false");
        }
        fprintf(out, "],\n \"false_clauses\": [");
        {
            int first = 1;
            for (unsigned i = 0; i < sizeof clause / sizeof clause[0]; i++) {
                if (clause[i].value || !clause[i].in_verdict) continue;
                if (!first) fprintf(out, ", ");
                first = 0;
                jstr(out, clause[i].name);
            }
        }
        fprintf(out, "],\n");
        /* The assertion map, generated from the clause table above rather
         * than written out a second time. It used to be a hand-kept list, and
         * a hand-kept copy of a list is a list that disagrees with it: the
         * copy was missing all nine thread clauses and all four Steam ones, so
         * every `asserts_failed` a tool printed from it was silently partial. */
        fprintf(out, " \"assert\": {");
        for (unsigned i = 0; i < sizeof clause / sizeof clause[0]; i++) {
            fprintf(out, "%s", i ? ", " : "");
            jstr(out, clause[i].name);
            fprintf(out, ": %s", clause[i].value ? "true" : "false");
        }
        fprintf(out, "},\n");
        fprintf(out, " \"require_stop\": %s, \"require_files\": %s, "
                     "\"require_loop\": %s, \"require_winmain_loop\": %s, "
                     "\"require_threads\": %s,\n \"ok\": %s\n}\n",
                require_stop ? "true" : "false", require_files ? "true" : "false",
                require_loop ? "true" : "false",
                require_winmain_loop ? "true" : "false",
                require_threads ? "true" : "false", ok ? "true" : "false");
        if (pass == 1) fclose(out);
    }

    if (dump_path) {
        FILE *d = fopen(dump_path, "w");
        if (!d) {
            fprintf(stderr, "crt_probe: cannot write %s\n", dump_path);
        } else {
            uint32_t impl = 0, called = 0;
            for (uint32_t i = 0; i < bound; i++) {
                impl += sa2_import_at(i)->implemented ? 1u : 0u;
                called += sa2_import_at(i)->calls ? 1u : 0u;
            }
            fprintf(d, "{\n \"imports\": %u, \"implemented\": %u, \"called_in_this_run\": %u,\n"
                       " \"rows\": [\n", bound, impl, called);
            for (uint32_t i = 0; i < bound; i++) {
                const sa2_import_info *in = sa2_import_at(i);
                fprintf(d, "  {\"dll\": ");
                jstr(d, in->dll);
                fprintf(d, ", \"name\": ");
                jstr(d, in->name);
                fprintf(d, ", \"slot\": \"0x%08x\", \"unbound\": \"0x%08x\", \"thunk\": \"0x%08x\", "
                           "\"implemented\": %s, \"argc\": %u, \"calls\": %u, "
                           "\"refused_calls\": %u, \"first_refused_ret\": \"0x%08x\", "
                           "\"first_refused_caller\": \"0x%08x\"}%s\n",
                        in->slot, in->unbound, in->thunk, in->implemented ? "true" : "false",
                        in->argc, in->calls, in->refused_calls, in->first_refused_ret,
                        in->first_refused_caller, i + 1 < bound ? "," : "");
            }
            fprintf(d, " ]\n}\n");
            fclose(d);
        }
    }

    sa2_runtime_shutdown();
    return ok ? 0 : 1;
}
