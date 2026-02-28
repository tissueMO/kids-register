#ifndef MODE_BASE_H
#define MODE_BASE_H

/**
 * モード共通の契約を定義します。
 */
class ModeBase {
 public:
  /**
   * 破棄処理を行います。
   */
  virtual ~ModeBase() = default;

  /**
   * モードへ遷移した直後の処理を行います。
   */
  virtual void enter() = 0;

  /**
   * タップ入力を処理します。
   */
  virtual void onTouch(int touchX, int touchY) = 0;

  /**
   * モード固有の更新処理を行います。
   */
  virtual void update() = 0;

 protected:
  /**
   * 音ステップ列を順番に再生します。
   */
  static void playToneSteps(const struct ToneStep* steps);
};

#endif
