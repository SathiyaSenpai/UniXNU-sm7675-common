/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

struct Sm7675ScmResult {
    int64_t x0;
    uint64_t x1, x2, x3;
};

// Sleepable non-atomic SCM call. The shared service serializes callers, handles
// QCOM_A6 interrupted calls and completes Qualcomm WAITQ before returning.
Sm7675ScmResult sm7675_scm_call(uint32_t service, uint32_t command, uint32_t arginfo,
                               uint64_t a, uint64_t b, uint64_t c);
