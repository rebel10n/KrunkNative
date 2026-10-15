// http_proxy_test: the proxy URL parser and the per-host choice (client/src/net/http_proxy.c):
// the client setting and its scope, the environment fallback, NO_PROXY, loopback.
#include <http_proxy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_count;

static void check(const char *name, const int ok) {
    g_count++;
    if (!ok) {
        g_fail++;
        printf("FAIL  %s\n", name);
    }
}

static int parses(const char *url, const HttpProxyKind kind, const char *host, const char *port, const char *user, const char *pass) {
    HttpProxy p;
    return http_proxy_parse(url, &p) && p.kind == kind && !strcmp(p.host, host) && !strcmp(p.port, port) && !strcmp(p.user, user) &&
           !strcmp(p.pass, pass);
}

static int via(const char *host, const int tls, const char *label) {
    HttpProxy p;
    if (!http_proxy_for(host, tls, &p)) return label == NULL;
    char got[160];
    http_proxy_label(&p, got, sizeof(got));
    return label && !strcmp(got, label);
}

static void clear_env(void) {
    static const char *const VARS[] = {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY", "KN_FETCH_PROXY"};
    for (size_t i = 0; i < sizeof(VARS) / sizeof(VARS[0]); i++) unsetenv(VARS[i]);
}

int main(void) {
    clear_env();
    // ---- parse
    check("http://host:port", parses("http://127.0.0.1:8888", HTTP_PROXY_HTTP, "127.0.0.1", "8888", "", ""));
    check("trailing slash, spaces", parses("  http://proxy.example:3128/ ", HTTP_PROXY_HTTP, "proxy.example", "3128", "", ""));
    check("bare host:port = http", parses("proxy.example:8080", HTTP_PROXY_HTTP, "proxy.example", "8080", "", ""));
    check("http default port 80", parses("http://proxy.example", HTTP_PROXY_HTTP, "proxy.example", "80", "", ""));
    check("https default port 443", parses("https://proxy.example", HTTP_PROXY_HTTPS, "proxy.example", "443", "", ""));
    check("socks5", parses("socks5://10.0.0.1:1080", HTTP_PROXY_SOCKS5, "10.0.0.1", "1080", "", ""));
    check("socks5h default port", parses("SOCKS5H://tor.local", HTTP_PROXY_SOCKS5H, "tor.local", "1080", "", ""));
    check("credentials", parses("http://me:p%40ss%3A@proxy.example:8080", HTTP_PROXY_HTTP, "proxy.example", "8080", "me", "p@ss:"));
    check("user only", parses("socks5://me@proxy:1080", HTTP_PROXY_SOCKS5, "proxy", "1080", "me", ""));
    check("[IPv6]:port", parses("http://[::1]:3128", HTTP_PROXY_HTTP, "::1", "3128", "", ""));
    HttpProxy p;
    check("empty refused", !http_proxy_parse("", &p) && !http_proxy_parse("   ", &p));
    check("socks4 refused", !http_proxy_parse("socks4://h:1080", &p));
    check("path refused", !http_proxy_parse("http://h:8080/x", &p));
    check("bad port refused", !http_proxy_parse("http://h:99999", &p) && !http_proxy_parse("http://h:", &p) && !http_proxy_parse("http://h:80x", &p));
    check("bad host refused", !http_proxy_parse("http://h st:80", &p) && !http_proxy_parse("http://:80", &p));
    char label[160], auth[200];
    http_proxy_parse("http://Aladdin:open%20sesame@h:1", &p);
    http_proxy_basic_auth(&p, auth, sizeof(auth));
    check("basic auth (RFC 7617 example)", !strcmp(auth, "QWxhZGRpbjpvcGVuIHNlc2FtZQ=="));
    http_proxy_label(&p, label, sizeof(label));
    check("label has no credentials", !strcmp(label, "http://h:1"));

    // ---- the setting: krunker.io only (default) and all
    const unsigned gen = http_proxy_generation();
    http_proxy_configure("http://127.0.0.1:8888", HTTP_PROXY_SCOPE_KRUNKER);
    check("configure bumps the generation", http_proxy_generation() != gen);
    check("krunker: matchmaker.krunker.io", via("matchmaker.krunker.io", 1, "http://127.0.0.1:8888"));
    check("krunker: krunker.io", via("krunker.io", 1, "http://127.0.0.1:8888"));
    check("krunker: KRUNKER.IO (case)", via("KRUNKER.IO", 0, "http://127.0.0.1:8888"));
    check("krunker: not notkrunker.io", via("notkrunker.io", 1, NULL));
    check("krunker: not krunker.io.evil.com", via("krunker.io.evil.com", 1, NULL));
    check("krunker: not coordinator.krunk.io", via("coordinator.krunk.io", 1, NULL));
    http_proxy_configure("socks5h://127.0.0.1:1080", HTTP_PROXY_SCOPE_ALL);
    check("all: coordinator.krunk.io", via("coordinator.krunk.io", 1, "socks5h://127.0.0.1:1080"));
    check("all: plain http too", via("example.com", 0, "socks5h://127.0.0.1:1080"));
    check("all: never loopback", via("127.0.0.1", 1, NULL) && via("localhost", 0, NULL) && via("::1", 1, NULL));
    setenv("HTTPS_PROXY", "http://env.example:1", 1);
    check("the setting wins over the environment", via("krunker.io", 1, "socks5h://127.0.0.1:1080"));
    http_proxy_configure("not a url", HTTP_PROXY_SCOPE_ALL);
    check("an invalid setting = none (the environment)", via("krunker.io", 1, "http://env.example:1"));

    // ---- empty setting: the environment
    http_proxy_configure("", HTTP_PROXY_SCOPE_KRUNKER);
    clear_env();
    check("no setting, no environment: direct", via("krunker.io", 1, NULL));
    setenv("HTTPS_PROXY", "http://env.example:3128", 1);
    check("HTTPS_PROXY for https (any host)", via("coordinator.krunk.io", 1, "http://env.example:3128"));
    check("HTTPS_PROXY not for http", via("coordinator.krunk.io", 0, NULL));
    setenv("https_proxy", "socks5://lower.example:1080", 1);
    check("https_proxy before HTTPS_PROXY", via("krunker.io", 1, "socks5://lower.example:1080"));
    setenv("HTTP_PROXY", "env2.example:8080", 1);
    check("HTTP_PROXY for http (bare host:port)", via("krunker.io", 0, "http://env2.example:8080"));
    clear_env();
    setenv("ALL_PROXY", "socks5h://all.example:9050", 1);
    check("ALL_PROXY for both", via("krunker.io", 1, "socks5h://all.example:9050") && via("krunker.io", 0, "socks5h://all.example:9050"));
    setenv("NO_PROXY", "localhost, .krunk.io,example.com:443", 1);
    check("NO_PROXY .krunk.io covers its subdomains", via("coordinator.krunk.io", 1, NULL) && via("krunk.io", 1, NULL));
    check("NO_PROXY example.com:443 covers example.com and www", via("example.com", 1, NULL) && via("www.example.com", 1, NULL));
    check("NO_PROXY: others still proxied", via("krunker.io", 1, "socks5h://all.example:9050") && via("badexample.com", 1, "socks5h://all.example:9050"));
    setenv("NO_PROXY", "*", 1);
    check("NO_PROXY *", via("krunker.io", 1, NULL));
    setenv("no_proxy", "*.krunker.io", 1);
    check("no_proxy before NO_PROXY, *.domain", via("matchmaker.krunker.io", 1, NULL) && via("example.com", 1, "socks5h://all.example:9050"));
    clear_env();
    setenv("HTTPS_PROXY", "ftp://nope", 1);
    check("an invalid environment proxy is ignored", via("krunker.io", 1, NULL));
    clear_env();
    // KN_FETCH_PROXY (the dev tools' convention): krunker.io hosts only, before the standard variables
    setenv("KN_FETCH_PROXY", "127.0.0.1:8888", 1);
    setenv("HTTPS_PROXY", "http://env.example:3128", 1);
    check("KN_FETCH_PROXY: user-assets.krunker.io goes through it",
          via("user-assets.krunker.io", 1, "http://127.0.0.1:8888") && via("gapi.svc.krunker.io", 1, "http://127.0.0.1:8888"));
    check("KN_FETCH_PROXY: other hosts keep the standard variables", via("coordinator.krunk.io", 1, "http://env.example:3128"));
    http_proxy_configure("http://setting.example:8080", HTTP_PROXY_SCOPE_KRUNKER);
    check("KN_FETCH_PROXY: the client setting wins", via("user-assets.krunker.io", 1, "http://setting.example:8080"));
    http_proxy_configure("", HTTP_PROXY_SCOPE_KRUNKER);
    setenv("KN_FETCH_PROXY", "ftp://nope", 1);
    check("KN_FETCH_PROXY: an invalid value is ignored", via("user-assets.krunker.io", 1, "http://env.example:3128"));
    clear_env();

    printf("%d/%d passed\n", g_count - g_fail, g_count);
    return g_fail ? 1 : 0;
}
