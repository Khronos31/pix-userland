/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../write-policy.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    assert(siano_write_request_size(0, UINT_MAX) == 0);
    assert(siano_write_request_size(1, UINT_MAX) == 1);
    assert(siano_write_request_size(UINT_MAX, UINT_MAX) == UINT_MAX);
    assert(siano_write_request_size(9, 4) == 4);
    assert(siano_write_request_size(3, 4) == 3);
    if (SIZE_MAX > (size_t)UINT_MAX) {
        size_t over_limit = (size_t)UINT_MAX;

        ++over_limit;
        assert(siano_write_request_size(over_limit, UINT_MAX) == UINT_MAX);
        assert(siano_write_request_size(over_limit, SIZE_MAX) == over_limit);
        over_limit += 16U;
        assert(siano_write_request_size(over_limit, UINT_MAX) == UINT_MAX);
        assert(siano_write_request_size(over_limit, SIZE_MAX) == over_limit);
    }
    puts("write policy tests: PASS");
    return 0;
}
