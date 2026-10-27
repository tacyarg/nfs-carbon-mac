// net_lan.h - virtual TCP over the net mailbox (net_proto.h), for winsock.cpp.
// From nfsu2-web runtime/web/net_lan.c. Copyright (c) 2026 th0mk; portions (c)
// 2026 Talha Müderrisoğlu. MIT License.
// Addresses are host order (10.77.0.1 = 0x0A4D0001). Errors are negative WSA codes.
#pragma once
#include "net_proto.h"
#include <cstdint>

enum LanState { LAN_FREE = 0, LAN_SYN_SENT, LAN_EST, LAN_REFUSED, LAN_TIMEDOUT, LAN_RESET };
net_mailbox *lan_mailbox();
bool lan_online();
uint32_t lan_vip();
int lan_tcp_connect(uint32_t ip_h, uint16_t port);
int lan_tcp_state(int h);
uint16_t lan_tcp_lport(int h);
int lan_tcp_send(int h, const uint8_t *p, int n);
int lan_tcp_recv(int h, uint8_t *p, int n);
bool lan_tcp_ready(int h);
void lan_tcp_close(int h);
void lan_pump();

// UDP (NET_KIND_LAN_UDP), unicast only. Ports share the same ephemeral counter
// and address-in-use check as TCP's. Addresses and ports are host order.
int lan_udp_open();                                       // a handle, or <0 (WSAEMFILE) if none free
int lan_udp_bind(int h, uint16_t port_h);                 // 0 = ephemeral; 0 on success, else a negative WSA code
uint16_t lan_udp_lport(int h);
// Binds an unbound socket to an ephemeral port on first use. Returns bytes
// sent, or a negative WSA code (WSAEMSGSIZE if n is too large; nothing sent then).
int lan_udp_sendto(int h, uint32_t rip_h, uint16_t rport, const uint8_t *p, int n);
// One datagram, truncated to n bytes (then WSAEMSGSIZE, the rest discarded).
// Empty queue: WSAEWOULDBLOCK. *from_ip_h/*from_port are the sender's address.
int lan_udp_recvfrom(int h, uint8_t *p, int n, uint32_t *from_ip_h, uint16_t *from_port);
bool lan_udp_ready(int h);
void lan_udp_close(int h);
// A queued datagram completes it at once (handled by the caller via
// lan_udp_recvfrom first); otherwise this posts the receive (at most one per
// socket, replacing any earlier post) for lan_pump to complete when a
// datagram next arrives: it fills the WSABUF at buf_addr/buflen, writes
// from_addr/fromlen_addr (0 to skip, WSARecv has no from), sets
// OVERLAPPED.Internal = 0 and .InternalHigh = the byte count at ov_addr, and
// signals event_handle (0 for none) the way WSASetEvent does.
void lan_udp_post_recv(int h, uint32_t buf_addr, uint32_t buflen, uint32_t from_addr, uint32_t fromlen_addr,
                       uint32_t ov_addr, uint32_t event_handle);
