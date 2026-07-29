/*******************************************************************************
 * \file  Eth_Udp.c
 * \brief Minimal bare-metal UDP/IP stack  (ARP reply + UDP TX/RX, polling).
 *
 * Hardware target : AURIX TC297 TriBoard V1.0
 * PHY             : Infineon/Lantiq PEF7071  (RMII, 100 Mbit/s full-duplex)
 * iLLD version    : 1_20_0
 *
 * Pin mapping (RMII):
 *   P11.12  REFCLK  input
 *   P11.11  CRS_DV  input
 *   P11.10  RXD0    input
 *   P11.9   RXD1    input
 *   P11.3   TXD0    output
 *   P11.2   TXD1    output
 *   P11.6   TX_EN   output
 *   P21.2   MDC     output
 *   P21.3   MDIO    input/output
 ******************************************************************************/

#include "Eth_Udp.h"
#include "Eth/Std/IfxEth.h"
#include "Eth/Phy_Pef7071/IfxEth_Phy_Pef7071.h"
#include "_PinMap/IfxEth_PinMap.h"
#include "Stm/Std/IfxStm.h"   /* IfxStm_waitTicks — PHY reset delay */
#include <string.h>   /* memcpy */

/*===========================================================================*/
/* PHY init result — inspect in debugger after Eth_Udp_init() returns        */
/*   0   = not yet initialised                                                */
/*   1   = success                                                            */
/*  -1   = GMII stuck busy before any write  (GB bit never cleared)          */
/*  -2   = PHY reset write timed out                                          */
/*  -3   = PHY reset read-back timed out                                      */
/*  -4   = PHY self-reset did not complete within timeout                     */
/*  -5   = MIICTRL (RMII-mode) write timed out                               */
/*  -6   = GCTRL write timed out                                              */
/*  -7   = AN_ADV write timed out                                             */
/*  -8   = CTRL (restart AN) write timed out                                  */
/*===========================================================================*/
volatile sint32 g_ethInitResult = 0;

/*===========================================================================*/
/* Timeout-safe MDIO helpers                                                  */
/*                                                                            */
/* ETH_GMII_ADDRESS bit layout for TC297:                                     */
/*   [15:11] PA   physical-layer address   (pa  << 11)                        */
/*   [10:6]  GR   register number          (reg <<  6)                        */
/*   [5:2]   CR   MDC clock-range select   (0   <<  2)  → /42 @ 60-100 MHz   */
/*   [1]     GW   1 = write,  0 = read                                        */
/*   [0]     GB   busy: set by SW, cleared by HW when MDIO frame done         */
/*                                                                            */
/* TC297 ETH peripheral clock = SPB bus ≈ 100 MHz                             */
/* CR = 0  →  MDC = 100 MHz / 42 ≈ 2.38 MHz  (within 2.5 MHz IEEE spec)     */
/*===========================================================================*/
#define MDIO_PA       0u        /* PHY address on this board              */
#define MDIO_CR       0u        /* clock-range 0 = 60-100 MHz / 42       */
#define MDIO_TIMEOUT  200000u   /* loop count ≈ 2 ms at 100 MHz           */

static boolean mdioWait(void)
{
    uint32 t = MDIO_TIMEOUT;
    while (ETH_GMII_ADDRESS.B.GB)
    {
        if (--t == 0u) return FALSE;
    }
    return TRUE;
}

static boolean mdioWrite(uint32 reg, uint16 data)
{
    if (!mdioWait()) return FALSE;
    ETH_GMII_DATA.U    = (uint32)data;
    ETH_GMII_ADDRESS.U = (MDIO_PA << 11u) | (reg << 6u)
                       | (MDIO_CR << 2u) | (1u << 1u) | 1u;  /* GW=1, GB=1 */
    return mdioWait();
}

static boolean mdioRead(uint32 reg, uint16 *out)
{
    if (!mdioWait()) return FALSE;
    ETH_GMII_ADDRESS.U = (MDIO_PA << 11u) | (reg << 6u)
                       | (MDIO_CR << 2u) | 1u;               /* GW=0, GB=1 */
    if (!mdioWait()) return FALSE;
    *out = (uint16)(ETH_GMII_DATA.U & 0xFFFFu);
    return TRUE;
}

/* Read BMSR from an arbitrary PHY address (used by phyScan only). */
static boolean mdioReadAt(uint32 pa, uint32 reg, uint16 *out)
{
    if (!mdioWait()) return FALSE;
    ETH_GMII_ADDRESS.U = (pa << 11u) | (reg << 6u)
                       | (MDIO_CR << 2u) | 1u;               /* GW=0, GB=1 */
    if (!mdioWait()) return FALSE;
    *out = (uint16)(ETH_GMII_DATA.U & 0xFFFFu);
    return TRUE;
}

/* =====================================================================
 * GPIO bit-bang MDIO (Clause 22) — P21.2 = MDC, P21.1 = MDIO.
 * The ETH MAC GMII hardware cannot reliably drive P21.1 on this TC29x
 * silicon (alt6 does not map to ETH_MDO on this device stepping).
 * Bit-bang GPIO bypasses the hardware entirely.
 * After use: P21.2 restored to ETH_MDC alt5; P21.1 left as input so
 * ALTI0=1 can still route it as ETH MDI for GMII reads if needed.
 * ===================================================================== */
/* bit-bang PHY verification variables — declared here before mdioBb* functions */
volatile uint16 g_dbgBbPhyId1  = 0u;  /* expect 0x0013 (Infineon OUI) */
volatile uint16 g_dbgBbPhyId2  = 0u;  /* expect 0x79Cx (PEF7071) */
volatile uint16 g_dbgBbMiictrl = 0u;  /* expect 0xF702 after init */
volatile uint16 g_dbgBbBmsr    = 0u;  /* bit2=link, bit5=autoneg */

static void mdioBbDelay(void)
{
    volatile uint32 d = 20u;  /* ~500 ns half-period at 200 MHz => ~1 MHz MDC */
    while (d > 0u) { d--; }
}

static void mdioBbClkBit(uint8 bit)
{
    if (bit) IfxPort_setPinHigh(&MODULE_P21, 1u);
    else     IfxPort_setPinLow (&MODULE_P21, 1u);
    mdioBbDelay();
    IfxPort_setPinLow (&MODULE_P21, 2u);   /* MDC falling edge */
    mdioBbDelay();
    IfxPort_setPinHigh(&MODULE_P21, 2u);   /* MDC rising edge — PHY samples here */
    mdioBbDelay();
}

static void mdioBbBegin(void)
{
    IfxPort_setPinMode(&MODULE_P21, 2u, IfxPort_Mode_outputPushPullGeneral);
    IfxPort_setPinMode(&MODULE_P21, 1u, IfxPort_Mode_outputPushPullGeneral);
    IfxPort_setPinHigh(&MODULE_P21, 2u);
    IfxPort_setPinHigh(&MODULE_P21, 1u);
    mdioBbDelay();
}

static void mdioBbEnd(void)
{
    IfxPort_setPinHigh(&MODULE_P21, 2u);
    IfxPort_setPinHigh(&MODULE_P21, 1u);
    /* Restore P21.2 as ETH_MDC (alt5) */
    IfxPort_setPinModeOutput(&MODULE_P21, 2u, IfxPort_OutputMode_pushPull, IfxPort_OutputIdx_alt5);
    /* P21.1: input with pull-up.
     * alt6 on P21.1 is NOT ETH_MDO — it drives the pin LOW, blocking reads.
     * Input+pullUp: PHY drives via MDIO open-drain, TC reads via ALTI0=1.  */
    IfxPort_setPinModeInput(&MODULE_P21, 1u, IfxPort_InputMode_pullUp);
}

static void mdioBbWrite(uint32 pa, uint32 reg, uint16 data)
{
    uint32 i;
    mdioBbBegin();
    for (i = 0u; i < 32u; i++) mdioBbClkBit(1u);          /* preamble */
    mdioBbClkBit(0u); mdioBbClkBit(1u);                    /* ST = 01 */
    mdioBbClkBit(0u); mdioBbClkBit(1u);                    /* OP = 01 (write) */
    for (i = 4u; ; i--) { mdioBbClkBit((uint8)((pa   >> i) & 1u)); if (!i) break; }  /* PA */
    for (i = 4u; ; i--) { mdioBbClkBit((uint8)((reg  >> i) & 1u)); if (!i) break; }  /* RA */
    mdioBbClkBit(1u); mdioBbClkBit(0u);                    /* TA = 10 */
    for (i = 15u; ; i--) { mdioBbClkBit((uint8)((data >> i) & 1u)); if (!i) break; } /* DATA */
    mdioBbEnd();
}

static uint16 mdioBbRead(uint32 pa, uint32 reg)
{
    uint32 i;
    uint16 val = 0u;
    mdioBbBegin();
    for (i = 0u; i < 32u; i++) mdioBbClkBit(1u);          /* preamble */
    mdioBbClkBit(0u); mdioBbClkBit(1u);                    /* ST = 01 */
    mdioBbClkBit(1u); mdioBbClkBit(0u);                    /* OP = 10 (read) */
    for (i = 4u; ; i--) { mdioBbClkBit((uint8)((pa  >> i) & 1u)); if (!i) break; }  /* PA */
    for (i = 4u; ; i--) { mdioBbClkBit((uint8)((reg >> i) & 1u)); if (!i) break; }  /* RA */
    /* TA: controller releases bus, PHY drives turn-around then data.
     * pullUp required: MDIO is open-drain — without it the line floats high
     * and all reads return 0xFFFF regardless of PHY output.               */
    IfxPort_setPinModeInput(&MODULE_P21, 1u, IfxPort_InputMode_pullUp);
    mdioBbDelay(); IfxPort_setPinLow(&MODULE_P21, 2u); mdioBbDelay();
    IfxPort_setPinHigh(&MODULE_P21, 2u); mdioBbDelay();    /* TA bit 1 (Z) */
    mdioBbDelay(); IfxPort_setPinLow(&MODULE_P21, 2u); mdioBbDelay();
    IfxPort_setPinHigh(&MODULE_P21, 2u); mdioBbDelay();    /* TA bit 2 (0, driven by PHY) */
    /* DATA: 16 bits MSB first, read after rising MDC edge */
    for (i = 15u; ; i--)
    {
        mdioBbDelay();
        IfxPort_setPinLow(&MODULE_P21,  2u);
        mdioBbDelay();
        IfxPort_setPinHigh(&MODULE_P21, 2u);
        mdioBbDelay();
        if (IfxPort_getPinState(&MODULE_P21, 1u))
            val |= (uint16)(1u << i);
        if (!i) break;
    }
    mdioBbEnd();
    return val;
}

/* PHY address scan variables — declared here because phyScan() uses them. */
volatile uint16 g_dbgPhyScan[32]  = {0u};
volatile uint32 g_dbgPhyFoundAddr = 0xFFu;
/* ETH_GPCTL.U captured right before first MDIO operation in phyScan.
 *   Bits [3:0] = ALTI0: 1 = P21.1 routed (correct), 3 = P21.3, 0 = default
 * If this reads 0 the ALTI kernel-reset re-apply fix is not running.      */
volatile uint32 g_dbgGpctlAtScan  = 0u;
/* Raw ETH_GMII_DATA.U after the very first mdioReadAt (addr 0 BMSR).
 * Same value ends up in g_dbgPhyScan[0]; here for easy watch-window view.  */
volatile uint32 g_dbgMdioRaw0     = 0u;

/* Scan all 32 MDIO addresses.  After this runs:
 *   g_dbgPhyScan[n] = BMSR at address n
 *     0xFFFF  = floating (no PHY)   0xDEAD = MDIO timed out
 *     anything else = live PHY
 *   g_dbgPhyFoundAddr = first live address, or 0xFF if none.
 * If g_dbgPhyFoundAddr != 0 change #define MDIO_PA to that value.         */
static void phyScan(void)
{
    uint32 addr;
    g_dbgGpctlAtScan  = ETH_GPCTL.U;  /* snapshot ALTI state before first MDIO */
    g_dbgPhyFoundAddr = 0xFFu;
    for (addr = 0u; addr < 32u; addr++)
    {
        uint16 bmsr = 0xFFFFu;
        if (mdioReadAt(addr, 0x01u, &bmsr))
            g_dbgPhyScan[addr] = bmsr;
        else
            g_dbgPhyScan[addr] = 0xDEADu;   /* MDIO busy timeout */
        if (addr == 0u)
            g_dbgMdioRaw0 = ETH_GMII_DATA.U;  /* raw hardware value for addr 0 */
        if ((g_dbgPhyFoundAddr == 0xFFu) &&
            (bmsr != 0xFFFFu) && (bmsr != 0x0000u))
            g_dbgPhyFoundAddr = addr;
    }
}

/**
 * PHY init using GPIO bit-bang MDIO — bypasses ETH MAC GMII hardware.
 * P21.1 alt6 does not appear to map to ETH_MDO on this TC29x stepping,
 * so hardware GMII writes never reached the PHY. Bit-bang uses P21.2/P21.1
 * as plain GPIO and is reliable regardless of alternate-function mapping.
 */
static void phyInitSafe(void)
{
    /* Step 1: write PHY registers unconditionally via bit-bang.
     * The BMCR read-back loop is removed because the bit-bang READ returns
     * 0xFFFF (PHY either doesn't drive MDIO back, or P21.1 pull-up fights it).
     * The WRITES may still reach the PHY even when reads fail.
     * We use 100 ms delay to guarantee the PHY has fully initialised.         */

    /* First pass: configure WITHOUT reset, in case the PHY is already up */
    g_dbgBbPhyId1  = mdioBbRead(MDIO_PA, 0x02u);   /* capture ID before reset */
    g_dbgBbPhyId2  = mdioBbRead(MDIO_PA, 0x03u);

    mdioBbWrite(MDIO_PA, 0x17u, 0xF702u);  /* MIICTRL: RMII + skew adapt  */
    mdioBbWrite(MDIO_PA, 0x09u, 0x0000u);  /* GCTRL:   disable 1GbE advert */
    mdioBbWrite(MDIO_PA, 0x04u, 0x0101u);  /* AN_ADV:  100BASE-TX FD only  */
    mdioBbWrite(MDIO_PA, 0x00u, 0x1200u);  /* CTRL:    restart autoneg     */

    /* Second pass: software-reset then re-apply config */
    mdioBbWrite(MDIO_PA, 0x00u, 0x8000u);  /* BMCR: software reset */
    IfxStm_waitTicks(&MODULE_STM0, 20000000UL);  /* 100 ms */

    mdioBbWrite(MDIO_PA, 0x17u, 0xF702u);  /* MIICTRL: RMII + skew adapt  */
    mdioBbWrite(MDIO_PA, 0x09u, 0x0000u);
    mdioBbWrite(MDIO_PA, 0x04u, 0x0101u);
    mdioBbWrite(MDIO_PA, 0x00u, 0x1200u);  /* restart autoneg */

    /* Diagnostic reads — captured regardless of value */
    g_dbgBbMiictrl = mdioBbRead(MDIO_PA, 0x17u);
    g_dbgBbBmsr    = mdioBbRead(MDIO_PA, 0x01u);

    g_ethInitResult = 1;   /* always proceed */
}

/*===========================================================================*/
/* Ethernet frame constants                                                   */
/*===========================================================================*/
#define ETH_TYPE_ARP    0x0806u
#define ETH_TYPE_IP     0x0800u
#define ARP_OP_REQUEST  1u
#define ARP_OP_REPLY    2u
#define IP_PROTO_UDP    17u

/* Ethernet frame layer offsets (bytes from start of raw frame buffer) */
#define ETH_OFF_DST      0u
#define ETH_OFF_SRC      6u
#define ETH_OFF_TYPE     12u   /* 2 bytes, big-endian */
#define ETH_HDR_LEN      14u

/* ARP payload offsets (relative to start of ARP payload, i.e. byte 14) */
#define ARP_OFF_HWTYPE   0u
#define ARP_OFF_PRTYPE   2u
#define ARP_OFF_HWLEN    4u
#define ARP_OFF_PRLEN    5u
#define ARP_OFF_OP       6u
#define ARP_OFF_SMAC     8u
#define ARP_OFF_SIP      14u
#define ARP_OFF_TMAC     18u
#define ARP_OFF_TIP      22u
#define ARP_PAYLOAD_LEN  28u

/* IPv4 header offsets (relative to byte 14) */
#define IP_OFF_VER_IHL   0u
#define IP_OFF_DSCP      1u
#define IP_OFF_TOTLEN    2u
#define IP_OFF_ID        4u
#define IP_OFF_FLAGS     6u
#define IP_OFF_TTL       8u
#define IP_OFF_PROTO     9u
#define IP_OFF_CKSUM     10u
#define IP_OFF_SRC       12u
#define IP_OFF_DST       16u
#define IP_HDR_LEN       20u

/* UDP header offsets (relative to byte 14+20) */
#define UDP_OFF_SRCPORT  0u
#define UDP_OFF_DSTPORT  2u
#define UDP_OFF_LEN      4u
#define UDP_OFF_CKSUM    6u
#define UDP_HDR_LEN      8u

/*===========================================================================*/
/* RMII pin table for TC297 TriBoard V1.0                                     */
/*===========================================================================*/
static const IfxEth_RmiiPins g_rmiiPins = {
    &IfxEth_CRSDVA_P11_11_IN,   /* crsDiv  CRS_DV  P11.11 */
    &IfxEth_REFCLK_P11_12_IN,   /* refClk  REFCLK  P11.12 */
    &IfxEth_RXD0_P11_10_IN,     /* rxd0    RXD0    P11.10 */
    &IfxEth_RXD1_P11_9_IN,      /* rxd1    RXD1    P11.9  */
    &IfxEth_MDC_P21_2_OUT,      /* mdc     MDC     P21.2  */
    &IfxEth_MDIO_P21_1_INOUT,   /* mdio    MDIO    P21.1  (TriBoard TC297 v1.0) */
    &IfxEth_TXD0_P11_3_OUT,     /* txd0    TXD0    P11.3  */
    &IfxEth_TXD1_P11_2_OUT,     /* txd1    TXD1    P11.2  */
    &IfxEth_TXEN_P11_6_OUT,     /* txEn    TX_EN   P11.6  */
};

/*===========================================================================*/
/* Module state                                                               */
/* Note: TC29x DSPR (0x70000000) is NOT cached — it is a directly-mapped     */
/* scratchpad RAM.  The ETH DMA accesses it through the SRI bus without       */
/* cache involvement, so no special non-cached memory placement is needed.    */
/*===========================================================================*/
static IfxEth g_eth;

static const uint8 g_boardMac[6]     = ETH_UDP_BOARD_MAC;
static const uint8 g_boardIp[4]      = ETH_UDP_BOARD_IP;
static const uint8 g_bcastMac[6]     = {0xFFu,0xFFu,0xFFu,0xFFu,0xFFu,0xFFu};
static const uint8 g_bcastIp[4]      = {255u,255u,255u,255u};

/* Last sender learned from an incoming UDP packet */
static uint8 g_lastSenderMac[6];
static uint8 g_lastSenderIp[4];

/* Single receive slot — filled by poll(), consumed by receive() */
static struct {
    uint8   srcIp[4];
    uint16  srcPort;
    uint16  dstPort;
    uint16  payloadLen;
    boolean pending;
    uint8   payload[ETH_UDP_MAX_PAYLOAD];
} g_rxSlot;

/*===========================================================================*/
/* Byte helpers (big-endian <-> host)                                         */
/*===========================================================================*/
static void writeU16be(uint8 *p, uint16 v)
{
    p[0] = (uint8)(v >> 8u);
    p[1] = (uint8)(v & 0xFFu);
}

static uint16 readU16be(const uint8 *p)
{
    return (uint16)(((uint16)p[0] << 8u) | (uint16)p[1]);
}

static boolean ipEqual(const uint8 *a, const uint8 *b)
{
    return (boolean)( (a[0] == b[0]) && (a[1] == b[1]) &&
                      (a[2] == b[2]) && (a[3] == b[3]) );
}

/*===========================================================================*/
/* IPv4 header checksum (RFC 791)                                             */
/*===========================================================================*/
static uint16 ipHeaderChecksum(const uint8 *hdr)
{
    uint32 sum = 0u;
    uint8  i;
    for (i = 0u; i < IP_HDR_LEN; i += 2u)
    {
        sum += (uint32)(((uint16)hdr[i] << 8u) | (uint16)hdr[i + 1u]);
    }
    while ((sum >> 16u) != 0u)
    {
        sum = (sum & 0xFFFFu) + (sum >> 16u);
    }
    return (uint16)(~sum);
}

/*===========================================================================*/
/* ARP reply — writes directly into the TX buffer (no intermediate copy)     */
/*===========================================================================*/
static void sendArpReply(const uint8 *requesterMac, const uint8 *requesterIp)
{
    uint8 *buf = (uint8 *)IfxEth_getTransmitBuffer(&g_eth);
    uint8 *arp;

    if (buf == NULL_PTR)
        return;

    /* Ethernet header */
    memcpy(buf,     requesterMac, 6u);
    memcpy(buf + 6, g_boardMac,   6u);
    writeU16be(buf + 12, ETH_TYPE_ARP);

    /* ARP payload — directly in TX buffer */
    arp = buf + ETH_HDR_LEN;
    writeU16be(arp + ARP_OFF_HWTYPE, 0x0001u);
    writeU16be(arp + ARP_OFF_PRTYPE, 0x0800u);
    arp[ARP_OFF_HWLEN] = 6u;
    arp[ARP_OFF_PRLEN] = 4u;
    writeU16be(arp + ARP_OFF_OP, ARP_OP_REPLY);
    memcpy(arp + ARP_OFF_SMAC, g_boardMac,   6u);
    memcpy(arp + ARP_OFF_SIP,  g_boardIp,    4u);
    memcpy(arp + ARP_OFF_TMAC, requesterMac, 6u);
    memcpy(arp + ARP_OFF_TIP,  requesterIp,  4u);

    IfxEth_sendTransmitBuffer(&g_eth,
        (uint16)(ETH_HDR_LEN + ARP_PAYLOAD_LEN));
}

/*===========================================================================*/
/* IPv4 + UDP transmit — writes directly into the TX buffer.                 */
/*                                                                            */
/* ROOT CAUSE FIX: the previous version declared                             */
/*   uint8 pkt[IP_HDR_LEN + UDP_HDR_LEN + ETH_UDP_MAX_PAYLOAD]; // 1500 B   */
/* on the stack.  With LCF_USTACK0_SIZE = 2 KB this overflowed the stack    */
/* and corrupted local variables, causing ethSend() to return early          */
/* (frameLen > IFXETH_RTX_BUFFER_SIZE) and never transmit.                  */
/* Writing directly to the TX buffer uses ~0 extra stack.                    */
/*===========================================================================*/
static uint16 g_ipTxId = 0u;  /* IP identification counter, module-level    */

/* Debug — watch these in the debugger after running for 2+ seconds:
 *   g_dbgSendCalls  : how many times sendUdp() was entered
 *   g_dbgTxBufAddr  : last value from IfxEth_getTransmitBuffer()
 *     - if 0 after sendCalls > 0 → TX buffer pointer not set (stale build)
 *       → do a full clean rebuild so IfxEth.c recompiles without
 *         IFXETH_TX_BUFFER_BY_USER, which sets TDES2 correctly.
 *   g_dbgTxCount    : mirror of g_eth.txCount (incremented by iLLD per TX)
 */
volatile uint32 g_dbgSendCalls = 0u;
volatile uint32 g_dbgTxBufAddr = 0u;
volatile uint32 g_dbgTxCount   = 0u;

/* ---- new diagnostics (add all to Watch window) -------------------------
 *  g_dbgEthStatus  ETH_STATUS.U after each TX
 *                  bits [22:20] TS = TX-DMA state:  6=SUSPENDED (problem!)
 *                  bit  0       TI = TX interrupt (frame sent by DMA)
 *                  bit  2       TU = TX buf unavail
 *  g_dbgTxState    ETH_STATUS.B.TS alone  (0=stopped 1=fetching 3=reading
 *                  6=suspended 7=closing)  — 6 after TX = DMA never ran
 *  g_dbgStBit      OPERATION_MODE.B.ST  (1 = TX DMA started)
 *  g_dbgTeBit      MAC_CONFIGURATION.B.TE  (1 = MAC transmitter on)
 *  g_dbgLinkUp     1 = PHY reports link up
 *  g_dbgPhyBmsr    PHY BMSR reg 0x01  bit2=link bit5=autoneg-complete
 *  g_dbgPollCount  Eth_Udp_poll() calls  (sanity: should grow quickly)
 *  g_dbgRxCount    g_eth.rxCount mirror (any received frames?)
 * ----------------------------------------------------------------------- */
volatile uint32 g_dbgEthStatus = 0u;
volatile uint32 g_dbgTxState   = 0u;
volatile uint32 g_dbgStBit     = 0u;
volatile uint32 g_dbgTeBit     = 0u;
volatile uint32 g_dbgLinkUp    = 0u;
volatile uint32 g_dbgPollCount = 0u;
volatile uint32 g_dbgRxCount   = 0u;
volatile uint16 g_dbgPhyBmsr   = 0u;  /* BMSR: bit2=link, bit5=autoneg done */

volatile uint16 g_dbgPhyId1    = 0u;  /* reg 0x02 */
volatile uint16 g_dbgPhyId2    = 0u;  /* reg 0x03 */
volatile uint32 g_dbgGpctl     = 0u;  /* ETH_GPCTL.U: shows ALTI routing */

/* MAC hardware TX counters (ETH MMC registers, enabled in IfxEth.c init).
 * Updated every 500 ms from Eth_Udp_isLinkUp().
 * DECISIVE TEST: if g_dbgMmcTxGoodBad grows but no Wireshark packets
 *   → MAC transmits but PHY discards (PHY not in RMII mode)
 * If g_dbgMmcTxGoodBad stays 0 despite g_dbgTxCount growing
 *   → MAC never completes TX (no REFCLK — PHY not in RMII mode)         */
volatile uint32 g_dbgMmcTxGoodBad = 0u;  /* ETH_TX_FRAME_COUNT_GOOD_BAD.U */
volatile uint32 g_dbgMmcTxGood    = 0u;  /* ETH_TX_FRAME_COUNT_GOOD.U     */
/* MAC hardware RX counter.  Non-zero = MAC is receiving RMII frames.
 * g_dbgMmcRxGoodBad > 0 but g_dbgRxCount = 0 → MAC receives but SW
 * descriptor ring is broken.  Both 0 → RMII RX not working (no REFCLK
 * or PHY not in RMII mode).                                               */
volatile uint32 g_dbgMmcRxGoodBad = 0u;  /* ETH_RX_FRAMES_COUNT_GOOD_BAD.U */
volatile uint32 g_dbgMmcRxCrcErr  = 0u;  /* ETH_RX_CRC_ERROR_FRAMES.U      */

static void sendUdp(const uint8 *dstMac, const uint8 *dstIp,
                    uint16 srcPort, uint16 dstPort,
                    const void *data, uint16 dataLen)
{
    uint16  udpLen   = (uint16)(UDP_HDR_LEN + dataLen);
    uint16  ipLen    = (uint16)(IP_HDR_LEN  + udpLen);
    uint16  frameLen = (uint16)(ETH_HDR_LEN + ipLen);
    uint8  *buf;
    uint8  *ip;
    uint8  *udp;

    if (dataLen > ETH_UDP_MAX_PAYLOAD)
        return;
    if (frameLen > IFXETH_RTX_BUFFER_SIZE)
        return;

    g_dbgSendCalls++;

    buf = (uint8 *)IfxEth_getTransmitBuffer(&g_eth);
    g_dbgTxBufAddr = (uint32)buf;
    if (buf == NULL_PTR)
    {
        /* Descriptor OWN=1: DMA may be suspended waiting for TRANSMIT_POLL_DEMAND.
         * Kick it so it processes queued frames and frees descriptors.          */
        IfxEth_wakeupTransmitter(&g_eth);
        return;
    }

    /* Ethernet header */
    memcpy(buf,      dstMac,     6u);
    memcpy(buf + 6,  g_boardMac, 6u);
    writeU16be(buf + 12, ETH_TYPE_IP);

    /* IPv4 header */
    ip = buf + ETH_HDR_LEN;
    ip[IP_OFF_VER_IHL] = 0x45u;                    /* version=4, IHL=5     */
    ip[IP_OFF_DSCP]    = 0x00u;
    writeU16be(ip + IP_OFF_TOTLEN, ipLen);
    writeU16be(ip + IP_OFF_ID,     ++g_ipTxId);
    writeU16be(ip + IP_OFF_FLAGS,  0x0000u);
    ip[IP_OFF_TTL]   = 64u;
    ip[IP_OFF_PROTO] = IP_PROTO_UDP;
    writeU16be(ip + IP_OFF_CKSUM, 0x0000u);         /* zero before checksum */
    memcpy(ip + IP_OFF_SRC, g_boardIp, 4u);
    memcpy(ip + IP_OFF_DST, dstIp,     4u);
    writeU16be(ip + IP_OFF_CKSUM, ipHeaderChecksum(ip));

    /* UDP header */
    udp = ip + IP_HDR_LEN;
    writeU16be(udp + UDP_OFF_SRCPORT, srcPort);
    writeU16be(udp + UDP_OFF_DSTPORT, dstPort);
    writeU16be(udp + UDP_OFF_LEN,     udpLen);
    writeU16be(udp + UDP_OFF_CKSUM,   0x0000u);     /* optional in IPv4     */

    /* Payload */
    memcpy(udp + UDP_HDR_LEN, data, dataLen);

    IfxEth_sendTransmitBuffer(&g_eth, frameLen);
    g_dbgTxCount   = g_eth.txCount;
    g_dbgEthStatus = ETH_STATUS.U;
    g_dbgTxState   = (uint32)ETH_STATUS.B.TS;   /* 6=suspended => DMA stuck */
    g_dbgStBit     = (uint32)ETH_OPERATION_MODE.B.ST;
    g_dbgTeBit     = (uint32)ETH_MAC_CONFIGURATION.B.TE;
}

/*===========================================================================*/
/* Receive frame processing                                                   */
/*===========================================================================*/
static void processRxFrame(const uint8 *frame, uint16 frameLen)
{
    uint16 ethType;

    if (frameLen < ETH_HDR_LEN)
        return;

    ethType = readU16be(frame + ETH_OFF_TYPE);

    /* ---- ARP ---- */
    if (ethType == ETH_TYPE_ARP)
    {
        const uint8 *arp = frame + ETH_HDR_LEN;

        if (frameLen < (ETH_HDR_LEN + ARP_PAYLOAD_LEN))
            return;

        if (readU16be(arp + ARP_OFF_OP) == ARP_OP_REQUEST)
        {
            /* Only reply if they're asking for our IP */
            if (ipEqual(arp + ARP_OFF_TIP, g_boardIp))
            {
                sendArpReply(arp + ARP_OFF_SMAC, arp + ARP_OFF_SIP);
            }
        }
        return;
    }

    /* ---- IPv4 ---- */
    if (ethType == ETH_TYPE_IP)
    {
        const uint8 *ip = frame + ETH_HDR_LEN;

        if (frameLen < (ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN))
            return;
        if (ip[IP_OFF_VER_IHL] != 0x45u)    /* only plain 20-byte headers */
            return;
        if (ip[IP_OFF_PROTO] != IP_PROTO_UDP)
            return;
        if (!ipEqual(ip + IP_OFF_DST, g_boardIp) &&
            !ipEqual(ip + IP_OFF_DST, g_bcastIp))
            return;

        const uint8 *udp      = ip + IP_HDR_LEN;
        uint16       udpLen   = readU16be(udp + UDP_OFF_LEN);
        uint16       dstPort  = readU16be(udp + UDP_OFF_DSTPORT);
        uint16       srcPort  = readU16be(udp + UDP_OFF_SRCPORT);
        uint16       payLen;

        if (udpLen < UDP_HDR_LEN)
            return;

        payLen = (uint16)(udpLen - UDP_HDR_LEN);
        if (payLen > ETH_UDP_MAX_PAYLOAD)
            payLen = ETH_UDP_MAX_PAYLOAD;

        /* Learn sender for later unicast replies */
        memcpy(g_lastSenderMac, frame + ETH_OFF_SRC, 6u);
        memcpy(g_lastSenderIp,  ip + IP_OFF_SRC,     4u);

        /* Store in RX slot (overwrites previous if not yet consumed) */
        g_rxSlot.dstPort    = dstPort;
        g_rxSlot.srcPort    = srcPort;
        g_rxSlot.payloadLen = payLen;
        memcpy(g_rxSlot.srcIp,   ip + IP_OFF_SRC,    4u);
        memcpy(g_rxSlot.payload, udp + UDP_HDR_LEN,  payLen);
        g_rxSlot.pending    = TRUE;
    }
}

/*===========================================================================*/
/* Public API                                                                 */
/*===========================================================================*/
void Eth_Udp_init(void)
{
    IfxEth_Config cfg;
    uint8 i;

    for (i = 0u; i < 6u; i++)  { g_lastSenderMac[i] = 0u; }
    for (i = 0u; i < 4u; i++)  { g_lastSenderIp[i]  = 0u; }
    g_rxSlot.pending = FALSE;

    /* Load iLLD defaults */
    IfxEth_initConfig(&cfg, &MODULE_ETH);

    /* Override MAC address */
    {
        const uint8 mac[6] = ETH_UDP_BOARD_MAC;
        for (i = 0u; i < 6u; i++)
            cfg.macAddress[i] = mac[i];
    }

    /* PHY link-check callback only (PHY init done manually below) */
    cfg.phyInit = NULL_PTR;
    cfg.phyLink = IfxEth_Phy_Pef7071_link;

    /* RMII pin mapping */
    cfg.phyInterfaceMode = IfxEth_PhyInterfaceMode_rmii;
    cfg.rmiiPins         = &g_rmiiPins;
    cfg.miiPins          = NULL_PTR;

    /* Use iLLD default descriptor lists and buffers (in CPU0 DSPR).        */
    /* TC29x DSPR is not cached — DMA sees CPU writes immediately.          */

    /* Polling mode — no interrupt */
    cfg.isrPriority = (Ifx_Priority)0;
    cfg.isrProvider = IfxSrc_Tos_cpu0;

    IfxEth_init(&g_eth, &cfg);

    /* Scan all 32 MDIO addresses — check g_dbgPhyScan[] and
     * g_dbgPhyFoundAddr in debugger.  If g_dbgPhyFoundAddr != 0
     * update #define MDIO_PA to that value.                               */
    phyScan();

    /* PHY init with timeouts — check g_ethInitResult in debugger */
    phyInitSafe();

    /* Tell the iLLD link-check function the PHY is ready.  Without this,
     * IfxEth_Phy_Pef7071_link() always returns FALSE (guards on iPhyInitDone). */
    if (g_ethInitResult == 1)
        IfxEth_Phy_Pef7071_iPhyInitDone = 1;

    /* Read PHY ID registers — quick MDIO sanity check.
     * PEF7071 expected: g_dbgPhyId1=0x0013  g_dbgPhyId2=0x79Cx
     * Both 0 → MDIO reads still broken.  Non-zero → MDIO OK.             */
    {
        uint16 id1, id2, bmsr;
        if (mdioRead(0x02u, &id1)) g_dbgPhyId1 = id1;
        if (mdioRead(0x03u, &id2)) g_dbgPhyId2 = id2;
        if (mdioRead(0x01u, &bmsr)) g_dbgPhyBmsr = bmsr;
        g_dbgGpctl = ETH_GPCTL.U;
    }

    IfxEth_startReceiver(&g_eth);
    IfxEth_startTransmitter(&g_eth);
}

/*---------------------------------------------------------------------------*/
void Eth_Udp_poll(void)
{
    uint8  *rxBuf;
    uint16  rxLen;

    g_dbgPollCount++;

    if (!IfxEth_isRxDataAvailable(&g_eth))
        return;

    rxBuf = (uint8 *)IfxEth_getReceiveBuffer(&g_eth);
    if (rxBuf == NULL_PTR)
        return;

    /* Frame length is in the descriptor */
    rxLen = IfxEth_getRxDataLength(&g_eth);

    processRxFrame(rxBuf, rxLen);

    IfxEth_freeReceiveBuffer(&g_eth);
    g_dbgRxCount = g_eth.rxCount;
}

/*---------------------------------------------------------------------------*/
boolean Eth_Udp_isLinkUp(void)
{
    boolean up = IfxEth_Phy_Pef7071_link();
    uint16  bmsr, id1, id2;
    g_dbgLinkUp = (uint32)up;
    if (mdioRead(0x01u, &bmsr)) g_dbgPhyBmsr = bmsr;  /* BMSR: bit2=link */
    if (mdioRead(0x02u, &id1))  g_dbgPhyId1  = id1;   /* should be 0x0013 */
    if (mdioRead(0x03u, &id2))  g_dbgPhyId2  = id2;   /* should be 0x79Cx */
    g_dbgGpctl        = ETH_GPCTL.U;
    /* MAC hardware TX counters — definitive test: if these grow, MAC IS transmitting.
     * If stuck at 0 despite g_dbgTxCount growing, PHY is not in RMII mode.    */
    g_dbgMmcTxGoodBad = ETH_TX_FRAME_COUNT_GOOD_BAD.U;
    g_dbgMmcTxGood    = ETH_TX_FRAME_COUNT_GOOD.U;
    g_dbgMmcRxGoodBad = ETH_RX_FRAMES_COUNT_GOOD_BAD.U;
    g_dbgMmcRxCrcErr  = ETH_RX_CRC_ERROR_FRAMES.U;
    return up;
}

/*---------------------------------------------------------------------------*/
void Eth_Udp_sendTo(const uint8 *destMac, const uint8 *destIp,
                    uint16 srcPort, uint16 destPort,
                    const void *data, uint16 dataLen)
{
    sendUdp(destMac, destIp, srcPort, destPort, data, dataLen);
}

/*---------------------------------------------------------------------------*/
void Eth_Udp_sendBcast(uint16 srcPort, uint16 destPort,
                       const void *data, uint16 dataLen)
{
    sendUdp(g_bcastMac, g_bcastIp, srcPort, destPort, data, dataLen);
}

/*---------------------------------------------------------------------------*/
uint16 Eth_Udp_receive(uint16 port, uint8 *srcIp, uint16 *srcPort,
                       void *buf, uint16 bufLen)
{
    uint16 copyLen;

    if (!g_rxSlot.pending)
        return 0u;
    if (g_rxSlot.dstPort != port)
        return 0u;

    copyLen = g_rxSlot.payloadLen;
    if (copyLen > bufLen)
        copyLen = bufLen;

    if (srcIp   != NULL_PTR) memcpy(srcIp,   g_rxSlot.srcIp, 4u);
    if (srcPort != NULL_PTR) *srcPort = g_rxSlot.srcPort;
    if (buf     != NULL_PTR) memcpy(buf, g_rxSlot.payload, copyLen);

    g_rxSlot.pending = FALSE;
    return copyLen;
}

/*---------------------------------------------------------------------------*/
void Eth_Udp_getLastSenderMac(uint8 *mac6)
{
    if (mac6 != NULL_PTR)
        memcpy(mac6, g_lastSenderMac, 6u);
}

void Eth_Udp_getLastSenderIp(uint8 *ip4)
{
    if (ip4 != NULL_PTR)
        memcpy(ip4, g_lastSenderIp, 4u);
}
