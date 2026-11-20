/*
 * The CGM host test's entry point. test_cgm.c is ESP-Sim's, copied
 * byte-identical; it defines test_cgm_all() but no main or counters.
 */
#include <stdio.h>

#include "test.h"

int failures, checks;

void test_cgm_all(void);

int main(void)
{
    test_cgm_all();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
