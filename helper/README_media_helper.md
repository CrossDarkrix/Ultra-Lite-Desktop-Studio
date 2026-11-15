# Media Helper (メディアメタデータ橋渡し)

C++ 版は MinGW から WinRT (GSMTC) を直接叩けないため、メディア情報
(曲名・アーティスト・ジャケット画像) の取得だけを Python に切り出した
ヘルパー。

## 仕組み

- C++ 本体が、メディアウィジェットまたはジャケット円盤を使う
  ビジュアライザスタイルが表示されている間だけ、本ヘルパーを
  QProcess で起動する (消費者不在なら起動しない・終了させる)
- ヘルパーは GSMTC を 1 秒間隔でポーリング (取得方法は Python 版
  runtime.py の実績ある経路そのまま) し、**変化があった時だけ**
  stdout に JSON を 1 行流す
- 曲の進行 (position) は timeline 行として最大 2 秒間隔で別途流す
- C++ が stdin を閉じる / 終了するとヘルパーも終了する (孤児化防止)

## 動作確認 (Windows 実機)

前提: Python 3.10+ と winsdk が入っていること
(無ければ `pip install winsdk`)

```
py media_helper.py
```

音楽を再生すると `{"type": "ready"...}` の後に曲情報の JSON 行が
出るはず。曲を変える/一時停止すると、その都度 1 行出る。
Ctrl+C で終了。

## パッケージング (配布用) — onedir 方式 

**onefile は使わない。** onefile は起動のたびにバンドル全体を
%TEMP% へ展開するため、HDD 環境やウイルス対策のスキャンが絡むと
起動が重くなる。本ヘルパーは「必要な時だけ起動」する設計なので、
展開なしで即起動できる **onedir (フォルダ形式)** が正解。

完成したフォルダを **`helper/MediaHelper/`** に置くと、本体ビルド時に
exe の隣へ自動コピーされ、`dist` の zip にも同梱される (CMake 済み)。
配置イメージ:

```
LiteDesktopStudioCpp.exe
MediaHelper/
  LiteDesktopStudio-MediaHelper.exe   (Nuitka: media_helper.exe をリネーム)
  python311.dll, winsdk/ ... (ビルダーが生成する一式)
```

### 第1候補: Nuitka (standalone)

```
pip install nuitka
cd helper
python -m nuitka --standalone --assume-yes-for-downloads ^
  --include-package=winsdk --output-dir=build_nuitka media_helper.py
```

生成される `build_nuitka/media_helper.dist/` の中身を
`helper/MediaHelper/` にコピーし、exe 名を
`LiteDesktopStudio-MediaHelper.exe` に揃える。
注意: winsdk は .winmd データを同梱する。認識されない場合は
`--include-package-data=winsdk` を追加する。

### 代替: PyInstaller (onedir)

```
pip install pyinstaller
cd helper
pyinstaller --onedir --noconsole --name LiteDesktopStudio-MediaHelper media_helper.py
```

`dist/LiteDesktopStudio-MediaHelper/` の中身を `helper/MediaHelper/` へ。

どちらのビルダーでも、`--noconsole` でも親 (C++) がパイプで起動すれば
stdout は使える (スクリプト冒頭の `_ensure_stdout` が fd 1 を開き直す)。
実機での最終確認: 完成した exe をターミナルから単体起動し、音楽再生で
JSON 行が出ること。

## 実機出力の検証 (2026-11-14, test.json)

実機 (foobar2000 再生中) の出力を検証し、動作を確認:
ready 行 + media 行 (title/artist/app=foobar2000.exe、サムネイル
1.8MB PNG を正しく検出)。その際に見つけたヘルパー側バグ2件を修正済み:

1. **タイトルの日本語が文字化け** — 原因は stdout の既定エンコーディングが
   cp932 になること。stdout を UTF-8 に強制するよう修正
2. **status が "4" の生数値** — この winsdk は enum を数値文字列で返す。
   int→名前マップで正規化するよう修正 (4 = Playing で再生中だった)

バグではないもの: position/duration が 0 で album が空なのは
foobar2000 の GSMTC 実装が timeline と album を提供していないため
(プレイヤー依存。C++ 側は 0 = 進行情報なしとして扱う)。

→ 再ビルド版を同日受領し差し替え済み。新 test.json で
「Doubt & Trust 〜ダウト&トラスト〜」が正しい UTF-8 で届き、
status も "Playing" になることを確認。**ヘルパーは完成**。
残るは C++ 受け口のみ (v0.7)。

## ビルド済みの受領記録

- 2026-11-14 **Nuitka standalone** でビルドし `helper/MediaHelper.zip`
  (21.7MB) として納品。こちらで `helper/MediaHelper/` へ展開済み (65MB)。
  exe 内に winsdk.windows.media.control のラッパがコンパイルされ、
  `winsdk/_winrt.pyd` が隣にあることを確認 (.winmd は winsdk の設計上不要、
  メタデータは Windows 本体から読む)
- zip 内の exe 名は `media_helper.exe` だったため、展開時に
  `LiteDesktopStudio-MediaHelper.exe` へリネームした (今後のビルドでは
  Nuitka の `--output-filename=LiteDesktopStudio-MediaHelper.exe` で
  最初から揃えられる)
- exe はコンソールサブシステム。C++ から起動する際はコンソール窓が
  一瞬出ないよう CREATE_NO_WINDOW を付けること (受け口側の作業)

## C++ 側の受け口 (未実装、v0.7 バッチで実施)

- `MediaHelperClient` (QProcess 管理 + 行単位 JSON パース) を新設。
  起動時は CREATE_NO_WINDOW を付ける (上記の通り)
- メディアウィジェット: 曲名/アーティスト/状態/進行バーを表示
- ビジュアライザ円形系: thumbnail_b64 を QImage 化して円盤に描画
- ヘルパーが無い環境 (exe 未同梱など) では従来通り単色ディスク+操作のみ

---

## 操作専用ヘルパー (MediaControlHelper) — 2026-11-15 追加

メディアキー合成 (keybd_event / SendInput) が実機でプレイヤーを動かせなかったため、
トランスポート操作は **GSMTC セッション直接操作** に切り替えた (
メタデータ用とは**別ヘルパー**にした)。`media_control_helper.py` が対象で、
引数 (`play` / `pause` / `play_pause` / `next` / `prev` / `stop`) を受けて
現在セッションの `try_*_async()` を1回呼んで終了する常駐しない exe。

### ビルド (Nuitka standalone、メタデータ用と同じ要領)

```
python -m nuitka --standalone --assume-yes-for-downloads ^
  --include-package=winsdk --output-dir=build_nuitka_control media_control_helper.py
```

生成される `build_nuitka_control/media_control_helper.dist/` の中身を
`helper/MediaControlHelper/` フォルダに入れ、exe を
`LiteDesktopStudio-MediaControlHelper.exe` にリネームする。
配置後は CMake が exe 隣へのコピーと dist zip 同梱を自動で行う。

C++ 側の優先順位: MediaControlHelper があれば GSMTC 経路、無ければ
メディアキー合成にフォールバックする。
