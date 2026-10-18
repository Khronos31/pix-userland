/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../siano-clock.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int timespec_before(const struct timespec *left,
                           const struct timespec *right)
{
    return left->tv_sec < right->tv_sec ||
           (left->tv_sec == right->tv_sec && left->tv_nsec < right->tv_nsec);
}

static void test_boundaries(void)
{
    struct timespec ts;

    assert(siano_qpc_to_timespec(0, 10000000, &ts) == 0);
    assert(ts.tv_sec == 0 && ts.tv_nsec == 0);

    assert(siano_qpc_to_timespec(10000000, 10000000, &ts) == 0);
    assert(ts.tv_sec == 1 && ts.tv_nsec == 0);

    assert(siano_qpc_to_timespec(1, 3, &ts) == 0);
    assert(ts.tv_sec == 0 && ts.tv_nsec == 333333333L);
    assert(siano_qpc_to_timespec(2, 3, &ts) == 0);
    assert(ts.tv_sec == 0 && ts.tv_nsec == 666666666L);

    assert(siano_qpc_to_timespec(6, 7, &ts) == 0);
    assert(ts.tv_sec == 0 && ts.tv_nsec == 857142857L);
    assert(siano_qpc_to_timespec(9, 10, &ts) == 0);
    assert(ts.tv_sec == 0 && ts.tv_nsec == 900000000L);
}

static void test_large_counter(void)
{
    struct timespec ts;

    /* counter * 1e9 overflows uint64_t, while the split conversion does not. */
    assert(siano_qpc_to_timespec(50000001234567ULL, 10000000, &ts) == 0);
    assert(ts.tv_sec == 5000000 && ts.tv_nsec == 123456700L);

    assert(siano_qpc_to_timespec(INT64_MAX, 10000000, &ts) == 0);
    assert(ts.tv_sec == (time_t)922337203685 && ts.tv_nsec == 477580700L);

    assert(siano_qpc_to_timespec(INT64_MAX, 10000003, &ts) == 0);
    assert(ts.tv_sec == (time_t)922336926984 && ts.tv_nsec == 399485380L);
}

static void test_monotonic_ordering(void)
{
    struct timespec previous;
    struct timespec current;
    uint64_t counter;

    assert(siano_qpc_to_timespec(1234567890123ULL, 1000000, &previous) == 0);
    for (counter = 1234567890124ULL; counter < 1234567890130ULL; counter++) {
        assert(siano_qpc_to_timespec(counter, 1000000, &current) == 0);
        assert(timespec_before(&previous, &current));
        assert(current.tv_nsec >= 0 && current.tv_nsec < 1000000000L);
        previous = current;
    }
}

static void test_invalid_frequency(void)
{
    struct timespec ts = {0, 0};

    assert(siano_qpc_to_timespec(1, 0, &ts) < 0);
    assert(ts.tv_sec == 0 && ts.tv_nsec == 0);
}

int main(void)
{
    test_boundaries();
    test_large_counter();
    test_monotonic_ordering();
    test_invalid_frequency();
    puts("clock tests: PASS");
    return 0;
}
