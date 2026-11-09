# Context-Aware Hearing Glasses — AIスマートメガネ型補聴器

[한국어](README.md) | **日本語**

漢陽女子大学校 ソフトウェア融合科 2026年度 キャップストーンデザイン · チーム 들어조

加齢性難聴の方のためのメガネ型補聴器の試作品です。メガネに取り付けたESP32-S3が3つのマイクとカメラで周囲の音と光景を集め、スマートフォンアプリのAIモデルが今の状況を判断して補聴器のモードを切り替えます。

> ソースコード内のコメントと `docs/` のドキュメントは韓国語で書かれています。

| 上から見た試作品（下側がメガネの前面） | 正面 |
|:---:|:---:|
| <img src="docs/images/prototype_top.jpg" width="320"> | <img src="docs/images/prototype_front.jpg" width="320"> |

マイクは前側の左右（ヒンジ付近）に2個、左のテンプル（つる）の先端に1個、合計3個です。上から見た写真では、右上のマイクが左テンプルの先端にあります。

## 主な機能

1. **音の方向推定 + ビームフォーミング**: INMP441マイク3個（前側の左右に2個 + 左テンプルの先端に1個）を48 kHzで同時にキャプチャし、GCC-PHAT/SRP-PHATで音の方向を求めます。delay-and-sumビームフォーミングの後に16 kHzモノラルへダウンサンプリングし、方向情報と一緒にスマートフォンへ送ります。
2. **状況認識によるモード切り替え**: 画像分類（MobileNetV2、6クラス）と音分類（Edge Impulse MFE+CNN、3クラス）の結果をルールベースで組み合わせ、屋内基本 / 屋外基本 / テレビ視聴 / 1対1の会話 / 多人数の会話 のモードを選びます。
3. **視界の外の危険音の通知**: 危険な音がすると、メガネのOLEDにアイコンと方向の矢印を表示します。
4. **音声コマンド（PTT）**: メガネのボタンを押して話すと、スマートフォンが音声をテキストに変換し、LLMが意図を分類して音量やモードを変更します。

イヤホン出力（ローカルのパススルー）は、DAC（PCM5102A）を確保し、これから実装する予定です。

## システム構成

```
[メガネ: ESP32-S3]                                    [スマートフォン: Androidアプリ]
 INMP441 ×3 ─ I2S ─ 3chキャプチャ → 方向推定           ┌ 音分類 (Edge Impulse, 2秒窓)
                     → ビームフォーミング               ├ 画像分類 (MobileNetV2)
                     → 16 kHzモノラル (IMA ADPCM)      ├ ルールベースの組み合わせ → モード決定
 OV2640 ──── DVP ─── 周期的なJPEGキャプチャ           └ 音声コマンド: スマホSTT → LLM意図分類
 ボタン(PTT) ─ GPIO ─ イベント                                  │
                         │                                     │
                         └──── BLE 5.0 (製品構成) ──────────────┘
                               WiFi (実験・デバッグ用)
 OLED ◄── コマンド受信: SET_MODE / SET_VOLUME / DISPLAY_ALERT
```

- 製品構成の通信は、インターネットなしで使えるBLE 5.0です。WiFiビルドはスループットの比較実験とデバッグに使います。
- ファームウェアはPlatformIOの環境2つ（`env:ble`、`env:wifi`）に分けてビルドします。
- 通信フレーム: `[A5 5A][type][flags][seq u16][len u16][payload][crc16]`（音声・画像・イベント・コマンド・PINGで共通）

## リポジトリ構成

| パス | 内容 |
|---|---|
| `firmware/` | ESP32-S3ファームウェア（音声キャプチャ、DSP、カメラ、BLE/WiFi送信、OLED UI） |
| `android/` | 統合Androidアプリ（受信、音・画像分類、モード決定、音声コマンド） |
| `ptt_module/` | 音声コマンドの意図分類の単体モジュール（アプリに移植する前のオリジナル） |
| `image_model/` | 画像分類モデル（`imageModel.tflite`）とPCカメラのデモ |
| `sound_model/` | Edge Impulse音分類モデルのパラメータとTFLiteモデル |
| `tools/` | PCツール: 受信機、方向推定・ビームフォーミングのプロトタイプ、同期検証、評価スクリプト |
| `docs/` | 要求分析仕様書、配線ガイド、試験文書、モデルの意思決定記録 |
| `cmd_list.md` | ビルド・シリアルコマンド・PCツールの使い方のまとめ |

## ビルドと実行

### 秘密情報の設定（最初に）

LLMゲートウェイのキーはリポジトリに含めていません。リポジトリのルートに `.env` を作成しておくと、2つのAndroidプロジェクトがビルド時に読み込みます。

```bash
cp .env.example .env
# .env を開き、LLM_API_KEY= の後ろにキーを書きます（.env は .gitignore で除外されています）
```

### ファームウェア

[PlatformIO Core](https://platformio.org/install/cli) が必要です。プラットフォームは pioarduino 55.03.311（Arduino-ESP32 3.3.11 + ESP-IDF 5.5.5）に固定しています。ボードはESP32-S3 N16R8です。

```bash
pio run -d firmware -e ble  -t upload   # 製品構成 (BLE)
pio run -d firmware -e wifi -t upload   # 実験・デバッグ (WiFi)
pio device monitor -b 115200
```

WiFiビルドでは、シリアルで `wifi <SSID>;<パスワード>` と入力して接続情報を保存します（NVS）。コードには含まれていません。

### Androidアプリ

`android/` をAndroid Studioで開くか、Gradle 9.3.1以降でビルドします（AGP 8.13、minSdk 26）。

```bash
gradle -p android assembleDebug
# 出力: android/app/build/outputs/apk/debug/app-debug.apk
```

`ptt_module/` は同梱のGradle Wrapperでビルドします（`./gradlew assembleDebug`）。

アプリの［🖼 컬러 vs 흑백］（カラー vs モノクロ）テストボタンは `android/app/src/main/assets/test_img1~3.jpg` を使います。元の写真には人が写っているため、リポジトリから除外しました。自分で撮った写真を入れると動作します。ファイルがない場合は「시험 실패」（テスト失敗）のログが出るだけです。

### PCツール

Python 3.10以上。スクリプトごとに必要なパッケージが異なり、主に `numpy`、`scipy`、`pyserial`、`bleak`、`ai-edge-litert` を使います。

```bash
python tools/receiver.py        # WiFi UDP音声の受信 → WAV保存、ドロップ統計
python tools/ble_receiver.py    # BLE音声の受信
python tools/doa_prototype.py   # 3チャンネルWAVで方向推定を検証
```

詳しい使い方は `cmd_list.md` にあります（文書内の絶対パスは開発PC基準です）。

## 測定結果（試作段階）

| 項目 | 結果 | 条件 |
|---|---|---|
| 音の方向推定の誤差（8方向） | 平均 9.15°、最大 15.15° | 2026-09-10、**以前の配置である14 cmの正三角形**で測定。メガネ装着、音源まで1 m、PC（Python）で計算、方向ごとに録音1件、独立検証前 |
| BLE音声の損失率（干渉あり、単独送信） | PCM 28.4% → IMA ADPCM 0.00% | 受信側はPC（BLEドングル + bleak）、干渉源は同じPCの2.4 GHzホットスポット |
| BLE音声+画像の同時送信（干渉なし） | ADPCM 0.00%、PCM 2.84% | 同じ受信条件、セルごとに1〜2回測定 |
| 画像分類の精度 | 95%（元画像250枚）/ 72.3%（Web独立セット65枚） | 250枚は学習データを含む評価。独立セットでは危険シーンが0/6 |
| 音分類の精度 | テスト 88.3%、危険音の再現率 84% | Edge Impulseのテストセット、float32モデル基準 |

これらの数値は試作段階の小規模な測定です。現在のマイク配置での方向誤差、スマートフォン実機での精度、BLE/WiFi比較実験の全体はまだ測定していません。

## チーム

| 名前 | 担当 |
|---|---|
| チョン・ヒョウォン（정효원） | ファームウェア・ハードウェア、アプリ統合、最終整理 |
| アン・ホヨン（안호연） | 音分類モデル |
| チェ・ダヒョン（최다현） | 画像分類モデル |
| ソン・チェウォン（송채원） | 音声コマンドの意図分類 |

## 過去の作業の保管（`archive/*` タグ）

初期の実験ブランチは、整理の際にタグとして残しました。これらのタグの履歴はmainにマージされているため、チームメンバーのコミットもそのまま表示されます。公開前に、ハードコードされていたAPIキーを履歴から削除しました。

| タグ | 元のブランチ | 内容 |
|---|---|---|
| `archive/2026-04-skeleton` | feature/Awareness | 初期フォルダ構成の骨組み |
| `archive/2026-05-app-fastapi-whisper` | feature/app | 初期アプリ + FastAPI（Whisper STT + LLMによる状況分析） |
| `archive/2026-05-app-fastapi-gemini` | feature/app2 | アプリ + FastAPI（Gemini STT + 状況分析） |
| `archive/2026-05-image-teachable-machine` | feature/image_analysis | Teachable Machineによる画像の状況判断 |
| `archive/2026-06-firmware-max4466-recording` | feature/Firmware | MAX4466アナログマイクの録音プロトタイプ |
| `archive/2026-06-firmware-3mic-oled` | feature/firmPractice | アナログマイク3個による方向推定 + OLED |
| `archive/2026-06-sound-learning-app` | feature/SoundLearningTest | 音の学習テスト用アプリ |
| `archive/2026-07-sound-edge-impulse-app` | SmartHearing_Edge | Edge Impulse SDKを使った音分類アプリ |
| `archive/2026-08-image-camera-mobilenet` | feature/camera | MobileNetV2画像モデル + カメラのデモ |
| `archive/2026-09-sound-model-v5` | AIModel | Edge Impulse音モデルの配布版 v5（SDKを含むzip） |

```bash
git fetch --tags
git checkout archive/2026-06-firmware-3mic-oled
```
