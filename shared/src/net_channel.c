// Which knet channel each game packet rides (shared/include/knet.h).
#include <knet.h>
#include <shared.h>

int net_packet_channel(const uint8_t type) {
    switch (type) {
        case NET_PACKET_STATE:       // the per-tick player sync: a newer snapshot replaces a lost one
            return KNET_SEND_UNRELIABLE;
        case NET_PACKET_INPUT:       // inputs: the server dedupes by seq (player_accept_input),
        case NET_PACKET_INPUT_BATCH: // so the redundant repeats cost nothing
        case NET_PACKET_PING:        // a lost probe or pong would cost a whole ping interval
        case NET_PACKET_PONG:
            return KNET_SEND_REDUNDANT;
        default:                     // spawns, kills, chat, recon, map stream, join ...
            return KNET_SEND_RELIABLE;
    }
}
