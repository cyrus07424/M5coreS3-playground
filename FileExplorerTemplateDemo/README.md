# FileExplorerTemplateDemo

`PxtonePlayerDemo` のファイル選択処理だけを抜き出して、汎用のファイルエクスプローラー雛形にした Arduino デモです。

## できること

- microSD 内のディレクトリ移動
- ディレクトリ優先・名前順での一覧表示
- 行タップまたは `OPEN` ボタンでディレクトリを開く
- ファイル選択時に詳細情報を表示

## 表示する詳細情報

- ファイル名
- 拡張子
- サイズ
- 親ディレクトリ
- フルパス

## 必要なライブラリ

- M5Unified
- SD
- SPI

## Arduino IDE 設定

- 開くファイル: `FileExplorerTemplateDemo.ino`
- ボード: `m5stack:esp32:m5stack_cores3`

## 使い方

1. microSD を M5CoreS3 に挿して起動する
2. ブラウザ画面では行タップで選択し、選択済み行を再タップするか `OPEN` を押す
3. ディレクトリならそのまま移動し、ファイルなら詳細画面を表示する
4. 詳細画面では `BACK` で一覧へ戻り、`REFRESH` で情報を再読込する
