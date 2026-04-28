# SPU PPU Fallback API

ps3recomp does not execute SPU code (no SPU ISA JIT, no SPU local-store
model). For most games this is fine — many SPU jobs produce side effects
that nothing actually depends on, or the PPU code is happy when the
group reports "all threads exited cleanly".

Some games need real SPU output: PhyreEngine asset decompressors, audio
mixers, particle simulations, physics. Stubbing those silently leaves
PPU code reading garbage.

The SPU PPU-fallback registry lets a per-game shim provide a PPU-side
implementation for any SPU job, keyed on the SPU image's entry point.
When the game starts a thread group, threads with a matching fallback
run on a host thread (real concurrency); `sys_spu_thread_group_join`
blocks until they're all done.

## API

`#include "ps3emu/spu_fallback.h"`

```c
typedef int32_t (*spu_ppu_fallback_fn)(uint32_t tid, uint32_t args_ea,
                                       uint32_t args_size, void* user);

int  spu_register_ppu_fallback(uint32_t entry_point,
                               spu_ppu_fallback_fn handler, void* user);
int  spu_unregister_ppu_fallback(uint32_t entry_point);
spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry_point,
                                            void** out_user);
```

Handler args:
- `tid` — synthesized SPU thread id from `sys_spu_thread_initialize`
- `args_ea` — guest EA of the args block (set via `sys_spu_thread_set_argument`
  or the args parameter of `sys_spu_thread_initialize`)
- `args_size` — currently always 0 (size isn't part of the syscall API)
- `user` — opaque pointer registered alongside the handler

Return value becomes the SPU thread's exit status. The worst (most
negative) status across all threads in a group becomes the group's
exit status, reported back via `sys_spu_thread_group_join`.

## Lifecycle

Register at startup, before any SPU activity:

```c
static int32_t my_decompress_fallback(uint32_t tid, uint32_t args_ea,
                                      uint32_t args_size, void* user)
{
    /* args_ea points at a guest struct the SPU job would have processed.
     * Decode it via vm_read*; do the work on the host; write results
     * back via vm_write*. */
    uint32_t src_ea  = vm_read32(args_ea + 0);
    uint32_t dst_ea  = vm_read32(args_ea + 4);
    uint32_t src_len = vm_read32(args_ea + 8);
    /* ... read src bytes from vm_base + src_ea, decompress on host,
     * write to vm_base + dst_ea ... */
    return 0;  /* CELL_OK */
}

static void register_my_spu_fallbacks(void)
{
    /* Entry point comes from sys_spu_image_open: it parses the ELF and
     * writes the entry to image+4. The "[SPU] image_open" log line
     * shows it; you'll typically read it once with an instrumented run
     * and then hard-code it. */
    spu_register_ppu_fallback(0x000028F0, my_decompress_fallback, NULL);
}
```

Find the entry point via the `[SPU] image_open` log:

```
[SPU] image_open img=0x10001234 path='/dev_flash/sys/spu/decompress.elf' entry=0x000028F0
```

## Execution model

- Synchronous registration; not thread-safe. Call all
  `spu_register_ppu_fallback()` once at startup.
- Asynchronous execution. `sys_spu_thread_group_start` spawns one host
  thread per registered fallback (Win32 `CreateThread`, POSIX
  `pthread_create`). Threads without a fallback complete instantly with
  status 0.
- `sys_spu_thread_group_join` blocks on each running thread's completion
  event, then collects the worst exit status into the group state.
- `sys_spu_thread_get_exit_status` returns CELL_ESTAT (0x80010003) if the
  thread is still in flight — match Sony's documented behaviour.

## Caveats

- The fallback runs on a host thread, not in the guest VM. It cannot
  call recompiled guest functions or take guest locks. It can read/write
  guest memory freely via `vm_read*` / `vm_write*` helpers.
- Be deterministic about output bytes — games may hash/checksum results.
- If you need to coordinate with PPU code that's running concurrently,
  use the existing host-side sync primitives (mutexes, atomics). Do
  *not* use the guest's lwmutex APIs from a fallback.
- The args_size parameter is currently always 0. If your job needs to
  know the descriptor size, encode it in the descriptor itself.

## Related

- `runtime/syscalls/lv2_register.c` — SPU group/thread state machine and
  the dispatch site in `sys_spu_thread_group_start_handler`.
- `include/ps3emu/spu_fallback.h` — public header.
- `runtime/syscalls/spu_fallback.c` — registry implementation.
