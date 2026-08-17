/* SPDX-License-Identifier: GPL-2.0-only */
#include "test_util.h"

int dhp_test_failures = 0;
int dhp_test_checks = 0;

void test_siphash_suite(void);
void test_frame_suite(void);
void test_pointer_suite(void);
void test_hid_suite(void);
void test_uhrp_suite(void);
void test_level_suite(void);
void test_link_suite(void);
void test_router_suite(void);
void test_sim_suite(void);
void test_auth_suite(void);

int main(void)
{
    printf("== siphash ==\n");  test_siphash_suite();
    printf("== frame ==\n");    test_frame_suite();
    printf("== pointer ==\n");  test_pointer_suite();
    printf("== hid ==\n");      test_hid_suite();
    printf("== level ==\n");    test_level_suite();
    printf("== uhrp ==\n");     test_uhrp_suite();
    printf("== link ==\n");     test_link_suite();
    printf("== router ==\n");   test_router_suite();
    printf("== chain sim ==\n"); test_sim_suite();
    printf("== pairing ==\n"); test_auth_suite();

    printf("\n%d checks, %d failures\n", dhp_test_checks, dhp_test_failures);
    return dhp_test_failures ? 1 : 0;
}
