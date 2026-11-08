// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/*
 * Stand-in for the microKORG2 SDK's unit_osc.h, for this host test only. The
 * real header pulls in NEON-only SIMD headers and does not compile off ARM.
 * The context and GetBufferOffset() are copied field for field; unit.h and
 * runtime.h come from the SDK.
 */

#include <stdint.h>

#include "runtime.h"

typedef struct unit_runtime_osc_context {
  float pitch[kMk2MaxVoices];
  uint8_t trigger;  /* bit array */
  float * unitModDataPlus;
  float * unitModDataPlusMinus;
  uint8_t modDataSize;
  uint16_t bufferOffset;
  uint8_t voiceOffset;
  uint8_t voiceLimit;
  uint16_t outputStride;
} unit_runtime_osc_context_t;

/* Copied from the SDK's unit_osc.h. */
static inline uint32_t GetBufferOffset(const unit_runtime_osc_context_t * context,
                                       int voice, int32_t frameSize) {
  return context->bufferOffset + (voice >> 2) * (frameSize << 2);
}
