// Window state text format, startup precedence and apply order (window_state.h).
// No GLFW here: client/tests/window_state_test.c links this file alone.
#include <window_state.h>

#include <stdio.h>
#include <string.h>

// RS_WINDOWED / RS_FULLSCREEN / RS_BORDERLESS (render_settings.h; ui_render_settings.c asserts it)
enum { MODE_WINDOWED = 0, MODE_FULLSCREEN = 1, MODE_BORDERLESS = 2 };

static int valid_mode(const int mode) {
    return mode >= MODE_WINDOWED && mode <= MODE_BORDERLESS;
}

static int valid_rect(const WindowState *s) {
    return s->w >= WINDOW_STATE_MIN_SIZE && s->h >= WINDOW_STATE_MIN_SIZE && s->w <= WINDOW_STATE_MAX_COORD &&
           s->h <= WINDOW_STATE_MAX_COORD && s->x >= -WINDOW_STATE_MAX_COORD && s->x <= WINDOW_STATE_MAX_COORD &&
           s->y >= -WINDOW_STATE_MAX_COORD && s->y <= WINDOW_STATE_MAX_COORD;
}

int window_state_encode(const WindowState *s, char *out, const size_t n) {
    if (!s || !out || !n) return 0;
    const int len = snprintf(out, n, "%d,%d,%d,%d,%d,%d,%d", s->mode, s->x, s->y, s->w, s->h, s->maximized ? 1 : 0,
                             s->forced ? 1 : 0);
    if (len < 0 || (size_t) len >= n) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

int window_state_decode(const char *text, WindowState *out) {
    if (!text || !out) return 0;
    WindowState s = {0};
    int end = 0;
    const int fields = sscanf(text, "%d,%d,%d,%d,%d,%d%n,%d%n", &s.mode, &s.x, &s.y, &s.w, &s.h, &s.maximized, &end,
                              &s.forced, &end);
    if (fields < 6 || text[end] != 0) return 0; // trailing garbage: not ours
    if (!valid_mode(s.mode) || !valid_rect(&s)) return 0;
    if ((s.maximized != 0 && s.maximized != 1) || (s.forced != 0 && s.forced != 1)) return 0;
    *out = s;
    return 1;
}

void window_state_resolve(const WindowStartupIn *in, WindowStartup *out) {
    memset(out, 0, sizeof(*out));
    out->state.w = WINDOW_STATE_DEFAULT_W;
    out->state.h = WINDOW_STATE_DEFAULT_H;
    if (!in) return;

    // F4 / relaunch: exactly the state the previous process had, forced flag included
    // (over the original --fullscreen still in argv, KRUNKNATIVE_WINDOW_SIZE and the store)
    if (in->restore) {
        out->state = *in->restore;
        out->has_pos = 1;
        out->restored = 1;
        return;
    }

    if (valid_mode(in->cli_mode)) {
        out->state.mode = in->cli_mode;
        out->state.forced = 1;
    } else if (valid_mode(in->stored_mode)) {
        out->state.mode = in->stored_mode;
    }

    if (in->env_w > 0 && in->env_h > 0) {
        // an explicit size (tests, the page windows app_window.c opens): no remembered geometry
        out->state.w = in->env_w;
        out->state.h = in->env_h;
    } else if (in->persisted) {
        out->state.x = in->persisted->x;
        out->state.y = in->persisted->y;
        out->state.w = in->persisted->w;
        out->state.h = in->persisted->h;
        out->state.maximized = in->persisted->maximized;
        out->has_pos = 1;
    }
}

int window_state_steps(const WindowStartup *s, WindowStep *out) {
    int n = 0;
    out[n++] = WINDOW_STEP_CREATE_HIDDEN;
    if (s->has_pos) out[n++] = WINDOW_STEP_POSITION;
    if (s->state.mode == MODE_FULLSCREEN) out[n++] = WINDOW_STEP_FULLSCREEN;
    else if (s->state.mode == MODE_BORDERLESS) out[n++] = WINDOW_STEP_BORDERLESS;
    else if (s->state.maximized) out[n++] = WINDOW_STEP_MAXIMIZE;
    out[n++] = WINDOW_STEP_SHOW;
    return n;
}
