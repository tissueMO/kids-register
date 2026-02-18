#include <M5Unified.h>
#include <MFRC522_I2C.h>
#include <Wire.h>

#include <algorithm>
#include <vector>

#include "register-config.h"

namespace {

// バーコードスキャナUART設定
constexpr long BARCODE_BAUD_CANDIDATES[] = {9600, 115200, 19200, 38400, 57600};
constexpr bool BARCODE_START_WITH_SWAPPED_LINES = false;
constexpr uint32_t BARCODE_FRAME_GAP_MS = 300;
constexpr int BARCODE_SHORT_FRAME_RETRY_THRESHOLD = 5;
constexpr uint32_t BARCODE_PROBE_INTERVAL_MS = 4000;

// RFIDリーダI2C設定
constexpr uint8_t RFID_I2C_ADDRESS = 0x28;
constexpr uint32_t RFID_I2C_CLOCK = 100000;
constexpr int RFID_RESET_DUMMY_PIN = 8;
constexpr uint32_t DEBUG_FRAME_GAP_MS = 0;

// デバッグ設定
constexpr bool ENABLE_SERIAL_DEBUG = true;

// 画面遷移タイミング
constexpr uint32_t THANK_YOU_DURATION_MS = 3000;

// 会計ロジック設定
constexpr int ITEM_VISIBLE_ROWS = 3;
constexpr int PRICE_MIN = 50;
constexpr int PRICE_STEP = 10;
constexpr int PRICE_LEVELS = 46;

// 画面レイアウト設定
constexpr int CLEAR_BUTTON_MARGIN_RIGHT = 8;
constexpr int CLEAR_BUTTON_MARGIN_BOTTOM = 8;
constexpr int CLEAR_BUTTON_W = 84;
constexpr int CLEAR_BUTTON_H = 34;
constexpr int CLEAR_BUTTON_HIT_INSET = 2;
constexpr int CAPTION_Y = 6;
constexpr int LIST_START_Y = 57;
constexpr int ITEM_ROW_HEIGHT = 36;
constexpr int ITEM_TEXT_OFFSET_Y = 3;
constexpr int ITEM_RULE_OFFSET_Y = 30;
constexpr int SUMMARY_MARGIN_BOTTOM = 4;
constexpr size_t FRAME_BUFFER_MAX_LENGTH = 128;
constexpr size_t BARCODE_SERIAL_CONFIG_VARIANTS = 2;
constexpr int MIN_VALID_INPUT_LENGTH = 2;
constexpr long USB_SERIAL_BAUD = 115200;

enum class AppState {
  NORMAL,
  THANK_YOU,
};

struct Item {
  String name;
  int price;
};

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

HardwareSerial barcodeSerial(1);
MFRC522_I2C rfidReader(RFID_I2C_ADDRESS, RFID_RESET_DUMMY_PIN, &Wire);

std::vector<Item> cart;
AppState appState = AppState::NORMAL;
uint32_t thankYouStartedAtMs = 0;
bool wasTouching = false;

String barcodeBuffer;
String debugBuffer;
uint32_t barcodeLastByteAtMs = 0;
uint32_t debugLastByteAtMs = 0;
uint32_t barcodeLastProbeAtMs = 0;
int barcodePortcRxdPin = -1;
int barcodePortcTxdPin = -1;
int barcodeRxPin = -1;
int barcodeTxPin = -1;
size_t barcodeBaudIndex = 0;
size_t barcodeConfigIndex = 0;
int barcodeShortFrameCount = 0;
bool barcodeUartLocked = false;

constexpr const lgfx::U8g2font* BODY_FONT = &fonts::lgfxJapanGothic_24;
constexpr const lgfx::U8g2font* SUMMARY_FONT = &fonts::lgfxJapanGothic_32;
constexpr const lgfx::U8g2font* BUTTON_FONT = &fonts::lgfxJapanGothic_16;

/**
 * デバッグログをUSBシリアルへ出力します。
 */
void logDebug(const String& message) {
  if (!ENABLE_SERIAL_DEBUG) {
    return;
  }

  Serial.println(message);
}

/**
 * RFIDリーダのI2C接続状態をログ出力します。
 */
void logRfidI2cStatus() {
  Wire.beginTransmission(RFID_I2C_ADDRESS);
  const uint8_t errorCode = Wire.endTransmission();

  if (errorCode == 0) {
    logDebug("[RFID] I2C address 0x28 detected");
    return;
  }

  logDebug("[RFID] I2C address 0x28 not found, error=" + String(errorCode));
}

/**
 * RFIDリーダのバージョンレジスタ値をログ出力します。
 */
void logRfidVersion() {
  const uint8_t version = rfidReader.PCD_ReadRegister(rfidReader.VersionReg);
  String text = "[RFID] version=0x";
  if (version < 0x10) {
    text += "0";
  }
  text += String(version, HEX);
  text.toUpperCase();
  logDebug(text);
}

/**
 * バーコードボーレート候補数を返します。
 */
size_t getBarcodeBaudCount() {
  return sizeof(BARCODE_BAUD_CANDIDATES) / sizeof(BARCODE_BAUD_CANDIDATES[0]);
}

/**
 * 現在の候補インデックスのボーレートを返します。
 */
long getCurrentBarcodeBaud() {
  return BARCODE_BAUD_CANDIDATES[barcodeBaudIndex];
}

/**
 * 現在設定が配線入れ替え状態かを返します。
 */
bool isBarcodeSwapped() {
  return barcodeRxPin == barcodePortcTxdPin && barcodeTxPin == barcodePortcRxdPin;
}

/**
 * 指定した設定インデックスをUART設定へ反映します。
 */
void applyBarcodeSerialConfig(const size_t configIndex) {
  const size_t baudCount = getBarcodeBaudCount();
  const bool swapLines = configIndex >= baudCount;

  barcodeConfigIndex = configIndex;
  barcodeBaudIndex = configIndex % baudCount;
  barcodeRxPin = swapLines ? barcodePortcTxdPin : barcodePortcRxdPin;
  barcodeTxPin = swapLines ? barcodePortcRxdPin : barcodePortcTxdPin;
}

/**
 * バーコードUARTを現在設定で初期化します。
 */
void beginBarcodeSerial(const bool shouldLog) {
  barcodeSerial.begin(getCurrentBarcodeBaud(), SERIAL_8N1, barcodeRxPin, barcodeTxPin);

  if (shouldLog) {
    logDebug(
      "[BC] serial begin RX=" + String(barcodeRxPin)
      + " TX=" + String(barcodeTxPin)
      + " BAUD=" + String(getCurrentBarcodeBaud())
    );
  }
}

/**
 * 現在設定でバーコードUARTを再初期化します。
 */
void reopenBarcodeSerial() {
  barcodeSerial.end();
  beginBarcodeSerial(false);
}

/**
 * バーコードUARTを次の候補ボーレートへ切り替えます。
 */
void rotateBarcodeBaud() {
  const size_t baudCount = getBarcodeBaudCount();
  const size_t swapBaseIndex = isBarcodeSwapped() ? baudCount : 0;
  const size_t nextConfigIndex = swapBaseIndex + (barcodeBaudIndex + 1) % baudCount;

  applyBarcodeSerialConfig(nextConfigIndex);
  reopenBarcodeSerial();
}

/**
 * バーコードUARTを次の配線/ボーレート候補へ切り替えます。
 */
void rotateBarcodeSerialConfig() {
  const size_t totalCount = getBarcodeBaudCount() * BARCODE_SERIAL_CONFIG_VARIANTS;
  const size_t nextConfigIndex = (barcodeConfigIndex + 1) % totalCount;

  applyBarcodeSerialConfig(nextConfigIndex);
  reopenBarcodeSerial();
}

/**
 * 無受信時の探索として配線/ボーレート候補を巡回します。
 */
void probeBarcodeSerialWhenSilent() {
  if (barcodeUartLocked) {
    return;
  }

  if (millis() - barcodeLastProbeAtMs < BARCODE_PROBE_INTERVAL_MS) {
    return;
  }

  barcodeLastProbeAtMs = millis();
  rotateBarcodeSerialConfig();
}

/**
 * CLEARボタンの表示領域を返します。
 */
Rect getClearButtonRect() {
  const int x = M5.Display.width() - CLEAR_BUTTON_W - CLEAR_BUTTON_MARGIN_RIGHT;
  const int y = M5.Display.height() - CLEAR_BUTTON_H - CLEAR_BUTTON_MARGIN_BOTTOM;
  return Rect{x, y, CLEAR_BUTTON_W, CLEAR_BUTTON_H};
}

/**
 * CLEARボタンのタッチ判定領域を返します。
 */
Rect getClearButtonHitRect() {
  const Rect buttonRect = getClearButtonRect();
  const int x = buttonRect.x + CLEAR_BUTTON_HIT_INSET;
  const int y = buttonRect.y + CLEAR_BUTTON_HIT_INSET;
  const int w = std::max(buttonRect.w - CLEAR_BUTTON_HIT_INSET * 2, 1);
  const int h = std::max(buttonRect.h - CLEAR_BUTTON_HIT_INSET * 2, 1);
  return Rect{x, y, w, h};
}

/**
 * 指定座標が矩形内かを返します。
 */
bool isPointInsideRect(const int x, const int y, const Rect& rect) {
  return x >= rect.x && x < rect.x + rect.w
    && y >= rect.y && y < rect.y + rect.h;
}

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
 * フレームバッファを上限長まで切り詰めます。
 */
void trimFrameBuffer(String& buffer) {
  if (buffer.length() <= FRAME_BUFFER_MAX_LENGTH) {
    return;
  }

  buffer.remove(0, buffer.length() - FRAME_BUFFER_MAX_LENGTH);
}

/**
 * 入力文字列を整形し、有効な長さかを返します。
 */
bool tryNormalizeInput(const String& rawInput, String& normalizedInput) {
  normalizedInput = rawInput;
  normalizedInput.trim();
  return normalizedInput.length() >= MIN_VALID_INPUT_LENGTH;
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
 * 指定矩形の中央に文字列を描画します。
 */
void drawCenteredTextInRect(const String& text, const Rect& rect) {
  const int x = rect.x + (rect.w - M5.Display.textWidth(text)) / 2;
  const int y = rect.y + (rect.h - M5.Display.fontHeight()) / 2;
  M5.Display.setCursor(std::max(x, 0), std::max(y, 0));
  M5.Display.print(text);
}

/**
 * CLEARボタンを描画します。
 */
void drawClearButton(const Rect& clearButtonRect) {
  M5.Display.fillRoundRect(
    clearButtonRect.x,
    clearButtonRect.y,
    clearButtonRect.w,
    clearButtonRect.h,
    6,
    TFT_RED
  );

  M5.Display.setFont(BUTTON_FONT);
  M5.Display.setTextColor(TFT_WHITE, TFT_RED);
  drawCenteredTextInRect("CLEAR", clearButtonRect);
  M5.Display.setFont(BODY_FONT);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
}

/**
 * 明細の罫線を描画します。
 */
void drawItemRules(const int displayWidth) {
  for (int rowIndex = 0; rowIndex < ITEM_VISIBLE_ROWS; ++rowIndex) {
    const int ruleY = LIST_START_Y + rowIndex * ITEM_ROW_HEIGHT + ITEM_RULE_OFFSET_Y;
    M5.Display.drawFastHLine(8, ruleY, displayWidth - 16, TFT_DARKGREY);
  }
}

/**
 * 明細の表示対象を最新件数に切り詰めます。
 */
void trimCartForDisplay() {
  while (static_cast<int>(cart.size()) > ITEM_VISIBLE_ROWS) {
    cart.erase(cart.begin());
  }
}

/**
 * 明細一覧を描画します。
 */
void drawCartItems(const int displayWidth) {
  const int lastIndex = static_cast<int>(cart.size()) - 1;
  const int firstIndex = std::max(lastIndex - ITEM_VISIBLE_ROWS + 1, 0);

  int rowY = LIST_START_Y;
  for (int index = lastIndex; index >= firstIndex; --index) {
    const String priceText = "￥" + String(cart[index].price);
    const int priceX = std::max(displayWidth - 12 - M5.Display.textWidth(priceText), 12);
    const int nameMaxWidth = std::max(priceX - 24, 0);
    const String nameText = ellipsizeText(cart[index].name, nameMaxWidth);

    M5.Display.setCursor(12, rowY + ITEM_TEXT_OFFSET_Y);
    M5.Display.print(nameText);
    M5.Display.setCursor(priceX, rowY + ITEM_TEXT_OFFSET_Y);
    M5.Display.print(priceText);
    rowY += ITEM_ROW_HEIGHT;
  }
}

/**
 * 合計金額表示を描画します。
 */
void drawTotalSummary(const int displayHeight) {
  const String labelText = "計";
  const String amountText = "￥" + String(calculateTotalSum());

  M5.Display.setFont(SUMMARY_FONT);
  const int amountY = std::max(displayHeight - M5.Display.fontHeight() - SUMMARY_MARGIN_BOTTOM, 0);
  const int amountHeight = M5.Display.fontHeight();

  M5.Display.setFont(BODY_FONT);
  const int labelHeight = M5.Display.fontHeight();
  const int labelY = std::max(amountY + std::max(amountHeight - labelHeight, 0) - 5, 0);
  M5.Display.setCursor(8, labelY);
  M5.Display.print(labelText);
  const int amountX = 8 + M5.Display.textWidth(labelText) + 8;

  M5.Display.setFont(SUMMARY_FONT);
  M5.Display.setCursor(amountX, amountY);
  M5.Display.print(amountText);
}

/**
 * 通常画面を描画します。
 */
void renderNormalScreen() {
  M5.Display.setFont(BODY_FONT);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

  const int displayWidth = M5.Display.width();
  const int displayHeight = M5.Display.height();
  const Rect clearButtonRect = getClearButtonRect();

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setCursor(8, CAPTION_Y);
  M5.Display.print("おうちレジ");

  drawClearButton(clearButtonRect);
  drawItemRules(displayWidth);
  drawCartItems(displayWidth);
  drawTotalSummary(displayHeight);
}

/**
 * 決済完了画面を描画します。
 */
void renderThankYouScreen() {
  const int centerY = M5.Display.height() / 2;

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setFont(BODY_FONT);
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
  if (appState != AppState::NORMAL) {
    return;
  }

  String code;
  if (!tryNormalizeInput(rawCode, code)) {
    ++barcodeShortFrameCount;
    if (barcodeShortFrameCount >= BARCODE_SHORT_FRAME_RETRY_THRESHOLD) {
      barcodeShortFrameCount = 0;
      rotateBarcodeBaud();
    }
    return;
  }

  barcodeShortFrameCount = 0;
  logDebug("[BC] code=" + code);
  playScanTone();

  const Item item = resolveItemFromCode(code);
  cart.push_back(item);
  trimCartForDisplay();
  renderNormalScreen();
}

/**
 * RFID UID文字列を処理します。
 */
void handleRfidUid(const String& rawUid) {
  if (appState != AppState::NORMAL) {
    return;
  }

  String uid;
  if (!tryNormalizeInput(rawUid, uid)) {
    return;
  }

  logDebug("[RFID] uid=" + uid);

  cart.clear();
  appState = AppState::THANK_YOU;
  thankYouStartedAtMs = millis();
  renderThankYouScreen();

  playPaymentTone();
}

/**
 * RFIDカードのUIDを16進文字列へ変換します。
 */
String getRfidUidHex() {
  String uid;

  for (byte index = 0; index < rfidReader.uid.size; ++index) {
    const uint8_t value = rfidReader.uid.uidByte[index];
    if (value < 0x10) {
      uid += "0";
    }
    uid += String(value, HEX);
  }

  uid.toUpperCase();
  return uid;
}

/**
 * ストリームから1フレームを読み取ります。
 * ※改行終端または無通信時間経過で1フレームを確定します。
 */
bool readFrame(
  Stream& stream,
  String& buffer,
  String& lineOut,
  uint32_t& lastByteAtMs,
  const uint32_t frameGapMs
) {
  while (stream.available() > 0) {
    const char ch = static_cast<char>(stream.read());
    lastByteAtMs = millis();

    if (ch == '\r' || ch == '\n') {
      if (buffer.isEmpty()) {
        continue;
      }
      lineOut = buffer;
      buffer = "";
      return true;
    }

    if (ch >= 0x20 && ch <= 0x7E) {
      buffer += ch;
    }

    trimFrameBuffer(buffer);
  }

  if (frameGapMs > 0 && !buffer.isEmpty() && millis() - lastByteAtMs >= frameGapMs) {
    lineOut = buffer;
    buffer = "";
    return true;
  }

  return false;
}

/**
 * UART経由のバーコード入力を処理します。
 */
void pollBarcodeSerial() {
  if (barcodeSerial.available() > 0 && !barcodeUartLocked) {
    barcodeUartLocked = true;
  }

  String line;
  while (readFrame(barcodeSerial, barcodeBuffer, line, barcodeLastByteAtMs, BARCODE_FRAME_GAP_MS)) {
    handleBarcodeCode(line);
  }

  probeBarcodeSerialWhenSilent();
}

/**
 * RFIDカード入力を処理します。
 */
void pollRfidCard() {
  if (!rfidReader.PICC_IsNewCardPresent()) {
    return;
  }

  if (!rfidReader.PICC_ReadCardSerial()) {
    return;
  }

  const String uid = getRfidUidHex();
  handleRfidUid(uid);
  rfidReader.PICC_HaltA();
  rfidReader.PCD_StopCrypto1();
}

/**
 * デバッグ入力1行を処理します。
 */
void handleDebugLine(const String& rawLine) {
  String line = rawLine;
  line.trim();

  if (line.startsWith("BC:")) {
    handleBarcodeCode(line.substring(3));
    return;
  }

  if (line.startsWith("RFID:")) {
    handleRfidUid(line.substring(5));
    return;
  }
}

/**
 * USBシリアルからのテスト入力を処理します。
 */
void pollDebugSerial() {
  String line;
  while (readFrame(Serial, debugBuffer, line, debugLastByteAtMs, DEBUG_FRAME_GAP_MS)) {
    handleDebugLine(line);
  }
}

/**
 * タッチ操作でCLEAR押下を検知します。
 */
void pollClearButton() {
  int32_t touchX = 0;
  int32_t touchY = 0;
  const bool isTouching = M5.Display.getTouch(&touchX, &touchY);
  const Rect clearButtonHitRect = getClearButtonHitRect();

  if (appState == AppState::NORMAL && isTouching && !wasTouching) {
    const bool inClearButton = isPointInsideRect(touchX, touchY, clearButtonHitRect);

    if (inClearButton) {
      playScanTone();
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

/**
 * 周辺機器ピン情報を取得します。
 */
void resolvePeripheralPins(int& rfidSdaPin, int& rfidSclPin) {
  barcodePortcRxdPin = M5.getPin(m5::pin_name_t::port_c_rxd);
  barcodePortcTxdPin = M5.getPin(m5::pin_name_t::port_c_txd);
  rfidSclPin = M5.getPin(m5::pin_name_t::port_a_scl);
  rfidSdaPin = M5.getPin(m5::pin_name_t::port_a_sda);
}

/**
 * バーコードUARTを初期化します。
 */
void initializeBarcodeSerial() {
  const size_t initialConfigIndex = BARCODE_START_WITH_SWAPPED_LINES ? getBarcodeBaudCount() : 0;
  applyBarcodeSerialConfig(initialConfigIndex);
  beginBarcodeSerial(true);
  barcodeLastProbeAtMs = millis();
}

/**
 * RFIDリーダを初期化します。
 */
void initializeRfidReader(const int rfidSdaPin, const int rfidSclPin) {
  Wire.begin(rfidSdaPin, rfidSclPin, RFID_I2C_CLOCK);
  logRfidI2cStatus();
  rfidReader.PCD_Init();
  logRfidVersion();
}

/**
 * 起動時ログを出力します。
 */
void logBootConfiguration(const int rfidSdaPin, const int rfidSclPin) {
  logDebug("[BOOT] portc RXD pin=" + String(barcodePortcRxdPin) + " TXD pin=" + String(barcodePortcTxdPin));
  logDebug("[BOOT] barcode swap start=" + String(BARCODE_START_WITH_SWAPPED_LINES ? 1 : 0));
  logDebug("[BOOT] rfid I2C SDA=" + String(rfidSdaPin) + " SCL=" + String(rfidSclPin));
  logDebug("[BOOT] rfid reset pin=" + String(RFID_RESET_DUMMY_PIN));
}

/**
 * 画面とスピーカーを初期化します。
 */
void initializeUi() {
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  M5.Display.setRotation(1);
  M5.Display.setFont(BODY_FONT);
  M5.Display.setTextSize(1);
}

}  // namespace

/**
 * 初期化処理を行います。
 */
void setup() {
  auto config = M5.config();
  M5.begin(config);

  int rfidSdaPin = -1;
  int rfidSclPin = -1;
  resolvePeripheralPins(rfidSdaPin, rfidSclPin);

  Serial.begin(USB_SERIAL_BAUD);
  initializeBarcodeSerial();
  initializeRfidReader(rfidSdaPin, rfidSclPin);
  logBootConfiguration(rfidSdaPin, rfidSclPin);
  initializeUi();

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
  pollRfidCard();

  // タイマー満了で通常状態へ復帰
  updateThankYouState();
}
