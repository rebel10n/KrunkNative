/* Minimal test harness for the knsvc unit tests (ctest runs each binary). */
#ifndef KN_TEST_H
#define KN_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int kn_test_failures;
static int kn_test_checks;

#define CHECK(cond) do { kn_test_checks++; if (!(cond)) { kn_test_failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); kn_test_checks++; \
    if (_a != _b) { kn_test_failures++; fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", \
    __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define CHECK_EQ_STR(a, b) do { const char *_a = (a), *_b = (b); kn_test_checks++; \
    if (!_a || !_b || strcmp(_a, _b) != 0) { kn_test_failures++; fprintf(stderr, \
    "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)

#define TEST_DONE(name) do { printf("%s: %d checks, %d failures\n", name, kn_test_checks, kn_test_failures); \
    return kn_test_failures ? 1 : 0; } while (0)

#endif
