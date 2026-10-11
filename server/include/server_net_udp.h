#pragma once
// UDP game transport of server_bin (server/src/net_udp.c, protocol: shared/include/knet.h,
// doc/transport.md). Runs next to the TCP listener on the same port number: UDP peers are
// ClientConnections whose fd is a virtual descriptor (>= NET_VIRTUAL_FD_BASE), so every
// net_send_packet(client->fd, ...) in server/src works unchanged. The game socket also
// answers server-info queries (0xFF 0xFF 0xFF 0xFF prefix, server/include/info_query.h).
//
// Threads: net thread = receive + handshake + flush; any thread holding g_server->lock may
// send. Lock order: g_server->lock before the transport's own lock.
#include <server.h>
#include <knet.h>

void net_add_client(ClientConnection *client); // server/src/net.c (caller holds g_server->lock)

// 1 unless KRUNKNATIVE_NET_UDP=0 (TCP only, the info-query responder keeps its own socket).
int net_udp_enabled(void);
// Net thread, at startup: bind UDP host:port. Returns the socket fd or -1 (logged).
int net_udp_open(const char *host, unsigned short port);
int net_udp_fd(void);
// Net thread, no g_server->lock: read every pending datagram (handshakes, info queries,
// DATA -> per-peer inboxes, PONG arrival stamps) and flush what is due.
void net_udp_receive(void);
// Net thread, holding g_server->lock: hand accepted peers to net_add_client() and drop
// the dead ones (timeouts / DISCONNECT / overflow) through net_remove_client().
void net_udp_service(void);
// Holding g_server->lock: next inbound game packet of a virtual fd. 1 = packet,
// 0 = none right now, -1 = the peer is gone (remove the client).
int net_udp_next_packet(int fd, NetPacket *packet);
// When the datagram carrying the last PONG of fd arrived (monotonic seconds, 0 unknown).
double net_udp_pong_received_at(int fd);
// Close a virtual fd: queued reliable data is still delivered (linger), then DISCONNECT.
void net_udp_close(int fd);
// Send everything queued for every peer now (end of tick, after handlers).
void net_udp_flush_all(void);
// Shutdown: linger briefly for the last reliable bytes, then close the socket.
void net_udp_shutdown(void);
