# PiyoPiyoPlayerDemo

microSD 上の `*.pmd` を選んで再生する Arduino デモです。

## 対応

- `*.pmd`
- SD カード内のディレクトリ移動と全ファイル表示
- 再生 / 一時停止 / 再スタート

## 実装メモ

- `PiyoPiyo` の 3 メロディトラックと 1 パーカッショントラックを読み込みます
- 再生前に PCM をメモリへ展開してから再生します
- ループ付き楽曲も読み込めますが、このデモでは **1 周分をワンショット再生** します
- 打楽器は M5CoreS3 向けに軽量な合成音で近似しています

## 必要なライブラリ

- M5Unified
- SD
- SPI

## Arduino IDE 設定

- 開くファイル: `PiyoPiyoPlayerDemo.ino`
- ボード: `m5stack:esp32:m5stack_cores3`

## 使い方

1. microSD に `*.pmd` を入れて M5CoreS3 に挿す
2. ブラウザ画面で行をタップして選択し、選択済み行を再タップするか `OPEN` を押す
3. 再生画面で `BACK / PLAY(PAUSE) / RESTART` を使って操作する
