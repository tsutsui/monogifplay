# monogifplay wscons 直接描画版 実装設計書

## 1. 目的

既存の `monogifplay` が行っている以下の処理を流用する。

1. giflib によるアニメーションGIFの読み込み
2. GIF論理画面サイズでの各フレーム合成
3. RGB輝度による1bpp二値化
4. 1バイト中の最上位ビットを左端画素とする MSB-first 形式への変換
5. GIFフレームごとの表示時間の取得

表示部分については、X11の `XImage`、`Pixmap`、`XCopyPlane()` を使用せず、NetBSD/luna68k の wsdisplay デバイスを `mmap(2)` し、1bppフレームバッファへ直接書き込む。

プログラム名は暫定的に次のものとする。

```text
monogifplay-wscons
```

---

## 2. 既存実装から流用する部分

既存の `MonoFrame` は、GIF論理画面全体の幅、高さ、表示時間、MSB-first 1bppビットマップを保持している。wscons版ではX11用の `Pixmap` メンバーだけを不要とする。 

```c
typedef struct {
    int width;
    int height;
    unsigned int delay;   /* milliseconds */
    uint8_t *bitmap_data; /* MSB-first packed 1bpp bitmap */
} MonoFrame;
```

`extract_mono_frames()` は基本的にそのまま使用する。

現行実装では、GIF論理画面幅に対する1行のバイト数を次のように算出している。

```c
line_bytes = (swidth + 7U) / 8U;
```

また、明るい色を1、暗い色を0として、画面左端を `0x80` のビットに対応させている。  

したがってデータ形式は次のとおりとなる。

```text
bitmap_data[0] bit 7 = x=0
bitmap_data[0] bit 6 = x=1
...
bitmap_data[0] bit 0 = x=7
bitmap_data[1] bit 7 = x=8
```

LUNAのフレームバッファも「上位バイトが低位アドレス」「最上位ビットが画面左側」という形式であり、既存のビットマップと一致する。ビット反転やバイト内ビット順変換は不要である。 

また、LUNAの1bppモードではVRAMビット1側が白となるカラーマップ設定であるため、既存実装の「明るい画素を1」とする表現をそのまま書き込める。 

---

## 3. 対象範囲

### 3.1 初版でサポートする環境

初版では対象を明確に限定する。

```text
OS              NetBSD
アーキテクチャ  luna68k
wsdisplay type  WSDISPLAY_TYPE_LUNA
画面深度        1bpp
デバイス        /dev/ttyE0（デフォルト）
```

次の条件を満たさない場合はエラー終了する。

```c
wstype == WSDISPLAY_TYPE_LUNA
fb_depth == 1
gif_width  <= fb_width
gif_height <= fb_height
```

他のNetBSD 1bppフレームバッファは、ビット順、画素値の極性、mmapオフセットが同じとは限らないため、初版では対象外とする。

### 3.2 表示位置

初版では次の固定位置とする。

```text
x = 0
y = 0
```

GIF画像の左上と画面左上を一致させる。

初版ではX座標が8の倍数であることを前提としたバイト単位転送を行う。ただし、これは1bppハードウェア自体の制約ではなく、シフト処理なしで高速にコピーするための実装上の制約である。

Y座標には8画素単位の制約はない。

### 3.3 再生方法

既存版と同じく、全フレームを順番に再生した後、先頭フレームへ戻る無限ループとする。

GIF内のNetscape Loop Extensionによるループ回数指定は初版では参照しない。

### 3.4 終了操作

以下で終了する。

```text
qキー
Ctrl-C
SIGTERM
SIGHUP
SIGQUIT
```

終了時には端末設定、画面内容、wsdisplayモードを可能な限り復元する。

---

## 4. wscons APIについての修正点

想定されている処理順序はおおむね正しいが、`WSDISPLAYIO_GTYPE` はフレームバッファサイズを取得するioctlではない。

各ioctlの役割は次のとおりである。

```text
WSDISPLAYIO_GMODE
    現在のEMUL/MAPPED/DUMBFBモードを取得する。

WSDISPLAYIO_SMODE
    EMUL/MAPPED/DUMBFBモードを変更する。

WSDISPLAYIO_GTYPE
    WSDISPLAY_TYPE_LUNAなど、表示デバイスの種類を取得する。

WSDISPLAYIO_GINFO
    width、height、depth、cmsizeを取得する。

WSDISPLAYIO_LINEBYTES
    VRAM上の1走査線あたりのバイト数、すなわちstrideを取得する。

WSDISPLAYIO_GET_FBINFO
    対応ドライバでは、上記に加えてmmapオフセット、VRAMサイズ、
    stride、ピクセル形式などを取得できる。
```

この区別はNetBSDの `wsdisplay(4)` にも明記されている。  

mlterm 3.9.5 は `WSDISPLAYIO_GET_FBINFO` を試し、失敗した場合は `WSDISPLAYIO_GINFO` と `WSDISPLAYIO_LINEBYTES` にフォールバックしている。LUNAの場合、フォールバック経路でmmap先頭から実フレームバッファまでのオフセットを8バイトとしている。  

---

## 5. LUNAフレームバッファ固有仕様

NetBSDの `lunafb` ドライバは、画面情報を次のように設定している。

```text
width     = 1280 pixels
height    = 1024 pixels
depth     = 1、4、または8bpp
linebytes = 2048 / 8 = 256 bytes
```

したがって、1bpp画面であっても次の関係になる。

```text
画面上の可視領域に必要なバイト数 = 1280 / 8 = 160 bytes
実際のstride                    = 256 bytes
```

各走査線の後半96バイトは表示幅とは別のハードウェア上の余白となる。`width / 8` をstrideとして使用してはならず、必ず `WSDISPLAYIO_LINEBYTES` の結果を使う。 

LUNAのDUMBFB mmapでは、物理フレームバッファアドレスがページ先頭から8バイトずれている。そのため、`mmap()` の戻り値そのものではなく、8バイト加算した位置が画面データの先頭となる。  

```c
map_base = mmap(..., offset = 0);
fb_base  = map_base + 8;
```

1bpp時のマップサイズは次のようにする。

```c
fb_data_size = stride * height;
map_size = fb_offset + fb_data_size;
```

LUNAの場合は次の値になる。

```text
fb_offset    = 8
fb_data_size = 256 * 1024 = 262144 bytes
map_size     = 262152 bytes
```

`mmap()` に渡すlengthはページサイズの倍数である必要はない。カーネル側で必要な単位に丸められる。

---

## 6. プログラム構成

### 6.1 推奨ファイル構成

初版では既存X11版への影響を避けるため、次の構成を推奨する。

```text
monogifplay.c
    既存X11版。初版では原則として変更しない。

monogifplay-wscons.c
    wscons版。
    GIF処理部分は既存コードから移植する。

Makefile
    X11版とwscons版の2ターゲットを生成する。
```

まず動作確認を優先し、共通コード化はその後に行う。

動作が安定した段階で、次のように分割する。

```text
mono_gif.c
mono_gif.h
    MonoFrame
    extract_mono_frames()
    時刻処理
    GIF読み込み処理

display_x11.c
    X11表示バックエンド

display_wscons.c
    wscons表示バックエンド
```

### 6.2 wsdisplay管理構造体

```c
typedef struct {
    int fd;
    const char *device;

    unsigned int type;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int stride;

    unsigned int original_mode;
    bool mode_changed;

    size_t fb_offset;
    size_t fb_size;
    size_t map_size;

    uint8_t *map_base;
    uint8_t *fb_base;
    uint8_t *saved_fb;

    struct termios original_termios;
    bool termios_changed;
} WsDisplay;
```

各フィールドは初期値を安全側に設定する。

```c
.fd       = -1
.map_base = MAP_FAILED
.fb_base  = NULL
.saved_fb = NULL
```

クリーンアップ関数は複数回呼ばれても問題が起きないようにする。

---

## 7. コマンドライン仕様

```text
monogifplay-wscons [-d] [-p] [-f device] gif-file
```

### オプション

```text
-d
    GIF読み込み、フレーム変換などの処理時間を表示する。
    既存版と同じ。

-p
    処理進捗を表示する。
    既存版と同じ。

-f device
    使用するwsdisplayデバイスを指定する。
    デフォルトは環境変数FRAMEBUFFER。
    FRAMEBUFFERが未指定なら /dev/ttyE0。
```

X11固有の以下のオプションはwscons版では使用しない。

```text
-g geometry
-a align
```

---

## 8. 初期化シーケンス

### 8.1 フレームバッファ情報の取得

まず画面モードを変更せずに `/dev/ttyE0` を開き、情報だけを取得する。

```c
fd = open(device, O_RDWR);
```

以後のwsdisplay ioctlは、mltermのように `STDIN_FILENO` とフレームバッファfdを混在させず、原則として明示的にopenした同一の `fd` に対して実行する。

```text
1. WSDISPLAYIO_GMODE
2. WSDISPLAYIO_GTYPE
3. WSDISPLAYIO_GET_FBINFOを試行
4. 失敗時はWSDISPLAYIO_GINFO
5. WSDISPLAYIO_LINEBYTES
```

`WSDISPLAYIO_GMODE` と `WSDISPLAYIO_SMODE` はwsdisplayの共通層で処理され、DUMBFBフラグを含む画面状態が管理される。 

初版では、起動時モードが次でなければエラー終了する。

```c
original_mode == WSDISPLAYIO_MODE_EMUL
```

すでにXサーバーや別のフレームバッファアプリケーションが使用している画面を奪わないためである。

### 8.2 GIFサイズの事前検査

フレームバッファ情報取得後にGIFをopenする。

`DGifOpenFileName()` の時点で論理画面幅と高さを確認し、フレーム展開前に次を検査する。

```c
gif->SWidth  <= wsdisplay.width
gif->SHeight <= wsdisplay.height
```

大きすぎるGIFは、`DGifSlurp()` を実行せずにエラー終了する。

### 8.3 GIFの読み込みと変換

既存版と同じ順序で処理する。

```text
DGifOpenFileName()
DGifSlurp()
MonoFrame配列確保
extract_mono_frames()
DGifCloseFile()
```

現行コードも `DGifSlurp()` 後に全フレームを1bppへ変換し、GIFデータをcloseしてから表示処理へ移っている。 

### 8.4 DUMBFBモードへの移行

すべてのGIF変換が完了してから画面モードを変更する。これにより、時間のかかるGIF展開中は通常のコンソール表示を維持できる。

```c
mode = WSDISPLAYIO_MODE_DUMBFB;
ioctl(fd, WSDISPLAYIO_SMODE, &mode);
mode_changed = true;
```

### 8.5 mmap

`WSDISPLAYIO_GET_FBINFO` が成功した場合は、取得した値を使用する。

```c
fb_offset = fbinfo.fbi_fboffset;
fb_size   = fbinfo.fbi_fbsize;
stride    = fbinfo.fbi_stride;
map_size  = fb_offset + fb_size;
```

ただし、以下を検査する。

```c
fb_size >= stride * height
stride >= (width + 7U) / 8U
```

`GET_FBINFO` が失敗したLUNAでは次の値を使用する。

```c
fb_offset = 8;
fb_size   = stride * height;
map_size  = fb_offset + fb_size;
```

マッピング処理は次のとおりとする。

```c
map_base = mmap(NULL, map_size,
    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

fb_base = map_base + fb_offset;
```

---

## 9. 画面内容の保存と復元

DUMBFBへ切り替えた直後、描画開始前のVRAM内容を保存する。

```c
saved_fb = malloc(fb_size);
memcpy(saved_fb, fb_base, fb_size);
```

保存対象は実画面先頭である `fb_base` から `stride * height` バイトとし、mmap先頭にある8バイトのオフセット領域は含めない。

その後、画面全体を白でクリアする。

```c
memset(fb_base, 0xff, fb_size);
```

GIFが画面より小さい場合、画像外領域は白になる。

終了時には、まだDUMBFBモードである間に画面内容を戻す。

```c
memcpy(fb_base, saved_fb, fb_size);
```

その後、以下の順で復元する。

```text
1. stdinのtermiosを復元
2. munmap()
3. WSDISPLAYIO_SMODEでoriginal_modeへ復元
4. framebuffer fdをclose
5. MonoFrameデータを解放
```

画面内容を保存しない場合、EMULモードへ戻しても、プログラムが上書きしたコンソール画面がそのまま残る可能性がある。そのため、256KiB程度の追加メモリを使用しても保存・復元する仕様を推奨する。

---

## 10. フレーム描画処理

### 10.1 基本アドレス計算

GIFの1行あたりのバイト数は次のとおり。

```c
src_stride = (gif_width + 7U) / 8U;
```

表示位置を `(dst_x, dst_y)` とした場合、各行の転送先は次のようになる。

```c
dst = fb_base
    + (dst_y + y) * fb_stride
    + dst_x / 8;
```

初版では `dst_x == 0`、`dst_y == 0` である。

### 10.2 GIF幅が8の倍数の場合

```c
for (y = 0; y < height; y++) {
    const uint8_t *src = bitmap + y * src_stride;
    uint8_t *dst = fb_base + y * fb_stride;

    memcpy(dst, src, src_stride);
}
```

### 10.3 GIF幅が8の倍数でない場合

現行X11版では `XPutImage()` に実際の画像幅を指定するため、最終バイト中の画像外ビットは無視される。

直接VRAMへ `memcpy()` すると、最終バイトの余った1～7ビットまで画面へ書かれてしまう。また、現行の `extract_mono_frames()` では、このパディング部分が必ずしも初期化されていない。

そのため、最後の1バイトだけread-modify-writeする。

```c
unsigned int full_bytes = width / 8U;
unsigned int rem_bits   = width & 7U;

for (y = 0; y < height; y++) {
    const uint8_t *src = bitmap + y * src_stride;
    uint8_t *dst = fb_base + y * fb_stride;

    if (full_bytes > 0)
        memcpy(dst, src, full_bytes);

    if (rem_bits != 0) {
        uint8_t mask = (uint8_t)(0xffU << (8U - rem_bits));

        dst[full_bytes] =
            (dst[full_bytes] & (uint8_t)~mask) |
            (src[full_bytes] & mask);
    }
}
```

これにより、GIF右端より外側の画素を壊さない。

### 10.4 API

```c
static int
wsdisplay_blit_frame(const WsDisplay *display,
    const MonoFrame *frame,
    unsigned int dst_x,
    unsigned int dst_y);
```

関数内で次を検査する。

```c
(dst_x & 7U) == 0
dst_x + frame->width  <= display->width
dst_y + frame->height <= display->height
```

---

## 11. 再生ループ

既存版の相対的なフレーム待ち時間を踏襲する。

```c
for (;;) {
    for (i = 0; i < frame_count; i++) {
        uint32_t nextframe_time;

        nextframe_time = gettime_ms() + frames[i].delay;

        wsdisplay_blit_frame(&display, &frames[i], 0, 0);

        while (!stop_requested &&
               gettime_ms() < nextframe_time) {
            /* stdin入力または短時間sleep */
        }

        if (stop_requested)
            goto cleanup;
    }
}
```

VRAM転送に要した時間もフレーム表示時間に含まれる。

処理がフレームdelayを超過した場合、フレームを飛ばさず、直ちに次フレームを表示する。初版ではリアルタイム追従より、全フレームを順番どおり表示することを優先する。

---

## 12. キー入力とシグナル

### 12.1 stdin設定

`isatty(STDIN_FILENO)` が真の場合、元のtermiosを保存し、少なくとも次のフラグを解除する。

```c
new_termios.c_lflag &= ~(ICANON | ECHO);
```

`ISIG` は維持し、Ctrl-CをSIGINTとして処理する。

```c
new_termios.c_cc[VMIN]  = 0;
new_termios.c_cc[VTIME] = 0;
```

再生待ち中に `poll()` または `select()` でstdinを確認し、`q` が読み込まれた場合は終了する。

stdinがttyでない場合はキー入力を監視せず、シグナルのみで終了する。

### 12.2 シグナルハンドラ

シグナルハンドラ内でioctl、munmap、freeなどを実行しない。

```c
static volatile sig_atomic_t stop_requested;

static void
handle_signal(int signo)
{
    stop_requested = 1;
}
```

対象シグナルは次のとおり。

```text
SIGINT
SIGTERM
SIGHUP
SIGQUIT
```

メインループがフラグを検出し、通常のクリーンアップ経路へ移る。

`SIGKILL`、カーネルパニック、プロセスの異常終了では復元処理を保証できない。これはユーザー空間でwsdisplayをDUMBFBモードにするプログラムに共通する制約である。

---

## 13. エラー処理

画面モード変更前のエラーは、通常どおり `err()` または `errx()` で終了してよい。

画面モード変更後は、直接 `err()` を呼ばず、必ず共通クリーンアップを経由する。

```c
saved_errno = errno;
error_message = "...";
goto cleanup_error;
```

クリーンアップ後にエラーを表示する。

### 主なエラー条件

```text
/dev/ttyE0をopenできない
WSDISPLAYIO_GMODEに失敗
起動時モードがEMULではない
WSDISPLAYIO_GTYPEに失敗
WSDISPLAY_TYPE_LUNAではない
WSDISPLAYIO_GINFOに失敗
depthが1ではない
WSDISPLAYIO_LINEBYTESに失敗
strideが画面幅に対して不足
GIFサイズが画面サイズを超える
フレーム数が0
メモリ確保失敗
WSDISPLAYIO_SMODEに失敗
mmapに失敗
フレーム描画範囲が画面外
```

---

## 14. クリーンアップ関数

```c
static void
wsdisplay_cleanup(WsDisplay *display)
{
    if (display->saved_fb != NULL &&
        display->fb_base != NULL) {
        memcpy(display->fb_base,
            display->saved_fb,
            display->fb_size);
    }

    if (display->termios_changed) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH,
            &display->original_termios);
        display->termios_changed = false;
    }

    if (display->map_base != MAP_FAILED) {
        munmap(display->map_base, display->map_size);
        display->map_base = MAP_FAILED;
        display->fb_base = NULL;
    }

    if (display->mode_changed && display->fd >= 0) {
        int mode = display->original_mode;

        ioctl(display->fd, WSDISPLAYIO_SMODE, &mode);
        display->mode_changed = false;
    }

    if (display->fd >= 0) {
        close(display->fd);
        display->fd = -1;
    }

    free(display->saved_fb);
    display->saved_fb = NULL;
}
```

実コードでは、画面保存完了前にエラーになった場合など、各初期化段階に応じて安全に動作するようにする。

---

## 15. ビルド仕様

wscons版ではX11関連のヘッダとライブラリを使用しない。

必要なライブラリはgiflibのみ。

```make
PROG_WSCONS = monogifplay-wscons
SRCS_WSCONS = monogifplay-wscons.c
OBJS_WSCONS = ${SRCS_WSCONS:.c=.o}

CPPFLAGS += -Wall
CPPFLAGS += -DUNROLL_BITMAP_EXTRACT
CPPFLAGS += -I/usr/pkg/include

LDFLAGS  += -L/usr/pkg/lib -Wl,-R/usr/pkg/lib
LDLIBS_WSCONS += -lgif

${PROG_WSCONS}: ${OBJS_WSCONS}
	${CC} -o ${PROG_WSCONS} ${CFLAGS} ${LDFLAGS} \
	    ${OBJS_WSCONS} ${LDLIBS_WSCONS}
```

不要となるものは次のとおり。

```text
X11/Xlib.h
X11/Xutil.h
X11/Xatom.h
-lX11
-I/usr/X11R7/include
-L/usr/X11R7/lib
```

既存X11版のビルドルールは残す。

---

## 16. テスト項目

### 16.1 フレームバッファ情報

起動時に `-p` で次を表示する。

```text
device: /dev/ttyE0
type: WSDISPLAY_TYPE_LUNA
mode: EMUL
size: 1280x1024
depth: 1
stride: 256
fb offset: 8
fb size: 262144
map size: 262152
```

### 16.2 ビット順確認

次の8画素パターンを表示する。

```text
10000000
01000000
00100000
...
00000001
```

左から右へ白画素が移動することを確認する。

逆方向になる場合はビット順の認識が誤っている。

### 16.3 画素値極性

全ビット0のフレームと全ビット1のフレームを表示し、次を確認する。

```text
0 = 黒
1 = 白
```

### 16.4 stride確認

640×480画像を表示し、各行が斜めにずれず、正常に表示されることを確認する。

`src_stride=80` と `fb_stride=256` を区別できていない場合、行単位で表示位置が崩れる。

### 16.5 右端マスク

幅13画素など、8の倍数でないGIFを表示し、右隣の画素が破壊されないことを確認する。

### 16.6 終了処理

以下の各方法で終了し、コンソール画面と端末設定が復元されることを確認する。

```text
q
Ctrl-C
kill -TERM
端末切断によるSIGHUP
```

### 16.7 エラー経路

以下のケースでもEMULモードへ戻ることを確認する。

```text
mmap失敗
画面保存用malloc失敗
termios設定失敗
再生中の内部エラー
```

---

## 17. 現行GIF処理から継承する制約

現行 `extract_mono_frames()` は、透過画素または部分フレームの場合、直前の1bppフレームをコピーしてから対象部分を上書きする。 

一方、Graphics Control ExtensionのDisposal Methodについては、表示時間と透過色番号は取得しているが、少なくとも次の処理は明示的には実装していない。

```text
Restore to background
Restore to previous
```

したがって初版のGIF互換性は「既存X11版と同等」とする。

展示用GIFについては、次のいずれかを推奨する。

```text
各フレームが論理画面全体を持つ
Disposal Methodがunspecifiedまたはdo not dispose
既存X11版で正しく表示できることを事前確認する
```

---

## 18. 最大の未解決事項：16MB環境でのメモリ使用量

初版は既存版と同様、`DGifSlurp()` で全GIFフレームを展開し、さらに全フレーム分の1bppデータを保持する。

640×480、10fps、60秒の場合は600フレームとなる。

1bppデータだけでも次の容量になる。

```text
640 × 480 / 8 = 38,400 bytes/frame

38,400 × 600 = 23,040,000 bytes
                 約21.97MiB
```

これだけでLUNAの16MB RAMを超える。

さらに `DGifSlurp()` は各フレームのカラーインデックスを概ね1画素1バイトで保持するため、単純計算では次の規模になる。

```text
640 × 480 × 600 = 184,320,000 bytes
                   約175.8MiB
```

GIF圧縮後のファイルサイズが小さくても、`DGifSlurp()` 後のメモリ量は圧縮前の画素数に依存する。

したがって、本設計の初版は次の用途に限定される。

```text
wscons直接描画方式の動作確認
LUNA固有のmmap、stride、offset、ビット順の確認
比較的短いGIFの再生
```

**60秒・600フレーム・16MB RAMという最終目標には、この全フレーム保持方式をそのまま使用できない。**

最終版では次のいずれかが必要になる。

### 案A：GIF逐次デコード

```text
DGifSlurp()を使用しない
DGifGetRecordType()などでフレームを順次読み込む
現在の合成済み1bpp画面だけを保持する
必要ならRestore to previous用の1画面を追加する
```

ただし、LUNA上でGIFのLZW展開と1bpp変換をリアルタイム実行できるかが課題となる。

### 案B：事前変換した1bppストリームを逐次読み込み

ホスト側で次の形式へ変換する。

```text
ファイルヘッダ
    画面幅
    画面高さ
    フレーム数
    ビット順
    背景極性

フレーム
    delay
    1bppフレームデータ
```

再生時には1フレームだけ読み込み、VRAMへ転送する。

640×480、10fpsの場合の読み込み帯域は次の程度である。

```text
38,400 × 10 = 384,000 bytes/second
```

全データをRAMへ置く必要はないため、16MB環境に適する。

### 案C：差分圧縮ストリーム

前フレームとの差分を次のいずれかで保存する。

```text
変更矩形
変更行
XOR差分
バイト単位RLE
```

展示用のモーショングラフィックスでは、毎フレームの変更領域が小さければ高い圧縮率を期待できる。

最終的には、GIFを入力形式として直接再生する方式よりも、ホスト側で生成した専用1bppストリームをLUNAで再生する方式のほうが、メモリ使用量、起動時間、再生負荷を予測しやすい。

---

## 19. 将来拡張

### 第2段階：位置指定

```text
-x x
-y y
```

`x` は8の倍数だけを許可し、`y` は任意とする。

### 第3段階：センタリング

```text
-c
```

```c
x = (fb_width  - gif_width)  / 2;
y = (fb_height - gif_height) / 2;
x &= ~7U;
```

X座標を8画素境界へ切り下げるため、厳密な中央から最大7画素左へずれる。

### 第4段階：任意X座標

1バイト未満のずれについて、隣接バイト間のシフトと両端マスクを実装する。

```text
dst_bit_offset = x & 7
dst_byte       = x >> 3
```

初版の単純な `memcpy()` より処理コストが増えるため、byte-aligned経路は別に残す。

### 第5段階：長尺再生

`DGifSlurp()` と全フレーム保持を廃止し、逐次デコードまたは専用1bppストリームへ移行する。

---

## 20. 初版実装の確定仕様

初版は次の仕様で実装する。

```text
・NetBSD/luna68k専用
・WSDISPLAY_TYPE_LUNAかつdepth=1のみ
・/dev/ttyE0をデフォルト使用
・起動時モードがEMULの場合のみ実行
・GIF処理は既存monogifplayと同じ
・全フレームを事前にMSB-first 1bppへ変換
・表示位置は左上 (0, 0)
・GIF幅は8の倍数でなくても右端マスクで対応
・GIFが画面より大きい場合はモード変更前にエラー終了
・strideはWSDISPLAYIO_LINEBYTESから取得
・LUNAフォールバック時のmmapオフセットは8
・mmapサイズは8 + stride × height
・描画前にコンソール画面全体を保存
・画像外領域を白でクリア
・各フレームを行単位でVRAMへコピー
・q、SIGINT、SIGTERM、SIGHUP、SIGQUITで終了
・終了時に画面、termios、wsdisplayモードを復元
・X11ライブラリには依存しない
・長尺GIFへの対応は初版の対象外
```