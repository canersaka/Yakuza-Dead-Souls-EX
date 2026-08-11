/* Independent host state for the process runtime's exit-spawn callbacks. */

#include "sysPrxForUser.h"
#include <stdatomic.h>

/* These values are guest OPD addresses, not host function pointers.  The
 * current runner does not replace its process image, but retaining the exact
 * registrations keeps startup state truthful and gives a future exit-spawn
 * implementation the callbacks it must dispatch through g_ps3_guest_caller. */
static atomic_uint_least32_t s_process_atexitspawn_callback;
static atomic_uint_least32_t s_process_at_Exitspawn_callback;

s32 _sys_process_atexitspawn(u32 callback)
{
    atomic_store_explicit(&s_process_atexitspawn_callback, callback,
                          memory_order_release);
    return CELL_OK;
}

s32 _sys_process_at_Exitspawn(u32 callback)
{
    atomic_store_explicit(&s_process_at_Exitspawn_callback, callback,
                          memory_order_release);
    return CELL_OK;
}

u32 sys_process_atexitspawn_callback(void)
{
    return atomic_load_explicit(&s_process_atexitspawn_callback,
                                memory_order_acquire);
}

u32 sys_process_at_Exitspawn_callback(void)
{
    return atomic_load_explicit(&s_process_at_Exitspawn_callback,
                                memory_order_acquire);
}
