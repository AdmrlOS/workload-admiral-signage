/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include "../native/standby-protocol.h"

int main(void)
{
    SignageStandby s = {0};
    /* Admission is explicit and fail closed, never a resolution downgrade. */
    assert(!signage_standby_prepare(&s, 10, 20, 0, 256, 128));
    assert(!signage_standby_prepare(&s, 10, 20, 2, 127, 128));
    assert(signage_standby_prepare(&s, 10, 20, 2, 256, 128));
    assert(!signage_standby_prepare(&s, 10, 30, 2, 256, 128));
    assert(!signage_standby_ready(&s, 19, 1, 8, false));
    assert(!signage_standby_ready(&s, 20, 1, 8, false));
    assert(signage_standby_ready(&s, 20, 1, 8, true));
    assert(!signage_standby_activate(&s, 10, 600, 7, 20, 99, 100, true));
    assert(!signage_standby_endpoint(&s, 9, 600, 7, 100, 17));
    assert(signage_standby_endpoint(&s, 10, 600, 7, 100, 17));
    assert(!signage_standby_activate(&s, 10, 599, 7, 20, 117, 100, true));
    assert(!signage_standby_activate(&s, 10, 600, 7, 20, 116, 100, true));
    assert(!signage_standby_activate(&s, 10, 600, 7, 20, 117, 100, false));
    assert(signage_standby_activate(&s, 10, 600, 7, 20, 117, 100, true));
    assert(!signage_standby_flipped(&s, 19, 1, 8));
    assert(signage_standby_flipped(&s, 20, 1, 8));
    assert(!signage_standby_prepare(&s, 20, 30, 2, 256, 128));
    assert(!signage_standby_retired(&s, 9));
    assert(signage_standby_retired(&s, 10));
    assert(signage_standby_prepare(&s, 20, 30, 2, 256, 128));
    assert(!signage_standby_cancel(&s, 20));
    assert(signage_standby_cancel(&s, 30));
    assert(!signage_standby_ready(&s, 30, 2, 9, true));
    assert(signage_standby_prepare(&s, 20, 40, 2, 256, 128));
    assert(!signage_standby_prepare(&s, 20, 30, 2, 256, 128));
    assert(signage_standby_cancel(&s, 40));
    return 0;
}
