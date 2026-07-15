# Ordered EDGE journal HLE

## Why this exists

The original RSX lock predates every host release lever. The game publishes a
0x20-byte-entry GCM/EDGE operation journal, the Sony SPU consumer is expected to
apply those entries, and RSX waits at a jump-to-self command until the preceding
patches are complete. In the measured bad state the producer head is frozen with
complete entries still available, while the lifted consumer cycles below the head
without draining them.

`YZ_APPLY_REL`, `YZ_PARK_REL`, and `YZ_LOOKAHEAD` were later bypass experiments.
They can clear a tag-0x7f stopper without first applying the patch entries before
it. That explains the later 800-frame run followed by torn FIFO content; it does
not explain the original consumer lock.

## Hardware contract being preserved

The EDGE library reserves a command-buffer hole with jump-to-self commands. SPU work
generates the geometry output and the RSX commands that fill the hole. Completion
is ordered: output/command DMA must complete before the jump-to-self is cleared or
another completion signal is published. SPURS job synchronization likewise treats
output-DMA completion as part of the work that precedes a `SYNC`/notification.

The ownership model is therefore:

1. PPU/SPU producers append complete journal records and publish the head.
2. One consumer applies records in order and retires each tag after its operation.
3. A release record clears the RSX stopper only after prior patches are visible.
4. RSX then consumes the completed command-buffer region.

This does not require cycle-accurate SPURS timing. It requires the same visible
ordering, completion, wait/wake, and retirement semantics.

## Current implementation

`YZ_JRNL_HLE=1` enables an opt-in wedge takeover in the RSX self-jump handler.
It is not a polling thread and does not scan ahead of GET.

Before the first write it requires:

- GET parked on a jump-to-self with PUT committed ahead;
- a valid journal arena and a producer head unchanged for the configured stability
  window (16 ms by default);
- a matching tag-0x7f release for that exact stopper;
- every live entry from the HLE cursor through that release to have a decoded,
  valid format; and
- the validated entry span to remain byte-for-byte stable across two reads.

It then applies the span in order, retires each tag only after completing its
operation, uses a release fence before clearing a stopper, and advances GET over
the resulting NOP. While this mode is enabled, the release-only `YZ_APPLY_REL` and
`YZ_PARK_REL` paths are disabled.

Decoded records in this checkout:

| Tag | Operation |
|---|---|
| `0x10` | `memcpy(source=entry+0x04, size=entry+0x08, destination=entry+0x0c)` |
| `0x7f` | ordered stopper clear; stopper EA is at `entry+0x04` |

Still undecoded: `0x04`, `0x08`, `0x09`, `0x0a`, `0x0d`, and `0x11`.

An undecoded tag is intentionally fail-closed. No prefix operation is applied and
the stopper remains locked. The log includes all eight words of the blocking entry:

```text
[jrnl-hle] BLOCKED status=unsupported-tag entry=... tag=... words=...
```

That line, plus the matching RPCS3/real-consumer writes for the same entry, is the
next input needed to add a decoder safely. Do not replace this with a guessed
generic copy or a release-only fallback.

`YZ_JRNL_HLE_STABLE_MS=<0..5000>` changes only the frozen-head takeover debounce;
it is not emulated SPU timing.

## Verification

The portable transaction core has a standalone test:

```text
cmake -S tests/edge_journal_hle -B tests/edge_journal_hle/build
cmake --build tests/edge_journal_hle/build
tests/edge_journal_hle/build/edge_journal_hle_test
```

It checks that memcpy runs before release/retirement and that an unknown tag causes
zero mutation. A Windows boot is still required to validate the game-specific
entry layout and decode the remaining tags. This Mac checkout lacks the generated
PPU/PRX/SPU outputs needed to build or boot `yakuza_recomp` itself.
