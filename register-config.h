#ifndef REGISTER_CONFIG_H
#define REGISTER_CONFIG_H

#include <stdint.h>

struct ToneStep {
  uint16_t frequencyHz;
  uint16_t durationMs;
  uint16_t waitMs;
};

/**
 * 商品名候補の配列です。
 * ※末尾のnullptrは終端判定に使うため必須です。
 * ※nullptrが欠けると件数探索が配列外まで進み不正動作します。
 */
static const char* const PRODUCT_NAMES[] = {
  "ぶろっこりー", "きゅうり", "とまと", "ぴーまん",
  "りんご", "いちご", "ばなな", "ぱいん",
  "おにぎり",
  "ぎゅうにゅう", "りんごじゅーす", "おれんじじゅーす", "おちゃ",
  "かむかむれもん", "おにぎりせんべい", "たべっこどうぶつ",
  "くーりっしゅ", "ゆきみだいふく", "こーんふれーく",
  nullptr,
};

/**
 * スキャン音のステップ定義です。
 * ※末尾の{0,0,0}は終端判定に使うため必須です。
 */
static constexpr ToneStep SCAN_TONE_STEPS[] = {
  {1760, 80, 0},
  {0, 0, 0},
};

/**
 * 決済音のステップ定義です。
 * ※末尾の{0,0,0}は終端判定に使うため必須です。
 */
static constexpr ToneStep PAYMENT_TONE_STEPS[] = {
  {1175, 120, 150},
  {1568, 140, 160},
  {2093, 220, 0},
  {0, 0, 0},
};

/**
 * 起動音のステップ定義です。
 * ※末尾の{0,0,0}は終端判定に使うため必須です。
 */
static constexpr ToneStep STARTUP_TONE_STEPS[] = {
  {1319, 90, 40},
  {1760, 110, 40},
  {2093, 150, 0},
  {0, 0, 0},
};

/**
 * スピーカー音量です。
 */
static constexpr uint8_t SPEAKER_VOLUME = 32;

#endif
