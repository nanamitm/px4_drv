# librtlsdr (RTL-SDR Blog V1.4.0)

px4-cused の RTL-SDR (RTL2838UHIDIR) で、RTL2832U と R820T を設定するために組み込む librtlsdr です。  
WinUSB 版の helper が使う `rtlsdr.dll` と同じ版で、同じ機器への設定の要求列を Linux 版でも使います。

| 項目 | 値 |
|---|---|
| 入手元 | https://github.com/rtlsdrblog/rtl-sdr-blog/tree/V1.4.0 |
| ソースの ZIP | https://github.com/rtlsdrblog/rtl-sdr-blog/archive/refs/tags/V1.4.0.zip |
| ZIP の SHA-256 | `EFAAC50D53EA2F12D9853167AC9470BF4F04DE944F17C5D52D2FD73A9EC24C18` (`winusb/tools/rtlsdr/runtime-lock.json` の `source_sha256` と同じ) |
| ライセンス | GPL-2.0-or-later (`COPYING`)。px4-cused (GPL-2.0-only) へ組み込める |

ZIP の `src/` からライブラリーの本体 (`librtlsdr.c`、`tuner_*.c`) を、`include/` からすべてのヘッダーを、そのほかに `COPYING` と `AUTHORS` を置いています。  
コマンド (`rtl_sdr` など) とビルドの設定は使わないため置いていません。

## 変更点

上流の `rtlsdr_open()` は自分で libusb を初期化し、番号で機器を探して開きます。  
px4-cused は、ほかの用途の RTL2838 を開かないよう接続位置で選んだ機器だけを開き、カーネルのドライバーを外して interface 0 を取ってから librtlsdr へ渡すため、次の変更を加えています。  
どの変更にも `px4-cused` の注釈があります。

- `rtlsdr_open_handle()` を加えた。呼び出し側が開いて interface 0 を取ったハンドルで、`rtlsdr_open()` の interface を取った後と同じ初期化 (RTL2832U の初期化、チューナーの判定と初期化) を行う。`rtlsdr_open()` は同じ処理 (`rtlsdr_open_impl()`) を呼ぶ形にして、動作は変えていない
- `rtlsdr_close()` は、`rtlsdr_open_handle()` で開いた場合に RTL2832U を休止させるところまで行い、interface の解放、カーネルのドライバーの再接続、ハンドルの close、libusb の終了は呼び出し側へ残す

上流との差分は、ZIP を展開した `src/librtlsdr.c` と `include/rtl-sdr.h` との `diff` で確かめられます。  
I/Q の取得は、上流の `rtlsdr_read_async()` (自分のイベントのループを持つ) を使わず、デーモンの libusb の非同期 Bulk 転送で、上流の既定と同じ本数と大きさ (15 本、256 KiB) を使います。
