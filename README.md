# kids-register

M5Stack CoreS3 を使った子ども向け「おうちレジ」アプリです。  
1Dバーコード入力で商品を追加し、RFIDで決済演出を行います。

## 必要環境
- M5Stack CoreS3
  - バーコードスキャナー
  - RFIDリーダー
- Arduino CLI または Arduino IDE
- ライブラリ
  - `M5Unified`
  - `M5GFX`

## 主な機能
- バーコード入力で商品を追加
- 商品名と価格はハッシュで決定（同じコードは同じ結果）
- 明細は最新3件を表示（長い文字列は `...` で省略）
- RFID入力で決済音を鳴らし、THANK YOU 画面を表示
- 起動時に起動音を再生

## 設定ファイル
- `register-config.h`
  - 商品名候補
  - スキャン音/決済音/起動音のステップ定義
  - 音量

## ビルド例
```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 .
```

## テスト入力（USBシリアル）
- `BC:1234567890` でバーコード入力扱い
- `RFID:ABCD1234` でRFID入力扱い
