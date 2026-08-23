/*
 * Verified highest-boundary contracts for the game's out-of-line GCM state
 * setters.  These functions accept (context, value) and emit exactly one
 * incrementing one-argument method packet.  Keeping the manifest in a small
 * data-only module lets the passive producer gate and offline tests share the
 * same audited facts without invoking the legacy RSX decoder.
 */

#ifndef PS3RECOMP_RSX_NR_PRODUCER_CONTRACT_H
#define PS3RECOMP_RSX_NR_PRODUCER_CONTRACT_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_nr_direct_setter_contract {
    u32 function_ea;
    u32 method;
} rsx_nr_direct_setter_contract;

const rsx_nr_direct_setter_contract*
rsx_nr_direct_setter_by_function(u32 function_ea);

const rsx_nr_direct_setter_contract*
rsx_nr_direct_setter_by_method(u32 method);

u32 rsx_nr_direct_setter_count(void);

/* Encode the byte-exact two-word oracle for tests and passive comparison.
 * The eventual active path consumes the typed (method,value) contract at the
 * wrapper and does not call this encoder or a packet decoder. */
int rsx_nr_direct_setter_packet(u32 function_ea, u32 value, u32 out[2]);

#ifdef __cplusplus
}
#endif

#endif
