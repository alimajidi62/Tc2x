/*******************************************************************************
 * \file  Eth_Udp.h
 * \brief Minimal bare-metal UDP/IP stack for AURIX TC297 TriBoard V1.0.
 *
 * Supports:
 *   - ARP reply  (so the PC can resolve the board's MAC)
 *   - UDP send   (unicast or broadcast)
 *   - UDP receive (polling, single-slot per poll cycle)
 *
 * No OS, no RTOS, no lwIP required.
 *
 * Usage:
 *   1. Call Eth_Udp_init() once at startup (after CPU sync).
 *   2. Call Eth_Udp_poll() every ~1 ms from the main loop.
 *   3. Use Eth_Udp_sendTo() / Eth_Udp_sendBcast() to transmit data.
 *   4. Use Eth_Udp_receive() to read data sent to the board from a PC.
 *
 * Network setup (change the defines below to match your LAN):
 *   Board IP  : 192.168.1.200
 *   Board MAC : 00:11:22:33:44:55  (any locally-administered value is fine)
 *   PC IP     : any — the board learns it from the first received UDP packet
 *   PC tool   : netcat / Python socket / Wireshark
 *
 *   PC send to board:
 *     > ncat -u 192.168.1.200 4000
 *   PC listen for board:
 *     > ncat -u -l 4000
 ******************************************************************************/
#ifndef ETH_UDP_H
#define ETH_UDP_H  1

#include "Ifx_Types.h"

/*---------------------------------------------------------------------------*/
/* Network identity — change to match your LAN                               */
/*---------------------------------------------------------------------------*/
#define ETH_UDP_BOARD_MAC    { 0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u }
#define ETH_UDP_BOARD_IP     { 192u,  168u,  1u,    200u }

/** Maximum UDP payload bytes that Eth_Udp_receive() will return. */
#define ETH_UDP_MAX_PAYLOAD  1472u

/*---------------------------------------------------------------------------*/
/* Public API                                                                 */
/*---------------------------------------------------------------------------*/

/**
 * Initialise the ETH MAC (RMII mode), PHY (PEF7071), and start RX/TX.
 * Must be called once before any other function.
 */
void    Eth_Udp_init(void);

/**
 * Process one pending receive frame (ARP / UDP).
 * Call this every ~1 ms from the main loop; it is non-blocking.
 */
void    Eth_Udp_poll(void);

/** Returns TRUE if the PHY has established a link. */
boolean Eth_Udp_isLinkUp(void);

/**
 * Send a UDP datagram to a specific host.
 *
 * destMac[6] : Ethernet destination MAC (use Eth_Udp_getLastSenderMac() after
 *              receiving a packet, or hardcode the PC MAC).
 * destIp[4]  : IPv4 destination address.
 * srcPort    : UDP source port (board's port).
 * destPort   : UDP destination port (PC's port).
 * data       : payload pointer.
 * dataLen    : payload length in bytes (max 1472).
 */
void    Eth_Udp_sendTo(const uint8 *destMac, const uint8 *destIp,
                       uint16 srcPort, uint16 destPort,
                       const void *data, uint16 dataLen);

/**
 * Send a UDP datagram to the LAN broadcast address (255.255.255.255).
 * The PC only needs to listen on 'destPort'; no MAC knowledge required.
 * Most convenient for one-way logging to a PC.
 */
void    Eth_Udp_sendBcast(uint16 srcPort, uint16 destPort,
                          const void *data, uint16 dataLen);

/**
 * Check if a UDP datagram has arrived on 'port'.
 *
 * Returns: payload length in bytes, 0 if nothing arrived.
 * srcIp[4]  : filled with sender's IPv4 address.
 * srcPort   : filled with sender's UDP port.
 * buf       : destination for payload bytes.
 * bufLen    : size of buf (payload is truncated if larger).
 *
 * One call to Eth_Udp_poll() may make one packet available; call receive()
 * immediately after poll() if you do not want to miss packets.
 */
uint16  Eth_Udp_receive(uint16 port, uint8 *srcIp, uint16 *srcPort,
                        void *buf, uint16 bufLen);

/**
 * After Eth_Udp_receive() returns > 0, these functions return the sender's
 * MAC and IP addresses — useful for replying without a manual ARP lookup.
 */
void    Eth_Udp_getLastSenderMac(uint8 *mac6);
void    Eth_Udp_getLastSenderIp (uint8 *ip4);

#endif /* ETH_UDP_H */
