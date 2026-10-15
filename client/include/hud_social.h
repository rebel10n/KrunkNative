#pragma once
// v7.1.4 in-match social features (player-viewer WP-E part 2b, client/src/ui/hud_social.c): the
// text & voice channel packets, per-player mute (mute_chat / mute_voice), quick chat, prop-hunt
// taunts and the spray / spray-wheel keys. Wire: shared/include/social_v7.h.
#include <client.h>
#include <social_v7.h>

// The server takes the v7 social packets: the offline sandbox, or a v7 server (INIT minor >=
// NET_PROTOCOL_MINOR_V7) that sent NET_PACKET_SOCIAL_V7 after its INIT.
int social_server_v7(const Client*);
void social_reset_server(void);         // INIT: until the server says so again
void social_on_server_caps(int version); // NET_PACKET_SOCIAL_V7

// mutedChatAccountIds / mutedVoiceAccountIds (localStorage "mute_chat" / "mute_voice", JSON arrays
// of account ids). v7 saves the voice list under "mute_chat" too (a bug KrunkNative does not copy).
int social_chat_muted(int account_id);
int social_voice_muted(int account_id);
int social_toggle_mute(int account_id, int voice); // muteUser: returns 1 when the account is muted now
void social_mute_reload(void);                     // forget the cached lists (tests, storage changes)

// NET_PACKET_CHAT_V7 (S->C): io_addChat(channel, name, msg, 0, null, color, accountId).
void social_on_chat_v7(Client*, const NetChatV7Packet*);
// f2028(i): a quick chat message, at most one per QUICK_CHAT_COOLDOWN. 1 = sent.
int social_quick_chat(Client*, const char *text);
// "prpt", i: on the prop team of a prop-hunt mode only. 1 = sent.
int social_taunt(Client*, int index);
// markPositionKey: the official game's ping ("mpos" {x, y, z} of the camera ray's hit). 1 = sent.
int social_mark_position(Client*);

// pressButton / releaseButton for the spray keys: with the wheel (premium accounts and the
// sandbox, v7 servers) the sprayWheel key opens the wheel and its release sprays the chosen item;
// otherwise the spray key sends "spry" (v7) or the v3 "int" 0.
int social_spray_wheel_allowed(const Client*);
void social_spray_press(Client*, int spray_key, int wheel_key);
void social_spray_release(Client*, int wheel_key);
// The mouse wheel while the spray wheel shows: sprayWheelChange (1 = consumed).
int social_spray_scroll(double dy);
// Prop team: #propControlHolder instead of #botRightHider, with the taunt keys. Per HUD frame;
// `layout_epoch` changes when hud_ui.c re-applied its visibility defaults.
void social_prop_hud_tick(Client*, int visible, int layout_epoch);
