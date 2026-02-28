#ifndef REGISTER_MODE_H
#define REGISTER_MODE_H

#include <M5Unified.h>
#include <MFRC522_I2C.h>

#include <vector>

#include "mode-base.h"

/**
 * おうちレジモードを提供します。
 */
class RegisterMode : public ModeBase {
 public:
  /**
   * 周辺機器ピン情報を保持します。
   */
  struct Pins {
    int barcodeRxdPin;
    int barcodeTxdPin;
    int rfidSdaPin;
    int rfidSclPin;
  };

  /**
   * おうちレジモードを初期化します。
   */
  RegisterMode();

  /**
   * 周辺機器を初期化します。
   */
  void initialize(const Pins& pins);

  /**
   * モード遷移時に通常画面を表示します。
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
   * 画面状態を表します。
   */
  enum class AppState {
    NORMAL,
    THANK_YOU,
  };

  /**
   * 商品情報を保持します。
   */
  struct Item {
    String name;
    int price;
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

  /**
   * デバッグログをUSBシリアルへ出力します。
   */
  void logDebug(const String& message) const;

  /**
   * スキャン音を鳴らします。
   */
  void playScanTone() const;

  /**
   * 決済音を鳴らします。
   */
  void playPaymentTone() const;

  /**
   * バーコードUARTを初期化します。
   */
  void beginBarcodeSerial(bool shouldLog);

  /**
   * バーコードUART受信バッファを破棄します。
   */
  void clearBarcodeSerialInput();

  /**
   * バーコードスキャナへコマンドを送信します。
   */
  void sendBarcodeCommand(const uint8_t* command, size_t length);

  /**
   * バーコードスキャナの起動時設定を適用します。
   */
  void applyBarcodeScannerSettings();

  /**
   * RFIDリーダのI2C接続状態をログ出力します。
   */
  bool checkRfidI2cStatus() const;

  /**
   * RFIDリーダのバージョンレジスタ値をログ出力します。
   */
  void logRfidVersion();

  /**
   * CLEARボタンの表示領域を返します。
   */
  Rect getClearButtonRect() const;

  /**
   * CLEARボタンのタッチ判定領域を返します。
   */
  Rect getClearButtonHitRect() const;

  /**
   * 指定座標が矩形内かを返します。
   */
  bool isPointInsideRect(int x, int y, const Rect& rect) const;

  /**
   * 商品名候補数を返します。
   */
  size_t getProductNameCount() const;

  /**
   * 指定した添字の商品名候補を返します。
   */
  const char* getProductName(size_t index) const;

  /**
   * FNV-1a 32bitハッシュ値を返します。
   */
  uint32_t fnv1a32(const String& input) const;

  /**
   * バーコード文字列から商品情報を決定します。
   */
  Item resolveItemFromCode(const String& code) const;

  /**
   * カート内の合計金額を返します。
   */
  int calculateTotalSum() const;

  /**
   * フレームバッファを上限長まで切り詰めます。
   */
  void trimFrameBuffer(String& buffer) const;

  /**
   * 入力文字列を整形し、有効な長さかを返します。
   */
  bool tryNormalizeInput(const String& rawInput, String& normalizedInput) const;

  /**
   * バーコードスキャナの制御応答フレームかを判定します。
   */
  bool isBarcodeControlResponse(const String& frame) const;

  /**
   * UTF-8文字列の末尾1文字を除去した文字列を返します。
   */
  String removeLastUtf8Character(const String& text) const;

  /**
   * 表示幅に収まるように必要時のみ省略記号を付与します。
   */
  String ellipsizeText(const String& text, int maxWidth) const;

  /**
   * 文字列を中央揃えで描画します。
   */
  void drawCenteredText(const String& text, int y) const;

  /**
   * 指定矩形の中央に文字列を描画します。
   */
  void drawCenteredTextInRect(const String& text, const Rect& rect) const;

  /**
   * CLEARボタンを描画します。
   */
  void drawClearButton(const Rect& clearButtonRect) const;

  /**
   * 明細の罫線を描画します。
   */
  void drawItemRules(int displayWidth) const;

  /**
   * 明細の表示対象を最新件数に切り詰めます。
   */
  void trimCartForDisplay();

  /**
   * 明細一覧を描画します。
   */
  void drawCartItems(int displayWidth) const;

  /**
   * 合計金額表示を描画します。
   */
  void drawTotalSummary(int displayHeight) const;

  /**
   * 通常画面を描画します。
   */
  void renderNormalScreen() const;

  /**
   * 決済完了画面を描画します。
   */
  void renderThankYouScreen() const;

  /**
   * カートを空にして通常画面を更新します。
   */
  void clearCart();

  /**
   * バーコード文字列を処理します。
   */
  void handleBarcodeCode(const String& rawCode);

  /**
   * RFID UID文字列を処理します。
   */
  void handleRfidUid(const String& rawUid);

  /**
   * RFIDカードのUIDを16進文字列へ変換します。
   */
  String getRfidUidHex() const;

  /**
   * ストリームから1フレームを読み取ります。
   */
  bool readFrame(
    Stream& stream,
    String& buffer,
    String& lineOut,
    uint32_t& lastByteAtMs,
    uint32_t frameGapMs
  );

  /**
   * UART経由のバーコード入力を処理します。
   */
  void pollBarcodeSerial();

  /**
   * RFIDカード入力を処理します。
   */
  void pollRfidCard();

  /**
   * デバッグ入力1行を処理します。
   */
  void handleDebugLine(const String& rawLine);

  /**
   * USBシリアルからのテスト入力を処理します。
   */
  void pollDebugSerial();

  /**
   * ありがとう画面の終了タイマーを処理します。
   */
  void updateThankYouState();

  HardwareSerial barcodeSerial_;
  MFRC522_I2C rfidReader_;
  std::vector<Item> cart_;
  AppState appState_;
  String barcodeBuffer_;
  String debugBuffer_;
  uint32_t thankYouStartedAtMs_;
  uint32_t barcodeLastByteAtMs_;
  uint32_t debugLastByteAtMs_;
  uint32_t barcodeCommandGuardUntilMs_;
  uint32_t barcodeInputReadyAtMs_;
  int barcodeRxdPin_;
  int barcodeTxdPin_;
  bool isRfidReady_;
};

#endif
