// v10's forceVideoAd in a game (client/include/kn_ad.h kn_ad_force_video, deob-10.0.5.js:227964-227981): the game
// server's FORCE_VIDEO_AD (the trigger action "Play Video Ad") or a client script's GAME.ADS.playVideo() (:187154) plays
// the placeholder ad, and its CUSTOM_VIDEO_AD states go back to the game server: an official game's "vad" [state] on
// the game socket, a Box Office / offline game's NET_EXT_KN_VIDEO_AD. The server fires onAdFinished(playerID, success).
#include <video_ad.h>
#include <kn_ad.h>
#include <krunker_net.h>
#include <net_ext.h>
#include <ui_logic.h>
#include <cJSON.h>
#include <string.h>

static void send_official(const int state) {
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("vad"));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(state));
    krunker_net_send_account_frame(frame);
    cJSON_Delete(frame);
}

static void send_native(const int state) {
    Client *client = menu_ui()->client;
    if (!client) return;
    NetVideoAdPacket p;
    memset(&p, 0, sizeof p);
    p.state = state;
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof p];
    const uint16_t n = net_ext_pack(buf, sizeof buf, NET_EXT_KN_VIDEO_AD, &p, sizeof p);
    if (n) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n);
}

int client_video_ad_force(void) { return kn_ad_force_video(krunker_net_active() ? send_official : send_native); }
