/* Stopper release and a010 command-chain recovery.
 * Part of import_overrides.cpp, which includes its parts once, in order. */

/* The game places a self-jump stopper at every commit and releases the
 * previous one. A release is either an immediate patch of the stopper word or
 * a deferred journal entry. The journal is a list of 0x20-byte entries in
 * [S[+8], S[+0]) of the gcm state struct S = *(game_toc - 0x7410); an entry
 * with tag 0x7F at +0 and the stopper address at +4 is a deferred release.
 * yz_gcm_stopper_release_entry() returns the address of the matching entry. */

static volatile LONG g_yz_cancel_released_stopper_scan;
static uint64_t g_yz_stopper_scan_calls, g_yz_stopper_scan_cancels;
static int yz_set_stopper_scan_cancel(int enabled)
{
    InterlockedExchange(&g_yz_cancel_released_stopper_scan, enabled != 0);
    return enabled != 0;
}

template<bool CancelReleased>
static uint32_t yz_gcm_stopper_release_entry_impl(
    uint32_t stopper_ea, uint32_t expected, bool* cancelled)
{
    if constexpr (CancelReleased) {
        ++g_yz_stopper_scan_calls;
        if (vm_read32(stopper_ea) != expected) {
            *cancelled = true;
            ++g_yz_stopper_scan_cancels;
            return 0;
        }
    }
    if (!g_yz_game_toc) return 0;
    uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
    if (S < 0x10000u || S >= 0xE0000000u) return 0;
    uint32_t base = vm_read32(S + 0x08u);
    uint32_t head = vm_read32(S + 0x00u);
    uint32_t end = vm_read32(S + 0x0Cu);
    if (base < 0x10000u || base >= 0xE0000000u) return 0;
    if (head < base || (head - base) > 0x1000000u) return 0;
    for (uint32_t e = base; e < head; e += 0x20u) {
        if constexpr (CancelReleased) {
            if (((e - base) & 0x7FFu) == 0u &&
                vm_read32(stopper_ea) != expected) {
                *cancelled = true;
                ++g_yz_stopper_scan_cancels;
                return 0;
            }
        }
        if (vm_read32(e + 0x00u) == 0x7Fu && vm_read32(e + 0x04u) == stopper_ea)
            return e;
    }
    /* Wrap-aware leg: the journal is a ring [base..end) (end = S+0x0C) and
     * head wraps to base; the linear scan above is blind to entries still
     * live in [head..end). An above-head match may also be a stale prior-lap
     * entry for a recycled stopper EA, so callers must treat it
     * conservatively: journal-owned, wait; never a reason to jump. */
    if (end > head && end - base <= 0x1000000u)
        for (uint32_t e = head; e + 0x20u <= end; e += 0x20u) {
            if constexpr (CancelReleased) {
                if (((e - head) & 0x7FFu) == 0u &&
                    vm_read32(stopper_ea) != expected) {
                    *cancelled = true;
                    ++g_yz_stopper_scan_cancels;
                    return 0;
                }
            }
            if (vm_read32(e + 0x00u) == 0x7Fu &&
                vm_read32(e + 0x04u) == stopper_ea)
                return e;
        }
    return 0;
}

static uint32_t yz_gcm_stopper_release_entry(uint32_t stopper_ea)
{
    return yz_gcm_stopper_release_entry_impl<false>(stopper_ea, 0, nullptr);
}

/*
 * a010 missed-immediate-release recovery.
 *
 * A committed stopper has two legitimate release paths in the game's flush
 * routine: a direct patch of the self-jump, or a tag-0x7F journal entry.
 * If GET stays parked on the old self-jump, PUT is already beyond it, no
 * matching 0x7F entry exists and S[0x20] names the newer stopper exactly at
 * PUT, the producer has finished the old commit and moved its pending-stopper
 * cursor, so the direct patch was lost.  The repair re-issues only that
 * patch, once the journal head, pending stopper and PUT form a stable
 * snapshot.  It never consumes or retires a journal entry.
 *
 * A direct release is not always "old stopper -> old + 4": the FIFO can link
 * around reserved data islands, so the resume point is chosen by walking the
 * candidate chain.
 *
 * The generated draw substreams begin with an exact eight-word prologue:
 * SET_VERTEX_DATA_ARRAY_OFFSET(0), three SET_VERTEX_DATA_BASE_OFFSET(0)
 * packets, then the vertex-array declarations.  The bytes just before that
 * prologue are vertex payload.  The resume point is the earliest prologue
 * whose flow reaches USER_COMMAND(label[0xFE0] + 1); the committed window
 * also holds older, still-valid chains, so the first prologue found is not
 * enough.
 */
static int yz_a010_generated_prologue_at(uint32_t io)
{
    const uint32_t ea = yz_rsx_io_to_ea(io);
    return ea &&
           vm_read32(ea + 0x00u) == 0x00041710u &&
           vm_read32(ea + 0x04u) == 0u &&
           vm_read32(ea + 0x08u) == 0x00041714u &&
           vm_read32(ea + 0x0Cu) == 0u &&
           vm_read32(ea + 0x10u) == 0x00041714u &&
           vm_read32(ea + 0x14u) == 0u &&
           vm_read32(ea + 0x18u) == 0x00041714u &&
           vm_read32(ea + 0x1Cu) == 0u &&
           (vm_read32(ea + 0x20u) & 0x3FFFCu) == 0x1740u &&
           (vm_read32(ea + 0x28u) & 0x3FFFCu) == 0x1680u;
}

/* Dry-run an RSX command chain without applying a single method.  The recovery
 * is allowed to release an old stopper only when the proposed entry reaches
 * both the next monotonic USER_COMMAND packet and a subsequent jump-to-self
 * stopper inside the producer's committed window.  This rejects a valid prefix
 * that later falls through into an EDGE data island, which a plain
 * reachability test would accept. */
static int yz_a010_chain_complete(uint32_t start,
                                  uint32_t target,
                                  uint32_t window_start,
                                  uint32_t put,
                                  uint32_t* out_steps,
                                  uint32_t* out_end)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = 0x7FFFFFu;
    uint32_t pc = start;
    uint32_t ret = ~0u;
    int reached_target = 0;
    const uint32_t committed = (put - window_start + ring) & mask;
    if (committed == 0u || committed >= (ring >> 1))
        return 0;

    for (uint32_t step = 0; step < 0x40000u; step++) {
        if (pc == target)
            reached_target = 1;

        /* Fixed RSX subroutines live outside the 8 MiB FIFO ring and are
         * reachable only while a CALL return is live.  Every ring address must
         * remain inside the producer-published [window_start, PUT] window. */
        if (pc < ring) {
            const uint32_t delta = (pc - window_start + ring) & mask;
            if (delta > committed)
                return 0;
        } else if (ret == ~0u) {
            return 0;
        }

        const uint32_t ea = yz_rsx_io_to_ea(pc);
        if (!ea)
            return 0;
        const uint32_t cmd = vm_read32(ea);
        if ((cmd & 0xE0000003u) == 0x20000000u ||
            (cmd & 3u) == 1u) {
            const uint32_t next =
                (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)
                                 : (cmd & 0x1FFFFFFCu);
            if (next == pc) {
                /* PUT can be several complete frame batches ahead while GET is
                 * stuck.  The first self-stop reached after the exact next
                 * completion packet is a finalized batch boundary and is the
                 * safe end of this one recovery step. */
                if (!reached_target || ret != ~0u)
                    return 0;
                if (out_steps)
                    *out_steps = step + 1u;
                if (out_end)
                    *out_end = pc;
                return 1;
            }
            if (!yz_rsx_io_to_ea(next))
                return 0;
            pc = next;
            continue;
        }
        if ((cmd & 3u) == 2u) {
            const uint32_t next = cmd & 0x1FFFFFFCu;
            if (ret != ~0u || !yz_rsx_io_to_ea(next))
                return 0;
            ret = (pc + 4u) & mask;
            pc = next;
            continue;
        }
        if ((cmd & 0xFFFF0003u) == 0x00020000u) {
            if (ret == ~0u)
                return 0;
            pc = ret;
            ret = ~0u;
            continue;
        }
        if (cmd & 0xA0030003u)
            return 0;
        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t bytes = 4u + count * 4u;
        pc = pc < ring ? ((pc + bytes) & mask) : (pc + bytes);
    }
    return 0;
}

/* The producer intentionally leaves the tail of a frame batch unpublished
 * until RSX consumes its USER_COMMAND completion marker.  Therefore the first
 * release must sometimes be proven only through that marker; consuming it
 * wakes the producer, which publishes the remainder and its next stopper.
 * Keep the same committed-window and one-level CALL checks as the full check. */
static int yz_a010_chain_reaches_completion(uint32_t start,
                                            uint32_t target,
                                            uint32_t window_start,
                                            uint32_t put,
                                            uint32_t* out_steps)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t committed = (put - window_start + ring) & mask;
    uint32_t pc = start;
    uint32_t ret = ~0u;
    if (committed == 0u || committed >= (ring >> 1))
        return 0;

    for (uint32_t step = 0; step < 0x40000u; step++) {
        if (pc == target) {
            if (ret != ~0u)
                return 0;
            if (out_steps)
                *out_steps = step;
            return 1;
        }
        if (pc < ring) {
            const uint32_t delta = (pc - window_start + ring) & mask;
            if (delta > committed)
                return 0;
        } else if (ret == ~0u) {
            return 0;
        }

        const uint32_t ea = yz_rsx_io_to_ea(pc);
        if (!ea)
            return 0;
        const uint32_t cmd = vm_read32(ea);
        if ((cmd & 0xE0000003u) == 0x20000000u ||
            (cmd & 3u) == 1u) {
            const uint32_t next =
                (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)
                                 : (cmd & 0x1FFFFFFCu);
            if (next == pc || !yz_rsx_io_to_ea(next))
                return 0;
            pc = next;
            continue;
        }
        if ((cmd & 3u) == 2u) {
            const uint32_t next = cmd & 0x1FFFFFFCu;
            if (ret != ~0u || !yz_rsx_io_to_ea(next))
                return 0;
            ret = pc + 4u;
            pc = next;
            continue;
        }
        if ((cmd & 0xFFFF0003u) == 0x00020000u) {
            if (ret == ~0u)
                return 0;
            pc = ret;
            ret = ~0u;
            continue;
        }
        if (cmd & 0xA0030003u)
            return 0;
        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t bytes = 4u + count * 4u;
        pc = pc < ring ? ((pc + bytes) & mask) : (pc + bytes);
    }
    return 0;
}

enum yz_a010_chain_probe_stop {
    YZ_A010_PROBE_COMPLETE,
    YZ_A010_PROBE_BAD_WINDOW,
    YZ_A010_PROBE_OUTSIDE_WINDOW,
    YZ_A010_PROBE_UNMAPPED,
    YZ_A010_PROBE_SELF_BEFORE_TARGET,
    YZ_A010_PROBE_BAD_JUMP,
    YZ_A010_PROBE_NESTED_CALL,
    YZ_A010_PROBE_RETURN_WITHOUT_CALL,
    YZ_A010_PROBE_MALFORMED,
    YZ_A010_PROBE_STEP_LIMIT
};

struct yz_a010_chain_probe {
    enum yz_a010_chain_probe_stop stop;
    uint32_t stop_pc;
    uint32_t stop_cmd;
    uint32_t calls;
    uint32_t returns;
    uint32_t begin;
    uint32_t end;
    uint32_t arrays;
    uint32_t indices;
    int reached_target;
};

/* Dry-run an RSX command chain without applying methods; records how it
 * stopped and the draw-method balance along its JUMP/CALL flow. */
static void yz_a010_probe_chain(uint32_t start,
                               uint32_t target,
                               uint32_t window_start,
                               uint32_t put,
                               struct yz_a010_chain_probe* out)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t committed = (put - window_start + ring) & mask;
    uint32_t pc = start;
    uint32_t ret = ~0u;
    memset(out, 0, sizeof(*out));
    out->stop = YZ_A010_PROBE_STEP_LIMIT;

    if (committed == 0u || committed >= (ring >> 1)) {
        out->stop = YZ_A010_PROBE_BAD_WINDOW;
        return;
    }

    for (uint32_t step = 0; step < 0x40000u; step++) {
        out->stop_pc = pc;
        if (pc == target)
            out->reached_target = 1;

        if (pc < ring) {
            const uint32_t delta = (pc - window_start + ring) & mask;
            if (delta > committed) {
                out->stop = YZ_A010_PROBE_OUTSIDE_WINDOW;
                return;
            }
        } else if (ret == ~0u) {
            out->stop = YZ_A010_PROBE_OUTSIDE_WINDOW;
            return;
        }

        const uint32_t ea = yz_rsx_io_to_ea(pc);
        if (!ea) {
            out->stop = YZ_A010_PROBE_UNMAPPED;
            return;
        }
        const uint32_t cmd = vm_read32(ea);
        out->stop_cmd = cmd;

        if ((cmd & 0xE0000003u) == 0x20000000u ||
            (cmd & 3u) == 1u) {
            const uint32_t next =
                (cmd & 3u) == 1u ? (cmd & 0xFFFFFFFCu)
                                 : (cmd & 0x1FFFFFFCu);
            if (next == pc) {
                out->stop = out->reached_target && ret == ~0u
                                ? YZ_A010_PROBE_COMPLETE
                                : YZ_A010_PROBE_SELF_BEFORE_TARGET;
                return;
            }
            if (!yz_rsx_io_to_ea(next)) {
                out->stop = YZ_A010_PROBE_BAD_JUMP;
                return;
            }
            pc = next;
            continue;
        }
        if ((cmd & 3u) == 2u) {
            const uint32_t next = cmd & 0x1FFFFFFCu;
            out->calls++;
            if (ret != ~0u) {
                out->stop = YZ_A010_PROBE_NESTED_CALL;
                return;
            }
            if (!yz_rsx_io_to_ea(next)) {
                out->stop = YZ_A010_PROBE_BAD_JUMP;
                return;
            }
            ret = (pc + 4u) & mask;
            pc = next;
            continue;
        }
        if ((cmd & 0xFFFF0003u) == 0x00020000u) {
            out->returns++;
            if (ret == ~0u) {
                out->stop = YZ_A010_PROBE_RETURN_WITHOUT_CALL;
                return;
            }
            pc = ret;
            ret = ~0u;
            continue;
        }
        if (cmd & 0xA0030003u) {
            out->stop = YZ_A010_PROBE_MALFORMED;
            return;
        }

        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t noninc = cmd & 0x40000000u;
        const uint32_t method = cmd & 0x3FFFCu;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t arg_io =
                pc < ring ? ((pc + 4u + i * 4u) & mask)
                          : (pc + 4u + i * 4u);
            const uint32_t arg_ea = yz_rsx_io_to_ea(arg_io);
            if (!arg_ea) {
                out->stop_pc = arg_io;
                out->stop = YZ_A010_PROBE_UNMAPPED;
                return;
            }
            const uint32_t eff = noninc ? method : method + i * 4u;
            const uint32_t canonical = eff & 0x1FFCu;
            const uint32_t value = vm_read32(arg_ea);
            if (canonical == 0x1808u) {
                if (value) out->begin++;
                else       out->end++;
            } else if (canonical == 0x1814u) {
                out->arrays++;
            } else if (canonical == 0x1820u) {
                out->indices++;
            }
        }
        const uint32_t bytes = 4u + count * 4u;
        pc = pc < ring ? ((pc + bytes) & mask) : (pc + bytes);
    }
}

/* A generated batch may be fully formed and draw-balanced even though its
 * terminal self-stop has not yet been linked to the next generated batch.
 * Releasing one such prefix at a time preserves producer order: RSX consumes
 * the batch, parks at its terminal stop, and the next recovery begins strictly
 * after that stop.  This is deliberately narrower than accepting an arbitrary
 * prefix; malformed flow, live calls, and unbalanced draws remain fail-closed. */
static int yz_a010_probe_is_safe_prefix(
    const struct yz_a010_chain_probe* probe,
    uint32_t candidate,
    uint32_t window_start,
    uint32_t put)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t committed = (put - window_start + ring) & mask;
    const uint32_t candidate_delta =
        (candidate - window_start + ring) & mask;
    const uint32_t stop_delta =
        (probe->stop_pc - window_start + ring) & mask;
    const uint32_t self_target =
        (probe->stop_cmd & 3u) == 1u
            ? (probe->stop_cmd & 0xFFFFFFFCu)
            : (probe->stop_cmd & 0x1FFFFFFCu);

    return probe->stop == YZ_A010_PROBE_SELF_BEFORE_TARGET &&
           !probe->reached_target &&
           probe->stop_pc < ring &&
           self_target == probe->stop_pc &&
           candidate_delta < stop_delta &&
           stop_delta <= committed &&
           probe->calls == probe->returns &&
           probe->begin != 0u &&
           probe->begin == probe->end &&
           (probe->arrays != 0u || probe->indices != 0u);
}

/* True for the first 16 messages of a kind and every 256th after that. */
static bool yz_a010_log_sparse(unsigned long n)
{
    return n <= 16 || (n & 255) == 0;
}

/* Find the next user-interrupt packet the guest is waiting to consume, then
 * find the earliest exact generated block that is safe to execute.  A complete
 * chain may reach the interrupt directly; otherwise a balanced prefix can run
 * up to its own self-stop and recovery resumes from there.  Never jump over a
 * validated prefix merely because a later tail already reaches completion. */
static uint32_t yz_a010_find_pending_chain(uint32_t start, uint32_t put)
{
    const uint32_t ring = 0x800000u;
    const uint32_t mask = ring - 1u;
    const uint32_t ahead = (put - start + ring) & mask;
    if (ahead == 0u || ahead >= (ring >> 1))
        return 0u;

    const uint32_t have = vm_read32(RSX_REPORTS + 0xFE0u);
    const uint32_t want = have + 1u;
    uint32_t ucmd = 0u;
    for (uint32_t delta = 0u; delta + 8u <= ahead; delta += 4u) {
        const uint32_t candidate = (start + delta) & mask;
        const uint32_t ea = yz_rsx_io_to_ea(candidate);
        if (!ea)
            continue;
        const uint32_t cmd = vm_read32(ea);
        const uint32_t method = cmd & 0x3FFFCu;
        const uint32_t count = (cmd >> 18) & 0x7FFu;
        const uint32_t arg_ea =
            yz_rsx_io_to_ea((candidate + 4u) & mask);
        if ((method == 0xEB00u || method == 0xEB04u) &&
            count != 0u && arg_ea && vm_read32(arg_ea) == want) {
            ucmd = candidate;
            break;
        }
    }
    if (!ucmd)
        return 0u;

    static unsigned long accepted = 0, refused = 0;
    uint32_t steps = 0u;
    uint32_t chain_end = 0u;
    if (yz_a010_chain_complete(start, ucmd, start, put,
                               &steps, &chain_end)) {
        if (yz_a010_log_sparse(++accepted)) {
            fprintf(stderr,
                    "[a010-pending-chain] label=0x%08X next=0x%08X "
                    "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                    "chain-end=0x%06X PUT=0x%06X complete-steps=%u direct\n",
                    have, want, ucmd, start, start, chain_end, put, steps);
            fflush(stderr);
        }
        return start;
    }

    steps = 0u;
    if (yz_a010_chain_reaches_completion(start, ucmd, start, put, &steps)) {
        if (yz_a010_log_sparse(++accepted)) {
            fprintf(stderr,
                    "[a010-pending-chain] label=0x%08X next=0x%08X "
                    "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                    "PUT=0x%06X prefix-steps=%u staged-direct\n",
                    have, want, ucmd, start, start, put, steps);
            fflush(stderr);
        }
        return start;
    }

    unsigned rejected = 0u;
    for (uint32_t delta = 0u; delta + 0x30u <= ahead; delta += 4u) {
        const uint32_t candidate = (start + delta) & mask;
        if (candidate == ucmd)
            break;
        if (!yz_a010_generated_prologue_at(candidate))
            continue;
        struct yz_a010_chain_probe prefix_probe;
        yz_a010_probe_chain(candidate, ucmd, start, put, &prefix_probe);
        if (yz_a010_probe_is_safe_prefix(&prefix_probe, candidate,
                                         start, put)) {
            if (yz_a010_log_sparse(++accepted)) {
                fprintf(stderr,
                        "[a010-pending-chain] label=0x%08X next=0x%08X "
                        "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                        "segment-stop=0x%06X PUT=0x%06X "
                        "draw=%u/%u/%u/%u ordered-prefix\n",
                        have, want, ucmd, candidate, start,
                        prefix_probe.stop_pc, put, prefix_probe.begin,
                        prefix_probe.end, prefix_probe.arrays,
                        prefix_probe.indices);
                fflush(stderr);
            }
            return candidate;
        }
        steps = 0u;
        chain_end = 0u;
        if (yz_a010_chain_complete(candidate, ucmd, start, put,
                                   &steps, &chain_end)) {
            if (yz_a010_log_sparse(++accepted)) {
                fprintf(stderr,
                        "[a010-pending-chain] label=0x%08X next=0x%08X "
                        "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                        "chain-end=0x%06X PUT=0x%06X complete-steps=%u\n",
                        have, want, ucmd, candidate, start,
                        chain_end, put, steps);
                fflush(stderr);
            }
            return candidate;
        }
        steps = 0u;
        if (yz_a010_chain_reaches_completion(candidate, ucmd, start, put,
                                             &steps)) {
            if (yz_a010_log_sparse(++accepted)) {
                fprintf(stderr,
                        "[a010-pending-chain] label=0x%08X next=0x%08X "
                        "ucmd=0x%06X entry=0x%06X scan-start=0x%06X "
                        "PUT=0x%06X prefix-steps=%u staged\n",
                        have, want, ucmd, candidate, start, put, steps);
                fflush(stderr);
            }
            return candidate;
        }
        rejected++;
    }
    if (yz_a010_log_sparse(++refused)) {
        fprintf(stderr,
                "[a010-pending-chain] REFUSED label=0x%08X next=0x%08X "
                "ucmd=0x%06X scan-start=0x%06X PUT=0x%06X "
                "incomplete-prologues=%u\n",
                have, want, ucmd, start, put, rejected);
        fflush(stderr);
    }
    return 0u;
}

static int yz_a010_missing_release_try(uint32_t stopper_ea,
                                       uint32_t get,
                                       uint32_t put)
{
    static unsigned long repairs = 0;

    if (!g_yz_game_toc)
        return 0;

    const uint32_t ring = 0x800000u;
    const uint32_t ahead = (put - get + ring) % ring;
    /* Ring-full / deep-lag face: GET is parked on a segment-entry
     * jump-to-self while the producer has written far ahead (possibly an
     * entire lap behind it, throttle-parked short of GET), and the game's own
     * ledger says the stopper is fully retired (S[0x20]=0, S[0x1C]=0) or
     * S[0x20] == PUT exactly; only the physical release write went missing.
     * `ahead` can approach ring-0x80 here, so a ring/2 sanity bound would
     * refuse the repair's own strongest face; the stable-snapshot recheck and
     * the 5 s journal-quiet check carry the actual guarantees. The whole
     * deep-lag range is admitted as a distinct branch with stricter checks
     * (the ledger check, the journal-absence check over the wrap-aware scan)
     * and a sequential-only resume, never the linked-guess resume whose
     * mis-rejoin derails the walk. */
    const int deep_lag = ahead >= (ring >> 1);
    if (ahead == 0u)
        return 0;

    MemoryBarrier();
    const uint32_t state = vm_read32(g_yz_game_toc - 0x7410u);
    if (state < 0x10000u || state >= 0xE0000000u)
        return 0;
    const uint32_t base = vm_read32(state + 0x08u);
    const uint32_t end = vm_read32(state + 0x0Cu);
    const uint32_t head = vm_read32(state + 0x00u);
    const uint32_t pending = vm_read32(state + 0x20u);
    if (base < 0x10000u || end <= base || end - base > 0x1000000u ||
        head < base || head >= end)
        return 0;

    const uint32_t put_ea = yz_rsx_io_to_ea(put);
    if (deep_lag) {
        /* Deep-lag branch: the game's own ledger must be coherent about
         * this stopper being someone else's problem: no deferred ops
         * (S[0x1C]=0) and the pending cursor either fully cleared
         * (ring-full throttle, S[0x20]=0) or naming the current
         * commit at PUT exactly (drain-park, S[0x20]==put_ea, the
         * original repair condition). A cursor still naming this stopper
         * (or anything else) means the game owes protocol; wait. */
        const uint32_t latch = vm_read32(state + 0x1Cu);
        const int ledger_ok =
            latch == 0u && pending != stopper_ea &&
            (pending == 0u || (put_ea && pending == put_ea));
        if (!ledger_ok)
            return 0;
    } else if (!put_ea || pending != put_ea || pending == stopper_ea)
        return 0;

    /* A deferred release owns this stopper if it appears at any point before
     * the stable producer head.  In that case the ordered journal consumer,
     * not this direct-release recovery, must apply it. */
    const uint32_t expected = 0x20000000u | (get & 0x1FFFFFFCu);
    bool released_during_scan = false;
    const bool cancel_scan =
        InterlockedCompareExchange(&g_yz_cancel_released_stopper_scan, 0, 0) != 0;
    const uint32_t journal_entry = cancel_scan
        ? yz_gcm_stopper_release_entry_impl<true>(stopper_ea, expected,
                                                  &released_during_scan)
        : yz_gcm_stopper_release_entry(stopper_ea);
    /* Cancellation is never evidence of journal absence. Stop this recovery
     * attempt completely; the next FIFO step re-reads the producer's word. */
    if (released_during_scan)
        return 0;
    if (journal_entry != 0u)
        return 0;

    if (vm_read32(stopper_ea) != expected)
        return 0;

    /* Journal-quiet check: refuse while the producer's journal head is still
     * advancing. A missing release during journal activity is a late deferred
     * release, not a lost one: the game may write the 0x7F entries for a
     * stopper tens of seconds after a repair would have jumped it, and the
     * jump's walk can then end on a terminal non-command park. Repair only
     * once the head has been quiet for 5 s. */
    {
        static uint32_t rel_head_last;
        static uint64_t rel_head_change_ms;
        const uint64_t rel_now = GetTickCount64();
        if (rel_head_last != head) {
            rel_head_last = head;
            rel_head_change_ms = rel_now;
        }
        if (rel_now - rel_head_change_ms < 5000u)
            return 0;
    }

    /* Deep-lag resume: a blind sequential step (get+4) can move GET into an
     * EDGE data island (the words at get+4 are vertex floats, leaving a
     * non-command park); EDGE reserves data islands in the FIFO and links
     * around them. Island stoppers release with a jump forward, and the
     * island length lives only in the lost journal entry. Order of
     * preference:
     *  1. the island-aware, fail-closed pending-chain hunt, scan-bounded
     *     one segment ahead (the real PUT is nearly a ring behind in
     *     deep-lag geometry; the hunt's own ahead<ring/2 guard would refuse
     *     the raw PUT);
     *  2. refuse (stay parked) when no chain is found.
     * A single command-looking word at get+4 is not enough to prove that the
     * rest of the segment is commands: the walk can still run into a data
     * island and park permanently.
     * The linked-resume hazard (jumping a stopper whose release was still
     * coming) stays excluded by the journal-quiet check and the ledger
     * checks above, so using the hunt here does not re-open it. */
    uint32_t resume;
    if (deep_lag) {
        resume = yz_a010_find_pending_chain((get + 4u) & (ring - 1u),
                                            (get + 0x8000u) & (ring - 1u));
    } else {
        resume = yz_a010_find_pending_chain((get + 4u) & (ring - 1u), put);
    }
    if (!resume)
        return 0;

    /*
     * PUT is the producer's publication boundary.  Recheck the entire
     * producer snapshot after validating the candidate chain so concurrent
     * publication cannot turn a sound repair into a stale one.
     */
    MemoryBarrier();
    if (vm_read32(RSX_DMA_CONTROL + RSX_DMACTL_PUT) != put ||
        vm_read32(g_yz_game_toc - 0x7410u) != state ||
        vm_read32(state + 0x00u) != head ||
        vm_read32(state + 0x20u) != pending ||
        vm_read32(stopper_ea) != expected)
        return 0;

    vm_write32(stopper_ea,
               0x20000000u | (resume & 0x1FFFFFFCu));
    vm_write32(RSX_DMA_CONTROL + RSX_DMACTL_GET, resume);
    repairs++;
    fprintf(stderr,
            "[a010-missing-rel] repaired n=%lu old=0x%08X "
            "GET=0x%06X -> resume=0x%06X (%s) PUT=0x%06X ahead=0x%X "
            "new-pending=0x%08X head=0x%08X snapshot=stable\n",
            repairs, stopper_ea, get, resume,
            resume == ((get + 4u) & (ring - 1u)) ? "sequential" : "linked",
            put, ahead, pending, head);
    fflush(stderr);
    return 1;
}
