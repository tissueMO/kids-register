#include <M5Unified.h>

#include <algorithm>
#include <vector>

#include "register-config.h"

namespace {

// バーコードスキャナUART設定
constexpr int BARCODE_RX_PIN = 18;
constexpr int BARCODE_TX_PIN = 17;
constexpr long BARCODE_BAUD = 9600;

// RFIDリーダUART設定
constexpr int RFID_RX_PIN = 14;
constexpr int RFID_TX_PIN = 13;
constexpr long RFID_BAUD = 9600;

// 画面遷移タイミング
constexpr uint32_t THANK_YOU_DURATION_MS = 3000;

// 会計ロジック設定
constexpr int ITEM_VISIBLE_ROWS = 3;
constexpr int PRICE_MIN = 50;
constexpr int PRICE_STEP = 10;
constexpr int PRICE_LEVELS = 46;

// 画面レイアウト設定
constexpr int CLEAR_BUTTON_X = 214;
constexpr int CLEAR_BUTTON_Y = 6;
constexpr int CLEAR_BUTTON_W = 100;
constexpr int CLEAR_BUTTON_H = 40;

enum class AppState {
  NORMAL,
  THANK_YOU,
};

struct Item {
  String name;
  int price;
};

HardwareSerial barcodeSerial(1);
HardwareSerial rfidSerial(2);

std::vector<Item> cart;
AppState appState = AppState::NORMAL;
uint32_t thankYouStartedAtMs = 0;
bool wasTouching = false;

String barcodeBuffer;
String rfidBuffer;
String debugBuffer;

/**
 * 商品名候補数を返します。
 * ※末尾のnullptrは件数に含めません。
 */
size_t getProductNameCount() {
  size_t count = 0;

  while (PRODUCT_NAMES[count] != nullptr) {
    ++count;
  }

  return count;
}

/**
 * 指定した添字の商品名候補を返します。
 * ※添字が候補数を超える場合は剰余で丸めます。
 */
const char* getProductName(const size_t index) {
  const size_t nameCount = getProductNameCount();
  if (nameCount == 0) {
    return "しょうひん";
  }

  return PRODUCT_NAMES[index % nameCount];
}

/**
 * FNV-1a 32bitハッシュ値を返します。
 */
uint32_t fnv1a32(const String& input) {
  uint32_t hash = 2166136261UL;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(input.c_str());
  const size_t length = input.length();

  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }

  return hash;
}

/**
 * バーコード文字列から商品情報を決定します。
 * ※同じバーコード文字列では同じ商品情報を返します。
 */
Item resolveItemFromCode(const String& code) {
  const size_t nameCount = getProductNameCount();

  // 候補ゼロ時の退避動作
  if (nameCount == 0) {
    return Item{"しょうひん", PRICE_MIN};
  }

  // 同じ入力で同じ商品名を得る写像
  const uint32_t nameHash = fnv1a32(code + "|NAME|v1");
  const size_t nameIndex = nameHash % nameCount;

  // 同じ入力で同じ価格を得る写像
  const uint32_t priceHash = fnv1a32(code + "|PRICE|v1");
  const int step = static_cast<int>(priceHash % PRICE_LEVELS);
  const int price = PRICE_MIN + step * PRICE_STEP;

  return Item{String(getProductName(nameIndex)), price};
}

/**
 * カート内の合計金額を返します。
 */
int calculateTotalSum() {
  int total = 0;

  for (const Item& item : cart) {
    total += item.price;
  }

  return total;
}

/**
 * 音ステップ列を順番に再生します。
 */
void playToneSteps(const ToneStep* steps) {
  size_t index = 0;
  while (steps[index].durationMs != 0) {
    M5.Speaker.tone(steps[index].frequencyHz, steps[index].durationMs);
    if (steps[index].waitMs > 0) {
      delay(steps[index].waitMs);
    }
    ++index;
  }
}

/**
 * スキャン音を鳴らします。
 */
void playScanTone() {
  playToneSteps(SCAN_TONE_STEPS);
}

/**
 * 決済音を鳴らします。
 */
void playPaymentTone() {
  playToneSteps(PAYMENT_TONE_STEPS);
}

/**
 * 起動音を鳴らします。
 */
void playStartupTone() {
  playToneSteps(STARTUP_TONE_STEPS);
}

/**
 * UTF-8文字列の末尾1文字を除去した文字列を返します。
 */
String removeLastUtf8Character(const String& text) {
  if (text.isEmpty()) {
    return text;
  }

  int index = text.length() - 1;
  while (index > 0) {
    const uint8_t byte = static_cast<uint8_t>(text[index]);
    if ((byte & 0xC0) != 0x80) {
      break;
    }
    --index;
  }

  return text.substring(0, index);
}

/**
 * 表示幅に収まるように必要時のみ省略記号を付与します。
 */
String ellipsizeText(const String& text, const int maxWidth) {
  if (M5.Display.textWidth(text) <= maxWidth) {
    return text;
  }

  const String ellipsis = "...";
  const int ellipsisWidth = M5.Display.textWidth(ellipsis);
  String shortened = text;

  while (!shortened.isEmpty() && M5.Display.textWidth(shortened) + ellipsisWidth > maxWidth) {
    shortened = removeLastUtf8Character(shortened);
  }

  return shortened + ellipsis;
}

/**
 * 文字列を中央揃えで描画します。
 */
void drawCenteredText(const String& text, const int y) {
  const int x = (M5.Display.width() - M5.Display.textWidth(text)) / 2;
  M5.Display.setCursor(std::max(x, 0), y);
  M5.Display.print(text);
}

/**
 * 通常画面を描画します。
 */
void renderNormalScreen() {
  const int displayHeight = M5.Display.height();
  const int displayWidth = M5.Display.width();
  const int lineMaxWidth = displayWidth - 24;

  // 画面の固定要素を先に再描画
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setCursor(8, 8);
  M5.Display.print("おうちレジ");

  M5.Display.fillRoundRect(CLEAR_BUTTON_X, CLEAR_BUTTON_Y, CLEAR_BUTTON_W, CLEAR_BUTTON_H, 6, TFT_RED);
  M5.Display.setTextColor(TFT_WHITE, TFT_RED);
  M5.Display.setCursor(CLEAR_BUTTON_X + 10, CLEAR_BUTTON_Y + 12);
  M5.Display.print("CLEAR");

  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setCursor(8, 50);
  M5.Display.print("明細");

  // 最新側を優先して表示
  const int firstIndex = static_cast<int>(cart.size()) > ITEM_VISIBLE_ROWS
    ? static_cast<int>(cart.size()) - ITEM_VISIBLE_ROWS
    : 0;

  // 行幅超過時は末尾を省略
  int rowY = 80;
  for (int index = firstIndex; index < static_cast<int>(cart.size()); ++index) {
    const String line = cart[index].name + "  " + String(cart[index].price) + "円";
    const String displayLine = ellipsizeText(line, lineMaxWidth);
    M5.Display.setCursor(12, rowY);
    M5.Display.print(displayLine);
    rowY += 30;
  }

  const int totalCount = static_cast<int>(cart.size());
  const int totalSum = calculateTotalSum();

  M5.Display.setCursor(8, displayHeight - 54);
  M5.Display.print("点数: ");
  M5.Display.print(totalCount);

  M5.Display.setCursor(8, displayHeight - 26);
  M5.Display.print("合計: ");
  M5.Display.print(totalSum);
  M5.Display.print("円");
}

/**
 * 決済完了画面を描画します。
 */
void renderThankYouScreen() {
  const int centerY = M5.Display.height() / 2;

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  drawCenteredText("お買いあげ", centerY - 24);
  drawCenteredText("ありがとうございます", centerY + 8);
}

/**
 * カートを空にして通常画面を更新します。
 */
void clearCart() {
  cart.clear();
  renderNormalScreen();
}

/**
 * バーコード文字列を処理します。
 */
void handleBarcodeCode(const String& rawCode) {
  // 遷移中の入力は破棄
  if (appState != AppState::NORMAL) {
    return;
  }

  // ノイズ入力を除外
  String code = rawCode;
  code.trim();
  if (code.length() < 2) {
    return;
  }

  playScanTone();

  const Item item = resolveItemFromCode(code);
  cart.push_back(item);
  renderNormalScreen();
}

/**
 * RFID UID文字列を処理します。
 */
void handleRfidUid(const String& rawUid) {
  // 遷移中の入力は破棄
  if (appState != AppState::NORMAL) {
    return;
  }

  // ノイズ入力を除外
  String uid = rawUid;
  uid.trim();
  if (uid.length() < 2) {
    return;
  }

  // ありがとう表示を先に確定
  cart.clear();
  appState = AppState::THANK_YOU;
  thankYouStartedAtMs = millis();
  renderThankYouScreen();

  // 画面表示中に決済音を再生
  playPaymentTone();
}

/**
 * ストリームから改行区切りで1行読み取ります。
 */
bool readLine(Stream& stream, String& buffer, String& lineOut) {
  while (stream.available() > 0) {
    const char ch = static_cast<char>(stream.read());

    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      lineOut = buffer;
      buffer = "";
      return true;
    }

    if (ch >= 0x20 && ch <= 0x7E) {
      buffer += ch;
    }

    if (buffer.length() > 128) {
      buffer.remove(0, buffer.length() - 128);
    }
  }

  return false;
}

/**
 * UART経由のバーコード入力を処理します。
 */
void pollBarcodeSerial() {
  String line;
  while (readLine(barcodeSerial, barcodeBuffer, line)) {
    handleBarcodeCode(line);
  }
}

/**
 * UART経由のRFID入力を処理します。
 */
void pollRfidSerial() {
  String line;
  while (readLine(rfidSerial, rfidBuffer, line)) {
    handleRfidUid(line);
  }
}

/**
 * USBシリアルからのテスト入力を処理します。
 */
void pollDebugSerial() {
  String line;
  while (readLine(Serial, debugBuffer, line)) {
    line.trim();
    if (line.startsWith("BC:")) {
      handleBarcodeCode(line.substring(3));
      continue;
    }

    if (line.startsWith("RFID:")) {
      handleRfidUid(line.substring(5));
      continue;
    }
  }
}

/**
 * タッチ操作でCLEAR押下を検知します。
 */
void pollClearButton() {
  int32_t touchX = 0;
  int32_t touchY = 0;
  const bool isTouching = M5.Display.getTouch(&touchX, &touchY);

  if (appState == AppState::NORMAL && isTouching && !wasTouching) {
    const bool inClearButton = touchX >= CLEAR_BUTTON_X && touchX <= CLEAR_BUTTON_X + CLEAR_BUTTON_W
      && touchY >= CLEAR_BUTTON_Y && touchY <= CLEAR_BUTTON_Y + CLEAR_BUTTON_H;

    if (inClearButton) {
      clearCart();
    }
  }

  wasTouching = isTouching;
}

/**
 * ありがとう画面の終了タイマーを処理します。
 */
void updateThankYouState() {
  if (appState != AppState::THANK_YOU) {
    return;
  }

  if (millis() - thankYouStartedAtMs < THANK_YOU_DURATION_MS) {
    return;
  }

  appState = AppState::NORMAL;
  renderNormalScreen();
}

}  // namespace

/**
 * 初期化処理を行います。
 */
void setup() {
  auto config = M5.config();
  M5.begin(config);

  Serial.begin(115200);
  barcodeSerial.begin(BARCODE_BAUD, SERIAL_8N1, BARCODE_RX_PIN, BARCODE_TX_PIN);
  rfidSerial.begin(RFID_BAUD, SERIAL_8N1, RFID_RX_PIN, RFID_TX_PIN);

  M5.Speaker.setVolume(SPEAKER_VOLUME);
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::lgfxJapanGothic_16);
  M5.Display.setTextSize(1);

  playStartupTone();
  renderNormalScreen();
}

/**
 * メインループ処理を行います。
 */
void loop() {
  M5.update();

  // 毎フレーム入力を先に取り込む
  pollClearButton();
  pollDebugSerial();
  pollBarcodeSerial();
  pollRfidSerial();

  // タイマー満了で通常状態へ復帰
  updateThankYouState();
}
