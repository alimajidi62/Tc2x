#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#include "Ifx_Types.h"
#include "IfxCpu.h"

/*
 * Three-core mailbox demo  (TC29B, TriBoard TC2X7 V1.0)
 *
 * All Mailbox structs live in the non-cached LMU (0xB0000000).
 * Non-cached memory has no cache-coherency issues between cores, so a
 * volatile flag is sufficient for the handshake — no cache flush needed.
 *
 * Protocol (same for both mailboxes):
 *
 *   CPU0                       Worker (CPU1 or CPU2)
 *   ----                       ---------------------
 *   write inputA / inputB
 *   cmd = MB_REQ  ─────────►   spin until cmd == MB_REQ
 *                              read inputA / inputB
 *                              write result
 *              ◄──────────────  cmd = MB_DONE
 *   read result
 *   cmd = MB_IDLE
 */

#define MB_IDLE  0u   /* mailbox is free                         */
#define MB_REQ   1u   /* CPU0 wrote inputs, worker must process  */
#define MB_DONE  2u   /* worker wrote result, CPU0 must read it  */

#define MB_CPU1  0u   /* index for the CPU1 worker slot */
#define MB_CPU2  1u   /* index for the CPU2 worker slot */
#define MB_NWORKERS 2u

/*
 * SHARED_NC: optional section attribute that places a variable in the
 * non-cached LMU alias (0xB0000000).  On TC29x this is unnecessary for
 * DSPR-based globals (DSPR is always non-cached), but shown here as an
 * example of how to force a specific RAM region.
 */
#ifdef __GNUC__
#define SHARED_NC __attribute__((section(".shared_nc"), used))
#else
#define SHARED_NC  /* TASKING: use #pragma section or leave at default DSPR */
#endif

/* Single struct covers all worker cores; use MB_CPU1 / MB_CPU2 as index. */
typedef struct {
    volatile uint32 cmd[MB_NWORKERS];     /* handshake state per worker */
    volatile uint32 inputA[MB_NWORKERS];  /* first  operand from CPU0   */
    volatile uint32 inputB[MB_NWORKERS];  /* second operand from CPU0   */
    volatile uint32 result[MB_NWORKERS];  /* answer written by worker   */
} Mailbox;

extern Mailbox g_mbCpus;

/* ==========================================================================
 * SpinBox — the same request/result demo, but with a symmetric shared
 * accumulator that all three cores update concurrently.
 *
 * Contrast with Mailbox
 * ---------------------
 *   Mailbox  : asymmetric — CPU0 is always master; each worker gets its own
 *              dedicated slot; a plain volatile flag is enough because only
 *              one writer ever touches each cmd word.
 *
 *   SpinBox  : symmetric — any core can call SpinBox_add() at any time;
 *              all three cores race to update the same total, so a hardware
 *              CMPSWAP-based mutex is required for correct mutual exclusion.
 *
 * Debugger invariant (true whenever no core holds the lock):
 *   total == perCore[0] + perCore[1] + perCore[2]
 *
 * If you remove the SpinBox_add() lock calls and re-run, the invariant will
 * eventually break — that's the race condition the spinlock prevents.
 * ==========================================================================
 */
typedef struct {
    IfxCpu_mutexLock lock;        /* 0=free; acquired via CMPSWAP instruction */
    volatile uint32  total;       /* sum of every core's contributions        */
    volatile uint32  perCore[3];  /* per-core tally, indexed by CPU ID 0–2   */
} SpinBox;

/* Acquire the lock, add val to total and perCore[coreId], then release. */
static inline void SpinBox_add(SpinBox *sb, uint8 coreId, uint32 val)
{
    while (!IfxCpu_acquireMutex(&sb->lock)) {}
    sb->perCore[coreId] += val;
    sb->total            += val;
    IfxCpu_releaseMutex(&sb->lock);
}

extern SpinBox g_spinBox;

#endif /* SHARED_MEM_H */
