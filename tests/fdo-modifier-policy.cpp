#include "../browser/fdo-modifier-policy.h"
#include <cassert>
int main() {
    constexpr unsigned argb = 0x34325241, xrgb = 0x34325258, nv12 = 0x3231564e;
    assert(signage_fdo_advertise_modifier(nullptr, argb, 0x0800000000000062ULL));
    assert(signage_fdo_advertise_modifier("linear", argb, 0));
    assert(!signage_fdo_advertise_modifier("linear", argb, 0x0800000000000062ULL));
    assert(!signage_fdo_advertise_modifier("linear", xrgb, 0xffffffffffffffffULL));
    assert(signage_fdo_advertise_modifier("linear", nv12, 0x1234));
    assert(signage_fdo_advertise_modifier("auto", argb, 0x0800000000000062ULL));
    assert(!signage_fdo_advertise_modifier("afbc-de350", argb, 0x0800000000000062ULL));
    assert(signage_fdo_advertise_modifier("afbc-de350", argb, 0x0800000000000072ULL));
    assert(!signage_fdo_advertise_modifier("afbc-de350", argb, 0));
    assert(signage_fdo_advertise_modifier("afbc-de350", nv12, 0x1234));
}
