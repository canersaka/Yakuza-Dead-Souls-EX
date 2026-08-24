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
    u32 packet_word_count;
    u32 semantic_hash;
} rsx_nr_draw_arrays_contract;

u32 rsx_nr_draw_hash_begin(u32 primitive, u32 indexed);
u32 rsx_nr_draw_hash_batch(u32 hash, u32 first, u32 count);

/* Returns zero for an invalid/unsupported argument shape. */
int rsx_nr_draw_arrays_contract_init(rsx_nr_draw_arrays_contract* out,
                                     u32 primitive, u32 first, u32 count);

/* Byte-exact retained fallback packet emitted by func_00EBEA48: three
 * non-incrementing VTX_CACHE_INVALIDATE zeros, BEGIN, remainder-first batch,
 * groups of at most 2047 full 256-vertex batches, then END. Returns the
 * encoded word count, or zero for an invalid contract/output capacity. */
u32 rsx_nr_draw_arrays_packet(const rsx_nr_draw_arrays_contract* draw,
                              u32* out, u32 out_capacity);

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

/* func_00EB0D90 is the game's highest complete fragment-package producer.
 * It owns allocation, microcode relocation and construction of the reusable
 * shader command segment. Passive validation obtains the final referenced
 * byte span once, but semantic identity is independent of that segment's
 * address so copied and SPU-published replays remain valid.
 *
 * Fragment inline constants live inside the program image. Exact content
 * identity therefore changes when parameters are patched; structural shader
 * identity is computed separately by rsx_fp_decompiler. */
#define RSX_NR_FRAGMENT_PROGRAM_FUNCTION 0x00EB0D90u
#define RSX_NR_FRAGMENT_PROGRAM_MAX_BYTES 0x10000u

typedef struct rsx_nr_fragment_program_contract {
    u32 byte_count;
    u32 control;
    u64 content_hash;
    u64 semantic_hash;
} rsx_nr_fragment_program_contract;

u64 rsx_nr_fragment_program_content_hash(const u8* bytes, u32 byte_count);
u64 rsx_nr_fragment_program_semantic_hash(u64 program_hash,
                                          u32 byte_count, u32 control);
int rsx_nr_fragment_program_contract_init(
    rsx_nr_fragment_program_contract* out, const u8* bytes,
    u32 byte_count, u32 control);

/* NV4097_SET_SHADER_PROGRAM and NV4097_SET_SHADER_CONTROL are independent,
 * persistent state. A draw is classifiable only after both have been seen;
 * repeated updates replace one field without consuming or clearing the
 * other. This tiny state machine is shared by the passive live oracle and
 * deterministic tests so adjacency is never inferred. */
typedef struct rsx_nr_fragment_binding_state {
    u32 program_word;
    u32 control;
    u32 valid_mask;
} rsx_nr_fragment_binding_state;

void rsx_nr_fragment_binding_init(rsx_nr_fragment_binding_state* state);
void rsx_nr_fragment_binding_set_program(
    rsx_nr_fragment_binding_state* state, u32 program_word);
void rsx_nr_fragment_binding_set_control(
    rsx_nr_fragment_binding_state* state, u32 control);
int rsx_nr_fragment_binding_snapshot(
    const rsx_nr_fragment_binding_state* state,
    u32* program_word, u32* control);

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

/* NV4097 main-memory report offsets are relative to Sony's dedicated
 * 16 MiB report IO aperture, not raw RSX IO addresses.  Keeping this
 * translation in the audited contract prevents a report timestamp from
 * being published over a command-buffer word at the same low offset. */
#define RSX_NR_MAIN_REPORT_IO_BASE 0x0E000000u
int rsx_nr_main_report_io_range(u32 offset, u32 size, u32* io_offset);

/* A CALL target is a separately published display-list segment.  While its
 * one-level return is live, primary FIFO PUT does not bound the target even
 * when its IO address happens to fall below the primary ring size.  The
 * section scanner still validates and copies every word before ownership;
 * this helper only answers whether [pc,pc+size) is available under the
 * correct publication domain. */
typedef enum rsx_nr_fifo_range_status {
    RSX_NR_FIFO_RANGE_READY = 0,
    RSX_NR_FIFO_RANGE_NOT_READY,
    RSX_NR_FIFO_RANGE_WINDOW
} rsx_nr_fifo_range_status;

rsx_nr_fifo_range_status rsx_nr_fifo_section_range_status(
    u32 pc, u32 size, u32 put, u32 call_return, u32 ring_size);

/* Fixed-memory exact control-flow visit set for transactional FIFO section
 * discovery. A generation reset is O(1); revisiting the same (PC,return)
 * pair proves a cycle and prevents an otherwise valid multi-packet loop from
 * consuming the section scanner's entire step budget. Hash collisions are
 * resolved by exact comparison, never treated as a cycle. */
/* Match the live section step/method ceiling. The exact visit set must detect
 * cycles without introducing a smaller artificial refusal boundary for a
 * valid long linear command section. Reset remains O(1), so this larger
 * fixed table has no per-section clearing cost. */
#define RSX_NR_FIFO_VISIT_CAPACITY 262144u
typedef struct rsx_nr_fifo_visit_set {
    u32 pc[RSX_NR_FIFO_VISIT_CAPACITY];
    u32 ret[RSX_NR_FIFO_VISIT_CAPACITY];
    u32 stamp[RSX_NR_FIFO_VISIT_CAPACITY];
    u32 generation;
} rsx_nr_fifo_visit_set;

void rsx_nr_fifo_visit_reset(rsx_nr_fifo_visit_set* set);
/* 1 = first visit, 0 = exact revisit, -1 = invalid/full. */
int rsx_nr_fifo_visit_note(rsx_nr_fifo_visit_set* set, u32 pc, u32 ret);
/* Exact membership in the current generation. This is deliberately read-only:
 * a rejected scanner path can remain legacy while GET walks that path without
 * clearing or rebuilding the visit table at every command. */
int rsx_nr_fifo_visit_contains(
    const rsx_nr_fifo_visit_set* set, u32 pc, u32 ret);

/* Complete-section graphics-family contract. A clear may accompany the draw
 * family only when the same already-decoded section contains a draw. This
 * admits an indivisible clear+draw render pass without reviving independently
 * mixed per-clear ownership. Clear-only sections still require the explicit
 * clear family. */
#define RSX_NR_GRAPHICS_FAMILY_DRAW  (1u << 0)
#define RSX_NR_GRAPHICS_FAMILY_CLEAR (1u << 1)
int rsx_nr_complete_section_family_allowed(
    u32 enabled_families, u32 action_family, u32 section_draw_count);

/* The title's two local-memory depth-only shadow producers are a proven
 * cross-section dependency: later world passes sample these zetas. Until the
 * native depth producer is bit-equivalent to the established renderer, the
 * complete containing FIFO section must remain legacy. This predicate is
 * deliberately exact so ordinary depth/color passes stay native-eligible. */
#define RSX_NR_YZ_SHADOW_ZETA0 0x02310000u
#define RSX_NR_YZ_SHADOW_ZETA1 0x02910000u
int rsx_nr_yz_unproven_shadow_depth_producer(
    u32 zeta_location, u32 zeta_offset, u32 color_mask,
    u32 depth_write_enable);

/* The imported flip contract is not complete at E944 (queue buffer).  Its
 * E924 head command is the first boundary after which a renderer may claim
 * the preceding command-list frame without mixing clears/draws/presentation
 * between native and legacy owners. */
int rsx_nr_fifo_frame_boundary(u32 method, u32 arg);

/* Legacy methods which can touch shared GPU/guest resources rather than only
 * updating decoder state. An ordered native list must retire before any of
 * these dispatch, including the A010-path NV308A inline-transfer window. */
int rsx_nr_legacy_gpu_action(u32 method, u32 arg);

#ifdef __cplusplus
}
#endif

#endif
