// The text and the place of a failed join (client/include/join_errors.h). Shared by servers_ui.c
// (the KrunkNative matchmaker's seek) and krunker_net.c (krunker.io's matchmaker and game server).
#include <join_errors.h>
#include <i18n.h>

#include <stdio.h>
#include <string.h>

// v10's matchmaker texts are HTML ("Title<br><small>detail</small>"): the page shows two lines. The
// native error screen and popup take plain text, so <br> becomes ". " and the tags go.
static void plain(const char *html, char *out, const size_t n) {
    size_t o = 0;
    for (const char *p = html; *p && o + 3 < n;) {
        if (!strncmp(p, "<br>", 4)) {
            out[o++] = '.';
            out[o++] = ' ';
            p += 4;
        } else if (*p == '<') {
            const char *close = strchr(p, '>');
            p = close ? close + 1 : p + strlen(p);
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

void join_error_text(const char *code, char *out, const size_t n) {
    if (!n) return;
    out[0] = 0;
    if (!code) { // nothing answered (DNS, refused, timeout, the proxy)
        snprintf(out, n, "%s", I18N("kn.matchmaker.unreachable", "Could not reach the matchmaker"));
        return;
    }
    // v10.0.2 (en.json matchmaker.* and error.connectedElsewhere): the same case, the same words
    if (!strcmp(code, "GameFull")) snprintf(out, n, "%s", I18N("matchmaker.GameFull", "Game is full."));
    else if (!strcmp(code, "InvalidGameId")) snprintf(out, n, "%s", I18N("matchmaker.InvalidGameId", "Game not found."));
    // menu_ui.c recognises the Game Updating screen by this text: it stays the English sentinel
    else if (!strcmp(code, "NoServersMatchQuery") || !strcmp(code, "updating")) snprintf(out, n, "%s", "GAME UPDATING");
    else if (!strcmp(code, "NoAvailableServers") || !strcmp(code, "no_lobbies"))
        snprintf(out, n, "%s", I18N("matchmaker.NoAvailableServers", "Servers are at Max Capacity"));
    else if (!strcmp(code, "NoMatchingServers"))
        plain(I18N("matchmaker.NoMatchingServers", "No Games Found<br><small>Try changing Quickplay options</small>"), out, n);
    // KrunkNative (not a matchmaker word): a rejoin gave up because the lobby's server did not come back
    // within its window and nothing else has room (servers_ui.c on_seek). "Servers are at Max Capacity"
    // would say the servers are full; the server is gone. v10's Lost Connection text for a server that
    // cannot be reached is player.disconnect, the same one its failed connect shows (v10.js _72689(null,
    // "000001", err): the error screen with ID 0x000001, the ID KN already shows for it).
    else if (!strcmp(code, "ServerUnreachable")) snprintf(out, n, "%s", I18N("player.disconnect", "DISCONNECTED"));
    // KrunkNative: the only lobby with room is the one a seek leaves (excludeGame): never sent back to
    // it; Find New Game / Quick Match keeps the player in that lobby (servers_ui.c on_seek)
    else if (!strcmp(code, "NoOtherLobby")) snprintf(out, n, "%s", I18N("kn.mm.noOtherLobby", "No other lobby has room: you stay in this one"));
    else if (!strcmp(code, "CaptchaRequired"))
        plain(I18N("matchmaker.CaptchaRequired", "Security check failed<br><small>Complete the security check and try again</small>"), out, n);
    else if (!strcmp(code, "RecaptchaFailed"))
        plain(I18N("matchmaker.RecaptchaFailed", "Security check failed<br><small>Refresh the page and try again</small>"), out, n);
    else if (!strcmp(code, "TooManyAccountSessions"))
        plain(I18N("matchmaker.TooManyAccountSessions", "Too many active sessions<br><small>Your account has too many games in progress. Close them and try again</small>"), out, n);
    else if (!strcmp(code, "GameIssue")) snprintf(out, n, "%s", I18N("matchmaker.GameIssue", "Unknown Join Issue"));
    // the account is playing on another device or tab: v10's text for that case (its own matchmaker
    // code shows the raw word; error.connectedElsewhere is the game server's "CONNECTED ELSEWHERE")
    else if (!strcmp(code, "AccountAlreadyPlaying"))
        plain(I18N("error.connectedElsewhere", "Connected Elsewhere<br><small>Your account started a game on another device or tab, so this session was closed</small>"), out, n);
    else if (!strcmp(code, "Failed")) snprintf(out, n, "%s", I18N("generic.failed", "Failed"));
    // KN's own: v10 has no text for AccountRequired (its matchmaker shows the raw word)
    else if (!strcmp(code, "AccountRequired")) snprintf(out, n, "%s", I18N("kn.matchmaker.accountRequired", "This game requires an account."));
    // the token refusals: the words v10's ERRORS table maps them to (v10.js:147684-147692)
    else if (!strcmp(code, "TooManyRequests")) snprintf(out, n, "%s", I18N("kn.matchmaker.tooManyRequests", "Too many requests, try again in a minute"));
    else if (!strcmp(code, "InvalidClientValidationToken")) snprintf(out, n, "%s", "Socket Error");
    else if (!strcmp(code, "InvalidClientIP")) snprintf(out, n, "%s", "Socket Mismatch");
    else if (!strcmp(code, "ExpiredClientValidationToken")) snprintf(out, n, "%s", "Validation Error");
    else if (!strcmp(code, "InvalidClientValidationTokenData")) snprintf(out, n, "%s", "Data Error");
    else { // any other word (InvalidKey, TooManyIPs, a word of a newer matchmaker): v10 has no text
        const char *args[] = {code};
        i18n_trf(out, n, "kn.matchmaker.refused", "The matchmaker refused this join ({0}).", 1, args);
    }
}

JoinErrorPlace join_error_place(const int in_game) {
    // v10's failed seek (v10.js:222076-222090: iîiìiïí) shows its error screen whether the player is in a
    // game or not: the game is left behind it. The popup is kept for the join's own notices only.
    (void) in_game;
    return JOIN_ERROR_SCREEN;
}
