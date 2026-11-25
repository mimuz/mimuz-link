# mimuz-link

mimuz-link は ESP32 を用いて ESP-NOW と BLE-MIDI に対応したワイヤレスMIDI接続環境を構築するためのライブラリです。

クライアントは、ESP-NOW でブリッジにつながります。PC からは、ブリッジは 8 ポートの USB-MIDI デバイスに見えます。ブリッジが見つからないときは、クライアントが BLE-MIDI デバイスとして PC に直接つながります。

```
PC ── USB-MIDI（8 ポート）── ブリッジ（XIAO ESP32-S3）~~ ESP-NOW ~~ クライアント（XIAO ESP32-C6）最大 8 台
PC ~~ BLE-MIDI ~~ クライアント（ブリッジが見つからないとき）
```

現時点の対応デバイスは以下の通りです。

- クライアント用：XIAO-ESP32C6    
- ブリッジ用：XIAO-ESP32S3   

特徴は以下の通りです。    

- 低遅延：ESP-NOW の区間は片道 1 ms 未満（中央値）です。PC → ブリッジ → クライアント → ブリッジ → PC の往復は、中央値で約 1.6 ms です。
- 複数台：1 台のブリッジに、最大 8 台のクライアントを同時につなげます。クライアント同士で、MIDI チャンネルごとにメッセージを届けることもできます。
- 配送：届かなかった Note Off は送り直します。待ち時間が長くなりすぎた Note On は捨てます。
- 設定：USB シリアルの 1 行コマンド、またはブリッジ経由の SysEx で設定します。設定は不揮発メモリーに保存されます。

> **注意：esp32 Arduino core 3.3.8 専用です。** mimuz-link は、esp32 core と ESP-IDF の公開 API ではない部分を使っているため、動作を確認したバージョン **esp32 core 3.3.8** でしか動きません。ほかのバージョンでは、ビルドがエラーで止まります。インストールは `arduino-cli core install esp32:esp32@3.3.8` で行います（Arduino IDE では、ボードマネージャー → esp32 by Espressif Systems → バージョン 3.3.8）。

## 次に読むもの

| ページ | 内容 |
|---|---|
| [チュートリアル 1：クイックスタート](tutorial-quick-start.md) | ESP32に最低限のソフトウエアを書き込んでmimuz-linkを体験する方法を解説します。|
| [チュートリアル 2：新規デバイスの作成](tutorial-new-client.md) | mimuz-linkに対応したデバイスを新たに作成する方法を解説します。 |
| [API リファレンス](../README.md)（英語） | [Web ドライバー](../js-driver.md)、[Arduino ライブラリ](../arduino-library.md)、[設定コマンドと SysEx](../config-commands.md)。 |
| [コンソール](../../web/README.md)（英語） | ブリッジとクライアントの設定や状態確認をブラウザーから実施するためのコンソールです。|
| [ファームウェアアップローダー](../../web/README.md#firmware-uploader)（英語） | ブリッジとクライアントのファームウェアを、ブラウザーから書き込みます。 |
| [リポジトリの README](../../README.md)（英語） | リポジトリの概要説明です。様々な情報へのリンクがあります。|
