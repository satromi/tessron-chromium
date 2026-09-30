# tessron-chromium

TessronOSの小物「ブラウザ」です。Chromium 153(Blink、V8、Skia)をベースに、TessronOSのプロセスとして動くように移植しています。

TessronOSのイメージを作るときに、`make BROWSER=1`で取り込みます。設計はTessronOSの`docs/tessronos/`にあります。

## 置き場

このリポジトリは、TessronOSのリポジトリと同じディレクトリに置きます(既定は`C:\home\TessronOS`と`C:\home\tessron-chromium`)。

別の場所に置くときは、TessronOSのmakeに`CHROMIUM=<このリポジトリ>`を、スクリプトを直接実行するときは環境変数`TESSRONOS=<TessronOSのリポジトリ>`を指定してください。

| ディレクトリ | 内容 |
| --- | --- |
| `application/browser/` | アプリケーション本体と、ChromiumをTessronOSに載せる移植層(`port/`) |
| `tools/browser/` | ChromiumのソースをTessronOSのツールチェーンでコンパイルし直すスクリプト、除外の一覧、Chromiumのソースへのパッチ |
| `build_make/browser.mk` | TessronOSのmakefileが`make BROWSER=1`のときに読む規則 |
| `tests/uprog/` | V8とSkiaの試験プログラム(TessronOSの試験`ktest_v8`が実行する) |

Chromium本体はこのリポジトリに含めていません。WSLのホームのチェックアウト(`~/cr6/src`)を読み、コンパイルし直した生成物は`~/tessronos-obj`に置いています。

## ビルド

```sh
cd tools/browser
bash build.sh gngen       # 最初に一度: aarch64のビルドの記述
bash build.sh hostgen     # 最初に一度: 生成するソース
JOBS=14 bash build.sh v8  # V8
JOBS=6 bash blink.sh all  # Blink
bash rust.sh build && bash rust.sh merge   # Rustの部分
bash ffmpeg.sh            # FFmpeg
cd ../../../TessronOS/build_make
make TARGET=_QEMU_VIRT_ DESKTOP=1 BROWSER=1 BROWSER_BLINK=1
```

## ライセンス

このリポジトリのファイルは、MITライセンス(`LICENSE`)で配布しています。取り込み先のTessronOSはT-License 2.2です。

`tools/browser/patches`のパッチはChromiumのソースへの差分なので、Chromiumのライセンス(BSD 3条項)に従います。

ブラウザを組み込んだイメージには、Chromiumとその第三者コンポーネント、FFmpegなどが含まれます。その一覧と条件は、TessronOSの`THIRD_PARTY_NOTICES.md`にまとめています。
