// Page sound handlers (ui/index.html inline on* attributes), registered in ui_handlers.c:
//   playTick(vol)           v7.1.4: window.playTick = v => SOUND.play("hover_0", v || 0.2) (index.js)
//   playSelect(vol)         v7.1.4: window.playSelect = v => SOUND.play("select_0", v || 0.15)
//   SOUND.play(name, vol)   e.g. onmouseover="SOUND.play('tick_0',0.1)"
#include <sound_events.h>
#include <audio.h>

static float arg_volume(const HtmlArg *args, const int argc, const float fallback) {
    return argc > 0 && args[0].kind == HTML_ARG_NUM && args[0].num != 0.0 ? (float) args[0].num : fallback;
}

int ui_sound_play_tick(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev;
    audio_play("hover_0", arg_volume(args, argc, 0.2f), 0, 1.0f);
    return 1;
}

int ui_sound_play_select(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev;
    audio_play("select_0", arg_volume(args, argc, 0.15f), 0, 1.0f);
    return 1;
}

int ui_sound_play(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev;
    if (argc < 1 || args[0].kind != HTML_ARG_STR || !args[0].str) return 1;
    const float volume = argc > 1 && args[1].kind == HTML_ARG_NUM ? (float) args[1].num : 1.0f;
    const int loop = argc > 2 && (args[2].kind == HTML_ARG_BOOL || args[2].kind == HTML_ARG_NUM) && args[2].num != 0.0;
    const float rate = argc > 3 && args[3].kind == HTML_ARG_NUM ? (float) args[3].num : 1.0f;
    audio_play(args[0].str, volume, loop, rate);
    return 1;
}
