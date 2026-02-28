#include "camera-mode.h"

#include <algorithm>

#include "register-config.h"

namespace {

constexpr bool ENABLE_SERIAL_DEBUG = true;
constexpr int STILL_FRAME_THICKNESS = 5;
constexpr int STILL_FRAME_INNER_LINE_OFFSET = 8;
constexpr const lgfx::U8g2font* BODY_FONT = &fonts::lgfxJapanGothic_24;

}  // namespace

/**
 * カメラモードを初期化します。
 */
CameraMode::CameraMode()
: cameraConfig_(),
  isCameraInitialized_(false),
  isCameraReady_(false),
  viewState_(ViewState::LIVE) {
}

/**
 * モード遷移時にライブ表示を開始します。
 */
void CameraMode::enter() {
  viewState_ = ViewState::LIVE;

  if (!initializeCameraModule()) {
    renderCameraUnavailableScreen();
    return;
  }

  updateCameraLiveScreen();
}

/**
 * タップ入力を処理します。
 */
void CameraMode::onTouch(const int touchX, const int touchY) {
  static_cast<void>(touchX);
  static_cast<void>(touchY);

  if (!isCameraReady_) {
    enter();
    return;
  }

  if (viewState_ == ViewState::LIVE) {
    playShutterTone();
    viewState_ = ViewState::STILL;
    drawStillPhotoFrame();
    return;
  }

  viewState_ = ViewState::LIVE;
  updateCameraLiveScreen();
}

/**
 * モードの定期更新を処理します。
 */
void CameraMode::update() {
  if (viewState_ != ViewState::LIVE) {
    return;
  }

  updateCameraLiveScreen();
}

/**
 * モード選択時の起動音を鳴らします。
 */
void CameraMode::playStartupTone() const {
  playToneSteps(STARTUP_TONE_STEPS);
}

/**
 * デバッグログをUSBシリアルへ出力します。
 */
void CameraMode::logDebug(const String& message) const {
  if (!ENABLE_SERIAL_DEBUG) {
    return;
  }

  Serial.println(message);
}

/**
 * シャッター音を鳴らします。
 */
void CameraMode::playShutterTone() const {
  playToneSteps(SHUTTER_TONE_STEPS);
}

/**
 * カメラ設定を初期化します。
 */
void CameraMode::initializeCameraConfig(const bool useCompactProfile) {
  cameraConfig_.pin_pwdn = -1;
  cameraConfig_.pin_reset = -1;
  cameraConfig_.pin_xclk = -1;
  cameraConfig_.pin_sscb_sda = 12;
  cameraConfig_.pin_sscb_scl = 11;
  cameraConfig_.pin_d7 = 47;
  cameraConfig_.pin_d6 = 48;
  cameraConfig_.pin_d5 = 16;
  cameraConfig_.pin_d4 = 15;
  cameraConfig_.pin_d3 = 42;
  cameraConfig_.pin_d2 = 41;
  cameraConfig_.pin_d1 = 40;
  cameraConfig_.pin_d0 = 39;
  cameraConfig_.pin_vsync = 46;
  cameraConfig_.pin_href = 38;
  cameraConfig_.pin_pclk = 45;
  cameraConfig_.xclk_freq_hz = 20000000;
  cameraConfig_.ledc_timer = LEDC_TIMER_0;
  cameraConfig_.ledc_channel = LEDC_CHANNEL_0;
  cameraConfig_.pixel_format = PIXFORMAT_RGB565;
  cameraConfig_.frame_size = FRAMESIZE_QVGA;
  cameraConfig_.jpeg_quality = 0;
  cameraConfig_.fb_count = useCompactProfile ? 1 : 2;
  cameraConfig_.fb_location = useCompactProfile ? CAMERA_FB_IN_DRAM : CAMERA_FB_IN_PSRAM;
  cameraConfig_.grab_mode = CAMERA_GRAB_LATEST;
  cameraConfig_.sccb_i2c_port = -1;
}

/**
 * カメラモジュールを初期化し、利用可否を返します。
 */
bool CameraMode::initializeCameraModule() {
  if (isCameraReady_) {
    return true;
  }

  if (isCameraInitialized_) {
    esp_camera_deinit();
    isCameraInitialized_ = false;
  }

  initializeCameraConfig(false);
  M5.In_I2C.release();
  esp_err_t result = esp_camera_init(&cameraConfig_);
  if (result == ESP_OK) {
    isCameraInitialized_ = true;
    isCameraReady_ = true;
    logDebug("[CAM] init ok profile=normal");
    return true;
  }

  logDebug("[CAM] init failed profile=normal err=" + String(static_cast<int>(result)));
  esp_camera_deinit();
  delay(20);

  initializeCameraConfig(true);
  result = esp_camera_init(&cameraConfig_);
  if (result != ESP_OK) {
    logDebug("[CAM] init failed profile=compact err=" + String(static_cast<int>(result)));
    isCameraInitialized_ = false;
    isCameraReady_ = false;
    return false;
  }

  isCameraInitialized_ = true;
  isCameraReady_ = true;
  logDebug("[CAM] init ok profile=compact");
  return true;
}

/**
 * カメラフレームを1枚取得します。
 */
bool CameraMode::captureCameraFrame(camera_fb_t*& frame) {
  frame = esp_camera_fb_get();
  return frame != nullptr;
}

/**
 * 取得済みカメラフレームを解放します。
 */
void CameraMode::releaseCameraFrame(camera_fb_t*& frame) {
  if (frame == nullptr) {
    return;
  }

  esp_camera_fb_return(frame);
  frame = nullptr;
}

/**
 * 文字列を中央揃えで描画します。
 */
void CameraMode::drawCenteredText(const String& text, const int y) const {
  const int x = (M5.Display.width() - M5.Display.textWidth(text)) / 2;
  M5.Display.setCursor(std::max(x, 0), y);
  M5.Display.print(text);
}

/**
 * カメラ未利用時の画面を描画します。
 */
void CameraMode::renderCameraUnavailableScreen() const {
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
void CameraMode::renderCameraFrame(const camera_fb_t* frame) const {
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
void CameraMode::drawStillPhotoFrame() const {
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
void CameraMode::updateCameraLiveScreen() {
  if (!isCameraReady_) {
    return;
  }

  camera_fb_t* frame = nullptr;
  if (!captureCameraFrame(frame)) {
    logDebug("[CAM] capture failed");
    isCameraReady_ = false;
    renderCameraUnavailableScreen();
    return;
  }

  renderCameraFrame(frame);
  releaseCameraFrame(frame);
}
