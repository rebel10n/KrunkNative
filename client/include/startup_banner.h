#pragma once
// Startup log of the client (client/src/startup_banner.c), once the GL context exists.
//
// Web: v7.1.4's dev-console badge (src/index.js: console.info("%c Browser %c <user agent tail> ")
// with its two inline styles), reproduced verbatim, followed by KrunkNative badges in the same
// style: the build, the WebGL renderer and the full user agent.
// Native: the same facts as plain "[startup]" lines on stdout: build (commit, build time, protocol),
// platform, GL backend (headless.h), GL_RENDERER and GL_VERSION.
void startup_banner(void);
#include <stddef.h>
// navigator.userAgent: the browser's on the web; natively "Mozilla/5.0 (<OS>; <arch>) KrunkNative/<commit>" (the
// disconnect report's ua / browser, disconnect_report.c).
void startup_user_agent(char *out, size_t n);
