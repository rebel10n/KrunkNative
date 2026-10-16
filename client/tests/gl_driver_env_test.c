// gl_driver_env_test: the V-Sync relaunch decision (client/src/gl_driver_env.c, gl_driver_env.h) and the
// V-Sync text rules it shares with ui_render_settings.c. Every branch: NVIDIA only, V-Sync off only, an
// explicit __GL_SYNC_TO_VBLANK wins, never twice, never in a tool run; all 32 input combinations.
#include <gl_driver_env.h>

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// The rule written out once more, independently of the implementation.
static int expected(const int nvidia, const int vsync, const int env, const int relaunched, const int tool) {
    return nvidia == 1 && vsync == 0 && env == 0 && relaunched == 0 && tool == 0;
}

static void test_relaunch_branches(void) {
    // the one case that relaunches
    CHECK(gl_driver_relaunch_wanted(1, 0, 0, 0, 0) == 1, "NVIDIA, V-Sync off, nothing set: relaunch");
    // each condition that stops it, alone
    CHECK(gl_driver_relaunch_wanted(0, 0, 0, 0, 0) == 0, "not NVIDIA (the variable is for NVIDIA only)");
    CHECK(gl_driver_relaunch_wanted(1, 1, 0, 0, 0) == 0, "V-Sync on: no relaunch");
    CHECK(gl_driver_relaunch_wanted(1, 0, 1, 0, 0) == 0, "__GL_SYNC_TO_VBLANK set by the user: it wins");
    CHECK(gl_driver_relaunch_wanted(1, 0, 0, 1, 0) == 0, "the relaunched process never relaunches again");
    CHECK(gl_driver_relaunch_wanted(1, 0, 0, 0, 1) == 0, "tool run (headless, scenario, capture): no relaunch");
    // the stop conditions together, and every other combination
    int cases = 0;
    for (int nvidia = 0; nvidia <= 1; nvidia++) {
        for (int vsync = 0; vsync <= 1; vsync++) {
            for (int env = 0; env <= 1; env++) {
                for (int relaunched = 0; relaunched <= 1; relaunched++) {
                    for (int tool = 0; tool <= 1; tool++) {
                        const int got = gl_driver_relaunch_wanted(nvidia, vsync, env, relaunched, tool);
                        CHECK(got == expected(nvidia, vsync, env, relaunched, tool),
                              "nvidia %d vsync %d env %d relaunched %d tool %d: got %d", nvidia, vsync, env,
                              relaunched, tool, got);
                        cases++;
                    }
                }
            }
        }
    }
    CHECK(cases == 32, "all 32 combinations ran (%d)", cases);
}

static void test_parse_vsync(void) {
    CHECK(gl_driver_parse_vsync(NULL) == 0, "NULL is off");
    CHECK(gl_driver_parse_vsync("") == 0, "empty is off");
    CHECK(gl_driver_parse_vsync("false") == 0, "false is off");
    CHECK(gl_driver_parse_vsync("true") == 1, "true is on");
    CHECK(gl_driver_parse_vsync("0") == 0, "0 is off");
    CHECK(gl_driver_parse_vsync("0.0") == 0, "0.0 is off");
    CHECK(gl_driver_parse_vsync("1") == 1, "1 is on");
    CHECK(gl_driver_parse_vsync("3") == 1, "a nonzero number is on");
    CHECK(gl_driver_parse_vsync(" 0 ") == 0, "a number with spaces is still its value");
    CHECK(gl_driver_parse_vsync("off") == 1, "other text is on (ui_render_settings.c parse_bool)");
}

int main(void) {
    test_relaunch_branches();
    test_parse_vsync();
    if (failures) {
        fprintf(stderr, "gl_driver_env_test: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("gl_driver_env_test OK (%d checks)\n", checks);
    return 0;
}
