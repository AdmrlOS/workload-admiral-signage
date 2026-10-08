/* SPDX-License-Identifier: MIT */
#pragma once
#include <glib.h>
#include <wpe/wpe.h>

typedef struct SignageInput SignageInput;
SignageInput *signage_input_start(struct wpe_view_backend *backend, guint width, guint height);
void signage_input_stop(SignageInput *input);
