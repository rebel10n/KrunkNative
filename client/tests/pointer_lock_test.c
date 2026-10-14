// pointer_lock_test: the pointer-lock owner's state machine (client/src/pointer_lock.c) through
// the transitions players go through, against v7.1.4 controls.js: CONTROLS.enabled follows the
// pointer lock, the mouse only turns the camera under it, the Esc menu / console / focus loss /
// end-screen results free it, a round that ends with the menu up keeps it free (and inert)
// through the title until a click, and the next round, a respawn or closing the menu lock it again.
//   pointer_lock_test
#include <pointer_lock.h>

#include <stdio.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// Playing: the game page, focused, nothing over the view.
static PointerLockInputs playing(void) {
    PointerLockInputs in = {0};
    in.game = 1;
    in.focused = 1;
    return in;
}

static PointerLockInputs in_menu(void) {
    PointerLockInputs in = playing();
    in.menu_open = 1;
    return in;
}

#define EXPECT(want, c, l, what) do { CHECK((want).controls == (c) && (want).lock == (l), \
    "%s: controls=%d lock=%d (want %d %d)", what, (want).controls, (want).lock, c, l); } while (0)

static void test_menu_round_trip(void) {
    PointerLockTracker t = {0};
    PointerLockInputs in = in_menu();
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "start in the menu");
    in = playing();
    PointerLockWant w = pointer_lock_step(&t, &in);
    EXPECT(w, 1, 1, "Click to Play");
    CHECK(pointer_lock_look_allowed(&w, 1), "locked: the mouse turns the camera");
    CHECK(!pointer_lock_look_allowed(&w, 0), "the lock not held (yet): no look");
    in = in_menu();
    w = pointer_lock_step(&t, &in);
    EXPECT(w, 0, 0, "Esc opens the menu");
    CHECK(!pointer_lock_look_allowed(&w, 1), "menu: no look even with a stale lock");
    for (int i = 0; i < 3; i++) EXPECT(pointer_lock_step(&t, &in), 0, 0, "menu held over frames");
    in = playing();
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "Esc closes the menu: locked again at once");
    in.console_open = 1;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "the dev console frees the pointer");
    in.console_open = 0;
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "console closed");
    in.game = 0;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "hub / editor page: not the game's lock");
}

static void test_end_locked(void) {
    // The END while playing: the title keeps the controls (v7: CONTROLS.disable() runs once
    // endAnim ran out), the results free the pointer, the next round locks it again.
    PointerLockTracker t = {0};
    PointerLockInputs in = playing();
    pointer_lock_step(&t, &in);
    in.end_up = 1;
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "END while playing: the title keeps the lock");
    in.end_results = 1;
    PointerLockWant w = pointer_lock_step(&t, &in);
    EXPECT(w, 0, 0, "results: CONTROLS.disable()");
    CHECK(!pointer_lock_look_allowed(&w, 0), "results: the free mouse turns nothing");
    in.menu_open = 1;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "Esc over the results: the menu");
    in.menu_open = 0;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "Click to Play over the results: they come back, still free");
    in.end_up = in.end_results = 0;
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "next round: locked again");
}

static void test_end_with_menu_open(void) {
    // The user's report: Esc menu up at the END. hideUI closes the menu, the lock stays lost:
    // the title shows a free cursor that must not turn the camera.
    PointerLockTracker t = {0};
    PointerLockInputs in = playing();
    pointer_lock_step(&t, &in);
    in = in_menu();
    pointer_lock_step(&t, &in);
    in = playing();
    in.end_up = 1; // the menu went with the END
    PointerLockWant w = pointer_lock_step(&t, &in);
    EXPECT(w, 0, 0, "END with the menu open: the title runs with the pointer free");
    CHECK(!pointer_lock_look_allowed(&w, 0), "free title: no look");
    CHECK(!pointer_lock_look_allowed(&w, 1), "free title: no look even if a lock were held");
    for (int i = 0; i < 5; i++) EXPECT(pointer_lock_step(&t, &in), 0, 0, "free title over frames");
    in.end_results = 1;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "then the results");
    in.end_up = in.end_results = 0;
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "next round: locked again");
    in.end_up = 1;
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "the following END while playing: a locked title again");
}

static void test_end_click_relocks(void) {
    // mousedown on the canvas during the free title: toggle(true) -> requestPointerLock.
    PointerLockTracker t = {0};
    PointerLockInputs in = in_menu();
    pointer_lock_step(&t, &in);
    in = playing();
    in.end_up = 1;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "free title");
    pointer_lock_resume(&t);
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "a click on the game view locks again");
    in.end_results = 1;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "results free it anyway");
}

static void test_end_focus_loss(void) {
    // alt-tab during a locked title: no menu (UI.active false), free until a click.
    PointerLockTracker t = {0};
    PointerLockInputs in = playing();
    pointer_lock_step(&t, &in);
    in.end_up = 1;
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "locked title");
    in.focused = 0;
    pointer_lock_end_free(&t);
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "unfocused");
    in.focused = 1;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "focus back: still free until a click");
    pointer_lock_resume(&t);
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "the click");

    PointerLockTracker u = {0};
    in = playing();
    pointer_lock_end_free(&u);
    EXPECT(pointer_lock_step(&u, &in), 1, 1, "end_free without an end screen does nothing");
    in.focused = 0;
    EXPECT(pointer_lock_step(&u, &in), 0, 0, "unfocused in game: free");
}

static void test_respawn(void) {
    // Death: the menu comes after the death delay (Click to Play), the respawn closes it.
    PointerLockTracker t = {0};
    PointerLockInputs in = playing();
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "alive");
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "dead, before the menu: the lock stays");
    in = in_menu();
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "Click to Play menu");
    in = playing();
    EXPECT(pointer_lock_step(&t, &in), 1, 1, "respawned: locked");
}

static void test_script(void) {
    PointerLockTracker t = {0};
    PointerLockInputs in = playing();
    in.script_unlock = 1;
    PointerLockWant w = pointer_lock_step(&t, &in);
    EXPECT(w, 0, 0, "INPUTS.unlockMouse: controls off, free");
    in.script_unlock = 0;
    in.script_free = 1;
    w = pointer_lock_step(&t, &in);
    EXPECT(w, 1, 0, "INPUTS.freeMouse: controls on, no lock");
    CHECK(!pointer_lock_look_allowed(&w, 0), "freeMouse: the camera does not follow the free mouse");
    in = in_menu();
    in.script_free = 1;
    EXPECT(pointer_lock_step(&t, &in), 0, 0, "freeMouse + menu");
}

int main(void) {
    test_menu_round_trip();
    test_end_locked();
    test_end_with_menu_open();
    test_end_click_relocks();
    test_end_focus_loss();
    test_respawn();
    test_script();
    printf("pointer_lock_test: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
