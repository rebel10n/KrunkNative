// Page sound handlers (ui/index.html inline on* attributes), registered in ui_handlers.c:
//   playTick()              window.playTick = () => SOUND.play("tick_0", 0.2)   (index.js)
//   SOUND.play(name, vol)   e.g. onmouseover="SOUND.play('tick_0',0.1)"
#include <sound_events.h>
#include <audio.h>

int ui_sound_play_tick(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    audio_play("tick_0", 0.2f, 0, 1.0f);
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
