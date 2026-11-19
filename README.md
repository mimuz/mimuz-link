# mimuz-link

ESP-NOW / BLE-MIDI に対応した、無線の MIDI 接続環境。

クライアント（XIAO ESP32-C6。コントローラーや音源）と、ブリッジ（XIAO ESP32-S3。PC につなぐ USB-MIDI ドングル）を ESP-NOW でつなぐ。PC からは、ブリッジが 8 ポートの USB-MIDI 機器として見える。ブリッジが見つからないとき、クライアントは BLE-MIDI 機器として、PC と直接つながる。

- 低遅延：ESP-NOW の区間は、片道の中央値で 1ms 以下（PC → ブリッジ → クライアント → ブリッジ → PC の往復の中央値は約 1.6ms）。
- 複数台：1 台のブリッジに、最大 8 台のクライアントが同時につながる。クライアントどうしで、MIDI チャンネルごとにメッセージを配ることもできる。
- 取りこぼし対策：Note Off は、届かなかったときに送り直す。古くなった Note On は捨てる。
- 設定：USB シリアルの 1 行のコマンドで行い、不揮発メモリに保存する。

## 構成

通信の部分（ライブラリ）と、機器ごとの部分（アプリケーション）を分けている。

```
libs/mimuz-link/               通信の層（Arduino ライブラリ。ヘッダだけ）
  src/mimuz_link_proto.h         パケットの形式・種類・定数（クライアントとブリッジで共通）
  src/mimuz_link_client.h        クライアント側（C6）
  src/mimuz_link_bridge.h        ブリッジ側（S3）
  src/mimuz_link_cmd.h           設定コマンド（USB シリアル）の共通部分
  src/mimuz_link_lathist.h       統計用の道具
test/mimuz-link-test/          試験用のメッセージの定義（通信の層ではない）
  src/mimuz_link_testproto.h
examples/
  client/c6_test_device/       試験用のクライアント：C6 ＋ スイッチ 1 個
    c6_test_device.ino           試験用のメッセージの処理、音源の代わり、シリアルコマンド
    workload.h                   コントローラーを想定した負荷（1ms のスキャン、スイッチ、エンコーダーの模擬）
  bridge/s3_bridge/            ブリッジ：試験用の処理とシリアルコマンド
    s3_bridge.ino
  pc/                          PC 側の計測と設定の道具（Python）
  pc/web/                      体感の確認用のビューア（Web MIDI）
```

### 通信の層が受け持つこと

| | クライアント（C6） | ブリッジ（S3） |
|---|---|---|
| 接続 | ブリッジへの登録と接続、接続の維持、切れたときの探し直し | クライアントの登録（保存）、ポートの割り当てと解放 |
| チャンネル | ブリッジの無線チャンネルに付いていく | 起動時に空いている無線チャンネルを選ぶ。動作中の切り替え |
| 送信 | 送信待ちの列、まとめ送り、Note Off の送り直し、古くなった Note On の破棄 | 同じ（PC → クライアント）。USB の書き込み待ち |
| 配信 | 聞く MIDI チャンネルをブリッジに伝える | クライアントから来たメッセージを、そのチャンネルを聞いているほかのクライアントへ送る |
| その他 | ブリッジにつながらないときの BLE-MIDI、通信方式の決定 | USB-MIDI の固定 8 ポート |
| 復帰 | 無線のドライバーが固まったことの検出と再起動 | 同じ |
| 重複 | 無線の層の送り直しで 2 回届いたパケットを捨てる | 同じ |
| 診断 | 無線の往復時間の計測、統計 | PING への応答、統計、記録 |

運ぶメッセージ：チャンネルつきのメッセージと、チャンネルを持たないメッセージ（クロック、スタート、ストップ、ソングポジションなど）。SysEx は運ばない。

通信の層は、MIDI メッセージを 1 件ずつ単独で扱う。Note On と Note Off の対応や、鳴っている音の状態は持たない。

### アプリケーションから使う入口

クライアント（`#include <mimuz_link_client.h>`、名前空間 `mlink`）：

```cpp
mlink::Config cfg;                       // namePrefix、repair（登録のやり直し）、タスクの優先度
mlink::onMidi(handler);                  // 受け取った MIDI（受信用のタスクから呼ばれる）
mlink::onMidiFast(handler);              // 省略可：無線の受信の処理の中で、すぐに呼ばれる
mlink::Transport tr = mlink::begin(cfg); // setup() で 1 回。TR_ESPNOW か TR_BLE を返す
mlink::sendMidi(status, d1, d2, aux, waitMs);  // どのタスクからでも呼べる
mlink::loop();                           // loop() から呼ぶ

// 設定
mlink::setRxChannels(mask, flags);       // 聞く MIDI チャンネル（bit0 = ch1。0xFFFF = OMNI）
mlink::setNoteOnMaxAgeMs(ms);            // 古くなった Note On を捨てるまでの時間（0 = 捨てない）
mlink::setRate(idx);                     // PHY レート（0=1M 1=6M 2=24M 3=MCS4）
mlink::restart(mlink::BOOT_AUTO);        // BOOT_AUTO / BOOT_BLE / BOOT_REPAIR

// 状態・診断
mlink::name(); mlink::isConnected(); mlink::port(); mlink::radioChannel(); mlink::transport();
mlink::radioPing(count); mlink::printStats(Serial); mlink::clearStats();
```

ブリッジ（`#include <mimuz_link_bridge.h>`、名前空間 `mlink`）：

```cpp
#define MIMUZ_BRIDGE_NAME_PREFIX "mi:muz:bridgeESPS3"   // 省略可（include の前に書く）
mlink::onUsbMidi(handler);               // 省略可：PC から来た MIDI を先に見る（true で、クライアントへ送らない）
mlink::setMidiLogFilter(filter);         // 省略可：1 件ずつの記録に出すものを選ぶ
mlink::begin();                          // setup() で 1 回（無線チャンネルの測定に約 12 秒かかる）
mlink::loop();                           // loop() から呼ぶ

mlink::usbSend(port, m); mlink::setNoteOnMaxAgeMs(ms);
mlink::clearRegistry(); mlink::resurvey(); mlink::switchChannel(ch); mlink::debugDeaf(ms);
mlink::printStats(Serial); mlink::printList(Serial); mlink::printSurvey(); mlink::clearStats();
```

- ヘッダだけのライブラリなので、スケッチの 1 つのファイルからだけ include する。
- よく通る処理は、内蔵の高速メモリ（IRAM）に置いている。ブリッジでは、往復が約 0.15ms 短くなる。比べるときは `MIMUZ_LINK_NO_IRAM` を定義してビルドすると、フラッシュに置く（arduino-cli では `--build-property "compiler.cpp.extra_flags=-DMIMUZ_LINK_NO_IRAM"`）。

## ビルドと書き込み（arduino-cli、esp32 core 3.3.8）

```sh
# ブリッジ（S3）：USB Mode = TinyUSB を明示する
cd examples/bridge
arduino-cli compile -u -p <port> --libraries ../../libs --libraries ../../test \
  --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default s3_bridge

# 試験用のクライアント（C6）：Wi-Fi と BLE の両方が入るので huge_app。シリアルを USB に出すため CDCOnBoot=cdc
cd examples/client
arduino-cli compile -u -p <port> --libraries ../../libs --libraries ../../test \
  --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app c6_test_device
```

- 新品の XIAO ESP32-C6 は、初めて書き込んだあと、USB を挿し直すまでファームが動かない。
- S3 への書き込みで `Failed uploading` と出たら、`arduino-cli board list` でブートローダーのポートが現れていないかを見て、そのポートに `arduino-cli upload` し直す。
- ブリッジを先に起動させてから（無線チャンネルの測定に約 12 秒）、C6 を起動すると、すぐにつながる。順番が逆でも、C6 は BLE で待ち受けながら 20 秒ごとにブリッジを探し直すので、あとからつながる。
- 初めて接続したとき、macOS が「アクセサリの接続を許可」で止めることがある。

## 配線（試験用のクライアント）

C6：GPIO17（D7）と 3.3V の間にタクトスイッチ。GPIO17 と GND の間に外付けのプルダウン抵抗（押すと High）。S3 は配線なし。

## 使い方

1. ブリッジを PC に挿す。C6 の電源を入れる。
   - 登録していない C6 は、ブリッジのすぐ近くにあれば、自動で登録して接続する。
   - 登録済みの C6 は、保存してあるブリッジに接続する。
   - スイッチを押しながら C6 を起動すると、登録を消して、やり直す。
2. PC には `mi:muz:bridgeESPS3-<固有ID> Port 1`〜`Port 8` が見える。接続した C6 は、空いているいちばん小さい番号のポートに入る。ポート番号は個体に固定していない（接続した順で決まる）。
3. ブリッジがないとき、C6 は BLE-MIDI（名前は `mi:muz:conESP32C6-<固有ID>`）で待ち受ける。

## 設定コマンド（USB シリアル）

設定は、C6 もブリッジも USB シリアル（115200）で行う。MIDI は USB-MIDI と無線で流し、シリアルは設定だけに使う。
ポートは DTR をオンにして開く（C6 は DTR オフで開くとリセットされる。S3 は DTR オンでないと返事を出さない）。

- PC → 機器：`@<番号> <コマンド> <引数…>`（1 行）
- 機器 → PC：`@<番号> ok <名前=値 …>` / `@<番号> err <理由>` / `@<番号> + <名前=値 …>`（複数行の返事の途中）
- 行の先頭が `@` でない行は、ログ。
- `set` した値は、すぐに不揮発メモリに保存し、次の起動時に読み込んで反映する（「保存しない」と書いた診断用の項目を除く）。

| 機器 | コマンド | 内容 |
|---|---|---|
| 両方 | `info` | 名前、種類、ファームの版、接続の状態 |
| 両方 | `get [項目]` / `set <項目> <値>` | 設定の読み書き |
| C6 | 項目 `rx_channels` | 聞く MIDI チャンネル（bit0 = チャンネル 1。`omni`、`none` も可） |
| C6 | 項目 `noteon_max_age_ms` | 古くなった Note On を捨てるまでの時間（0 = 捨てない。初めの値は 80） |
| C6 | 項目 `phy_rate` | 無線の速さ（0=1M 1=6M 2=24M 3=MCS4。初めの値は 1） |
| C6 | `reset` / `unpair` / `restart [auto\|ble]` | 設定を初めの値に戻す / 登録を消して再起動 / 再起動 |
| 両方 | `radio_restart [wifi\|stall]` | 診断用：ESP-NOW を再起動する。`wifi` = Wi-Fi ごと、`stall` = 固まった状態をまねる（検出して自分で再起動する） |
| ブリッジ | 項目 `channel_min`、`channel_max` | 選んでよい無線チャンネルの範囲（次に測り直したときから効く） |
| ブリッジ | 項目 `noteon_max_age_ms` | 同上（PC → クライアントの向き） |
| ブリッジ | `ports` / `registry` / `survey` | 接続中のポート（ポートごとの受信の強さ `rssi`、届かなかった数 `tx_fail` も出る）/ 登録の一覧 / 直近の無線チャンネルの測定結果 |
| ブリッジ | `unregister <MAC\|all>` | 登録の解除 |
| ブリッジ | `resurvey` / `switch_channel <n>` | 無線チャンネルを測り直して選び直す（約 12 秒）/ 指定の無線チャンネルへ移る |
| ブリッジ | `port <n> <コマンド…>` | ポート n のクライアントへ無線で中継する（例：`@5 port 2 set rx_channels omni`）。返事が来なければ 150ms ごとに送り直し、1.5 秒で `err timeout` |
| ブリッジ | 項目 `tx_parallel` | 診断用：1 = クライアントごとに並行して無線へ渡す、0 = 全体で 1 パケットずつ（初めの値）。保存しない |
| ブリッジ | 項目 `tx_fail_max`、`tx_probe` | 診断用：応答しないクライアントを見切るまでの回数（初めの値 3）、見切っているあいだの試し送り（1 = あり。250ms から倍々、上限 1 秒）。保存しない |
| ブリッジ | 項目 `tx_impl` | 診断用：0 = 1 本の列で送る送信タスク、1 = クライアントごとの送信タスク（初めの値）。同じファームの中で比べるためのもの。保存しない |

PC から試す道具：`examples/pc/mlink_config.py <シリアルポート> "コマンド" …`

## シリアルコマンド（試験用のファーム）

- C6（`c6_test_device`）：`p` / `P` = 無線の往復を 1 万回 / 10 万回測る、`s` = 統計、`c` = 統計を消す、`r0`〜`r3` = PHY レート、`b` = BLE に固定して再起動、`e` = 自動で再起動、`u` = 登録を消してやり直す、`n` / `N` = スイッチの入力を無効 / 有効にする（保存する）。
- ブリッジ（`s3_bridge`）：`s` = 統計、`c` = 統計を消す、`l` = 登録とポートの一覧、`x` = 登録を全て消す、`v` = 全メッセージの記録の切り替え、`h` = 無線チャンネルの測定結果、`H` = 測り直して選び直す、`1`〜`9`・`A`〜`D` = 無線チャンネル 1〜13 に移る、`z` = 150ms のあいだ、届かない状態を作る。

## PC 側の道具（`examples/pc/`）

準備：`cd examples/pc && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`

| スクリプト | 用途 |
|---|---|
| `latency_test.py` | `ping` / `burst` / `rxtest` / `notetest` / `looptest` / `systest` / `info` / `listen` / `list`。`--port` はポート名の一部（例：`"Port 1"`、`conESP32C6`） |
| `multi_test.py` | 複数のクライアントを同時につないだときの確認（往復、同時の送信、クライアントどうしの配信） |
| `weak_test.py` | 届きにくいクライアントが 1 台あるとき、ほかのクライアントの遅れがどう変わるかを、条件ごとに測る |
| `dead_peer_test.py` | 応答しなくなったクライアントあての送信が、ほかのクライアントをどれだけ待たせるかを測る |
| `port_watch.py` | ポートの解放と復帰の見張り |
| `mlink_config.py` | 設定コマンドを送る |
| `run_suite.py` | BLE と ESP-NOW の計測の一括実行 |
| `duolog.py` | 2 つのシリアルを同時に読み、時刻を付けて 1 つの記録にまとめる。途中でコマンドを送れる |
| `rate_sweep.py`、`load_matrix.py`、`serial_open_test.py`、`c6cmd.py`、`monitor.py` | PHY レートごとの計測、負荷ごとの計測、シリアルを開いたときのリセットの確認、コマンド 1 つの送信、長時間の記録 |

- 長い計測は、macOS がスリープしないように `caffeinate -i` を付けて実行する（スリープ中は macOS の MIDI が止まる）。

体感の確認用のビューア：`cd examples/pc/web && npm install && npm start` を実行し、Chrome で http://localhost:3000 を開く。

## ライセンス

[LICENSE](LICENSE)（MIT）
