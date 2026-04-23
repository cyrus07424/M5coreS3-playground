# PxtonePlayerDemo

`libpxtone` を同梱し、M5CoreS3 で `pxtone collage` 形式の楽曲を microSD から選んで再生する Arduino デモです。

## 対応

- `*.ptcop`
- `*.pttune`
- SD カード内のディレクトリ移動と全ファイル表示
- 再生中のシークバー表示

## 制約

- 再生は **mono / 44100Hz** です
- 内部の音色展開もこの出力設定に合わせて行います
- 読み込み後は PCM をメモリ上に展開して再生します。PSRAM が使える環境ではそちらを優先して利用します
- `libpxtone` の OGG Vorbis 系音色はこのデモでは有効化していません
- CoreS3 では PSRAM が使える場合、ノート保持バッファを空き PSRAM に応じて自動で拡張します

## 必要なライブラリ

- M5Unified
- SD
- SPI

`libpxtone` 本体のソースはこのデモに同梱しています。

## Arduino IDE 設定

- 開くファイル: `PxtonePlayerDemo.ino`
- ボード: `m5stack:esp32:m5stack_cores3`

## 使い方

1. microSD に `*.ptcop` または `*.pttune` を入れて M5CoreS3 に挿す
2. Arduino IDE で書き込んで起動する
3. ブラウザ画面では行タップで選択し、選択済み行を再タップするとディレクトリを開く / 対応楽曲を再生する
4. 画面下の `UP / DOWN / OPEN / BACK` ボタンでもブラウズ操作できる
5. ブラウザ画面では全ファイルを表示し、非対応ファイルは `*` 付きで表示する
6. 再生画面では下部の `BACK / PLAY(PAUSE) / RESTART` ボタンで操作する

## 同梱ライブラリ

- `libpxtone`: https://github.com/Wohlstand/libpxtone
- ライセンス: `libpxtone-LICENSE.txt`
