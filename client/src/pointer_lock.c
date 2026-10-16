// "Should the pointer be locked", derived from the UI state (pointer_lock.h). No GLFW here.
#include <pointer_lock.h>

PointerLockWant pointer_lock_step(PointerLockTracker *t, const PointerLockInputs *in) {
    // showEndScreen with the menu up (or the pointer otherwise free): toggleMenu(false) hides the
    // menu but nothing requests the lock again, so the title runs with a free, inert cursor.
    if (in->end_up && !t->end_up) t->end_freed = !t->was_controls;
    if (!in->end_up) t->end_freed = 0;
    t->end_up = in->end_up;

    // in play: the game view with nothing over it (the focus left out; the window's focus is what
    // turns the controls on and takes the pointer)
    const int play = in->game && !in->menu_open && !in->console_open && !in->end_results && !in->modal &&
                     !(in->end_up && t->end_freed) && !in->script_unlock;
    PointerLockWant want;
    want.play = play;
    want.controls = play && in->focused;
    want.lock = want.controls && !in->script_free;
    t->was_controls = want.controls;
    return want;
}

void pointer_lock_end_free(PointerLockTracker *t) {
    if (t->end_up) t->end_freed = 1;
}

void pointer_lock_resume(PointerLockTracker *t) {
    t->end_freed = 0;
}

int pointer_lock_look_allowed(const PointerLockWant *want, const int held) {
    return want->controls && want->lock && held;
}
