/* Filter WPEBackend-fdo's compositor advertisement, not EGL capabilities.
 * Keep non-RGB formats untouched so video import is not constrained.
 * Explicit linear policy is opt-in until plane support is verified on target. */
#ifndef SIGNAGE_FDO_MODIFIER_POLICY_H
#define SIGNAGE_FDO_MODIFIER_POLICY_H
#include <stdint.h>
#include <string.h>
static inline bool signage_fdo_advertise_modifier(const char *policy,
                                                 uint32_t format, uint64_t modifier)
{
    const uint32_t argb8888 = 0x34325241, xrgb8888 = 0x34325258;
    if (!policy || (format != argb8888 && format != xrgb8888))
        return true;
    if (!strcmp(policy, "linear"))
        return modifier == 0;
    /* Opt-in exact DE350 RGB contract; filter actual EGL-reported pairs only.
     * This does not add support to a kernel that only exposes linear scanout. */
    if (!strcmp(policy, "afbc-de350"))
        return modifier == 0x0800000000000072ULL;
    return true;
}
#endif
