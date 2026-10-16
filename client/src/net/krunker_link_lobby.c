// Official krunker.io lobby UI packets (client/src/net/krunker_link.c's default case chains here): the
// private lobby's password prompt. The translations are client-local EXTs (net_ext.h 0x8060-0x8065)
// the client turns into v10's gameCode() popup (client/src/ui/v10_party.c).
//   code   ENTER_CODE         []         gameCode(): the "Enter Server Password to join" popup (:219217)
//   codeT  UPDATE_CODE_TIMER  [seconds]  #confirmBtn "Submit - Ns" (:219224)
//   codeP  CODE_PASSED        []         clearPops() (:219221)
// Our typed password comes back as NET_EXT_KN_OFFICIAL_GAME_CODE and goes out as ["code", value]
// (submitGameCode, :219238; krunker_link.c krunker_link_send_ours).
#include "krunker_link_internal.h"
#include <net_ext.h>
#include <stdint.h>
#include <string.h>

static void emit_lobby_ext(KrunkerLink *l, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buf[NET_MAX_PAYLOAD];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n) krunker_link_emit(l, NET_PACKET_EXT, buf, n);
}

int krunker_link_lobby_frame(KrunkerLink *l, const KrunkerS2C id, const KrunkerFrame *f) {
    switch (id) {
        case KRUNKER_S2C_ENTER_CODE: emit_lobby_ext(l, NET_EXT_KN_OFFICIAL_CODE_PROMPT, NULL, 0); return 1;
        case KRUNKER_S2C_CODE_PASSED: emit_lobby_ext(l, NET_EXT_KN_OFFICIAL_CODE_PASSED, NULL, 0); return 1;
        case KRUNKER_S2C_UPDATE_CODE_TIMER: {
            const int32_t seconds = (int32_t) krunker_num(krunker_frame_arg(f, 0), 0);
            emit_lobby_ext(l, NET_EXT_KN_OFFICIAL_CODE_TIMER, &seconds, sizeof(seconds));
            return 1;
        }
        default: return 0;
    }
}
