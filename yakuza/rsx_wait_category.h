#ifndef YAKUZA_RSX_WAIT_CATEGORY_H
#define YAKUZA_RSX_WAIT_CATEGORY_H

#include <stdint.h>

/* Outcome of one FIFO consumer step: ADVANCING when GET moved or a method was
 * dispatched, otherwise why the consumer stayed where it was. */
enum yz_rsx_wait_category : uint32_t {
    YZ_RSX_WAIT_ADVANCING = 0,
    YZ_RSX_WAIT_EMPTY,
    YZ_RSX_WAIT_PARTIAL_PACKET,
    YZ_RSX_WAIT_SELF_STOPPER,
    YZ_RSX_WAIT_SEMAPHORE,
    YZ_RSX_WAIT_UNFINALIZED_HOLE,
    YZ_RSX_WAIT_BAD_FLOW,
    YZ_RSX_WAIT_NO_CONTEXT,
    YZ_RSX_WAIT_CATEGORY_COUNT
};

#endif
