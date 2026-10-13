/*
 * services/brain/roles.c - brain.roles (roles.h): which modules of main.c's table run.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "roles.h"
#include "knsvc/config.h"
#include "knsvc/log.h"

static unsigned g_roles = BRAIN_ROLE_ALL;

static unsigned role_bit(const char *name) {
    if (!strcasecmp(name, "auth")) return BRAIN_ROLE_AUTH;
    if (!strcasecmp(name, "community") || !strcasecmp(name, "coordinator") || !strcasecmp(name, "brain"))
        return BRAIN_ROLE_COMMUNITY;
    if (!strcasecmp(name, "finder") || !strcasecmp(name, "matchmaker")) return BRAIN_ROLE_FINDER;
    if (!strcasecmp(name, "all")) return BRAIN_ROLE_ALL;
    return 0;
}

unsigned brain_roles_load(const struct kn_config *cfg) {
    if (!kn_config_has(cfg, "brain.roles")) {
        g_roles = BRAIN_ROLE_ALL;
        return g_roles;
    }
    const char *names[16];
    size_t n = kn_config_str_list(cfg, "brain.roles", names, 16);
    if (n > 16) n = 16;
    unsigned mask = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned bit = role_bit(names[i]);
        if (!bit) KN_WARN("brain", "brain.roles: unknown role \"%s\" ignored (auth, community, finder)", names[i]);
        mask |= bit;
    }
    g_roles = mask;
    return mask;
}

unsigned brain_roles(void) { return g_roles; }

int brain_has_role(unsigned role) { return (g_roles & role) != 0; }

const char *brain_roles_text(void) {
    static char buf[64];
    buf[0] = 0;
    if (g_roles & BRAIN_ROLE_AUTH) strcat(buf, "auth,");
    if (g_roles & BRAIN_ROLE_COMMUNITY) strcat(buf, "community,");
    if (g_roles & BRAIN_ROLE_FINDER) strcat(buf, "finder,");
    size_t len = strlen(buf);
    if (len) buf[len - 1] = 0;
    return buf;
}

/* Module -> roles that need it. Unknown modules (added by other owners later) belong to
 * the community role, which is everything kn_brain did before the roles existed. */
int brain_module_enabled(const char *name) {
    static const struct {
        const char *name;
        unsigned roles;
    } k_map[] = {
        {"registry", BRAIN_ROLE_ALL},  /* in-memory; public-routes / internal-ws read it */
        {"hot", BRAIN_ROLE_ALL},       /* optional Redis hot tier (hot.h): every role */
        {"accounts", BRAIN_ROLE_AUTH | BRAIN_ROLE_COMMUNITY},
        {"auth", BRAIN_ROLE_AUTH},
        {"finder", BRAIN_ROLE_FINDER},
        {"releases", BRAIN_ROLE_ALL},  /* latest server release: finder list + matchmaking */
        {"store", BRAIN_ROLE_AUTH | BRAIN_ROLE_COMMUNITY},
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
        if (!strcmp(k_map[i].name, name)) return (g_roles & k_map[i].roles) != 0;
    }
    return (g_roles & BRAIN_ROLE_COMMUNITY) != 0;
}
