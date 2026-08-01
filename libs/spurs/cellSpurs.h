/*
 * Firmware-free, host-native cellSpurs ABI.
 *
 * Public objects are exact-size guest byte arrays.  Native synchronization
 * and worker state lives in side tables keyed by the object address.
 */
#ifndef PS3RECOMP_CELL_SPURS_H
#define PS3RECOMP_CELL_SPURS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#  define SPURS_ALIGNAS(n) alignas(n)
#else
#  define SPURS_ALIGNAS(n) _Alignas(n)
#endif

#define CELL_SPURS_CORE_ERROR_AGAIN        ((s32)0x80410701u)
#define CELL_SPURS_CORE_ERROR_INVAL        ((s32)0x80410702u)
#define CELL_SPURS_CORE_ERROR_NOSYS        ((s32)0x80410703u)
#define CELL_SPURS_CORE_ERROR_NOMEM        ((s32)0x80410704u)
#define CELL_SPURS_CORE_ERROR_NOENT        ((s32)0x80410706u)
#define CELL_SPURS_CORE_ERROR_BUSY         ((s32)0x8041070Au)
#define CELL_SPURS_CORE_ERROR_STAT         ((s32)0x8041070Fu)
#define CELL_SPURS_CORE_ERROR_ALIGN        ((s32)0x80410710u)
#define CELL_SPURS_CORE_ERROR_NULL_POINTER ((s32)0x80410711u)

#define CELL_SPURS_TASK_ERROR_AGAIN        ((s32)0x80410901u)
#define CELL_SPURS_TASK_ERROR_INVAL        ((s32)0x80410902u)
#define CELL_SPURS_TASK_ERROR_NOSYS        ((s32)0x80410903u)
#define CELL_SPURS_TASK_ERROR_NOMEM        ((s32)0x80410904u)
#define CELL_SPURS_TASK_ERROR_NOENT        ((s32)0x80410906u)
#define CELL_SPURS_TASK_ERROR_NOEXEC       ((s32)0x80410907u)
#define CELL_SPURS_TASK_ERROR_PERM         ((s32)0x80410909u)
#define CELL_SPURS_TASK_ERROR_BUSY         ((s32)0x8041090Au)
#define CELL_SPURS_TASK_ERROR_STAT         ((s32)0x8041090Fu)
#define CELL_SPURS_TASK_ERROR_ALIGN        ((s32)0x80410910u)
#define CELL_SPURS_TASK_ERROR_NULL_POINTER ((s32)0x80410911u)
#define CELL_SPURS_TASK_ERROR_FATAL        ((s32)0x80410914u)
#define CELL_SPURS_TASK_ERROR_SHUTDOWN     ((s32)0x80410920u)

#define CELL_SPURS_JOB_ERROR_AGAIN         ((s32)0x80410A01u)
#define CELL_SPURS_JOB_ERROR_INVAL         ((s32)0x80410A02u)
#define CELL_SPURS_JOB_ERROR_NOMEM         ((s32)0x80410A04u)
#define CELL_SPURS_JOB_ERROR_NOEXEC        ((s32)0x80410A07u)
#define CELL_SPURS_JOB_ERROR_BUSY          ((s32)0x80410A0Au)
#define CELL_SPURS_JOB_ERROR_DESCRIPTOR    ((s32)0x80410A0Bu)
#define CELL_SPURS_JOB_ERROR_STAT          ((s32)0x80410A0Fu)
#define CELL_SPURS_JOB_ERROR_ALIGN         ((s32)0x80410A10u)
#define CELL_SPURS_JOB_ERROR_NULL_POINTER  ((s32)0x80410A11u)
#define CELL_SPURS_JOB_ERROR_UNKNOWN_CMD   ((s32)0x80410A18u)
#define CELL_SPURS_JOB_ERROR_ABORT         ((s32)0x80410A1Cu)
#define CELL_SPURS_JOB_ERROR_INVALID_BIN   ((s32)0x80410A1Fu)

#define CELL_SPURS_MAX_SPU 8
#define CELL_SPURS_MAX_TASK 128
#define CELL_SPURS_TASKSET_SIZE 0x1900
#define CELL_SPURS_TASKSET2_SIZE 0x2900
#define CELL_SPURS_EVENT_FLAG_CLEAR_AUTO 0
#define CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL 1
#define CELL_SPURS_EVENT_FLAG_OR 0
#define CELL_SPURS_EVENT_FLAG_AND 1
#define CELL_SPURS_EVENT_FLAG_SPU2SPU 0
#define CELL_SPURS_EVENT_FLAG_SPU2PPU 1
#define CELL_SPURS_EVENT_FLAG_PPU2SPU 2
#define CELL_SPURS_EVENT_FLAG_ANY2ANY 3

typedef u32 CellSpursWorkloadId;
typedef u32 CellSpursTaskId;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[4096]; } CellSpurs;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[8192]; } CellSpurs2;
typedef struct { SPURS_ALIGNAS(8) u8 bytes[512]; } CellSpursAttribute;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[CELL_SPURS_TASKSET_SIZE]; } CellSpursTaskset;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[CELL_SPURS_TASKSET2_SIZE]; } CellSpursTaskset2;
typedef struct { SPURS_ALIGNAS(8) u8 bytes[512]; } CellSpursTasksetAttribute;
typedef struct { SPURS_ALIGNAS(8) u8 bytes[512]; } CellSpursTasksetAttribute2;
typedef struct { SPURS_ALIGNAS(16) u8 bytes[256]; } CellSpursTaskAttribute;
typedef struct { SPURS_ALIGNAS(16) u8 bytes[256]; } CellSpursTaskAttribute2;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[128]; } CellSpursEventFlag;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[128]; } CellSpursQueue;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[128]; } CellSpursLFQueue;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[128]; } CellSpursBarrier;
/* SDK payload is 272 bytes even though each standalone object must start on a
 * 128-byte boundary (the SDK provides a separate padded array wrapper). */
typedef struct { u8 bytes[272]; } CellSpursJobChain;
typedef struct { SPURS_ALIGNAS(8) u8 bytes[512]; } CellSpursJobChainAttribute;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[128]; } CellSpursJobGuard;
typedef struct { SPURS_ALIGNAS(8) u8 bytes[256]; } CellSpursWorkloadAttribute;
typedef struct { SPURS_ALIGNAS(128) u8 bytes[128]; } CellSpursTaskExitCode;

typedef union {
    u32 u32[4];
    u64 u64[2];
    u8 bytes[16];
} CellSpursTaskArgument;
typedef CellSpursTaskArgument CellSpursTaskLsPattern;
typedef struct {
    u64 eaElf;
    u32 sizeContext;
    u32 reserved;
    CellSpursTaskLsPattern lsPattern;
} CellSpursTaskBinInfo;

s32 _cellSpursAttributeInitialize(CellSpursAttribute*, u32, u32, u32, s32, s32, u8);
s32 cellSpursAttributeInitialize(CellSpursAttribute*, s32, s32, s32, u8);
s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute*, const char*, u32);
s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute*, s32);
s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute*);
s32 cellSpursInitializeWithAttribute(CellSpurs*, const CellSpursAttribute*);
s32 cellSpursInitializeWithAttribute2(CellSpurs2*, const CellSpursAttribute*);
s32 cellSpursFinalize(CellSpurs*);

s32 _cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute*, u32, u32,
                                         u64, const u8*, u32);
void _cellSpursTasksetAttribute2Initialize(CellSpursTasksetAttribute2*, u32);
s32 cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute*, const char*);
s32 cellSpursTasksetAttributeSetTasksetSize(CellSpursTasksetAttribute*, size_t);
s32 cellSpursCreateTaskset(CellSpurs*, CellSpursTaskset*, u64, const u8*, u32);
s32 cellSpursCreateTasksetWithAttribute(CellSpurs*, CellSpursTaskset*,
                                        const CellSpursTasksetAttribute*);
s32 cellSpursCreateTaskset2(CellSpurs*, CellSpursTaskset2*,
                            const CellSpursTasksetAttribute2*);
s32 cellSpursDestroyTaskset2(CellSpursTaskset2*);
s32 cellSpursShutdownTaskset(CellSpursTaskset*);
s32 cellSpursJoinTaskset(CellSpursTaskset*);

s32 _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute*, u32, u32,
                                      const void*, const void*,
                                      const CellSpursTaskArgument*);
s32 cellSpursTaskAttributeSetExitCodeContainer(CellSpursTaskAttribute*,
                                               CellSpursTaskExitCode*);
s32 cellSpursCreateTaskWithAttribute(CellSpursTaskset*, CellSpursTaskId*,
                                     const CellSpursTaskAttribute*);
s32 cellSpursCreateTask(CellSpursTaskset*, CellSpursTaskId*, const void*,
                        const void*, u32, const CellSpursTaskLsPattern*,
                        const CellSpursTaskArgument*);
s32 cellSpursCreateTask2WithBinInfo(CellSpursTaskset2*, CellSpursTaskId*,
                                    const CellSpursTaskBinInfo*,
                                    const CellSpursTaskArgument*, void*,
                                    const char*, void*);
s32 _cellSpursSendSignal(CellSpursTaskset*, CellSpursTaskId);
s32 cellSpursJoinTask2(CellSpursTaskset2*, CellSpursTaskId, s32*);
s32 cellSpursTryJoinTask2(CellSpursTaskset2*, CellSpursTaskId, s32*);
s32 cellSpursTaskExitCodeTryGet(CellSpursTaskExitCode*, s32*);
s32 cellSpursTaskGetContextSaveAreaSize(u32*, const CellSpursTaskLsPattern*);

s32 _cellSpursEventFlagInitialize(CellSpurs*, CellSpursTaskset*,
                                  CellSpursEventFlag*, u32, u32);
s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag*);
s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag*);
s32 cellSpursEventFlagSet(CellSpursEventFlag*, u16);
s32 cellSpursEventFlagWait(CellSpursEventFlag*, u16*, u32);
s32 cellSpursEventFlagTryWait(CellSpursEventFlag*, u16*, u32);
s32 cellSpursEventFlagClear(CellSpursEventFlag*, u16);

s32 _cellSpursQueueInitialize(CellSpurs*, CellSpursTaskset*, CellSpursQueue*,
                              const void*, u32, u32, u32);
s32 cellSpursQueueAttachLv2EventQueue(CellSpursQueue*);
s32 cellSpursQueueDetachLv2EventQueue(CellSpursQueue*);
s32 cellSpursQueuePushBody(CellSpursQueue*, const void*, u32);
s32 cellSpursQueuePopBody(CellSpursQueue*, void*, u32, u32);
s32 _cellSpursLFQueueInitialize(void*, CellSpursLFQueue*, const void*,
                                u32, u32, u32);
s32 cellSpursLFQueueAttachLv2EventQueue(CellSpursLFQueue*);
s32 cellSpursLFQueueDetachLv2EventQueue(CellSpursLFQueue*);
s32 _cellSpursLFQueuePushBody(CellSpursLFQueue*, const void*, u32);
s32 cellSpursBarrierInitialize(CellSpursTaskset*, CellSpursBarrier*, u32);

s32 _cellSpursJobChainAttributeInitialize(u32, u32, CellSpursJobChainAttribute*,
                                          const u64*, u16, u16, const u8*, u32,
                                          u32, u32, u32, u32, u32, u32);
s32 cellSpursJobChainAttributeSetName(CellSpursJobChainAttribute*, const char*);
s32 cellSpursCreateJobChainWithAttribute(CellSpurs*, CellSpursJobChain*,
                                         const CellSpursJobChainAttribute*);
s32 cellSpursRunJobChain(const CellSpursJobChain*);
s32 cellSpursShutdownJobChain(const CellSpursJobChain*);
s32 cellSpursJoinJobChain(CellSpursJobChain*);
s32 cellSpursJobGuardInitialize(const CellSpursJobChain*, CellSpursJobGuard*,
                                u32, u8, u8);
s32 cellSpursJobGuardNotify(CellSpursJobGuard*);
s32 cellSpursJobGuardReset(CellSpursJobGuard*);

/* Runtime coherence bridge: wake native waiters after an SPU writes guest
 * memory. The write itself remains authoritative; this function only notifies
 * predicates that re-read the guest bytes. */
void cellSpursNotifyGuestWrite(u32 ea, u32 size);
void cellSpursNotifyPpuGuestWrite(u32 ea, u32 size);

/* Compatibility surface used elsewhere in the generic runtime. */
s32 cellSpursInitialize(CellSpurs*, s32, s32, s32, u8);
s32 cellSpursSendSignal(CellSpursTaskset*, CellSpursTaskId);
s32 cellSpursJoinTask(CellSpursTaskset*, CellSpursTaskId, s32*);
s32 cellSpursDestroyTaskset(CellSpursTaskset*);
s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute*);
s32 cellSpursTaskAttributeInitialize(CellSpursTaskAttribute*);
s32 cellSpursEventFlagInitialize(CellSpursTaskset*, CellSpursEventFlag*, u32, u32);

#ifdef __cplusplus
}
#endif
#undef SPURS_ALIGNAS
#endif
