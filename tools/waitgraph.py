#!/usr/bin/env python3
"""waitgraph.py - stall-shape analyzer for ps3recomp boot .err logs (T1).

One command that turns a boot .err log (or several) into the wait-graph
picture that s11-s13 assembled by hand: who waits on what, who signals what,
who is starved.

Usage:
    py -3 tools\\waitgraph.py <file.err> [file2.err ...] [--tail N] [--min-count N]

Input format (see docs/TOOLING_WORKORDER.md "Input formats", verified
2026-07-05): our boot stderr emits one line per LV2 syscall of interest,
written by yakuza/shims.cpp's syscall trampoline (yakuza/shims.cpp:614-623,
the `fprintf(stderr, "[LV2%s t%u] sc %u (r3=... ...) -> 0x...")` call):

    [LV2 t11] sc 107 (r3=0x4 r4=0x0 r5=0xD00FFA20 r6=0xFFFFFFFFD00FFA20) -> 0x0
    [LV2:first t1] sc 352 (r3=0xD0100D04 r4=0x11D3D60 r5=0xFDF0044 r6=0x2A0) -> 0x0

Only lines matching `[LV2...tN] sc NUM (r3=... ...) -> 0xRC` are consumed;
every other line (`[thread]`, `[import]`, `[lle-call]`, `[SPU]`, `[t1-spin]`,
diagnostics, etc.) is ignored. Multiple input files are concatenated in the
order given (each file's lines are independently in program order; there is
no cross-file interleaving by timestamp since these logs carry none).

Syscall number -> name map
---------------------------
Derived from the runtime's OWN lv2 dispatcher, NOT the cheat-sheet numbers
(per docs/TOOLING_WORKORDER.md's instruction to re-derive from source).
Source of truth: runtime/syscalls/lv2_syscall_table.h (the #define block,
lines 29-230) -- this is the header lv2_syscall_dispatch() (line 278) and
every HLE module actually #include and register against, so it is the
authoritative number->name table for this codebase (cross-checked against
yakuza/shims.cpp:616's "[LV2%s t%u] sc %u" trace line, which prints these
same raw numbers). MEASURED: the "working map" numbers quoted in
docs/TOOLING_WORKORDER.md's Input-formats section (92/94/102/103/104/107/
108/109/130) match this header exactly.
"""

import argparse
import re
import sys
from collections import defaultdict, deque

# ---------------------------------------------------------------------------
# Syscall number -> (name, class) map.
#
# Source: runtime/syscalls/lv2_syscall_table.h, the #define block starting
# at line 29 through line 230 (SYS_PROCESS_GETPID=1 .. SYS_DBG_GET_THREAD_LIST
# =610). Only the sync-primitive families relevant to wait-graph analysis are
# classified into {mutex, cond, sema, equeue}; everything else keeps its name
# (for tail-window display) but class=None (not graphed as an object).
# ---------------------------------------------------------------------------
SYSCALL_NAMES = {
    1: "sys_process_getpid",
    2: "sys_process_wait_for_child",
    3: "sys_process_exit",
    4: "sys_process_get_status",
    5: "sys_process_detach_child",
    12: "sys_process_get_number_of_object",
    13: "sys_process_get_id",
    14: "sys_process_is_spu_lock_line_reservation_address",
    41: "sys_ppu_thread_create",
    42: "sys_ppu_thread_exit",
    43: "sys_ppu_thread_yield",
    44: "sys_ppu_thread_join",
    45: "sys_ppu_thread_detach",
    46: "sys_ppu_thread_get_join_state",
    47: "sys_ppu_thread_set_priority",
    48: "sys_ppu_thread_get_priority",
    49: "sys_ppu_thread_get_stack_information",
    56: "sys_ppu_thread_rename",
    100: "sys_mutex_create",
    101: "sys_mutex_destroy",
    102: "sys_mutex_lock",
    103: "sys_mutex_trylock",
    104: "sys_mutex_unlock",
    105: "sys_cond_create",
    106: "sys_cond_destroy",
    107: "sys_cond_wait",
    108: "sys_cond_signal",
    109: "sys_cond_signal_all",
    110: "sys_cond_signal_to",
    90: "sys_semaphore_create",
    91: "sys_semaphore_destroy",
    92: "sys_semaphore_wait",
    93: "sys_semaphore_trywait",
    94: "sys_semaphore_post",
    114: "sys_semaphore_get_value",
    120: "sys_rwlock_create",
    121: "sys_rwlock_destroy",
    122: "sys_rwlock_rlock",
    123: "sys_rwlock_tryrlock",
    124: "sys_rwlock_runlock",
    125: "sys_rwlock_wlock",
    126: "sys_rwlock_trywlock",
    127: "sys_rwlock_wunlock",
    128: "sys_event_queue_create",
    129: "sys_event_queue_destroy",
    130: "sys_event_queue_receive",
    131: "sys_event_queue_tryreceive",
    133: "sys_event_queue_drain",
    134: "sys_event_port_create",
    135: "sys_event_port_destroy",
    136: "sys_event_port_connect_local",
    137: "sys_event_port_disconnect",
    138: "sys_event_port_send",
    82: "sys_event_flag_create",
    83: "sys_event_flag_destroy",
    85: "sys_event_flag_wait",
    86: "sys_event_flag_trywait",
    87: "sys_event_flag_set",
    118: "sys_event_flag_clear",
    132: "sys_event_flag_cancel",
    139: "sys_event_flag_get",
    95: "sys_lwmutex_create",
    96: "sys_lwmutex_destroy",
    97: "sys_lwmutex_lock",
    98: "sys_lwmutex_unlock",
    99: "sys_lwmutex_trylock",
    111: "sys_lwcond_create",
    112: "sys_lwcond_destroy",
    113: "sys_lwcond_queue_wait",
    115: "sys_lwcond_signal",
    116: "sys_lwcond_signal_all",
    70: "sys_timer_create",
    71: "sys_timer_destroy",
    72: "sys_timer_get_information",
    73: "sys_timer_start",
    74: "sys_timer_stop",
    75: "sys_timer_connect_event_queue",
    76: "sys_timer_disconnect_event_queue",
    141: "sys_timer_usleep",
    142: "sys_timer_sleep",
    145: "sys_time_get_current_time",
    147: "sys_time_get_timebase_frequency",
    348: "sys_memory_allocate",
    349: "sys_memory_free",
    350: "sys_memory_allocate_from_container",
    358: "sys_memory_get_page_attribute",
    352: "sys_memory_get_user_memory_size",
    353: "sys_memory_container_create",
    354: "sys_memory_container_destroy",
    355: "sys_memory_container_get_size",
    330: "sys_mmapper_allocate_address",
    331: "sys_mmapper_free_address",
    332: "sys_mmapper_allocate_shared_memory",
    333: "sys_mmapper_free_shared_memory",
    334: "sys_mmapper_map_shared_memory",
    335: "sys_mmapper_unmap_shared_memory",
    337: "sys_mmapper_search_and_map",
    156: "sys_spu_image_open",
    157: "sys_spu_image_import",
    158: "sys_spu_image_close",
    159: "sys_spu_image_get_segments",
    165: "sys_spu_thread_get_exit_status",
    166: "sys_spu_thread_set_argument",
    169: "sys_spu_initialize",
    170: "sys_spu_thread_group_create",
    171: "sys_spu_thread_group_destroy",
    172: "sys_spu_thread_initialize",
    173: "sys_spu_thread_group_start",
    174: "sys_spu_thread_group_suspend",
    175: "sys_spu_thread_group_resume",
    176: "sys_spu_thread_group_yield",
    177: "sys_spu_thread_group_terminate",
    178: "sys_spu_thread_group_join",
    179: "sys_spu_thread_group_set_priority",
    180: "sys_spu_thread_group_get_priority",
    181: "sys_spu_thread_write_ls",
    182: "sys_spu_thread_read_ls",
    184: "sys_spu_thread_write_snr",
    187: "sys_spu_thread_set_spu_cfg",
    188: "sys_spu_thread_get_spu_cfg",
    185: "sys_spu_thread_group_connect_event",
    186: "sys_spu_thread_group_disconnect_event",
    190: "sys_spu_thread_write_spu_mb",
    191: "sys_spu_thread_connect_event",
    192: "sys_spu_thread_disconnect_event",
    193: "sys_spu_thread_bind_queue",
    194: "sys_spu_thread_unbind_queue",
    251: "sys_spu_thread_group_connect_event_all_threads",
    480: "sys_prx_load_module",
    481: "sys_prx_start_module",
    482: "sys_prx_stop_module",
    483: "sys_prx_unload_module",
    484: "sys_prx_register_module",
    485: "sys_prx_query_module",
    486: "sys_prx_register_library",
    487: "sys_prx_unregister_library",
    488: "sys_prx_link_library",
    489: "sys_prx_unlink_library",
    490: "sys_prx_query_library",
    491: "sys_prx_get_module_list",
    492: "sys_prx_get_module_info",
    493: "sys_prx_get_module_id_by_name",
    494: "sys_prx_get_module_id_by_address",
    801: "sys_fs_open",
    802: "sys_fs_read",
    803: "sys_fs_write",
    804: "sys_fs_close",
    805: "sys_fs_opendir",
    806: "sys_fs_readdir",
    807: "sys_fs_closedir",
    808: "sys_fs_stat",
    809: "sys_fs_fstat",
    811: "sys_fs_mkdir",
    812: "sys_fs_rename",
    813: "sys_fs_rmdir",
    814: "sys_fs_unlink",
    818: "sys_fs_lseek",
    820: "sys_fs_ftruncate",
    840: "sys_fs_fget_block_size",
    841: "sys_fs_get_block_size",
    402: "sys_tty_read",
    403: "sys_tty_write",
    610: "sys_dbg_get_thread_list",
}

# Which syscall numbers are "sync-class" (waitgraph-relevant), and what
# (class, verb) each one is. verb in {"lock","trylock","unlock","wait",
# "signal","signal_all","post","receive"}.
# class in {"mutex","cond","sema","equeue"} -- matches the T1 spec's object
# key `(class, id)` where id = r3.
SYNC_SYSCALLS = {
    102: ("mutex", "lock"),
    103: ("mutex", "trylock"),
    104: ("mutex", "unlock"),
    107: ("cond", "wait"),
    108: ("cond", "signal"),
    109: ("cond", "signal_all"),
    110: ("cond", "signal_to"),
    92: ("sema", "wait"),
    93: ("sema", "trywait"),
    94: ("sema", "post"),
    130: ("equeue", "receive"),
    131: ("equeue", "tryreceive"),
    138: ("equeue", "send"),
    97: ("mutex", "lock"),      # sys_lwmutex_lock -- lightweight mutex, same shape
    98: ("mutex", "unlock"),    # sys_lwmutex_unlock
    99: ("mutex", "trylock"),   # sys_lwmutex_trylock
    113: ("cond", "wait"),      # sys_lwcond_queue_wait
    115: ("cond", "signal"),    # sys_lwcond_signal
    116: ("cond", "signal_all"),  # sys_lwcond_signal_all
    85: ("equeue", "wait"),     # sys_event_flag_wait (flag, not equeue proper,
                                # but same waiter/signaler shape; kept in its
                                # own id-space via the "flag" class below)
    87: ("equeue", "post"),     # sys_event_flag_set
}
# event flags are a distinct object namespace from event queues even though
# both flow through class "equeue" conceptually -- keep them separate so a
# flag id 4 doesn't merge with a cond/mutex id 4's unrelated equeue id 4.
SYNC_SYSCALLS[85] = ("flag", "wait")
SYNC_SYSCALLS[86] = ("flag", "trywait")
SYNC_SYSCALLS[87] = ("flag", "post")
SYNC_SYSCALLS[118] = ("flag", "clear")

# Waiter verbs vs signaler verbs, used for the per-object VERDICT.
WAIT_VERBS = {"lock", "trylock", "wait", "tryreceive", "receive", "trywait"}
SIGNAL_VERBS = {"unlock", "signal", "signal_all", "signal_to", "post", "send"}

# The CELL error-code convention: LV2 handlers return errors as a sign-
# extended 32-bit value whose top half is 0xFFFFFFFF and whose low 32 bits
# start with 0x8001 (e.g. 0xFFFFFFFF8001000D, or bare 0x8001000D on a 32-bit
# print). rc==0 is success. Anything else is bucketed under "other" so the
# histogram degrades gracefully instead of mis-classifying an unexpected rc.
def rc_bucket(rc):
    if rc == 0:
        return "0x0"
    low32 = rc & 0xFFFFFFFF
    if (low32 >> 16) == 0x8001:
        return "0x8001xxxx"
    return "other(0x%X)" % rc


LINE_RE = re.compile(
    r"^\[LV2(?::first)?\s+t(?P<tid>\d+)\]\s+sc\s+(?P<num>\d+)\s+"
    r"\(r3=0x(?P<r3>[0-9A-Fa-f]+)\s+r4=0x(?P<r4>[0-9A-Fa-f]+)\s+"
    r"r5=0x(?P<r5>[0-9A-Fa-f]+)\s+r6=0x(?P<r6>[0-9A-Fa-f]+)\)\s+"
    r"->\s+0x(?P<rc>[0-9A-Fa-f]+)\s*$"
)

THREAD_CREATE_RE = re.compile(
    r'^\[thread\]\s+create\s+"(?P<name>[^"]*)"\s+tid=(?P<tid>\d+)'
)


class Event:
    __slots__ = ("tid", "num", "name", "r3", "r4", "r5", "r6", "rc", "raw")

    def __init__(self, tid, num, name, r3, r4, r5, r6, rc, raw):
        self.tid = tid
        self.num = num
        self.name = name
        self.r3 = r3
        self.r4 = r4
        self.r5 = r5
        self.r6 = r6
        self.rc = rc
        self.raw = raw


def parse_files(paths):
    """Parse one or more .err files in order; return (events, thread_names)."""
    events = []
    thread_names = {}
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                m = THREAD_CREATE_RE.match(line)
                if m:
                    thread_names[int(m.group("tid"))] = m.group("name")
                    continue
                m = LINE_RE.match(line)
                if not m:
                    continue
                num = int(m.group("num"))
                tid = int(m.group("tid"))
                name = SYSCALL_NAMES.get(num, "sc_%d" % num)
                events.append(Event(
                    tid=tid,
                    num=num,
                    name=name,
                    r3=int(m.group("r3"), 16),
                    r4=int(m.group("r4"), 16),
                    r5=int(m.group("r5"), 16),
                    r6=int(m.group("r6"), 16),
                    rc=int(m.group("rc"), 16),
                    raw=line,
                ))
    return events, thread_names


def detect_cycle(tail_events, min_len=2, max_len=6):
    """Detect a repeating tail window of 2-6 sync calls in a per-thread event
    list (already filtered to sync-class events only, in order). Returns a
    (pattern_str, period, window) tuple, or None if no repeat is found.
    `window` is the list of Events making up one period of the cycle (the
    last `period` events) -- the caller uses it to attribute a WOKEN-STARVED
    verdict to every (class, id) object the cycle actually touches via a
    wait verb, not just whichever call happens to fall last in the tail.

    Method: try period lengths max_len down to min_len; a "signature" per
    event is (num, r3). If the last `period` signatures repeat immediately
    before them (at least 2 full repeats available), call it cycling.
    """
    if len(tail_events) < min_len * 2:
        return None

    def sig(e):
        return (e.num, e.r3)

    sigs = [sig(e) for e in tail_events]
    n = len(sigs)
    for period in range(min_len, max_len + 1):
        if n < period * 2:
            continue
        last = sigs[-period:]
        prev = sigs[-2 * period:-period]
        if last == prev:
            # Describe the pattern using the underlying events (dedup by
            # num while preserving order of first appearance).
            window = tail_events[-period:]
            obj_ids = sorted(set(e.r3 for e in window))
            classes = sorted(set(
                SYNC_SYSCALLS.get(e.num, (None, None))[0] for e in window
                if SYNC_SYSCALLS.get(e.num, (None, None))[0] is not None
            ))
            nums = "/".join(str(e.num) for e in window)
            obj_str = ",".join(str(i) for i in obj_ids)
            cls_str = "+".join(classes) if classes else "?"
            return ("%s on %s id %s" % (nums, cls_str, obj_str), period, window)
    return None


def format_tail_line(e):
    cls_verb = SYNC_SYSCALLS.get(e.num)
    tag = ""
    if cls_verb:
        tag = " [%s:%s obj=%d]" % (cls_verb[0], cls_verb[1], e.r3)
    return "  sc %d (%s) r3=0x%X r4=0x%X r5=0x%X r6=0x%X -> rc=0x%X%s" % (
        e.num, e.name, e.r3, e.r4, e.r5, e.r6, e.rc, tag
    )


def per_thread_section(events, thread_names, tail_n):
    """Returns (output_lines, cycling_waits) where cycling_waits is a set of
    (class, id) object keys that some thread is CYCLING on via a WAIT verb
    (lock/trylock/wait/receive/...) with its most recent wait rc==0x0 -- i.e.
    the concrete WOKEN-STARVED signature (waits keep returning success and
    the thread keeps re-waiting instead of making progress). Used by
    per_object_section() so an object's VERDICT agrees with its waiter
    thread's own CLASSIFICATION line, instead of an independent heuristic
    that would flag ordinary steady-state producer/consumer traffic.
    """
    out = []
    out.append("=" * 78)
    out.append("PER-THREAD")
    out.append("=" * 78)

    by_tid = defaultdict(list)
    for e in events:
        by_tid[e.tid].append(e)

    cycling_waits = set()

    for tid in sorted(by_tid.keys()):
        tevents = by_tid[tid]
        sync_events = [e for e in tevents if e.num in SYNC_SYSCALLS]
        name = thread_names.get(tid)
        header = "t%d" % tid
        if name:
            header += ' ("%s")' % name
        out.append("")
        out.append(header)
        out.append("  total sync-class syscalls: %d  (total syscalls seen: %d)"
                    % (len(sync_events), len(tevents)))

        tail = sync_events[-tail_n:] if sync_events else []
        out.append("  last %d sync calls:" % len(tail))
        for e in tail:
            out.append(format_tail_line(e))

        cyc = detect_cycle(sync_events)
        if cyc:
            pattern, period, window = cyc
            last = sync_events[-1]
            extra = ""
            cls_verb = SYNC_SYSCALLS.get(last.num)
            last_is_wait = cls_verb and cls_verb[1] in WAIT_VERBS
            if last_is_wait and last.rc == 0:
                extra = " (%s rc=0x0 -> WOKEN-STARVED)" % last.name
            elif last_is_wait:
                extra = " (%s rc=0x%X)" % (last.name, last.rc)
            # Attribute WOKEN-STARVED to every (class, id) object this cycle
            # window actually waits on with rc==0, not just whichever call
            # happens to be last in the tail -- a 3-call trylock/cond_wait/
            # unlock cycle on the SAME numeric id touches both a mutex
            # object and a cond object, and both re-wait on 0x0 in lockstep.
            for we in window:
                wcls_verb = SYNC_SYSCALLS.get(we.num)
                if wcls_verb and wcls_verb[1] in WAIT_VERBS and we.rc == 0:
                    cycling_waits.add((wcls_verb[0], we.r3))
            out.append("  CLASSIFICATION: CYCLING %s%s" % (pattern, extra))
        elif sync_events:
            last = sync_events[-1]
            out.append("  CLASSIFICATION: LAST-CALL %s(%d) rc=0x%X"
                        % (last.name, last.r3, last.rc))
        else:
            out.append("  CLASSIFICATION: (no sync-class syscalls observed)")

    return out, cycling_waits


def per_object_section(events, min_count, cycling_waits):
    """cycling_waits: set of (class, id) keys where some thread's PER-THREAD
    CLASSIFICATION was CYCLING ending on a WAIT verb with rc==0x0 (the
    concrete "waits return 0x0 repeatedly and the thread re-waits" signature
    from the T1 spec). This is the sole driver of the WOKEN-STARVED verdict,
    so an object's verdict always agrees with its waiter's own tail-window
    classification instead of flagging ordinary steady-state traffic where a
    thread simply waits successfully many times over a long boot log.
    """
    out = []
    out.append("")
    out.append("=" * 78)
    out.append("PER-OBJECT")
    out.append("=" * 78)

    # key = (class, id)
    waiters = defaultdict(lambda: defaultdict(int))     # key -> tid -> count
    signalers = defaultdict(lambda: defaultdict(int))   # key -> tid -> count
    rc_hist = defaultdict(lambda: defaultdict(int))      # key -> bucket -> count

    for e in events:
        cls_verb = SYNC_SYSCALLS.get(e.num)
        if not cls_verb:
            continue
        cls, verb = cls_verb
        key = (cls, e.r3)
        if verb in WAIT_VERBS:
            waiters[key][e.tid] += 1
            rc_hist[key][rc_bucket(e.rc)] += 1
        elif verb in SIGNAL_VERBS:
            signalers[key][e.tid] += 1

    all_keys = sorted(set(list(waiters.keys()) + list(signalers.keys())),
                       key=lambda k: (k[0], k[1]))

    verdicts = {}
    for key in all_keys:
        w = waiters.get(key, {})
        s = signalers.get(key, {})
        total_waits = sum(w.values())
        total_signals = sum(s.values())
        if total_waits < min_count and total_signals < min_count:
            continue

        rc_h = rc_hist.get(key, {})
        zero_waits = rc_h.get("0x0", 0)

        if key in cycling_waits:
            # A waiter thread is confirmed CYCLING on this exact object with
            # its wait verb returning 0x0 and re-waiting (see
            # per_thread_section). This takes priority: it is the concrete,
            # tail-window-confirmed signature, not an inference from
            # aggregate counts.
            verdict = "WOKEN-STARVED"
        elif total_signals > 0 and w and total_waits > 0 and zero_waits == 0:
            # signals arrive, waiters exist, but no wait ever returned 0x0
            # (every observed wait is still outstanding / errored) --
            # possible lost wakeup or wrong object.
            verdict = "SIGNALED-NEVER-WOKEN"
        elif w and total_signals == 0:
            verdict = "NO-SIGNALER"
        else:
            verdict = "HEALTHY"

        verdicts[key] = verdict

        out.append("")
        out.append("(%s, id=%d)" % key)
        if w:
            out.append("  waiters: " + ", ".join(
                "t%d(x%d)" % (tid, c) for tid, c in sorted(w.items())))
        else:
            out.append("  waiters: (none)")
        if s:
            out.append("  signalers: " + ", ".join(
                "t%d(x%d)" % (tid, c) for tid, c in sorted(s.items())))
        else:
            out.append("  signalers: (none)")
        if rc_h:
            out.append("  wait rc histogram: " + ", ".join(
                "%s=%d" % (b, c) for b, c in sorted(rc_h.items())))
        out.append("  VERDICT: %s" % verdict)

    return out, verdicts, waiters, signalers


def suspects_section(verdicts, waiters, signalers):
    out = []
    out.append("")
    out.append("=" * 78)
    out.append("SUSPECTS SUMMARY")
    out.append("=" * 78)

    bad = [(k, v) for k, v in verdicts.items() if v != "HEALTHY"]
    if not bad:
        out.append("(none -- all graphed objects HEALTHY)")
        return out

    # Rank by total wait+signal traffic (busiest suspects first).
    def traffic(k):
        return sum(waiters.get(k, {}).values()) + sum(signalers.get(k, {}).values())

    bad.sort(key=lambda kv: -traffic(kv[0]))
    for key, verdict in bad:
        cls, oid = key
        w = waiters.get(key, {})
        s = signalers.get(key, {})
        w_str = ",".join("t%d" % t for t in sorted(w.keys())) or "-"
        s_str = ",".join("t%d" % t for t in sorted(s.keys())) or "-"
        out.append("  %-22s %s obj=%d  waiters=[%s] signalers=[%s]"
                    % (verdict, cls, oid, w_str, s_str))
    return out


def main():
    ap = argparse.ArgumentParser(
        description="Stall-shape analyzer for ps3recomp boot .err logs (T1)."
    )
    ap.add_argument("files", nargs="+", help="one or more boot .err files")
    ap.add_argument("--tail", type=int, default=8,
                     help="how many trailing sync calls to print per thread (default 8)")
    ap.add_argument("--min-count", type=int, default=1,
                     help="minimum wait+signal count for an object to be reported (default 1)")
    args = ap.parse_args()

    for p in args.files:
        try:
            open(p, "rb").close()
        except OSError as ex:
            print("error: cannot open %s: %s" % (p, ex), file=sys.stderr)
            return 1

    events, thread_names = parse_files(args.files)

    if not events:
        print("waitgraph: no [LV2 ...] sc lines matched in: %s"
              % ", ".join(args.files))
        return 0

    print("waitgraph: %d input file(s), %d sync-relevant events parsed"
          % (len(args.files), len(events)))

    lines = []
    thread_lines, cycling_waits = per_thread_section(events, thread_names, args.tail)
    lines += thread_lines
    obj_lines, verdicts, waiters, signalers = per_object_section(
        events, args.min_count, cycling_waits)
    lines += obj_lines
    lines += suspects_section(verdicts, waiters, signalers)

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
