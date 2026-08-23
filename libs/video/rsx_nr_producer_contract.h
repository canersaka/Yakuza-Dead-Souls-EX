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

/* func_00EBEA48 is the title's out-of-line CellGcmSetDrawArrays-style
 * producer. Its arguments are (context, primitive, first, count). The wire
 * format splits the range into at most 256 vertices per DRAW_ARRAYS word;
 * this contract hashes the normalized typed draw rather than its packet
 * headers, so the producer and consumer can be compared without routing the
 * producer through an RSX decoder. */
#define RSX_NR_DRAW_ARRAYS_FUNCTION 0x00EBEA48u
#define RSX_NR_DRAW_CONTRACT_MAX_BATCHES 4096u

typedef struct rsx_nr_draw_arrays_contract {
    u32 primitive;
    u32 first;
    u32 count;
    u32 batch_count;
    u32 semantic_hash;
} rsx_nr_draw_arrays_contract;

u32 rsx_nr_draw_hash_begin(u32 primitive, u32 indexed);
u32 rsx_nr_draw_hash_batch(u32 hash, u32 first, u32 count);

/* Returns zero for an invalid/unsupported argument shape. */
int rsx_nr_draw_arrays_contract_init(rsx_nr_draw_arrays_contract* out,
                                     u32 primitive, u32 first, u32 count);

/* func_00EBD92C is the real ABI entry for the title's transform-program
 * producer (00EBD968 is only its post-prologue continuation).  The wrapper
 * emits LOAD+START, the complete instruction image, and ATTRIB_EN before any
 * program-associated constant metadata.  This compact semantic contract is
 * intentionally independent of the legacy packet encoding. */
#define RSX_NR_VERTEX_PROGRAM_FUNCTION 0x00EBD92Cu
/* The NV4097 transform store has 544 instruction slots, four words each
 * (the same limit as RSX_DSP_VP_INSTR). */
#define RSX_NR_VERTEX_PROGRAM_MAX_WORDS (544u * 4u)

typedef struct rsx_nr_vertex_program_contract {
    u32 start_slot;
    u32 instruction_count;
    u32 word_count;
    u32 attrib_input_mask;
    u32 code_hash;      /* start slot + instruction words only */
    u32 semantic_hash;
    u32 packet_word_count; /* through ATTRIB_EN + the 1EF8 metadata word */
} rsx_nr_vertex_program_contract;

u32 rsx_nr_vertex_program_hash_begin(u32 start_slot);
u32 rsx_nr_vertex_program_hash_word(u32 hash, u32 word);
u32 rsx_nr_vertex_program_hash_end(u32 hash, u32 word_count,
                                   u32 attrib_input_mask);

/* words contains instruction_count * 4 host-order guest words. */
int rsx_nr_vertex_program_contract_init(
    rsx_nr_vertex_program_contract* out, u32 instruction_count,
    u32 start_slot, u32 attrib_input_mask, const u32* words);

/* Native-GCM's imported flip wrapper is a fixed queue+flip template.  The
 * wait-label variant prefixes the same template with one NV406E acquire.
 * Keeping the byte-exact words in this audited contract lets an exact-EA
 * native owner remain copy/replay safe: copied bytes have no sidecar owner
 * and execute as complete legacy FIFO commands. */
#define RSX_NR_FLIP_CONTRACT_MAX_WORDS 10u

typedef struct rsx_nr_flip_contract {
    u32 buffer_id;
    u32 wait_for_label;
    u32 label_offset;
    u32 label_value;
    u32 word_count;
    u32 flip_word_index;
    u32 words[RSX_NR_FLIP_CONTRACT_MAX_WORDS];
} rsx_nr_flip_contract;

/* Returns zero for buffer ids outside the eight GCM display slots. */
int rsx_nr_flip_contract_init(rsx_nr_flip_contract* out, u32 buffer_id,
                              int wait_for_label, u32 label_index,
                              u32 label_value);

#ifdef __cplusplus
}
#endif

#endif
