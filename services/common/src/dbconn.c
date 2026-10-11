/* Synchronous connection + config helpers (knsvc/db.h). */
#include "db_internal.h"
#include "knsvc/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kn_db_config_from(const kn_config *cfg, kn_db_config *o) {
    memset(o, 0, sizeof *o);
    o->host = kn_config_str(cfg, "database.host", "127.0.0.1");
    o->port = (int)kn_config_int(cfg, "database.port", 5432);
    o->name = kn_config_str(cfg, "database.name", "krunknative");
    o->user = kn_config_str(cfg, "database.user", "krunknative");
    o->password = kn_config_str(cfg, "database.password", "");
    o->search_path = kn_config_str(cfg, "database.search_path", "");
    if (o->search_path && !*o->search_path) o->search_path = NULL;
    o->application_name = kn_config_str(cfg, "database.application_name", NULL);
    o->pool_size = (int)kn_config_int(cfg, "database.pool_size", 4);
    o->connect_timeout_s = (int)kn_config_int(cfg, "database.connect_timeout_s", 5);
    o->statement_timeout_ms = (int)kn_config_int(cfg, "database.statement_timeout_ms", 0);
}

/* "-c search_path=a,b -c statement_timeout=N" with libpq option escaping (backslash before spaces). */
static void build_options(const kn_db_config *cfg, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    if (cfg->search_path && *cfg->search_path) {
        o += (size_t)snprintf(out + o, cap - o, "-c search_path=");
        for (const char *p = cfg->search_path; *p && o + 3 < cap; p++) {
            if (*p == ' ' || *p == '\\') out[o++] = '\\';
            out[o++] = *p;
        }
        out[o] = 0;
    }
    if (cfg->statement_timeout_ms > 0 && o < cap)
        snprintf(out + o, cap - o, "%s-c statement_timeout=%d", o ? " " : "", cfg->statement_timeout_ms);
}

void kn_db__conninfo_build(const kn_db_config *cfg, kn_db__conninfo *ci) {
    int n = 0;
    snprintf(ci->port, sizeof ci->port, "%d", cfg->port > 0 ? cfg->port : 5432);
    snprintf(ci->timeout, sizeof ci->timeout, "%d", cfg->connect_timeout_s > 0 ? cfg->connect_timeout_s : 5);
    build_options(cfg, ci->options, sizeof ci->options);
#define KV(k, v) do { if ((v) && *(v)) { ci->keys[n] = (k); ci->vals[n] = (v); n++; } } while (0)
    KV("host", cfg->host ? cfg->host : "127.0.0.1");
    KV("port", ci->port);
    KV("dbname", cfg->name);
    KV("user", cfg->user);
    KV("password", cfg->password);
    KV("connect_timeout", ci->timeout);
    KV("application_name", cfg->application_name ? cfg->application_name : "knsvc");
    KV("options", ci->options);
    KV("client_encoding", "UTF8");
#undef KV
    ci->keys[n] = NULL;
    ci->vals[n] = NULL;
}

struct kn_dbconn {
    PGconn *pg;
};

static void quiet_notice(void *arg, const PGresult *res) { (void)arg; (void)res; }

kn_dbconn *kn_dbconn_open(const kn_db_config *cfg, char *err, size_t errlen) {
    kn_db__conninfo ci;
    kn_db__conninfo_build(cfg, &ci);
    PGconn *pg = PQconnectdbParams(ci.keys, ci.vals, 0);
    if (!pg || PQstatus(pg) != CONNECTION_OK) {
        if (err && errlen) snprintf(err, errlen, "%s", pg ? PQerrorMessage(pg) : "out of memory");
        if (err && errlen) {
            size_t n = strlen(err);
            while (n && err[n - 1] == '\n') err[--n] = 0;
        }
        PQfinish(pg);
        return NULL;
    }
    PQsetNoticeReceiver(pg, quiet_notice, NULL);
    kn_dbconn *c = calloc(1, sizeof *c);
    if (!c) {
        PQfinish(pg);
        return NULL;
    }
    c->pg = pg;
    return c;
}

void kn_dbconn_close(kn_dbconn *c) {
    if (!c) return;
    PQfinish(c->pg);
    free(c);
}

kn_db_result *kn_dbconn_exec(kn_dbconn *c, const char *sql, int nparams, const char *const *params) {
    PGresult *r = PQexecParams(c->pg, sql, nparams, NULL, params, NULL, NULL, 0);
    return kn_db__result_from_pg(r, c->pg);
}

int kn_dbconn_exec_script(kn_dbconn *c, const char *sql, char *err, size_t errlen) {
    PGresult *r = PQexec(c->pg, sql);
    kn_db_result *res = kn_db__result_from_pg(r, c->pg);
    int ok = kn_db_ok(res);
    if (!ok && err && errlen) snprintf(err, errlen, "%s", kn_db_error(res));
    kn_db_result_free(res);
    return ok ? 0 : -1;
}

void *kn_dbconn_pg(kn_dbconn *c) { return c->pg; }
