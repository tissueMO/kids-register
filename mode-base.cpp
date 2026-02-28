#include "mode-base.h"

#include <M5Unified.h>

#include "register-config.h"

/**
 * 音ステップ列を順番に再生します。
 */
void ModeBase::playToneSteps(const ToneStep* steps) {
  size_t index = 0;

  while (steps[index].durationMs != 0) {
    M5.Speaker.tone(steps[index].frequencyHz, steps[index].durationMs);
    if (steps[index].waitMs > 0) {
      delay(steps[index].waitMs);
    }
    ++index;
  }
}
