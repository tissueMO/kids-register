#include <M5Unified.h>

#include <algorithm>

#include "camera-mode.h"
#include "mode-base.h"
#include "register-config.h"
#include "register-mode.h"

namespace {

constexpr long USB_SERIAL_BAUD = 115200;
constexpr int MODE_BUTTON_W = 146;
constexpr int MODE_BUTTON_H = 164;
constexpr int MODE_BUTTON_GAP = 16;
constexpr int MODE_BUTTON_TOP = 56;
constexpr int MODE_ICON_SIZE = 64;
constexpr int MODE_LABEL_GAP_Y = 14;
constexpr int MODE_TITLE_Y = 10;

constexpr const lgfx::U8g2font* BODY_FONT = &fonts::lgfxJapanGothic_24;

/**
 * アプリの画面モードを表します。
 */
enum class AppMode {
  SELECT,
  REGISTER,
  CAMERA,
};

/**
 * 矩形領域を保持します。
 */
struct Rect {
  int x;
  int y;
  int w;
  int h;
};

RegisterMode registerMode;
CameraMode cameraMode;
ModeBase* activeMode = nullptr;
AppMode appMode = AppMode::SELECT;
bool wasTouching = false;

/**
 * 周辺機器ピン情報を取得します。
 */
RegisterMode::Pins resolvePeripheralPins() {
  const int barcodeRxdPin = M5.getPin(m5::pin_name_t::port_c_rxd);
  const int barcodeTxdPin = M5.getPin(m5::pin_name_t::port_c_txd);
  const int rfidSclPin = M5.getPin(m5::pin_name_t::port_a_scl);
  const int rfidSdaPin = M5.getPin(m5::pin_name_t::port_a_sda);

  return RegisterMode::Pins{barcodeRxdPin, barcodeTxdPin, rfidSdaPin, rfidSclPin};
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
 * 指定座標が矩形内かを返します。
 */
bool isPointInsideRect(const int x, const int y, const Rect& rect) {
  return x >= rect.x && x < rect.x + rect.w
    && y >= rect.y && y < rect.y + rect.h;
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
 * おうちレジモードへ切り替えます。
 */
void switchToRegisterMode() {
  appMode = AppMode::REGISTER;
  activeMode = &registerMode;
  activeMode->enter();
}

/**
 * カメラモードへ切り替えます。
 */
void switchToCameraMode() {
  appMode = AppMode::CAMERA;
  activeMode = &cameraMode;
  activeMode->enter();
}

/**
 * 起動モード選択画面のタップを処理します。
 */
void handleModeSelectionTouch(const int touchX, const int touchY) {
  if (isPointInsideRect(touchX, touchY, getRegisterModeButtonRect())) {
    registerMode.playStartupTone();
    switchToRegisterMode();
    return;
  }

  if (isPointInsideRect(touchX, touchY, getCameraModeButtonRect())) {
    cameraMode.playStartupTone();
    switchToCameraMode();
  }
}

/**
 * タップ入力を現在モードへ振り分けます。
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
  } else if (activeMode != nullptr) {
    activeMode->onTouch(touchX, touchY);
  }

  wasTouching = isTouching;
}

}  // namespace

/**
 * 初期化処理を行います。
 */
void setup() {
  auto config = M5.config();
  M5.begin(config);
  initializeUi();

  Serial.begin(USB_SERIAL_BAUD);

  const RegisterMode::Pins pins = resolvePeripheralPins();
  registerMode.initialize(pins);

  renderModeSelectionScreen();
}

/**
 * メインループ処理を行います。
 */
void loop() {
  M5.update();
  pollTouchInput();

  if (activeMode != nullptr) {
    activeMode->update();
  }
}
