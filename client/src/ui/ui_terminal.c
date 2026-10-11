// Krunker v3.1.1 map terminals, client side (client/include/ui_terminal.h).
//
//   index.js openTerminal(hj): fS = hj; restart .terminalEffect (the typewriter) on
//     #terminalText; #terminalHolder display inline-block; terminalInput.focus().
//   index.js chat key: activeElement == terminalInput -> io.send("tmc", fS, value) when
//     not empty, clear, blur, hide #terminalHolder.
//   index.js interactKey / interactSecKey (player active): io.send("int", 1[, true]).
//   players.js updateInteract: #interactMsg for gates (type 0), deposit boxes (1), terminals
//     without noMsg (2) and onInteract triggers (4) in range (game_interact_prompt,
//     shared/src/interact.c); updateInteractProgress: #interactProg / #interProgB.
#include <ui_terminal.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define INTERACT_KEY_COLOR "#FBC02D"        // COLORS.interactPopup.key
#define INTERACT_ENOUGH_COLOR "#9eeb56"     // COLORS.interactPopup.enough
#define INTERACT_NOT_ENOUGH_COLOR "#eb5656" // COLORS.interactPopup.notEnough

static struct {
    int uid;             // fS
    int focused;         // document.activeElement == terminalInput
    int effect_pending;  // re-add .terminalEffect next frame (the reflow between remove and add)
    int focus_pending;   // focus #terminalInput next frame: the interact key's own character must not land in it
    int prompt_shown;    // #interactMsg shows our line
    char prompt[640];
} g_term = {.uid = -1};

static int in_game(const Client *client) {
    return client && client->me && client->me->active && client->game.ready && !html_ui_is_menu_open();
}

// UTILS.commaFormatNum: "1234567" -> "1,234,567" (a leading "-" stays in front)
static void comma_format(const int value, char *out, const size_t size) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%d", value);
    const char *d = digits[0] == '-' ? digits + 1 : digits;
    const size_t n = strlen(d);
    size_t o = 0;
    if (d != digits && o + 1 < size) out[o++] = '-';
    for (size_t i = 0; i < n && o + 2 < size; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

// players.js updateInteract: the #interactMsg line of changeInteract's winner this tick
// (game_interact_prompt: gates, deposit boxes, terminals, onInteract triggers).
static void build_prompt(const Client *client, char *html, const size_t size) {
    int index = -1;
    const int type = game_interact_prompt(&client->game, client->me, &index);
    const Object *object = index >= 0 ? client->game.map->objects[index] : NULL;
    char key[32], sec[32];
    ui_controls_key_name(ui_controls_bind(UI_CONTROL_INTERACT), key, sizeof(key));
    html[0] = 0;
    switch (type) {
        case GAME_INTERACT_GATE: {
            char price[160] = "";
            if (object->score_points) {
                const int enough = object->gate_method < 2 ? client->me->score >= (int) object->score_points
                                                           : (int) client->me->kills >= (int) object->score_points;
                snprintf(price, sizeof(price), " <span style='color:%s'>[%u]</span>%s", enough ? INTERACT_ENOUGH_COLOR : INTERACT_NOT_ENOUGH_COLOR,
                         object->score_points, object->gate_method == 2 ? " Kills" : "");
            }
            snprintf(html, size, "Press <span style=\"color:%s\">[%s]</span> to %s gate%s", INTERACT_KEY_COLOR, key,
                     !object->active && object->closeable ? "Close" : "Open", price);
            break;
        }
        case GAME_INTERACT_BANK: {
            char deposited[32], deposit[32], withdraw[32];
            comma_format(object->deposited, deposited, sizeof(deposited));
            if (object->deposit_amount) snprintf(deposit, sizeof(deposit), "%d", object->deposit_amount); else snprintf(deposit, sizeof(deposit), "All");
            if (object->withdraw_amount) snprintf(withdraw, sizeof(withdraw), "%d", object->withdraw_amount); else snprintf(withdraw, sizeof(withdraw), "All");
            ui_controls_key_name(ui_controls_bind(UI_CONTROL_INTERACT_SEC), sec, sizeof(sec));
            snprintf(html, size,
                     "<span style='color:%s'>[%s]</span><div style='color: #fff;margin-top:-30px;'>Press <span style='color:%s'>[%s]</span> to Deposit - %s</div>"
                     "<div style='color: #fff;margin-top:-30px;'>Press <span style='color:%s'>[%s]</span> to Withdraw - %s</div>",
                     INTERACT_NOT_ENOUGH_COLOR, deposited, INTERACT_KEY_COLOR, key, deposit, INTERACT_KEY_COLOR, sec, withdraw);
            break;
        }
        case GAME_INTERACT_TERMINAL:
            snprintf(html, size, "Press <span style=\"color:%s\">[%s]</span> to Open Terminal", INTERACT_KEY_COLOR, key);
            break;
        case GAME_INTERACT_TRIGGER: // getKeyName(10001)
            snprintf(html, size, "Press <span style=\"color:%s\">[M1]</span> to Interact", INTERACT_KEY_COLOR);
            break;
        default:
            break;
    }
}

static void update_prompt(Client *client) {
    const HtmlElem msg = html_dom_by_id("interactMsg");
    if (!msg) return;
    char html[sizeof(g_term.prompt)] = "";
    if (in_game(client)) build_prompt(client, html, sizeof(html));
    if (html[0]) {
        if (!g_term.prompt_shown || strcmp(html, g_term.prompt)) {
            html_dom_set_html(msg, html);
            snprintf(g_term.prompt, sizeof(g_term.prompt), "%s", html);
        }
        if (!g_term.prompt_shown) html_dom_set_display(msg, "block");
        g_term.prompt_shown = 1;
    } else if (g_term.prompt_shown) {
        html_dom_set_html(msg, "");
        html_dom_set_display(msg, "none");
        g_term.prompt_shown = 0;
    }
}

// updateInteractProgress ("p" = the player's interProg): #interactProg shows while > 0,
// #interProgB is the bar. The local copy computes the same interact_progress.
static void update_progress(Client *client) {
    static float shown = -1.0f;
    const float progress = in_game(client) ? roundf(client->me->interact_progress * 1000.0f) / 1000.0f : 0.0f;
    if (progress == shown) return;
    const HtmlElem holder = html_dom_by_id("interactProg");
    const HtmlElem bar = html_dom_by_id("interProgB");
    if (holder) html_dom_set_display(holder, progress > 0.0f ? "block" : "none");
    if (bar && progress > 0.0f) {
        char width[32];
        snprintf(width, sizeof(width), "%g%%", progress * 100.0f);
        html_dom_set_style(bar, "width", width);
    }
    shown = progress;
}

void ui_terminal_tick(Client *client) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    if (g_term.focus_pending) {
        const HtmlElem input = html_dom_by_id("terminalInput");
        if (input && g_term.focused) html_dom_focus(input);
        g_term.focus_pending = 0;
    }
    if (g_term.effect_pending) {
        const HtmlElem text = html_dom_by_id("terminalText");
        if (text) html_dom_set_class(text, "terminalEffect", 1);
        g_term.effect_pending = 0;
    }
    update_prompt(client);
    update_progress(client);
}

void ui_terminal_interact(Client *client, const int secondary) {
    if (!client || !client->me || !client->me->active) return;
    const NetInteractPacket packet = {.action = 1, .secondary = (uint8_t) !!secondary};
    client_net_send(client, NET_PACKET_INTERACT, &packet, sizeof(packet)); // io.send("int", 1[, true])
}

void ui_terminal_open(Client *client, const int uid) {
    (void) client;
    g_term.uid = uid;
    const HtmlElem text = html_dom_by_id("terminalText");
    if (text) html_dom_set_class(text, "terminalEffect", 0);
    g_term.effect_pending = 1;
    const HtmlElem holder = html_dom_by_id("terminalHolder");
    if (holder) html_dom_set_display(holder, "inline-block");
    const HtmlElem input = html_dom_by_id("terminalInput");
    if (input) {
        ui_controls_reset_keys();
        g_term.focused = 1;       // typing from now on; the element takes focus next frame
        g_term.focus_pending = 1; // (offline "ot" arrives inside the interact key's own keydown)
    }
}

int ui_terminal_focused(void) {
    return g_term.focused;
}

static void leave_input(void) {
    const HtmlElem input = html_dom_by_id("terminalInput");
    if (input) html_dom_blur(input);
    g_term.focused = 0;
}

// The chat key with terminalInput focused.
static void submit(Client *client) {
    const HtmlElem input = html_dom_by_id("terminalInput");
    if (!input) return;
    char value[NET_CHAT_TEXT_SIZE] = "";
    html_dom_get_value(input, value, sizeof(value));
    if (value[0] && client) {
        NetTerminalMsgPacket packet;
        memset(&packet, 0, sizeof(packet));
        packet.uid = g_term.uid;
        snprintf(packet.text, sizeof(packet.text), "%s", value);
        client_net_send(client, NET_PACKET_TERMINAL_MSG, &packet, (uint16_t) (sizeof(int32_t) + strlen(packet.text) + 1));
    }
    html_dom_set_value(input, "");
    leave_input();
    const HtmlElem holder = html_dom_by_id("terminalHolder");
    if (holder) html_dom_set_display(holder, "none");
}

int ui_terminal_key_event(Client *client, const int key, const int scancode, const int action, const int mods, const int chat_key) {
    if (!g_term.focused) return 0;
    if (action == GLFW_PRESS && chat_key) {
        submit(client ? client : menu_ui()->client);
        return 1;
    }
    if (html_ui_is_menu_open()) return 0; // the page edits the focused field itself
    if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) {
        leave_input();
        return 0; // main.c opens the menu (the pointer lock is lost)
    }
    html_ui_key(key, scancode, action, mods); // typing goes to #terminalInput, not the player
    return 1;
}

int ui_terminal_page_key(const HtmlEvent *ev) {
    if (!ev || !ev->target || ev->target != html_dom_by_id("terminalInput")) return 0;
    if (html_key_is_enter(ev->key)) {
        submit(menu_ui()->client);
        return 1;
    }
    if (html_key_is_escape(ev->key)) {
        leave_input();
        return 1;
    }
    return 0;
}
