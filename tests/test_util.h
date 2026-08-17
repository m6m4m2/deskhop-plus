/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Minimal test scaffolding. Deliberately dependency-free: core/ builds on a
 * freestanding target, and the tests should not drag in a framework that
 * cannot follow it there.
 */
#ifndef DHP_TEST_UTIL_H
#define DHP_TEST_UTIL_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int dhp_test_failures;
extern int dhp_test_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        dhp_test_checks++;                                                     \
        if (!(cond)) {                                                         \
            dhp_test_failures++;                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        dhp_test_checks++;                                                     \
        const long long _a = (long long)(a);                                   \
        const long long _b = (long long)(b);                                   \
        if (_a != _b) {                                                        \
            dhp_test_failures++;                                               \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,        \
                   __LINE__, #a, #b, _a, _b);                                  \
        }                                                                      \
    } while (0)

#define CHECK_STR(a, b)                                                        \
    do {                                                                       \
        dhp_test_checks++;                                                     \
        if (strcmp((a), (b)) != 0) {                                           \
            dhp_test_failures++;                                               \
            printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,     \
                   (a), (b));                                                  \
        }                                                                      \
    } while (0)

#define RUN(fn)                                                                \
    do {                                                                       \
        const int before = dhp_test_failures;                                  \
        printf("- %s\n", #fn);                                                 \
        fn();                                                                  \
        if (dhp_test_failures != before) {                                      \
            printf("  ^ %d failure(s)\n", dhp_test_failures - before);         \
        }                                                                      \
    } while (0)

#endif /* DHP_TEST_UTIL_H */
