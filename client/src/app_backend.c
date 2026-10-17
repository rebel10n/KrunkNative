// The backend session across the windows of one client (client/include/app_backend.h). Pure state: the
// router and the child windows feed it, account_ui.c and the brain gate read it.
#include <app_backend.h>
#include <string.h>

static int g_router = -1;     // a child: the router's session (1 official, 0 not, -1 no router)
static int g_children;        // a router: its child windows connected to an official krunker.io server

void app_backend_set_router(const int official) { g_router = official < 0 ? -1 : official ? 1 : 0; }

void app_backend_set_children_official(const int count) { g_children = count > 0 ? count : 0; }

int app_backend_official_session(void) { return g_router == 1 || g_children > 0; }

int app_backend_parse_line(const char *line) {
    if (!line || strncmp(line, "backend ", 8) != 0) return -1;
    if (!strcmp(line + 8, "krunker")) return 1;
    if (!strcmp(line + 8, "krunkio")) return 0;
    return -1;
}

const char *app_backend_line(const int official) { return official ? "backend krunker" : "backend krunkio"; }

const char *app_backend_env_value(const int official) { return official ? "krunker" : "krunkio"; }

int app_backend_parse_env(const char *value) {
    if (!value) return -1;
    if (!strcmp(value, "krunker")) return 1;
    if (!strcmp(value, "krunkio")) return 0;
    return -1;
}
