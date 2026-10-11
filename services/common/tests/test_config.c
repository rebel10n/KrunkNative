/* kn_config: file + env + CLI precedence, typed getters, redaction. */
#include "kn_test.h"
#include "knsvc/config.h"

#include <unistd.h>

static const char *DOC =
    "{\"database\":{\"host\":\"db\",\"port\":5432,\"password\":\"hunter2\"},"
    "\"brain\":{\"port\":8080,\"public_url\":\"http://x\",\"cors_origins\":[\"a\",\"b\"],\"debug\":false},"
    "\"gameserver\":{\"lobby\":{\"max_players\":8},\"billboard\":\"hi\"},\"ratio\":0.25}";

int main(void) {
    char path[] = "/tmp/kn-svccore-cfg-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, DOC, strlen(DOC)) == (ssize_t)strlen(DOC));
    close(fd);

    setenv("KN_BRAIN_PUBLIC_URL", "http://env", 1);
    setenv("KN_GAMESERVER_LOBBY_MAX_PLAYERS", "12", 1);
    setenv("KN_BRAIN_DEBUG", "yes", 1);
    setenv("KN_NEW_KEY", "env-only", 1);
    char *argv[] = {"prog", "--config", path, "--set", "brain.port=9001", "--gameserver.billboard=cli text",
                    "positional", "--flag", NULL};
    int argc = 8;
    char err[256] = "";
    kn_config *cfg = kn_config_load(NULL, argc, argv, err, sizeof err);
    CHECK(cfg != NULL);
    if (!cfg) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    CHECK_EQ_STR(kn_config_str(cfg, "database.host", "x"), "db");
    CHECK_EQ_INT(kn_config_int(cfg, "database.port", 0), 5432);
    CHECK_EQ_STR(kn_config_str(cfg, "database.port", NULL), "5432");
    CHECK_EQ_INT(kn_config_int(cfg, "brain.port", 0), 9001);          /* CLI beats file */
    CHECK_EQ_STR(kn_config_str(cfg, "brain.public_url", ""), "http://env"); /* env beats file */
    CHECK_EQ_INT(kn_config_int(cfg, "gameserver.lobby.max_players", 0), 12);
    CHECK_EQ_STR(kn_config_str(cfg, "gameserver.billboard", ""), "cli text");
    CHECK_EQ_INT(kn_config_bool(cfg, "brain.debug", 0), 1);
    CHECK_EQ_STR(kn_config_str(cfg, "new.key", "d"), "env-only");       /* env path not in file */
    CHECK(kn_config_num(cfg, "ratio", 0) == 0.25);
    CHECK_EQ_INT(kn_config_int(cfg, "missing.key", -1), -1);
    CHECK(!kn_config_has(cfg, "missing.key"));
    CHECK(kn_config_has(cfg, "brain.port"));
    /* merged document reflects env + CLI */
    const cJSON *lobby = kn_config_get(cfg, "gameserver.lobby");
    CHECK(cJSON_IsObject(lobby));
    CHECK_EQ_INT(cJSON_GetObjectItem(lobby, "max_players")->valuedouble, 12);
    const char *origins[4];
    CHECK_EQ_INT(kn_config_str_list(cfg, "brain.cors_origins", origins, 4), 2);
    CHECK_EQ_STR(origins[1], "b");
    kn_config_set(cfg, "brain.cors_origins", "x.com, y.com,z.com");
    CHECK_EQ_INT(kn_config_str_list(cfg, "brain.cors_origins", origins, 4), 3);
    CHECK_EQ_STR(origins[2], "z.com");
    CHECK_EQ_STR(kn_config_str(cfg, "brain.cors_origins.0", "-"), "-"); /* now a string */
    /* consumed args */
    CHECK(kn_config_arg_consumed(cfg, 1) && kn_config_arg_consumed(cfg, 2));
    CHECK(kn_config_arg_consumed(cfg, 3) && kn_config_arg_consumed(cfg, 4) && kn_config_arg_consumed(cfg, 5));
    CHECK(!kn_config_arg_consumed(cfg, 6) && !kn_config_arg_consumed(cfg, 7));
    /* root = parent of the config file's directory */
    CHECK_EQ_STR(kn_config_root(cfg), "/");
    char rp[256];
    CHECK_EQ_STR(kn_config_resolve_path(cfg, "/abs/x", rp, sizeof rp), "/abs/x");
    /* redaction */
    char *dump = kn_config_dump_redacted(cfg);
    CHECK(dump && !strstr(dump, "hunter2") && strstr(dump, "\"***\""));
    free(dump);
    /* env password override */
    setenv("KN_DATABASE_PASSWORD", "fromenv", 1);
    CHECK_EQ_STR(kn_config_str(cfg, "database.password", ""), "fromenv");
    kn_config_free(cfg);

    /* errors */
    CHECK(kn_config_load("/nonexistent/kn.json", 0, NULL, err, sizeof err) == NULL);
    CHECK(strstr(err, "cannot read") != NULL);
    FILE *f = fopen(path, "w");
    fputs("{\"a\": [1,2,", f);
    fclose(f);
    CHECK(kn_config_load(path, 0, NULL, err, sizeof err) == NULL);
    CHECK(strstr(err, "invalid JSON") != NULL);
    unlink(path);

    kn_config *s = kn_config_from_string("{\"a\":{\"b\":[{\"c\":3}]}}", err, sizeof err);
    CHECK(s != NULL);
    CHECK_EQ_INT(kn_config_int(s, "a.b.0.c", 0), 3);
    setenv("KN_A_B_0_C", "4", 1);
    CHECK_EQ_INT(kn_config_int(s, "a.b.0.c", 0), 4);
    kn_config_free(s);
    TEST_DONE("knsvc_config");
}
