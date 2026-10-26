// net_lan.h - virtual TCP over the net mailbox (net_proto.h), for winsock.cpp.
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
