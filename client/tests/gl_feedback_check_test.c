// gl_feedback_check_test: the debug-build feedback-loop assertion (client/include/gl_feedback_check.h).
// In a headless GL context: a framebuffer with a colour texture and a depth texture attached;
// a draw with a program whose sampler's unit holds either attachment is a feedback loop (what
// WebGL / ANGLE reject), the same draw with that unit holding another texture (or 0), with the
// default framebuffer bound, or with a program that samples another unit is not. The check runs
// in warn mode (KRUNKNATIVE_GL_FEEDBACK_CHECK=warn) and its stderr report is what is counted.
// Exit 77 (skipped) without GL or in a build without the check (NDEBUG).
#include <gl.h>
#include <GLFW/glfw3.h>
#include <headless.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef KN_GL_FEEDBACK_CHECK
int main(void) {
    printf("gl_feedback_check_test: built without the check (NDEBUG), skipped\n");
    return 77;
}
#else

static int failures;

// setenv for MinGW, whose CRT has only _putenv_s (the same guard as client/src/headless.c).
static int test_setenv(const char *name, const char *value, const int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
#else
    return setenv(name, value, overwrite);
#endif
}

static GLuint compile(const GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

static GLuint program_with(const char *frag) {
#ifdef KN_GLES
    const char *head = "#version 300 es\nprecision mediump float;\n";
#else
    const char *head = "#version 330 core\n";
#endif
    char vs[256], fs[1024];
    snprintf(vs, sizeof(vs), "%svoid main() { gl_Position = vec4(float(gl_VertexID) - 1.0, 0.0, 0.0, 1.0); }\n", head);
    snprintf(fs, sizeof(fs), "%s%s", head, frag);
    const GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    return p;
}

// stderr lines with "FEEDBACK LOOP" written by the draw at `line` (warn mode reports each call site once,
// so every case draws from its own line through the macro below)
static int reports(const char *path, const int line) {
    fflush(stderr);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[512], site[64];
    snprintf(site, sizeof(site), "gl_feedback_check_test.c:%d: program", line);
    int n = 0;
    while (fgets(buf, sizeof(buf), f))
        if (strstr(buf, "FEEDBACK LOOP") && strstr(buf, site)) n++;
    fclose(f);
    return n;
}

#define CASE(expect, what)                                                                                   \
    do {                                                                                                     \
        const int line = __LINE__;                                                                           \
        glDrawArrays(GL_TRIANGLES, 0, 3);                                                                    \
        const int got = reports(log_path, line);                                                             \
        const int ok = (expect) ? got > 0 : got == 0;                                                        \
        printf("%s %s (%d report%s)\n", ok ? "ok  " : "FAIL", what, got, got == 1 ? "" : "s");               \
        if (!ok) failures++;                                                                                 \
    } while (0)

int main(int argc, char **argv) {
    headless_parse_args(&argc, argv);
    test_setenv("KRUNKNATIVE_GL_FEEDBACK_CHECK", "warn", 1);
    char log_path[] = "/tmp/kn-gl-feedback-test-XXXXXX";
    const int fd = mkstemp(log_path);
    if (fd < 0 || !freopen(log_path, "w", stderr)) return 1;
    close(fd);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (!headless_glfw_init()) {
        printf("gl_feedback_check_test: no GL (glfwInit failed), skipped\n");
        unlink(log_path);
        return 77;
    }
    headless_window_hints();
    GLFWwindow *win = glfwCreateWindow(64, 64, "gl feedback test", NULL, NULL);
    if (!win || !headless_make_context_current(win) || !kn_gl_load(headless_get_proc_address)) {
        printf("gl_feedback_check_test: no GL context, skipped\n");
        unlink(log_path);
        return 77;
    }

    GLuint tex[3];
    glGenTextures(3, tex);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        if (i == 1) glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 16, 16, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex[1], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("FAIL framebuffer incomplete\n");
        return 1;
    }
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 16, 16);

    // sampler `a` on unit 0 (its default), `b` on unit 3
    const GLuint prog = program_with("uniform sampler2D a; uniform sampler2D b; out vec4 o;\n"
                                     "void main() { o = texture(a, vec2(0.5)) + texture(b, vec2(0.5)); }\n");
    const GLuint plain = program_with("out vec4 o; void main() { o = vec4(1.0); }\n");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "b"), 3);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    CASE(1, "colour attachment on a sampled unit (0, the sampler default)");
    glBindTexture(GL_TEXTURE_2D, tex[2]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    CASE(1, "depth attachment on a sampled unit (3)");
    GLint active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    if (active != GL_TEXTURE3) {
        printf("FAIL the check changed GL_ACTIVE_TEXTURE\n");
        failures++;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    CASE(0, "units hold another texture / none");
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    CASE(0, "attachment on a unit no sampler reads (5)");
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glUseProgram(plain);
    CASE(0, "program without samplers");
    glUseProgram(prog);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    CASE(0, "default framebuffer");

    glfwDestroyWindow(win);
    headless_shutdown();
    unlink(log_path);
    printf(failures ? "gl_feedback_check_test: %d FAILED\n" : "gl_feedback_check_test: all passed\n", failures);
    return failures ? 1 : 0;
}
#endif
