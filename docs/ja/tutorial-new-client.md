# チュートリアル 2：新規デバイスの作成

自分の mimuz-link クライアントを、一歩ずつ作ります。題材にするのは **knob1** です。XIAO ESP32-C6 に、つまみ（ポテンショメーター）1 個、ボタン 1 個、LED 1 個が付いたデバイスです。つまみを Control Change、ボタンをノートとして送り、PC からそのノートが届くと LED を点けます。Web ページから名前で使えるように、小さなドライバーも作ります。

同じ手順で、どんなクライアントでも作れます。つまみや鍵盤の多いコントローラー、ノートを受け取る音源、ランプ、… 無線、ペアリング、BLE-MIDI、設定コマンドはライブラリが受け持ち、スケッチはデバイスそのものの動きだけを書きます。

先に [チュートリアル 1：クイックスタート](tutorial-quick-start.md) を済ませてください。ブリッジ、Web サイト、ペアリングは、そこで用意したものを使います。

## 目次

- [ステップ 1：送るものと受け取るものを決める](#ステップ-1送るものと受け取るものを決める)
- [ステップ 2：ビルドの環境を整える](#ステップ-2ビルドの環境を整える)
- [ステップ 3：つまみを配線する](#ステップ-3つまみを配線する)
- [ステップ 4：スケッチを書く](#ステップ-4スケッチを書く)
- [ステップ 5：ビルドして書き込む](#ステップ-5ビルドして書き込む)
- [ステップ 6：ペアリングして確かめる](#ステップ-6ペアリングして確かめる)
- [ステップ 7：デバイスに保存する設定](#ステップ-7デバイスに保存する設定)
- [ステップ 8：Web ページ用のドライバー](#ステップ-8web-ページ用のドライバー)
- [ステップ 9：ドライバーを試す](#ステップ-9ドライバーを試す)
- [気をつけること](#気をつけること)
- [次へ](#次へ)

## ステップ 1：送るものと受け取るものを決める

まず、デバイスの **モデル** と MIDI メッセージを書き出します。スケッチもドライバーも、この表に従って作ります。

- モデル：`knob1`（`A-Z a-z 0-9 - _ .` で 16 文字まで）。クライアントがどんな種類のデバイスかを、アプリケーションに伝えます。デバイスの種類ごとに別のモデルを付け、同じ種類のデバイスどうしは同じモデルにします。

| 部品 | 向き | MIDI（チャンネル 1） |
|---|---|---|
| つまみ | デバイス → PC | Control Change 20、値 0〜127 |
| ボタン | デバイス → PC | 押すと Note On 60（ベロシティ 100）、離すと Note Off 60 |
| LED | PC → デバイス | Note On 60（ベロシティが 0 より大きい）で点灯、Note Off 60 で消灯 |

mimuz-link が運ぶのは、チャンネルつきのメッセージと、チャンネルを持たないメッセージ（クロック、スタート、ストップ、…）です。**SysEx は運びません。**

## ステップ 2：ビルドの環境を整える

mimuz-link は **esp32 Arduino core 3.3.8** でしかビルドできません。[arduino-cli](https://arduino.github.io/arduino-cli/) の場合：

```sh
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.8
```

ライブラリはこのリポジトリの中（`libs/mimuz-link`）にあり、`--libraries` でコンパイラーに渡すので、インストールは要りません。

Arduino IDE を使う場合は、ボードマネージャーで **esp32 by Espressif Systems** のバージョン **3.3.8** を入れ、`libs/mimuz-link` のフォルダーを、スケッチブックの `libraries` フォルダー（たとえば `~/Documents/Arduino/libraries/`）にコピーします。

## ステップ 3：つまみを配線する

ポテンショメーター（10 kΩ、B カーブ）を XIAO ESP32-C6 につなぎます。

| ポテンショメーター | XIAO ESP32-C6 |
|---|---|
| 片方の端 | 3V3 |
| ワイパー（真ん中） | D0（A0） |
| もう片方の端 | GND |

ボタンと LED は、ボードに付いている BOOT ボタンとユーザー LED を使います。ポテンショメーターがなくても、つまみ以外は動きます。

## ステップ 4：スケッチを書く

basic の example（`examples/client/basic/basic.ino`）から始めます。ボタン、LED、設定コマンド、ライブラリの呼び出しが、すでに入っています。新しいスケッチのフォルダーにコピーします。ファイル名は、フォルダー名と同じにする必要があります。

```sh
cd examples/client
cp -R basic knob1
mv knob1/basic.ino knob1/knob1.ino
```

続けて、`knob1.ino` を次のように直します。

1. 先頭のコメントを書き直します。デバイスが何をするかと、ビルドの行の `knob1`。
2. **Board settings** に、つまみのピン（`KNOB_PIN`、A0）を足します。ボタンと LED の設定（`BASIC_BUTTON_PIN`、`BASIC_LED_PIN`、`BASIC_LED_ON`）はそのままです。
3. **Application** に、`KNOB_CC`（20）と `KNOB_MS`（つまみを読む間隔）を足します。
4. `onMidi()` で、チャンネル 1 のボタンのノートにだけ反応するようにします（ステップ 1）。
5. `setup()` で、`cfg.model = "knob1"` と `analogReadResolution(12)` を設定し、準備完了のメッセージの名前を変えます。
6. `readKnob()` を足し、`loop()` から `KNOB_MS` ごとに呼びます。

できあがり：

```cpp
/*

 knob1 mimuz-link client: one knob, one button and one LED (made from the basic example)

*/
// What it does:
//   - turning the knob (a potentiometer on A0) sends Control Change 20 (channel 1)
//   - pressing the button sends Note On (middle C, channel 1), releasing it sends Note Off
//   - Note On / Note Off of middle C from the PC turn the LED on / off
//   - lines starting with "@" on the USB serial are config commands (web console, examples/pc/mlink_config.py)
//
// Build (from examples/client), XIAO ESP32-C6:
//   arduino-cli compile --libraries ../../libs --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app knob1
#include <mimuz_link_client.h>

// ---------------- Board settings ----------------

// Button: the BOOT button by default (GPIO9 on ESP32-C3/C6/H2, GPIO0 on the others), pressed = LOW
#ifndef BASIC_BUTTON_PIN
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
#define BASIC_BUTTON_PIN 9
#else
#define BASIC_BUTTON_PIN 0
#endif
#endif

// LED: the board's built-in LED; -1 = no LED
#ifndef BASIC_LED_PIN
#ifdef LED_BUILTIN
#define BASIC_LED_PIN LED_BUILTIN
#else
#define BASIC_LED_PIN -1
#endif
#endif

// Level that turns the LED on (the XIAO boards' user LED is on at LOW)
#ifndef BASIC_LED_ON
#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32C6) || defined(ARDUINO_XIAO_ESP32S3)
#define BASIC_LED_ON LOW
#else
#define BASIC_LED_ON HIGH
#endif
#endif

// Knob: a potentiometer on A0 (D0 of the XIAO ESP32-C6)
#ifndef KNOB_PIN
#define KNOB_PIN A0
#endif

// ---------------- Application ----------------

static const uint8_t NOTE = 60;      // middle C
static const uint8_t VELOCITY = 100;
static const uint8_t CHANNEL = 0;    // 0 = MIDI channel 1
static const uint32_t DEBOUNCE_MS = 10;
static const uint8_t KNOB_CC = 20;   // Control Change number of the knob
static const uint32_t KNOB_MS = 5;   // how often the knob is read

static void led(bool on) {
  if (BASIC_LED_PIN >= 0) digitalWrite(BASIC_LED_PIN, on ? BASIC_LED_ON : !BASIC_LED_ON);
}

// Received MIDI (called from the mimuz-link receive task, not from loop())
static void onMidi(const uint8_t m[3], uint32_t rxUs) {
  (void)rxUs;
  if ((m[0] & 0x0F) != CHANNEL || m[1] != NOTE) return;  // only the button's note
  uint8_t type = m[0] & 0xF0;
  if (type == 0x90 && m[2] > 0) led(true);                          // Note On
  else if (type == 0x80 || (type == 0x90 && m[2] == 0)) led(false);  // Note Off (or Note On with velocity 0)
}

void setup() {
  Serial.begin(115200);
  pinMode(BASIC_BUTTON_PIN, INPUT_PULLUP);
  if (BASIC_LED_PIN >= 0) pinMode(BASIC_LED_PIN, OUTPUT);
  led(false);
  analogReadResolution(12);  // 0-4095

  mlink::Config cfg;  // defaults: device name "mi:muz:conESP32C6-<unique ID>", pairs with a bridge on first start
  cfg.model = "knob1";  // what kind of device this is: applications find clients of this kind by it
  mlink::onMidi(onMidi);
  mlink::Transport tr = mlink::begin(cfg);
  Serial.printf("knob1: ready (%s)\n", tr == mlink::TR_BLE ? "BLE-MIDI" : "ESP-NOW");
}

// The knob: smoothed, and sent only when it really moved (so that a resting knob sends nothing)
static void readKnob() {
  static int avg = -1;    // smoothed reading, 0-4095
  static int sent = -1;   // last value sent, 0-127
  static int sentAt = 0;  // the reading it was sent at
  int raw = analogRead(KNOB_PIN);
  avg = avg < 0 ? raw : (avg * 7 + raw) / 8;
  int v = avg * 127 / 4095;
  if (v == sent) return;
  if (sent >= 0 && abs(avg - sentAt) < 24 && v != 0 && v != 127) return;  // noise around a step
  sent = v;
  sentAt = avg;
  mlink::sendMidi(0xB0 | CHANNEL, KNOB_CC, v);
}

void loop() {
  static uint32_t knobMs = 0;
  if (millis() - knobMs >= KNOB_MS) {
    knobMs = millis();
    readKnob();
  }

  // Button, with a simple debounce: the level has to stay the same for DEBOUNCE_MS
  static bool pressed = false;
  static bool lastRead = false;
  static uint32_t changedMs = 0;
  bool now = digitalRead(BASIC_BUTTON_PIN) == LOW;
  uint32_t ms = millis();
  if (now != lastRead) {
    lastRead = now;
    changedMs = ms;
  } else if (now != pressed && ms - changedMs >= DEBOUNCE_MS) {
    pressed = now;
    if (pressed) mlink::sendMidi(0x90 | CHANNEL, NOTE, VELOCITY);
    else mlink::sendMidi(0x80 | CHANNEL, NOTE, 0);
  }

  // Config commands from the USB serial (lines starting with "@")
  while (Serial.available()) mlink::serialInput((char)Serial.read());

  mlink::loop();  // keeps the link alive, follows the bridge, searches again when the link is lost
  delay(1);
}
```

それぞれの部分の役目：

- Board settings（`#define`）：どれもビルドのときに変えられます。たとえば `--build-property "compiler.cpp.extra_flags=-DKNOB_PIN=A1"`。
- `#include <mimuz_link_client.h>`：ライブラリのクライアント側（名前空間 `mlink`）です。1 つのファイルからだけ include します。
- `cfg.model = "knob1"`：ステップ 1 のモデルです。クライアントがつながったときにブリッジがこれを受け取り、アプリケーションはこれでクライアントを見つけます。
- `mlink::begin(cfg)`：`setup()` で 1 回呼びます。ペアリング済みのブリッジに ESP-NOW でつなぎます。まだペアリングしていなければ、すぐ隣のブリッジとペアリングします。どのブリッジも応えなければ、BLE-MIDI デバイスになります。
- `mlink::sendMidi(status, d1, d2)`：メッセージを 1 つ送信の列に積んで、すぐに戻ります。チャンネルはステータスバイトに入れます。`0xB0 | 0` はチャンネル 1 の Control Change です。
- `onMidi`：PC からのメッセージごとに呼ばれます。呼ぶのは `loop()` ではなく、ライブラリの **受信タスク** です。短く済ませ、中で待たないでください。
- `mlink::serialInput()`：USB シリアルで `@` から始まる行は、設定コマンドです（コンソールの **Load device (serial)** と `examples/pc/mlink_config.py` が使います）。
- `mlink::loop()`：`loop()` からいつも呼びます。接続を保ち、ブリッジの無線チャンネルに付いていき、ブリッジが無線で中継してきた設定コマンドを実行します。
- つまみの値はならしてから、ある程度動いたときだけ送ります。止まっているつまみは何も送りません。

関数の一覧は [Arduino ライブラリの API](../arduino-library.md)（英語）にあります。

## ステップ 5：ビルドして書き込む

XIAO ESP32-C6 を PC につなぎ、`arduino-cli board list` でポートを調べます。それから、`examples/client` で：

```sh
arduino-cli compile -u -p <port> --libraries ../../libs \
  --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app knob1
```

- `PartitionScheme=huge_app`：ファームウェアに Wi-Fi（ESP-NOW）と BLE の両方が入るので、既定のパーティションには収まりません。
- `CDCOnBoot=cdc`：`Serial` を USB のポートにします。設定コマンドはここに届きます。
- Arduino IDE の場合：ボードは **XIAO_ESP32C6**、**USB CDC On Boot: Enabled**、**Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)**。
- 新品の XIAO ESP32-C6 は、最初の書き込みのあと、一度抜いて挿し直す必要があります。

Web サイトの Firmware uploader が書き込めるのは、このリポジトリのファームウェアだけです。自分のファームウェアは、arduino-cli か Arduino IDE で書き込んでください。

## ステップ 6：ペアリングして確かめる

1. ブリッジを動かしたまま、クライアントをそのすぐ隣に置いて電源を入れます（[チュートリアル 1 のステップ 4](tutorial-quick-start.md#ステップ-4ペアリング) と同じです）。このブリッジとすでにペアリングしたクライアント（たとえば basic の example を入れていたもの）は、そのままペアリングされています。ファームウェアを書き直しても、ペアリングは消えません。
2. **Console** のページで、ブリッジの **Clients** タブに、モデル **knob1** のクライアントが表示されます。
3. つまみを回すと、クライアントのポートに Control Change 20 が届きます（`examples/node/monitor.mjs` では `b0 14 <値>`）。ボタンを押すと Note On / Note Off 60 です。
4. クライアントのポートに Note On 60 を送ると、LED が点きます。

クライアント自身の USB シリアルからも確かめられます。**Console** のページで **Load device (serial)** を押し、そのポートを選びます。**Overview** に `info` が表示され、その中に `model=knob1` があります。

## ステップ 7：デバイスに保存する設定

1 台ごとの設定はデバイスに保存され、コンソールやアプリケーションから [設定コマンド](../config-commands.md)（英語）で変えます。

- `label`：その 1 台に付ける名前です（チュートリアル 1 のステップ 6）。knob1 が何台かあるときに使います。
- `rx_channels`：同じブリッジの **ほかのクライアント** から、このクライアントも受け取る MIDI チャンネルです（たとえば、鍵盤のクライアントがチャンネル 1 で送るものを鳴らす音源）。PC からクライアントのポートに送ったメッセージは、いつでも届きます。knob1 には、ほかの設定は要りません。

`set` で行った設定はクライアントに保存され、ファームウェアを書き直しても残ります。

## ステップ 8：Web ページ用のドライバー

アプリケーションは、クライアントのポート番号を使いません。ポートは、クライアントがつながった順で決まるからです。代わりに、モデルごとの小さな **デバイスドライバー** を使います。ドライバーは、モデル（とラベルやデバイス名）でデバイスを見つけ、つながり直しても付いていき、MIDI メッセージをデバイスにとっての意味に置き換えます。

デバイスドライバーは、Web ドライバー `web/www/drivers/mimuz-link.mjs` の `MimuzDevice` を継承して作ります。MIDI へのアクセス、デバイスを見つけること、送信は基底クラスが受け持ち、ドライバーには knob1 に固有のことだけを書きます。テンプレート `web/www/drivers/device-template.mjs` から始めます。

```sh
cp web/www/drivers/device-template.mjs web/www/drivers/knob1.mjs
```

参考：`web/www/drivers/basic-device.mjs` は、basic の example（チュートリアル 1）のドライバーで、同じようにテンプレートから作ったものです。続けて、テンプレートの先頭のコメントにあるとおり、`knob1.mjs` を直します。

1. クラス名を `Template` から `Knob1` に変え、`static model` を `'knob1'`（スケッチの `cfg.model`）にします。
2. `CHANNEL = 1` はそのままにして、ステップ 1 の番号（`KNOB_CC`、`BUTTON_NOTE`）を足します。
3. `onMidi()` の example のイベントを、`knob` と `button` に置き換えます。
4. example のメソッドを、`setLed()` に置き換えます。
5. 先頭のコメントを、ドライバーのイベントとメソッドの説明に書き換えます。

できあがり：

```js
// knob1.mjs: device driver for the "knob1" clients (made from device-template.mjs)
//   events: knob ({ value }), button ({ pressed }), and those of MimuzDevice (connect, disconnect, change, midi)
//   methods: setLed(on), and those of MimuzDevice
import { MimuzDevice } from './mimuz-link.mjs';

const CHANNEL = 1;      // the MIDI channel the device uses (1-16)
const KNOB_CC = 20;
const BUTTON_NOTE = 60;

export default class Knob1 extends MimuzDevice {
  static model = 'knob1';

  // Every message from the device: the knob and the button become events
  onMidi(data) {
    const [status, d1, d2] = data;
    if (status >= 0xF0 || (status & 0x0F) !== CHANNEL - 1) return;  // only channel messages on CHANNEL
    const type = status & 0xF0;
    if (type === 0xB0 && d1 === KNOB_CC) this.emit('knob', { value: d2 });
    else if ((type === 0x90 || type === 0x80) && d1 === BUTTON_NOTE) this.emit('button', { pressed: type === 0x90 && d2 > 0 });
  }

  // The LED follows Note On / Note Off of the button's note
  setLed(on) {
    return on ? this.noteOn(CHANNEL, BUTTON_NOTE, 127) : this.noteOff(CHANNEL, BUTTON_NOTE);
  }
}
```

- `static model = 'knob1'`：ドライバーは、このモデルの、最初につながったクライアントに付いていきます。どのブリッジのどのポートにいてもかまいません。
- `onMidi(data)` は、デバイスからのメッセージごとに呼ばれます。ドライバーはステップ 1 のメッセージだけを読み、自分のイベントとして伝えます（`emit`）。それ以外は無視します。
- `setLed` は、基底クラスの送信のメソッド（`noteOn`、`noteOff`、`controlChange`、…。チャンネルは 1〜16）を使います。
- 基底クラスには、ほかに `connected`、`label`、`name`、`command()`（設定コマンド）、`setLabel()` と、イベント `connect`、`disconnect`、`midi` があります。

アプリケーションが読み込むのは `knob1.mjs` だけで、`mimuz-link.mjs` を意識する必要はありません。基底クラスの説明は [Device drivers (MimuzDevice)](../js-driver.md#device-drivers-mimuzdevice)（英語）にあります。

## ステップ 9：ドライバーを試す

試験用のページ `web/www/knob1.html` を作ります。

```html
<!doctype html>
<meta charset="utf-8">
<title>knob1 test</title>
<p id="state">Waiting for a knob1…</p>
<p>Knob: <meter id="knob" min="0" max="127" value="0"></meter> <span id="value">–</span></p>
<p>Button: <span id="button">released</span></p>
<label><input type="checkbox" id="led"> LED</label>
<script type="module">
  import Knob1 from './drivers/knob1.mjs';

  const $ = (id) => document.getElementById(id);
  const knob = new Knob1();
  knob.addEventListener('connect', () => { $('state').textContent = `Connected: ${knob.label || 'knob1'}`; });
  knob.addEventListener('disconnect', () => { $('state').textContent = 'Waiting for a knob1…'; });
  knob.addEventListener('knob', (e) => { $('knob').value = e.detail.value; $('value').textContent = e.detail.value; });
  knob.addEventListener('button', (e) => { $('button').textContent = e.detail.pressed ? 'pressed' : 'released'; });
  $('led').addEventListener('change', (e) => knob.setLed(e.target.checked));
  try {
    await knob.open();   // the browser asks for MIDI (with SysEx)
  } catch (e) {
    $('state').textContent = e.message;
  }
</script>
```

- `new Knob1()` は、最初の knob1 を使います。何台かあるときは、`new Knob1({ label: 'left' })` でラベルを指定して選びます。
- `open()` は MIDI へのアクセスを求めます。1 つのページの何個かのドライバー（たとえば knob1 とパッド）は、MIDI へのアクセスを共有するので、ブラウザーが聞いてくるのは 1 回だけです。MIDI が許可されなかったときは、`open()` が理由を付けて reject します。
- リスナーは `open()` の前に付けても後に付けてもかまいません。どちらでも、`connect` イベントは `open()` のあとに来ます。

`npm start` を動かしたまま、http://localhost:8080/knob1.html を開き、MIDI を許可します。knob1 がつながると、つまみを回せばメーターが動き、ボタンの状態が表示され、チェックボックスで LED が切り替わります。

同じドライバーは、Node.js 20 以降でも動きます（[Node.js](../js-driver.md#nodejs)（英語）を参照）。`open()` に MIDI のライブラリを渡し、最後に `close()` を呼びます。MIDI のポートが開いていると、プロセスが終わらないためです。

```js
import Knob1 from '../../web/www/drivers/knob1.mjs';

const knob = new Knob1();
knob.addEventListener('knob', (e) => console.log('knob', e.detail.value));
await knob.open({ nodeMidi: await import('@julusian/midi') });
process.on('SIGINT', () => knob.close());
```

`examples/node/` に置き（`npm install` で `@julusian/midi` が入っている場所）、Ctrl-C で止めます。

## 気をつけること

- **BLE-MIDI のとき**（ブリッジに届かないとき）、アプリケーションに見えるのはデバイス名だけです。モデルもラベルもなく、設定コマンドも使えません。`model: 'knob1'` で探すドライバーが見つけられるのは、ブリッジを通したときだけです。
- `onMidi` は、ライブラリの受信タスクで動きます。`loop()`（やほかのタスク）と共有するデータは、そのことを意識して扱ってください。ピンやフラグを設定するだけなら問題ありません。
- `sendMidi` は、どのタスクからでも呼べます。たとえば、スイッチの多いデバイスの 1 ms のスキャンのタスクからでもかまいません。呼んでも待たされません。送信の列（128 件）がいっぱいのときは、メッセージを捨てて `false` を返します。
- 続けて送ったメッセージは、1 つの無線パケットにまとめて送られます。届かなかった Note Off は送り直します。長く待たされた Note On（既定で 80 ms）は、遅れて鳴らす代わりに捨てます。
- ライブラリは、メッセージを 1 つずつ別々に扱います。Note On と Note Off を組にしては扱いません。

## 次へ

- [Arduino ライブラリの API](../arduino-library.md)（英語）：ライブラリのすべての関数、ブリッジ側、ビルドのオプション。
- [Web ドライバーの API](../js-driver.md)（英語）：Web ページと Node.js での、ブリッジ、クライアント、コマンド、イベント。
- [設定コマンドと SysEx](../config-commands.md)（英語）：クライアントとブリッジのすべての設定。
