#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#include "Ifx_Types.h"

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

typedef struct {
    volatile uint32 cmd;     /* handshake state (MB_IDLE / MB_REQ / MB_DONE) */
    volatile uint32 inputA;  /* first  operand — written by CPU0              */
    volatile uint32 inputB;  /* second operand — written by CPU0              */
    volatile uint32 result;  /* answer         — written by the worker core   */
} Mailbox;

/* g_mbCpu1: CPU0 <-> CPU1.  Worker computes  result = inputA * inputA  */
/* g_mbCpu2: CPU0 <-> CPU2.  Worker computes  result = inputA + inputB  */
extern Mailbox g_mbCpu1;
extern Mailbox g_mbCpu2;

#endif /* SHARED_MEM_H */
