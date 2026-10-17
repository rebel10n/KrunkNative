// The backend session across the windows of one client (client/include/app_backend.h): a router's
// session from its child windows and the router's own connection, and the wire / environment forms the
// router and its children exchange (app_window.c). Pure state, no window or network.
#include <app_backend.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const int ok, const char *name) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

int main(void) {
    check(app_backend_official_session() == 0, "a window of its own (no router, no children): not official");

    app_backend_set_router(1);
    check(app_backend_official_session() == 1, "a child whose router is on an official krunker.io server: official");
    app_backend_set_router(0);
    check(app_backend_official_session() == 0, "a child whose router is on KrunkNative: not official");
    app_backend_set_router(-1);
    check(app_backend_official_session() == 0, "no router state: not official");

    app_backend_set_children_official(2);
    check(app_backend_official_session() == 1, "a router with an official child window: official");
    app_backend_set_children_official(0);
    check(app_backend_official_session() == 0, "the child is gone: not official again");
    app_backend_set_children_official(-3);
    check(app_backend_official_session() == 0, "a negative count is no child");
    app_backend_set_router(1);
    app_backend_set_children_official(0);
    check(app_backend_official_session() == 1, "a child's router state holds with no children of its own");
    app_backend_set_router(-1);

    check(app_backend_parse_line("backend krunker") == 1, "wire: \"backend krunker\" is official");
    check(app_backend_parse_line("backend krunkio") == 0, "wire: \"backend krunkio\" is KrunkNative");
    check(app_backend_parse_line("backend krunkerx") == -1, "wire: an unknown backend is no session line");
    check(app_backend_parse_line("open hub") == -1, "wire: other lines are not session lines");
    check(app_backend_parse_line(NULL) == -1, "wire: NULL is no session line");
    check(strcmp(app_backend_line(1), "backend krunker") == 0 && strcmp(app_backend_line(0), "backend krunkio") == 0,
          "wire: the lines written");
    check(app_backend_parse_line(app_backend_line(1)) == 1 && app_backend_parse_line(app_backend_line(0)) == 0,
          "wire: the lines read back");

    check(app_backend_parse_env(app_backend_env_value(1)) == 1, "env: krunker at spawn is official");
    check(app_backend_parse_env(app_backend_env_value(0)) == 0, "env: krunkio at spawn is KrunkNative");
    check(app_backend_parse_env(NULL) == -1 && app_backend_parse_env("") == -1, "env: unset means no router state");

    printf("app_backend_test: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
