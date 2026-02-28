#include <M5Unified.h>
#include <MFRC522_I2C.h>
#include <Wire.h>
#include <esp_camera.h>

#include <algorithm>
#include <vector>

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
constexpr int MODE_BUTTON_W = 146;
constexpr int MODE_BUTTON_H = 164;
constexpr int MODE_BUTTON_GAP = 16;
constexpr int MODE_BUTTON_TOP = 56;
constexpr int MODE_ICON_SIZE = 64;
constexpr int MODE_LABEL_GAP_Y = 14;
constexpr int MODE_TITLE_Y = 10;
constexpr int STILL_FRAME_THICKNESS = 5;
constexpr int STILL_FRAME_INNER_LINE_OFFSET = 8;
constexpr size_t FRAME_BUFFER_MAX_LENGTH = 128;
constexpr int MIN_VALID_INPUT_LENGTH = 2;
constexpr int BARCODE_MIN_VALID_LENGTH = 6;
constexpr long USB_SERIAL_BAUD = 115200;

enum class AppMode {
  SELECT,
  REGISTER,
  CAMERA,
};

enum class AppState {
  NORMAL,
  THANK_YOU,
};

enum class CameraViewState {
  LIVE,
  STILL,
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
AppMode appMode = AppMode::SELECT;
AppState appState = AppState::NORMAL;
CameraViewState cameraViewState = CameraViewState::LIVE;
uint32_t thankYouStartedAtMs = 0;
bool wasTouching = false;

String barcodeBuffer;
String debugBuffer;
uint32_t barcodeLastByteAtMs = 0;
uint32_t debugLastByteAtMs = 0;
int barcodePortcRxdPin = -1;
int barcodePortcTxdPin = -1;
uint32_t barcodeCommandGuardUntilMs = 0;
uint32_t barcodeInputReadyAtMs = 0;
bool isCameraInitialized = false;
bool isCameraReady = false;
bool isRfidReady = false;

camera_config_t cameraConfig = {};

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
bool checkRfidI2cStatus() {
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
 * バーコードUARTを初期化します。
 */
void beginBarcodeSerial(const bool shouldLog) {
  barcodeSerial.begin(BARCODE_UART_BAUD, SERIAL_8N1, barcodePortcRxdPin, barcodePortcTxdPin);

  if (shouldLog) {
    logDebug(
      "[BC] serial begin RX=" + String(barcodePortcRxdPin)
      + " TX=" + String(barcodePortcTxdPin)
      + " BAUD=" + String(BARCODE_UART_BAUD)
    );
  }
}

/**
 * バーコードUART受信バッファを破棄します。
 */
void clearBarcodeSerialInput() {
  while (barcodeSerial.available() > 0) {
    static_cast<void>(barcodeSerial.read());
  }

  barcodeBuffer = "";
  barcodeLastByteAtMs = 0;
}

/**
 * バーコードスキャナへコマンドを送信します。
 */
void sendBarcodeCommand(const uint8_t* command, const size_t length) {
  clearBarcodeSerialInput();
  barcodeSerial.write(command, length);
  barcodeSerial.flush();
  delay(BARCODE_COMMAND_GUARD_MS);
  clearBarcodeSerialInput();
  barcodeCommandGuardUntilMs = millis() + BARCODE_COMMAND_GUARD_MS;
}

/**
 * バーコードスキャナを本体ボタントリガーモードに設定します。
 */
void setBarcodeButtonTriggerMode() {
  sendBarcodeCommand(BARCODE_CMD_TRIGGER_MODE_BUTTON, sizeof(BARCODE_CMD_TRIGGER_MODE_BUTTON));
}

/**
 * バーコードの補光LEDを常時消灯設定にします。
 */
void setBarcodeFillLightOff() {
  sendBarcodeCommand(BARCODE_CMD_FILL_LIGHT_OFF, sizeof(BARCODE_CMD_FILL_LIGHT_OFF));
}

/**
 * バーコードの照準LEDを作動時連続点灯設定にします。
 */
void setBarcodeAimLightOn() {
  sendBarcodeCommand(BARCODE_CMD_AIM_LIGHT_ON, sizeof(BARCODE_CMD_AIM_LIGHT_ON));
}

/**
 * バーコードスキャナの起動時設定を適用します。
 */
void applyBarcodeScannerSettings() {
  setBarcodeButtonTriggerMode();
  setBarcodeFillLightOff();
  setBarcodeAimLightOn();
}

/**
 * カメラ設定を初期化します。
 */
void initializeCameraConfig(const bool useCompactProfile) {
  cameraConfig.pin_pwdn = -1;
  cameraConfig.pin_reset = -1;
  cameraConfig.pin_xclk = -1;
  cameraConfig.pin_sscb_sda = 12;
  cameraConfig.pin_sscb_scl = 11;
  cameraConfig.pin_d7 = 47;
  cameraConfig.pin_d6 = 48;
  cameraConfig.pin_d5 = 16;
  cameraConfig.pin_d4 = 15;
  cameraConfig.pin_d3 = 42;
  cameraConfig.pin_d2 = 41;
  cameraConfig.pin_d1 = 40;
  cameraConfig.pin_d0 = 39;
  cameraConfig.pin_vsync = 46;
  cameraConfig.pin_href = 38;
  cameraConfig.pin_pclk = 45;
  cameraConfig.xclk_freq_hz = 20000000;
  cameraConfig.ledc_timer = LEDC_TIMER_0;
  cameraConfig.ledc_channel = LEDC_CHANNEL_0;
  cameraConfig.pixel_format = PIXFORMAT_RGB565;
  cameraConfig.frame_size = FRAMESIZE_QVGA;
  cameraConfig.jpeg_quality = 0;
  cameraConfig.fb_count = useCompactProfile ? 1 : 2;
  cameraConfig.fb_location = useCompactProfile ? CAMERA_FB_IN_DRAM : CAMERA_FB_IN_PSRAM;
  cameraConfig.grab_mode = CAMERA_GRAB_LATEST;
  cameraConfig.sccb_i2c_port = -1;
}

/**
 * カメラモジュールを初期化し、利用可否を返します。
 */
bool initializeCameraModule() {
  if (isCameraReady) {
    return true;
  }

  if (isCameraInitialized) {
    esp_camera_deinit();
    isCameraInitialized = false;
  }

  initializeCameraConfig(false);
  M5.In_I2C.release();
  esp_err_t result = esp_camera_init(&cameraConfig);
  if (result == ESP_OK) {
    isCameraInitialized = true;
    isCameraReady = true;
    logDebug("[CAM] init ok profile=normal");
    return true;
  }

  logDebug("[CAM] init failed profile=normal err=" + String(static_cast<int>(result)));
  esp_camera_deinit();
  delay(20);

  initializeCameraConfig(true);
  result = esp_camera_init(&cameraConfig);
  if (result != ESP_OK) {
    logDebug("[CAM] init failed profile=compact err=" + String(static_cast<int>(result)));
    isCameraInitialized = false;
    isCameraReady = false;
    return false;
  }

  isCameraInitialized = true;
  isCameraReady = true;
  logDebug("[CAM] init ok profile=compact");
  return true;
}

/**
 * カメラフレームを1枚取得します。
 */
bool captureCameraFrame(camera_fb_t*& frame) {
  frame = esp_camera_fb_get();
  return frame != nullptr;
}

/**
 * 取得済みカメラフレームを解放します。
 */
void releaseCameraFrame(camera_fb_t*& frame) {
  if (frame == nullptr) {
    return;
  }

  esp_camera_fb_return(frame);
  frame = nullptr;
}

/**
 * おうちレジボタンの表示領域を返します。
 */
Rect getRegisterModeButtonRect() {
  const int totalWidth = MODE_BUTTON_W * 2 + MODE_BUTTON_GAP;
  const int startX = (M5.Display.width() - totalWidth) / 2;
  return Rect{startX, MODE_BUTTON_TOP, MODE_BUTTON_W, MODE_BUTTON_H};
}

/**
 * カメラボタンの表示領域を返します。
 */
Rect getCameraModeButtonRect() {
  const Rect registerButtonRect = getRegisterModeButtonRect();
  return Rect{
    registerButtonRect.x + registerButtonRect.w + MODE_BUTTON_GAP,
    registerButtonRect.y,
    registerButtonRect.w,
    registerButtonRect.h,
  };
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
 * シャッター音を鳴らします。
 */
void playShutterTone() {
  playToneSteps(SHUTTER_TONE_STEPS);
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
 * バーコードスキャナの制御応答フレームかを判定します。
 */
bool isBarcodeControlResponse(const String& frame) {
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
 * おうちレジのアイコンを描画します。
 */
void drawRegisterModeIcon(const Rect& iconRect) {
  const int roofTopY = iconRect.y + 4;
  const int roofBottomY = iconRect.y + 26;
  const int roofLeftX = iconRect.x + 6;
  const int roofRightX = iconRect.x + iconRect.w - 6;
  const int bodyX = iconRect.x + 12;
  const int bodyY = roofBottomY;
  const int bodyW = iconRect.w - 24;
  const int bodyH = iconRect.h - 30;

  M5.Display.fillTriangle(
    roofLeftX,
    roofBottomY,
    iconRect.x + iconRect.w / 2,
    roofTopY,
    roofRightX,
    roofBottomY,
    TFT_DARKGREEN
  );
  M5.Display.fillRect(bodyX, bodyY, bodyW, bodyH, TFT_DARKGREEN);
  M5.Display.fillRect(iconRect.x + iconRect.w / 2 - 8, bodyY + 14, 16, bodyH - 16, TFT_WHITE);
  M5.Display.fillRect(bodyX + 8, bodyY + 10, bodyW - 16, 12, TFT_WHITE);
}

/**
 * カメラのアイコンを描画します。
 */
void drawCameraModeIcon(const Rect& iconRect) {
  const int bodyX = iconRect.x + 6;
  const int bodyY = iconRect.y + 18;
  const int bodyW = iconRect.w - 12;
  const int bodyH = iconRect.h - 24;

  M5.Display.fillRoundRect(bodyX, bodyY, bodyW, bodyH, 10, TFT_NAVY);
  M5.Display.fillRoundRect(bodyX + 14, iconRect.y + 6, bodyW - 28, 16, 5, TFT_NAVY);
  M5.Display.fillCircle(iconRect.x + iconRect.w / 2, bodyY + bodyH / 2, 17, TFT_WHITE);
  M5.Display.fillCircle(iconRect.x + iconRect.w / 2, bodyY + bodyH / 2, 8, TFT_NAVY);
}

/**
 * モード選択ボタンの共通枠を描画します。
 */
void drawModeButtonFrame(const Rect& buttonRect, const String& label) {
  M5.Display.fillRoundRect(buttonRect.x, buttonRect.y, buttonRect.w, buttonRect.h, 10, TFT_WHITE);
  M5.Display.drawRoundRect(buttonRect.x, buttonRect.y, buttonRect.w, buttonRect.h, 10, TFT_DARKGREY);

  const Rect iconRect{
    buttonRect.x + (buttonRect.w - MODE_ICON_SIZE) / 2,
    buttonRect.y + 16,
    MODE_ICON_SIZE,
    MODE_ICON_SIZE,
  };

  if (label == "おうちレジ") {
    drawRegisterModeIcon(iconRect);
  } else {
    drawCameraModeIcon(iconRect);
  }

  M5.Display.setCursor(
    buttonRect.x + (buttonRect.w - M5.Display.textWidth(label)) / 2,
    iconRect.y + iconRect.h + MODE_LABEL_GAP_Y
  );
  M5.Display.print(label);
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
 * 起動モード選択画面を描画します。
 */
void renderModeSelectionScreen() {
  const Rect registerButtonRect = getRegisterModeButtonRect();
  const Rect cameraButtonRect = getCameraModeButtonRect();

  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setFont(BODY_FONT);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  drawCenteredText("モードを選ぶ", MODE_TITLE_Y + 8);

  drawModeButtonFrame(registerButtonRect, "おうちレジ");
  drawModeButtonFrame(cameraButtonRect, "カメラ");
}

/**
 * カメラ未利用時の画面を描画します。
 */
void renderCameraUnavailableScreen() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(BODY_FONT);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  drawCenteredText("カメラを", 82);
  drawCenteredText("つかえません", 118);
  drawCenteredText("タップでさいしこう", 168);
}

/**
 * カメラフレームを画面へ描画します。
 */
void renderCameraFrame(const camera_fb_t* frame) {
  if (frame == nullptr) {
    return;
  }

  if (frame->width != M5.Display.width() || frame->height != M5.Display.height()) {
    M5.Display.fillScreen(TFT_BLACK);
  }

  const int drawX = (M5.Display.width() - frame->width) / 2;
  const int drawY = (M5.Display.height() - frame->height) / 2;
  M5.Display.pushImage(
    std::max(drawX, 0),
    std::max(drawY, 0),
    frame->width,
    frame->height,
    reinterpret_cast<const uint16_t*>(frame->buf)
  );
}

/**
 * 静止画の外枠を描画します。
 */
void drawStillPhotoFrame() {
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  const int innerX = STILL_FRAME_THICKNESS;
  const int innerY = STILL_FRAME_THICKNESS;
  const int innerW = std::max(width - STILL_FRAME_THICKNESS * 2, 1);
  const int innerH = std::max(height - STILL_FRAME_THICKNESS * 2, 1);
  const int lineOffset = std::min(STILL_FRAME_INNER_LINE_OFFSET, STILL_FRAME_THICKNESS + 4);
  const int lineX = lineOffset;
  const int lineY = lineOffset;
  const int lineW = std::max(width - lineOffset * 2, 1);
  const int lineH = std::max(height - lineOffset * 2, 1);

  M5.Display.fillRect(0, 0, width, STILL_FRAME_THICKNESS, TFT_WHITE);
  M5.Display.fillRect(0, height - STILL_FRAME_THICKNESS, width, STILL_FRAME_THICKNESS, TFT_WHITE);
  M5.Display.fillRect(0, STILL_FRAME_THICKNESS, STILL_FRAME_THICKNESS, innerH, TFT_WHITE);
  M5.Display.fillRect(width - STILL_FRAME_THICKNESS, STILL_FRAME_THICKNESS, STILL_FRAME_THICKNESS, innerH, TFT_WHITE);
  M5.Display.drawRect(innerX, innerY, innerW, innerH, TFT_LIGHTGREY);
  M5.Display.drawRect(lineX, lineY, lineW, lineH, TFT_DARKGREY);
}

/**
 * ライブカメラ表示を更新します。
 */
void updateCameraLiveScreen() {
  if (!isCameraReady) {
    return;
  }

  camera_fb_t* frame = nullptr;
  if (!captureCameraFrame(frame)) {
    logDebug("[CAM] capture failed");
    isCameraReady = false;
    renderCameraUnavailableScreen();
    return;
  }

  renderCameraFrame(frame);
  releaseCameraFrame(frame);
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
  if (appMode != AppMode::REGISTER || appState != AppState::NORMAL) {
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
  cart.push_back(item);
  trimCartForDisplay();
  renderNormalScreen();
}

/**
 * RFID UID文字列を処理します。
 */
void handleRfidUid(const String& rawUid) {
  if (appMode != AppMode::REGISTER || appState != AppState::NORMAL) {
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
  if (millis() < barcodeInputReadyAtMs) {
    clearBarcodeSerialInput();
    return;
  }

  if (millis() < barcodeCommandGuardUntilMs) {
    clearBarcodeSerialInput();
    return;
  }

  String line;
  while (readFrame(barcodeSerial, barcodeBuffer, line, barcodeLastByteAtMs, BARCODE_FRAME_GAP_MS)) {
    handleBarcodeCode(line);
  }
}

/**
 * RFIDカード入力を処理します。
 */
void pollRfidCard() {
  if (!isRfidReady) {
    return;
  }

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
 * おうちレジモードへ切り替えます。
 */
void switchToRegisterMode() {
  appMode = AppMode::REGISTER;
  appState = AppState::NORMAL;
  cameraViewState = CameraViewState::LIVE;
  wasTouching = false;
  renderNormalScreen();
}

/**
 * カメラモードへ切り替えます。
 */
void switchToCameraMode() {
  appMode = AppMode::CAMERA;
  appState = AppState::NORMAL;
  cameraViewState = CameraViewState::LIVE;
  wasTouching = false;

  if (!initializeCameraModule()) {
    renderCameraUnavailableScreen();
    return;
  }

  updateCameraLiveScreen();
}

/**
 * 起動モード選択画面のタップを処理します。
 */
void handleModeSelectionTouch(const int touchX, const int touchY) {
  if (isPointInsideRect(touchX, touchY, getRegisterModeButtonRect())) {
    playStartupTone();
    switchToRegisterMode();
    return;
  }

  if (isPointInsideRect(touchX, touchY, getCameraModeButtonRect())) {
    playStartupTone();
    switchToCameraMode();
  }
}

/**
 * おうちレジ画面のタップを処理します。
 */
void handleRegisterTouch(const int touchX, const int touchY) {
  if (appState != AppState::NORMAL) {
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
 * カメラ画面のタップを処理します。
 */
void handleCameraTouch() {
  if (!isCameraReady) {
    switchToCameraMode();
    return;
  }

  if (cameraViewState == CameraViewState::LIVE) {
    playShutterTone();
    cameraViewState = CameraViewState::STILL;
    drawStillPhotoFrame();
    return;
  }

  cameraViewState = CameraViewState::LIVE;
  updateCameraLiveScreen();
}

/**
 * タッチ入力をモードごとに処理します。
 */
void pollTouchInput() {
  int32_t touchX = 0;
  int32_t touchY = 0;
  const bool isTouching = M5.Display.getTouch(&touchX, &touchY);

  if (!isTouching || wasTouching) {
    wasTouching = isTouching;
    return;
  }

  if (appMode == AppMode::SELECT) {
    handleModeSelectionTouch(touchX, touchY);
  } else if (appMode == AppMode::REGISTER) {
    handleRegisterTouch(touchX, touchY);
  } else {
    handleCameraTouch();
  }

  wasTouching = isTouching;
}

/**
 * ありがとう画面の終了タイマーを処理します。
 */
void updateThankYouState() {
  if (appMode != AppMode::REGISTER || appState != AppState::THANK_YOU) {
    return;
  }

  if (millis() - thankYouStartedAtMs < THANK_YOU_DURATION_MS) {
    return;
  }

  appState = AppState::NORMAL;
  renderNormalScreen();
}

/**
 * カメラモード中の描画更新を処理します。
 */
void updateCameraState() {
  if (appMode != AppMode::CAMERA) {
    return;
  }

  if (cameraViewState != CameraViewState::LIVE) {
    return;
  }

  updateCameraLiveScreen();
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
  beginBarcodeSerial(true);
  applyBarcodeScannerSettings();
  barcodeInputReadyAtMs = millis() + BARCODE_BOOT_STABILIZE_MS;
  clearBarcodeSerialInput();
}

/**
 * RFIDリーダを初期化します。
 */
void initializeRfidReader(const int rfidSdaPin, const int rfidSclPin) {
  Wire.begin(rfidSdaPin, rfidSclPin, RFID_I2C_CLOCK);
  if (!checkRfidI2cStatus()) {
    isRfidReady = false;
    return;
  }

  rfidReader.PCD_Init();
  logRfidVersion();
  isRfidReady = true;
}

/**
 * 起動時ログを出力します。
 */
void logBootConfiguration(const int rfidSdaPin, const int rfidSclPin) {
  logDebug("[BOOT] portc RXD pin=" + String(barcodePortcRxdPin) + " TXD pin=" + String(barcodePortcTxdPin));
  logDebug("[BOOT] barcode BAUD=" + String(BARCODE_UART_BAUD));
  logDebug("[BOOT] barcode trigger=unit button");
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

  renderModeSelectionScreen();
}

/**
 * メインループ処理を行います。
 */
void loop() {
  M5.update();

  // 入力は最初に取り込む
  pollTouchInput();

  // おうちレジ側の入力と状態更新
  if (appMode == AppMode::REGISTER) {
    pollDebugSerial();
    pollBarcodeSerial();
    pollRfidCard();
    updateThankYouState();
  }

  // カメラ側の描画更新
  updateCameraState();
}
