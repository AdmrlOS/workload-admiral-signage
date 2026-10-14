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

    /* UI-only: boot (no outgoing stream) prepares without a decoder context. */
    SignageStandby u = {0};
    assert(!signage_standby_prepare_ui(&u, 0, 0));
    assert(signage_standby_prepare_ui(&u, 0, 5));
    assert(!signage_standby_ready(&u, 5, 1, 8, true));          /* never a fake video frame */
    assert(!signage_standby_ready_ui(&u, 4, 30, true));         /* stale token */
    assert(!signage_standby_ready_ui(&u, 5, 30, false));        /* no TEST_ONLY */
    assert(!signage_standby_ready_ui(&u, 5, 0, true));          /* no exported UI fb */
    assert(signage_standby_ready_ui(&u, 5, 30, true));
    assert(!signage_standby_activate(&u, 0, 0, 0, 5, 1, 1, true)); /* video path cannot take a UI token */
    assert(!signage_standby_activate_ui(&u, 0, 5, false, 1, 0, true));
    assert(signage_standby_activate_ui(&u, 0, 5, true, 1, 1, true)); /* no outgoing: endpoint moot */
    assert(!signage_standby_cancel(&u, 5));                     /* in-flight cannot be recalled */
    assert(!signage_standby_flipped(&u, 5, 0, 30));
    assert(!signage_standby_flipped_ui(&u, 5, 31));             /* mismatched flip */
    assert(signage_standby_flipped_ui(&u, 5, 30));
    assert(u.phase == SIGNAGE_STANDBY_EMPTY && u.active == 0 && !u.ui_only);
    /* HTML -> HTML: no outgoing stream either. */
    assert(!signage_standby_prepare_ui(&u, 0, 5));              /* tokens are monotonic */
    assert(signage_standby_prepare_ui(&u, 0, 6));
    assert(signage_standby_cancel(&u, 6) && !u.ui_only);
    /* HTML (UI-only) -> video: active 0, no endpoint exists or is fabricated. */
    assert(signage_standby_prepare(&u, 0, 7, 2, 256, 128));
    assert(!signage_standby_endpoint(&u, 0, 1, 1, 1, 1));
    assert(signage_standby_ready(&u, 7, 1, 9, true));
    assert(!signage_standby_activate(&u, 0, 1, 1, 7, 1, 1, true));
    assert(signage_standby_activate(&u, 0, 0, 0, 7, 1, 1, true));
    assert(signage_standby_flipped(&u, 7, 1, 9));
    assert(u.phase == SIGNAGE_STANDBY_EMPTY && u.active == 7);
    /* video -> HTML at EOS: endpoint interval is required; then exact retirement. */
    assert(signage_standby_prepare_ui(&u, 7, 8));
    assert(signage_standby_ready_ui(&u, 8, 40, true));
    assert(!signage_standby_activate_ui(&u, 7, 8, true, 500, 1, true));
    assert(signage_standby_endpoint(&u, 7, 600, 9, 100, 17));
    assert(!signage_standby_activate_ui(&u, 7, 8, true, 116, 1, true));
    assert(!signage_standby_activate_ui(&u, 6, 8, true, 117, 1, true));
    assert(signage_standby_activate_ui(&u, 7, 8, true, 117, 1, true));
    assert(signage_standby_activation_failed(&u, 8) && u.phase == SIGNAGE_STANDBY_READY);
    assert(signage_standby_activate_ui(&u, 7, 8, true, 117, 1, true));
    assert(signage_standby_flipped_ui(&u, 8, 40) && u.phase == SIGNAGE_STANDBY_RETIRING);
    assert(!signage_standby_retired(&u, 8));
    assert(signage_standby_retired(&u, 7));
    assert(u.phase == SIGNAGE_STANDBY_EMPTY && u.active == 0 && !u.standby);
    /* timed HTML outgoing with video: no endpoint, video hides in the same commit. */
    assert(signage_standby_prepare(&u, 0, 9, 2, 256, 128) && signage_standby_ready(&u, 9, 1, 9, true));
    assert(signage_standby_activate(&u, 0, 0, 0, 9, 1, 1, true) && signage_standby_flipped(&u, 9, 1, 9));
    assert(signage_standby_prepare_ui(&u, 9, 10) && signage_standby_ready_ui(&u, 10, 41, true));
    assert(signage_standby_activate_ui(&u, 9, 10, false, 0, 1, true));
    return 0;
}
