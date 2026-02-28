#ifndef CAMERA_MODE_H
#define CAMERA_MODE_H

#include <M5Unified.h>
#include <esp_camera.h>

#include "mode-base.h"

/**
 * カメラモードを提供します。
 */
class CameraMode : public ModeBase {
 public:
  /**
   * カメラモードを初期化します。
   */
  CameraMode();

  /**
   * モード遷移時にライブ表示を開始します。
   */
  void enter() override;

  /**
   * タップ入力を処理します。
   */
  void onTouch(int touchX, int touchY) override;

  /**
   * モードの定期更新を処理します。
   */
  void update() override;

  /**
   * モード選択時の起動音を鳴らします。
   */
  void playStartupTone() const;

 private:
  /**
   * カメラ表示状態を表します。
   */
  enum class ViewState {
    LIVE,
    STILL,
  };

  /**
   * デバッグログをUSBシリアルへ出力します。
   */
  void logDebug(const String& message) const;

  /**
   * シャッター音を鳴らします。
   */
  void playShutterTone() const;

  /**
   * カメラ設定を初期化します。
   */
  void initializeCameraConfig(bool useCompactProfile);

  /**
   * カメラモジュールを初期化し、利用可否を返します。
   */
  bool initializeCameraModule();

  /**
   * カメラフレームを1枚取得します。
   */
  bool captureCameraFrame(camera_fb_t*& frame);

  /**
   * 取得済みカメラフレームを解放します。
   */
  void releaseCameraFrame(camera_fb_t*& frame);

  /**
   * 文字列を中央揃えで描画します。
   */
  void drawCenteredText(const String& text, int y) const;

  /**
   * カメラ未利用時の画面を描画します。
   */
  void renderCameraUnavailableScreen() const;

  /**
   * カメラフレームを画面へ描画します。
   */
  void renderCameraFrame(const camera_fb_t* frame) const;

  /**
   * 静止画の外枠を描画します。
   */
  void drawStillPhotoFrame() const;

  /**
   * ライブカメラ表示を更新します。
   */
  void updateCameraLiveScreen();

  camera_config_t cameraConfig_;
  bool isCameraInitialized_;
  bool isCameraReady_;
  ViewState viewState_;
};

#endif
