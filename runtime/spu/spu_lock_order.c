/*
 * Keep SPURS callbacks outside the process-wide SPU reservation-line lock.
 * Queue operations take their host mutex before entering the reservation
 * critical section, so notifying a queue before releasing that lock creates
 * an AB-BA cycle.
 */

#include <stdint.h>

extern void spu_lockline_unlock(void);
extern void cellSpursNotifyGuestWrite(uint32_t ea, uint32_t size);

void spu_lockline_unlock_then_notify_guest_write(
    int notify, uint32_t ea, uint32_t size)
{
    spu_lockline_unlock();
    if (notify)
        cellSpursNotifyGuestWrite(ea, size);
}
