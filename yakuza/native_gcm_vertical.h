/*
 * Highest-safe-boundary GCM producer interception.
 *
 * Selected lifted SDK wrappers are renamed to func_XXXXXXXX_lifted by the
 * PPU lifter. native_gcm_vertical.cpp owns the original symbols, so every
 * direct call, indirect lookup and trampoline reaches the same gate before
 * any guest FIFO packet is constructed.
 *
 * YZ_NR_VERTICAL is deliberately default-off. The first supported mode is
 * "shadow": wrappers record the typed semantic operation, call the unchanged
 * lifted body, and the FIFO consumer records the corresponding decoded wire
 * operation. "active-basic" owns only the proven primary-context reference
 * and user-command wrappers, records their typed payload against the exact
 * guest FIFO address, and lets the FIFO consumer execute that payload in
 * place of the reserved NOP span. No operation is skipped in shadow mode.
 */

#ifndef YAKUZA_NATIVE_GCM_VERTICAL_H
#define YAKUZA_NATIVE_GCM_VERTICAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void yz_nr_vertical_init(void);
void yz_nr_vertical_observe_method(uint32_t method, uint32_t arg,
                                   uint32_t packet_ea);

typedef enum yz_nr_vertical_consume_result {
    YZ_NR_VERTICAL_CONSUME_MISS = 0,
    YZ_NR_VERTICAL_CONSUME_EXECUTED,
    YZ_NR_VERTICAL_CONSUME_WAIT,
    YZ_NR_VERTICAL_CONSUME_FATAL,
} yz_nr_vertical_consume_result;

/* Called only by the serialized RSX FIFO consumer. A successful claim owns
 * exactly *word_count guest words and has already executed the typed action. */
yz_nr_vertical_consume_result
yz_nr_vertical_consume(uint32_t packet_ea, uint32_t* word_count);

void yz_nr_vertical_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
