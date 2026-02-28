#include "register-mode.h"

#include <Wire.h>

#include <algorithm>

#include "register-config.h"

namespace {

// バーコードスキャナUART設定
constexpr long BARCODE_UART_BAUD = 115200;
constexpr uint32_t BARCODE_FRAME_GAP_MS = 300;
constexpr uint32_t BARCODE_COMMAND_GUARD_MS = 120;
constexpr uint32_t BARCODE_BOOT_STABILIZE_MS = 1500;
constexpr uint8_t BARCODE_CMD_TRIGGER_MODE_BUTTON[] = {0x21, 0x61, 0x41, 0x00};
constexpr uint8_t BARCODE_CMD_FILL_LIGHT_OFF[] = {0x21, 0x62, 0x41, 0x00};
constexpr uint8_t BARCODE_CMD_AIM_LIGHT_ON[] = {0x21, 0x62, 0x42, 0x02};

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
constexpr int MIN_VALID_INPUT_LENGTH = 2;
constexpr int BARCODE_MIN_VALID_LENGTH = 6;

constexpr const lgfx::U8g2font* BODY_FONT = &fonts::lgfxJapanGothic_24;
constexpr const lgfx::U8g2font* SUMMARY_FONT = &fonts::lgfxJapanGothic_32;
constexpr const lgfx::U8g2font* BUTTON_FONT = &fonts::lgfxJapanGothic_16;

}  // namespace

/**
 * おうちレジモードを初期化します。
 */
RegisterMode::RegisterMode()
: barcodeSerial_(1),
  rfidReader_(RFID_I2C_ADDRESS, RFID_RESET_DUMMY_PIN, &Wire),
  appState_(AppState::NORMAL),
  thankYouStartedAtMs_(0),
  barcodeLastByteAtMs_(0),
  debugLastByteAtMs_(0),
  barcodeCommandGuardUntilMs_(0),
  barcodeInputReadyAtMs_(0),
  barcodeRxdPin_(-1),
  barcodeTxdPin_(-1),
  isRfidReady_(false) {
}

/**
 * 周辺機器を初期化します。
 */
void RegisterMode::initialize(const Pins& pins) {
  barcodeRxdPin_ = pins.barcodeRxdPin;
  barcodeTxdPin_ = pins.barcodeTxdPin;

  beginBarcodeSerial(true);
  applyBarcodeScannerSettings();
  barcodeInputReadyAtMs_ = millis() + BARCODE_BOOT_STABILIZE_MS;
  clearBarcodeSerialInput();

  Wire.begin(pins.rfidSdaPin, pins.rfidSclPin, RFID_I2C_CLOCK);
  if (checkRfidI2cStatus()) {
    rfidReader_.PCD_Init();
    logRfidVersion();
    isRfidReady_ = true;
  } else {
    isRfidReady_ = false;
  }

  logDebug("[BOOT] portc RXD pin=" + String(barcodeRxdPin_) + " TXD pin=" + String(barcodeTxdPin_));
  logDebug("[BOOT] barcode BAUD=" + String(BARCODE_UART_BAUD));
  logDebug("[BOOT] barcode trigger=unit button");
  logDebug("[BOOT] rfid I2C SDA=" + String(pins.rfidSdaPin) + " SCL=" + String(pins.rfidSclPin));
  logDebug("[BOOT] rfid reset pin=" + String(RFID_RESET_DUMMY_PIN));
}

/**
 * モード遷移時に通常画面を表示します。
 */
void RegisterMode::enter() {
  appState_ = AppState::NORMAL;
  renderNormalScreen();
}

/**
 * タップ入力を処理します。
 */
void RegisterMode::onTouch(const int touchX, const int touchY) {
  if (appState_ != AppState::NORMAL) {
    return;
  }

  const Rect clearButtonHitRect = getClearButtonHitRect();
  if (!isPointInsideRect(touchX, touchY, clearButtonHitRect)) {
    return;
  }

  playScanTone();
  clearCart();
}

/**
 * モードの定期更新を処理します。
 */
void RegisterMode::update() {
  pollDebugSerial();
  pollBarcodeSerial();
  pollRfidCard();
  updateThankYouState();
}

/**
 * モード選択時の起動音を鳴らします。
 */
void RegisterMode::playStartupTone() const {
  playToneSteps(STARTUP_TONE_STEPS);
}

/**
 * デバッグログをUSBシリアルへ出力します。
 */
void RegisterMode::logDebug(const String& message) const {
  if (!ENABLE_SERIAL_DEBUG) {
    return;
  }

  Serial.println(message);
}

/**
 * スキャン音を鳴らします。
 */
void RegisterMode::playScanTone() const {
  playToneSteps(SCAN_TONE_STEPS);
}

/**
 * 決済音を鳴らします。
 */
void RegisterMode::playPaymentTone() const {
  playToneSteps(PAYMENT_TONE_STEPS);
}

/**
 * バーコードUARTを初期化します。
 */
void RegisterMode::beginBarcodeSerial(const bool shouldLog) {
  barcodeSerial_.begin(BARCODE_UART_BAUD, SERIAL_8N1, barcodeRxdPin_, barcodeTxdPin_);

  if (shouldLog) {
    logDebug(
      "[BC] serial begin RX=" + String(barcodeRxdPin_)
      + " TX=" + String(barcodeTxdPin_)
      + " BAUD=" + String(BARCODE_UART_BAUD)
    );
  }
}

/**
 * バーコードUART受信バッファを破棄します。
 */
void RegisterMode::clearBarcodeSerialInput() {
  while (barcodeSerial_.available() > 0) {
    static_cast<void>(barcodeSerial_.read());
  }

  barcodeBuffer_ = "";
  barcodeLastByteAtMs_ = 0;
}

/**
 * バーコードスキャナへコマンドを送信します。
 */
void RegisterMode::sendBarcodeCommand(const uint8_t* command, const size_t length) {
  clearBarcodeSerialInput();
  barcodeSerial_.write(command, length);
  barcodeSerial_.flush();
  delay(BARCODE_COMMAND_GUARD_MS);
  clearBarcodeSerialInput();
  barcodeCommandGuardUntilMs_ = millis() + BARCODE_COMMAND_GUARD_MS;
}

/**
 * バーコードスキャナの起動時設定を適用します。
 */
void RegisterMode::applyBarcodeScannerSettings() {
  sendBarcodeCommand(BARCODE_CMD_TRIGGER_MODE_BUTTON, sizeof(BARCODE_CMD_TRIGGER_MODE_BUTTON));
  sendBarcodeCommand(BARCODE_CMD_FILL_LIGHT_OFF, sizeof(BARCODE_CMD_FILL_LIGHT_OFF));
  sendBarcodeCommand(BARCODE_CMD_AIM_LIGHT_ON, sizeof(BARCODE_CMD_AIM_LIGHT_ON));
}

/**
 * RFIDリーダのI2C接続状態をログ出力します。
 */
bool RegisterMode::checkRfidI2cStatus() const {
  Wire.beginTransmission(RFID_I2C_ADDRESS);
  const uint8_t errorCode = Wire.endTransmission();

  if (errorCode == 0) {
    logDebug("[RFID] I2C address 0x28 detected");
    return true;
  }

  logDebug("[RFID] I2C address 0x28 not found, error=" + String(errorCode));
  return false;
}

/**
 * RFIDリーダのバージョンレジスタ値をログ出力します。
 */
void RegisterMode::logRfidVersion() {
  const uint8_t version = rfidReader_.PCD_ReadRegister(rfidReader_.VersionReg);
  String text = "[RFID] version=0x";
  if (version < 0x10) {
    text += "0";
  }
  text += String(version, HEX);
  text.toUpperCase();
  logDebug(text);
}

/**
 * CLEARボタンの表示領域を返します。
 */
RegisterMode::Rect RegisterMode::getClearButtonRect() const {
  const int x = M5.Display.width() - CLEAR_BUTTON_W - CLEAR_BUTTON_MARGIN_RIGHT;
  const int y = M5.Display.height() - CLEAR_BUTTON_H - CLEAR_BUTTON_MARGIN_BOTTOM;
  return Rect{x, y, CLEAR_BUTTON_W, CLEAR_BUTTON_H};
}

/**
 * CLEARボタンのタッチ判定領域を返します。
 */
RegisterMode::Rect RegisterMode::getClearButtonHitRect() const {
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
bool RegisterMode::isPointInsideRect(const int x, const int y, const Rect& rect) const {
  return x >= rect.x && x < rect.x + rect.w
    && y >= rect.y && y < rect.y + rect.h;
}

/**
 * 商品名候補数を返します。
 */
size_t RegisterMode::getProductNameCount() const {
  size_t count = 0;

  while (PRODUCT_NAMES[count] != nullptr) {
    ++count;
  }

  return count;
}

/**
 * 指定した添字の商品名候補を返します。
 */
const char* RegisterMode::getProductName(const size_t index) const {
  const size_t nameCount = getProductNameCount();
  if (nameCount == 0) {
    return "しょうひん";
  }

  return PRODUCT_NAMES[index % nameCount];
}

/**
 * FNV-1a 32bitハッシュ値を返します。
 */
uint32_t RegisterMode::fnv1a32(const String& input) const {
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
 */
RegisterMode::Item RegisterMode::resolveItemFromCode(const String& code) const {
  const size_t nameCount = getProductNameCount();

  if (nameCount == 0) {
    return Item{"しょうひん", PRICE_MIN};
  }

  const uint32_t nameHash = fnv1a32(code + "|NAME|v1");
  const size_t nameIndex = nameHash % nameCount;

  const uint32_t priceHash = fnv1a32(code + "|PRICE|v1");
  const int step = static_cast<int>(priceHash % PRICE_LEVELS);
  const int price = PRICE_MIN + step * PRICE_STEP;

  return Item{String(getProductName(nameIndex)), price};
}

/**
 * カート内の合計金額を返します。
 */
int RegisterMode::calculateTotalSum() const {
  int total = 0;

  for (const Item& item : cart_) {
    total += item.price;
  }

  return total;
}

/**
 * フレームバッファを上限長まで切り詰めます。
 */
void RegisterMode::trimFrameBuffer(String& buffer) const {
  if (buffer.length() <= FRAME_BUFFER_MAX_LENGTH) {
    return;
  }

  buffer.remove(0, buffer.length() - FRAME_BUFFER_MAX_LENGTH);
}

/**
 * 入力文字列を整形し、有効な長さかを返します。
 */
bool RegisterMode::tryNormalizeInput(const String& rawInput, String& normalizedInput) const {
  normalizedInput = rawInput;
  normalizedInput.trim();
  return normalizedInput.length() >= 2;
}

/**
 * バーコードスキャナの制御応答フレームかを判定します。
 */
bool RegisterMode::isBarcodeControlResponse(const String& frame) const {
  if (frame == "3u") {
    return true;
  }

  if (frame.length() <= 4 && (frame.startsWith("\"") || frame.startsWith("$"))) {
    return true;
  }

  return false;
}

/**
 * UTF-8文字列の末尾1文字を除去した文字列を返します。
 */
String RegisterMode::removeLastUtf8Character(const String& text) const {
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
String RegisterMode::ellipsizeText(const String& text, const int maxWidth) const {
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
void RegisterMode::drawCenteredText(const String& text, const int y) const {
  const int x = (M5.Display.width() - M5.Display.textWidth(text)) / 2;
  M5.Display.setCursor(std::max(x, 0), y);
  M5.Display.print(text);
}

/**
 * 指定矩形の中央に文字列を描画します。
 */
void RegisterMode::drawCenteredTextInRect(const String& text, const Rect& rect) const {
  const int x = rect.x + (rect.w - M5.Display.textWidth(text)) / 2;
  const int y = rect.y + (rect.h - M5.Display.fontHeight()) / 2;
  M5.Display.setCursor(std::max(x, 0), std::max(y, 0));
  M5.Display.print(text);
}

/**
 * CLEARボタンを描画します。
 */
void RegisterMode::drawClearButton(const Rect& clearButtonRect) const {
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
void RegisterMode::drawItemRules(const int displayWidth) const {
  for (int rowIndex = 0; rowIndex < ITEM_VISIBLE_ROWS; ++rowIndex) {
    const int ruleY = LIST_START_Y + rowIndex * ITEM_ROW_HEIGHT + ITEM_RULE_OFFSET_Y;
    M5.Display.drawFastHLine(8, ruleY, displayWidth - 16, TFT_DARKGREY);
  }
}

/**
 * 明細の表示対象を最新件数に切り詰めます。
 */
void RegisterMode::trimCartForDisplay() {
  while (static_cast<int>(cart_.size()) > ITEM_VISIBLE_ROWS) {
    cart_.erase(cart_.begin());
  }
}

/**
 * 明細一覧を描画します。
 */
void RegisterMode::drawCartItems(const int displayWidth) const {
  const int lastIndex = static_cast<int>(cart_.size()) - 1;
  const int firstIndex = std::max(lastIndex - ITEM_VISIBLE_ROWS + 1, 0);

  int rowY = LIST_START_Y;
  for (int index = lastIndex; index >= firstIndex; --index) {
    const String priceText = "￥" + String(cart_[index].price);
    const int priceX = std::max(displayWidth - 12 - M5.Display.textWidth(priceText), 12);
    const int nameMaxWidth = std::max(priceX - 24, 0);
    const String nameText = ellipsizeText(cart_[index].name, nameMaxWidth);

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
void RegisterMode::drawTotalSummary(const int displayHeight) const {
  const String labelText = "計";
  const String amountText = "￥" + String(calculateTotalSum());

  M5.Display.setFont(SUMMARY_FONT);
  const int amountY = std::max(displayHeight - M5.Display.fontHeight() - 4, 0);
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
void RegisterMode::renderNormalScreen() const {
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
void RegisterMode::renderThankYouScreen() const {
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
void RegisterMode::clearCart() {
  cart_.clear();
  renderNormalScreen();
}

/**
 * バーコード文字列を処理します。
 */
void RegisterMode::handleBarcodeCode(const String& rawCode) {
  if (appState_ != AppState::NORMAL) {
    return;
  }

  if (isBarcodeControlResponse(rawCode)) {
    return;
  }

  String code;
  if (!tryNormalizeInput(rawCode, code)) {
    return;
  }

  if (code.length() < BARCODE_MIN_VALID_LENGTH) {
    return;
  }

  logDebug("[BC] code=" + code);
  playScanTone();

  const Item item = resolveItemFromCode(code);
  cart_.push_back(item);
  trimCartForDisplay();
  renderNormalScreen();
}

/**
 * RFID UID文字列を処理します。
 */
void RegisterMode::handleRfidUid(const String& rawUid) {
  if (appState_ != AppState::NORMAL) {
    return;
  }

  String uid;
  if (!tryNormalizeInput(rawUid, uid)) {
    return;
  }

  logDebug("[RFID] uid=" + uid);

  cart_.clear();
  appState_ = AppState::THANK_YOU;
  thankYouStartedAtMs_ = millis();
  renderThankYouScreen();

  playPaymentTone();
}

/**
 * RFIDカードのUIDを16進文字列へ変換します。
 */
String RegisterMode::getRfidUidHex() const {
  String uid;

  for (byte index = 0; index < rfidReader_.uid.size; ++index) {
    const uint8_t value = rfidReader_.uid.uidByte[index];
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
 */
bool RegisterMode::readFrame(
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
void RegisterMode::pollBarcodeSerial() {
  if (millis() < barcodeInputReadyAtMs_) {
    clearBarcodeSerialInput();
    return;
  }

  if (millis() < barcodeCommandGuardUntilMs_) {
    clearBarcodeSerialInput();
    return;
  }

  String line;
  while (readFrame(barcodeSerial_, barcodeBuffer_, line, barcodeLastByteAtMs_, BARCODE_FRAME_GAP_MS)) {
    handleBarcodeCode(line);
  }
}

/**
 * RFIDカード入力を処理します。
 */
void RegisterMode::pollRfidCard() {
  if (!isRfidReady_) {
    return;
  }

  if (!rfidReader_.PICC_IsNewCardPresent()) {
    return;
  }

  if (!rfidReader_.PICC_ReadCardSerial()) {
    return;
  }

  const String uid = getRfidUidHex();
  handleRfidUid(uid);
  rfidReader_.PICC_HaltA();
  rfidReader_.PCD_StopCrypto1();
}

/**
 * デバッグ入力1行を処理します。
 */
void RegisterMode::handleDebugLine(const String& rawLine) {
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
void RegisterMode::pollDebugSerial() {
  String line;
  while (readFrame(Serial, debugBuffer_, line, debugLastByteAtMs_, DEBUG_FRAME_GAP_MS)) {
    handleDebugLine(line);
  }
}

/**
 * ありがとう画面の終了タイマーを処理します。
 */
void RegisterMode::updateThankYouState() {
  if (appState_ != AppState::THANK_YOU) {
    return;
  }

  if (millis() - thankYouStartedAtMs_ < THANK_YOU_DURATION_MS) {
    return;
  }

  appState_ = AppState::NORMAL;
  renderNormalScreen();
}
