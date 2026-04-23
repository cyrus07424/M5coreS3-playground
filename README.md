# M5coreS3-playground

M5coreS3 向けの Arduino デモ集です。Arduino IDE で開いてそのまま試せる、最小構成のサンプルを置いています。

## 含まれているデモ

| Project | 内容 | 主な外部ライブラリ |
| --- | --- | --- |
| `AnalogClockDemo` | クロノグラフアナログ時計 | `M5Unified` |
| `NekoFlightAdvancedDemo` | `NekoFlight` を移植した 3D フライト / 戦闘デモ(拡張版) | `M5Unified` |
| `PxtonePlayerDemo` | SD カード上の `*.ptcop` / `*.pttune` を選んで再生するpxtone プレイヤー | `M5Unified`, `SD`, `SPI` |

## 共通前提

- ボード設定: `m5stack:esp32:m5stack_cores3`
- ライブラリはできるだけ Arduino IDE のライブラリマネージャからインストール
- 各プロジェクトの詳しい手順は、それぞれの `README.md` を参照
