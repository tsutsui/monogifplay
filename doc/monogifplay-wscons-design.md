# monogifplay wscons直接描画版 実装設計書 v2

## 1. 目的

既存の `monogifplay` が行っている以下の処理を流用する。

1. giflibによるアニメーションGIFの読み込み
2. GIF論理画面サイズでの各フレーム合成
3. RGB輝度による1bpp二値化
4. 1バイト中の最上位ビットを左端画素とするMSB-first形式への変換
5. GIFフレームごとの表示時間取得

表示処理については、X11の `XImage`、`Pixmap`、`XCopyPlane()` を使用せず、NetBSD/luna68kのwsdisplayデバイスを `mmap(2)` し、1bppフレームバッファへ直接書き込む。

プログラム名は暫定的に次のものとする。

```text
monogifplay-wscons
```

---

## 2. 対象環境

初版では対象を次の環境に限定する。

```text
OS              NetBSD
アーキテクチャ  luna68k
wsdisplay type  WSDISPLAY_TYPE_LUNA
画面深度        1bpp
標準デバイス    /dev/ttyE0
```

次の条件を満たさない場合はエラー終了する。

```c
wstype == WSDISPLAY_TYPE_LUNA
fb_depth == 1
gif_width  <= fb_width
gif_height <= fb_height
```

他のNetBSD 1bppフレームバッファについては、次の仕様がLUNAと同一とは限らないため、初版では対象外とする。

- VRAMのビット順
- 画素値0および1の表示色
- 1走査線当たりのバイト数
- mmap領域内のフレームバッファ開始位置
- VRAMアクセスに必要なハードウェア固有処理

---

## 3. 既存実装から流用する処理

### 3.1 MonoFrame

既存の `MonoFrame` は、GIF論理画面全体の幅、高さ、表示時間、MSB-first形式の1bppビットマップを保持している。

wscons版ではX11用の `Pixmap` メンバーを削除する。既存版の構造体では `bitmap_data` と `Pixmap` の両方を保持している。

```c
typedef struct {
    int width;
    int height;
    unsigned int delay;   /* milliseconds */
    uint8_t *bitmap_data; /* MSB-first packed 1bpp bitmap */
} MonoFrame;
```

### 3.2 1bppフレーム生成

`extract_mono_frames()` は基本的にそのまま使用する。

GIF論理画面の1行当たりのバイト数は次の式で求める。

```c
line_bytes = (swidth + 7U) / 8U;
```

現行実装もこの式を使用して、GIF論理画面全体の1bppバッファを各フレームに確保している。

ビット配置は次のとおりである。

```text
bitmap_data[0] bit 7 = x=0
bitmap_data[0] bit 6 = x=1
...
bitmap_data[0] bit 0 = x=7
bitmap_data[1] bit 7 = x=8
```

明るい色を1、暗い色を0とする。

### 3.3 LUNA VRAMとの互換性

NetBSD/luna68kのフレームバッファ処理は、次のビット順を前提としている。

- 上位バイトを低位アドレスへ格納
- 最上位ビットを画面左側へ表示

これは既存 `monogifplay` のMSB-firstビットマップと一致するため、バイト内ビット反転は不要である。

LUNAの1bppモードでは、VRAMビット1側が白として表示されるため、既存実装の画素値もそのまま使用できる。

---

## 4. 初版の機能仕様

### 4.1 再生対象

giflibで読み込めるアニメーションGIFを対象とする。

カラーGIFについても、既存版と同じ輝度判定で白黒化する。

### 4.2 表示位置

初版では次の固定位置へ表示する。

```text
x = 0
y = 0
```

GIF画像の左上と画面左上を一致させる。

初版ではX座標が8の倍数であることを前提とし、バイト単位でVRAMへ転送する。

これは1bppフレームバッファ自体の制約ではなく、ビットシフトを行わず高速にコピーするための初版実装上の制約である。

Y座標には8画素単位の制約はない。

### 4.3 GIFサイズ

次のいずれかを満たさない場合はエラー終了する。

```c
gif_width  <= fb_width
gif_height <= fb_height
```

この検査は、可能な限り `DGifSlurp()` を呼び出す前に行う。

### 4.4 再生方法

全フレームを順番に表示し、最終フレームの次は先頭フレームへ戻る。

初版では無限ループ再生とする。

GIF内のNetscape Loop Extensionによるループ回数指定は参照しない。

### 4.5 終了方法

次の操作またはシグナルで終了する。

```text
qキー
Ctrl-C
SIGTERM
SIGHUP
SIGQUIT
```

終了時には以下を復元する。

- 起動前のフレームバッファ内容
- stdinのtermios設定
- 起動前のwsdisplay画面モード

---

## 5. コマンドライン仕様

```text
monogifplay-wscons [-c] [-d] [-p] [-f device] gif-file
```

### 5.1 オプション

```text
-c
    アニメーション再生開始前に画面全体を白でクリアする。
    未指定時は既存の画面内容を維持する。

-d
    GIF読み込み、フレーム変換などの処理時間を表示する。
    -pも暗黙に有効とする。

-p
    GIF読み込みとフレーム変換の進捗を表示する。

-f device
    使用するwsdisplayデバイスを指定する。
```

フレームバッファデバイスの決定順序は次のとおりとする。

1. `-f device`
2. 環境変数 `FRAMEBUFFER`
3. `/dev/ttyE0`

### 5.2 wscons版で使用しない既存オプション

X11版の次のオプションはwscons版では使用しない。

```text
-g geometry
-a align
```

表示位置指定およびセンタリングは将来拡張とする。

---

## 6. 再生開始前の画面処理

### 6.1 起動画面の保存

DUMBFBモードへ切り替えて `mmap()` が成功した後、描画を開始する前に現在のフレームバッファ内容を保存する。

```c
saved_fb = malloc(fb_size);

if (saved_fb == NULL)
    goto error;

memcpy(saved_fb, fb_base, fb_size);
```

保存対象は、実フレームバッファ先頭 `fb_base` から次の範囲とする。

```c
fb_size = stride * height;
```

mmap領域先頭からフレームバッファまでのオフセット領域は保存対象に含めない。

### 6.2 `-c` 指定時

`-c` が指定されている場合は、画面保存後、最初のフレームを描画する前に画面全体を白でクリアする。

```c
memset(fb_base, 0xff, fb_size);
```

LUNA 1bppではビット1が白なので、`0xff` で画面全体が白になる。

### 6.3 `-c` 未指定時

`-c` が指定されていない場合、画面全体のクリアは行わない。

GIF画像が画面より小さい場合、GIF表示領域外には再生開始前の画面内容が残る。

これにより将来、次の処理順序を実装できる。

```text
1. 起動前画面を保存
2. 指定された背景画像をフレームバッファへ描画
3. アニメーションGIFを指定位置へ描画
4. 終了時に起動前画面を復元
```

初版では背景画像ファイルの読み込み機能は実装しない。

### 6.4 GIF透明画素と背景画像の関係

現在の `extract_mono_frames()` は、GIFの透明画素や部分フレームを、直前のGIFフレーム内容を引き継ぐ形で1bpp画面へ合成する。

したがって初版では、GIF論理画面内の透明画素からフレームバッファ上の背景画像を透過表示する機能は提供しない。

`-c` 未指定時に維持される背景は、基本的にGIF論理画面の外側である。

将来、背景画像上へ透明GIFを重ねる場合は、次のどちらかを別途設計する必要がある。

- 透明画素ではVRAMを書き換えない
- 背景画像をバックバッファとして保持し、各GIFフレームと合成する

---

## 7. wsdisplay管理構造体

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

初期値は次のように設定する。

```c
.fd              = -1
.map_base        = MAP_FAILED
.fb_base         = NULL
.saved_fb        = NULL
.mode_changed    = false
.termios_changed = false
```

クリーンアップ関数は、初期化途中の状態でも安全に呼び出せるようにする。

---

## 8. 初期化シーケンス

### 8.1 全体順序

初期化は次の順序で実施する。

```text
1. コマンドライン解析
2. wsdisplayデバイスをopen
3. 現在の画面モード取得
4. フレームバッファ種別と画面情報取得
5. GIFファイルをopen
6. GIF論理画面サイズ検査
7. DGifSlurp()
8. 全フレームを1bppへ変換
9. DGifCloseFile()でgiflib側データを解放
10. WSDISPLAYIO_MODE_DUMBFBへ変更
11. フレームバッファをmmap
12. 起動前の画面内容を保存
13. 必要ならstdinのtermiosを変更
14. -c指定時は画面を白でクリア
15. アニメーション再生開始
```

GIFの読み込みと変換をDUMBFBモードへの切り替えより前に行うことで、時間のかかる前処理中も通常のコンソールを使用できる。

また、画面保存用バッファは `DGifCloseFile()` の後に確保し、GIF変換中のメモリピークを増加させない。

### 8.2 デバイスopen

```c
fd = open(device, O_RDWR);
```

失敗した場合は画面モードを変更せずにエラー終了する。

可能であれば、fdに `FD_CLOEXEC` を設定する。

### 8.3 起動時画面モード

```c
ioctl(fd, WSDISPLAYIO_GMODE, &original_mode);
```

初版では次の場合だけ実行を継続する。

```c
original_mode == WSDISPLAYIO_MODE_EMUL
```

すでにXサーバーや他のフレームバッファアプリケーションが使用している画面を奪わないため、MAPPEDまたはDUMBFBの場合はエラー終了する。

### 8.4 フレームバッファ種別

```c
ioctl(fd, WSDISPLAYIO_GTYPE, &wstype);
```

次の条件を検査する。

```c
wstype == WSDISPLAY_TYPE_LUNA
```

### 8.5 画面情報取得

可能な場合は、まず `WSDISPLAYIO_GET_FBINFO` を試行する。

```c
struct wsdisplayio_fbinfo fbinfo;

memset(&fbinfo, 0, sizeof(fbinfo));

if (ioctl(fd, WSDISPLAYIO_GET_FBINFO, &fbinfo) == 0) {
    width     = fbinfo.fbi_width;
    height    = fbinfo.fbi_height;
    depth     = fbinfo.fbi_bitsperpixel;
    stride    = fbinfo.fbi_stride;
    fb_offset = fbinfo.fbi_fboffset;
    fb_size   = fbinfo.fbi_fbsize;
}
```

失敗した場合は次のioctlを使用する。

```c
struct wsdisplay_fbinfo vinfo;

ioctl(fd, WSDISPLAYIO_GINFO, &vinfo);
ioctl(fd, WSDISPLAYIO_LINEBYTES, &stride);
```

`WSDISPLAYIO_GINFO` から次を取得する。

```text
width
height
depth
cmsize
```

`WSDISPLAYIO_LINEBYTES` からVRAM上の1走査線当たりのバイト数を取得する。各ioctlの仕様は `wsdisplay(4)` に定義されている。 

### 8.6 取得値検査

```c
if (width == 0 || height == 0)
    goto error;

if (depth != 1)
    goto error;

if (stride < (width + 7U) / 8U)
    goto error;
```

---

## 9. LUNA固有のフレームバッファ仕様

NetBSD/luna68kの `lunafb` ドライバは、画面情報を次のように設定している。

```text
width     = 1280 pixels
height    = 1024 pixels
depth     = 1、4、または8bpp
linebytes = 2048 / 8 = 256 bytes
```



1bpp画面の可視領域に必要なバイト数は次のとおりである。

```text
1280 / 8 = 160 bytes/line
```

一方、実際のstrideは256バイトである。

```text
可視部分       160 bytes
走査線全体     256 bytes
行末余白        96 bytes
```

したがって、VRAM描画時に `width / 8` を次の行への移動量として使用してはならない。

必ず取得したstrideを使用する。

### 9.1 mmapオフセット

LUNAのフレームバッファ実体は、mmap領域のページ先頭から8バイトずれた位置に存在する。LUNAドライバではフレームバッファ物理アドレスにも8バイトのオフセットが定義されている。

`WSDISPLAYIO_GET_FBINFO` が使用できず、`WSDISPLAYIO_GINFO` にフォールバックした場合は次の値を使用する。

```c
fb_offset = 8;
fb_size   = stride * height;
map_size  = fb_offset + fb_size;
```

mltermのwscons実装でも、`WSDISPLAY_TYPE_LUNA` のフォールバック時は8バイトのオフセットを使用している。

### 9.2 mmap処理

```c
map_base = mmap(NULL, map_size,
    PROT_READ | PROT_WRITE,
    MAP_SHARED,
    fd,
    0);

if (map_base == MAP_FAILED)
    goto error;

fb_base = map_base + fb_offset;
```

LUNA 1bpp、1280×1024の場合は次の値となる。

```text
stride       = 256
height       = 1024
fb_size      = 262144 bytes
fb_offset    = 8 bytes
map_size     = 262152 bytes
```

---

## 10. GIF読み込みと変換

### 10.1 GIFファイルopen

```c
gif = DGifOpenFileName(giffile, &err);
```

open後、GIF論理画面サイズを取得する。

```c
swidth  = gif->SWidth;
sheight = gif->SHeight;
```

この時点で画面サイズを超えていないか検査する。

```c
if (swidth > fb_width || sheight > fb_height)
    goto gif_error;
```

### 10.2 全フレーム読み込み

```c
if (DGifSlurp(gif) != GIF_OK)
    goto gif_error;
```

### 10.3 MonoFrame確保

```c
frame_count = gif->ImageCount;

if (frame_count <= 0)
    goto gif_error;

frames = calloc(frame_count, sizeof(*frames));
```

### 10.4 1bpp変換

```c
if (extract_mono_frames(gif, frames) < 0)
    goto gif_error;
```

各 `MonoFrame.bitmap_data` はGIF論理画面全体の1bppデータを保持する。

### 10.5 giflibデータの解放

全フレームの1bpp変換が完了したら、再生開始前に次を実行する。

```c
DGifCloseFile(gif, NULL);
gif = NULL;
```

これにより、`DGifSlurp()` が確保した次のデータは再生前に解放される。

- `SavedImages`
- 各フレームの `RasterBits`
- GIF拡張ブロック
- giflib内部管理領域
- GIFカラーマップ

現行X11版も、必要な1bppデータを全フレームへ格納した後、表示処理へ移る前に `DGifCloseFile()` を実行している。

---

## 11. DUMBFBモードへの移行

GIFの読み込みと1bpp変換がすべて成功した後、画面モードを変更する。

```c
int mode = WSDISPLAYIO_MODE_DUMBFB;

if (ioctl(fd, WSDISPLAYIO_SMODE, &mode) == -1)
    goto error;

mode_changed = true;
```

この後にエラーが発生した場合は、必ず共通クリーンアップ処理を経由する。

---

## 12. フレーム描画処理

### 12.1 基本アドレス計算

GIFの1行当たりのバイト数は次のとおりである。

```c
src_stride = (frame->width + 7U) / 8U;
```

表示位置を `(dst_x, dst_y)` とした場合、各行の転送先は次のように求める。

```c
dst = fb_base
    + (dst_y + y) * fb_stride
    + dst_x / 8U;
```

初版では次の値で固定する。

```c
dst_x = 0;
dst_y = 0;
```

### 12.2 描画関数

```c
static int
wsdisplay_blit_frame(const WsDisplay *display,
    const MonoFrame *frame,
    unsigned int dst_x,
    unsigned int dst_y);
```

描画前に次を検査する。

```c
if ((dst_x & 7U) != 0)
    return -1;

if (dst_x + frame->width > display->width)
    return -1;

if (dst_y + frame->height > display->height)
    return -1;
```

### 12.3 幅が8の倍数の場合

```c
for (y = 0; y < frame->height; y++) {
    const uint8_t *src;
    uint8_t *dst;

    src = frame->bitmap_data + y * src_stride;
    dst = display->fb_base
        + (dst_y + y) * display->stride
        + dst_x / 8U;

    memcpy(dst, src, src_stride);
}
```

### 12.4 幅が8の倍数でない場合

最終バイトには、GIF画像幅より右側の未使用ビットが含まれる。

VRAMへ最終バイトをそのままコピーすると、GIF表示領域外の最大7画素を上書きする可能性がある。

そのため、完全なバイト部分と最終端部分を分けて処理する。

```c
unsigned int full_bytes;
unsigned int rem_bits;

full_bytes = frame->width / 8U;
rem_bits   = frame->width & 7U;

for (y = 0; y < frame->height; y++) {
    const uint8_t *src;
    uint8_t *dst;

    src = frame->bitmap_data + y * src_stride;
    dst = display->fb_base
        + (dst_y + y) * display->stride
        + dst_x / 8U;

    if (full_bytes > 0)
        memcpy(dst, src, full_bytes);

    if (rem_bits != 0) {
        uint8_t mask;

        mask = (uint8_t)(0xffU << (8U - rem_bits));

        dst[full_bytes] =
            (uint8_t)((dst[full_bytes] & (uint8_t)~mask) |
            (src[full_bytes] & mask));
    }
}
```

これにより、GIF右端より外側のフレームバッファ内容を維持する。

---

## 13. 再生ループ

基本的な再生ループは既存X11版と同じとする。

```c
for (;;) {
    for (i = 0; i < frame_count; i++) {
        uint32_t nextframe_time;

        nextframe_time =
            gettime_ms() + frames[i].delay;

        if (wsdisplay_blit_frame(&display,
            &frames[i], 0, 0) != 0) {
            playback_error = true;
            goto cleanup;
        }

        while (!stop_requested &&
            gettime_ms() < nextframe_time) {
            wait_for_input_or_timeout();
        }

        if (stop_requested)
            goto cleanup;
    }
}
```

VRAMへの転送時間はフレーム表示時間に含める。

描画処理が指定delayを超えた場合は、フレームを飛ばさず、直ちに次のフレームを描画する。

初版では実時間への追従よりも、全フレームを順番どおり表示することを優先する。

---

## 14. キー入力

### 14.1 stdinがttyの場合

```c
if (isatty(STDIN_FILENO)) {
    tcgetattr(STDIN_FILENO,
        &display.original_termios);

    tm = display.original_termios;
    tm.c_lflag &= ~(ICANON | ECHO);
    tm.c_cc[VMIN]  = 0;
    tm.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tm);
    display.termios_changed = true;
}
```

`ISIG` は解除しない。

このため、Ctrl-Cは通常どおりSIGINTとして処理される。

### 14.2 入力監視

フレーム待ち時間中は `poll()` または `select()` を使用し、stdinとタイムアウトを同時に監視する。

stdinから `q` を受信した場合は終了する。

stdinがttyでない場合はキー入力を監視せず、シグナルだけで終了する。

---

## 15. シグナル処理

対象シグナルは次のとおりとする。

```text
SIGINT
SIGTERM
SIGHUP
SIGQUIT
```

シグナルハンドラはフラグだけを設定する。

```c
static volatile sig_atomic_t stop_requested;

static void
handle_signal(int signo)
{
    stop_requested = 1;
}
```

シグナルハンドラ内では次の処理を実行しない。

- `free()`
- `memcpy()`
- `munmap()`
- `ioctl()`
- `tcsetattr()`
- `fprintf()`

メインループが `stop_requested` を検出した後、通常のクリーンアップ処理へ移行する。

SIGKILL、カーネルパニック、ハードウェアリセットなどでは復元を保証できない。

---

## 16. 終了時クリーンアップ

### 16.1 復元順序

終了時は次の順序で処理する。

```text
1. 起動前の画面内容をVRAMへ書き戻す
2. stdinのtermiosを復元する
3. mmap領域をmunmapする
4. wsdisplayモードを起動時モードへ戻す
5. wsdisplayデバイスをcloseする
6. 保存画面バッファを解放する
7. 全MonoFrameのbitmap_dataを解放する
8. MonoFrame配列を解放する
```

画面内容はDUMBFBモード中に復元する。

### 16.2 クリーンアップ関数

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
        (void)tcsetattr(STDIN_FILENO,
            TCSAFLUSH,
            &display->original_termios);
        display->termios_changed = false;
    }

    if (display->map_base != MAP_FAILED) {
        (void)munmap(display->map_base,
            display->map_size);
        display->map_base = MAP_FAILED;
        display->fb_base = NULL;
    }

    if (display->mode_changed &&
        display->fd >= 0) {
        int mode;

        mode = display->original_mode;
        (void)ioctl(display->fd,
            WSDISPLAYIO_SMODE, &mode);
        display->mode_changed = false;
    }

    if (display->fd >= 0) {
        (void)close(display->fd);
        display->fd = -1;
    }

    free(display->saved_fb);
    display->saved_fb = NULL;
}
```

### 16.3 フレーム解放

```c
static void
free_frames(MonoFrame *frames, int frame_count)
{
    int i;

    if (frames == NULL)
        return;

    for (i = 0; i < frame_count; i++) {
        free(frames[i].bitmap_data);
        frames[i].bitmap_data = NULL;
    }

    free(frames);
}
```

---

## 17. エラー処理

### 17.1 画面モード変更前

DUMBFBモードへ移行する前のエラーでは、通常の `err()` または `errx()` による終了が可能である。

対象例は次のとおり。

```text
GIFファイルをopenできない
GIFサイズが画面を超える
DGifSlurp()が失敗
フレーム数が0
MonoFrame配列を確保できない
1bppフレーム変換が失敗
WSDISPLAYIO_GINFOが失敗
画面深度が1bppでない
```

### 17.2 画面モード変更後

DUMBFBモードへ移行した後は、直接 `err()` や `exit()` を呼び出さない。

```c
saved_errno = errno;
error_message = "...";
goto cleanup_error;
```

共通クリーンアップを実行した後、保存したエラー情報を表示して終了する。

対象例は次のとおり。

```text
mmap失敗
画面保存用バッファのmalloc失敗
termios設定失敗
フレーム描画範囲エラー
再生中の内部エラー
```

---

## 18. RAM使用量見積もり

### 18.1 前提

LUNAに物理RAMが16MB搭載されている場合でも、NetBSD/luna68kカーネル起動後のフリーメモリはおおむね12MB程度とする。

12MBすべてを画像データに使用すると、次の領域が不足する可能性がある。

- プログラムのテキストおよびデータ
- libc、giflib等の実行時領域
- スタック
- malloc管理領域
- `MonoFrame` 配列
- GIF拡張データおよびカラーマップ
- 他の常駐プロセス
- ファイルシステムバッファ
- 一時的なカーネルメモリ
- 終了時復元用の画面保存バッファ

そのため、設計上は画像変換中の主要データに使用する容量を約8MiBまでとする保守的な見積もりを使用する。

これは厳密な実行上限ではなく、スワップや極端なメモリ不足を避けるための実用上の目安である。

### 18.2 変換中のメモリ構成

現在の実装では、`extract_mono_frames()` の終了直前に次のデータが同時に存在する。

1. `DGifSlurp()` が保持する各フレームの8bppカラーインデックスデータ
2. 変換済みの全フレーム分1bppデータ

全フレームがGIF論理画面全体を持つ場合、1フレーム当たりの主要データ量は次の式となる。

```text
giflib側データ = width × height

1bpp側データ =
    ceil(width / 8) × height

変換中合計 =
    width × height
    + ceil(width / 8) × height
```

### 18.3 フルサイズフレームの場合のロード可能数

画像主要領域を8MiBとした場合の概算を次に示す。

| GIFサイズ | giflib側8bpp／フレーム | 1bpp／フレーム | 変換中合計／フレーム | 8MiB単純換算 | 実用目安 |
|---|---:|---:|---:|---:|---:|
| 800×600 | 480,000 B | 60,000 B | 540,000 B | 15フレーム | 約14フレーム |
| 640×480 | 307,200 B | 38,400 B | 345,600 B | 24フレーム | 約23フレーム |
| 512×384 | 196,608 B | 24,576 B | 221,184 B | 37フレーム | 約36フレーム |

実用目安では、次の小規模な付随領域を考慮して単純換算から1フレーム程度差し引いている。

- `SavedImage`
- `MonoFrame`
- Graphics Control Extension
- ローカルカラーマップ
- mallocアラインメントおよび管理領域

GIFファイルの構成によって付随データ量は変わるため、上記は保証値ではない。

### 18.4 部分フレームGIFの場合

giflib側の `RasterBits` は、必ずしもGIF論理画面全体のサイズではない。

GIF内の各フレームが変更矩形だけを持つ場合、giflib側の総画素データ量は概ね次の式となる。

```text
Σ(frame_width × frame_height)
```

一方、現在の `MonoFrame.bitmap_data` は各フレームについてGIF論理画面全体を保持する。

したがって、部分フレームGIFの変換中ピークは概ね次の式となる。

```text
Σ(frame_width × frame_height)
+
frame_count
× ceil(logical_width / 8)
× logical_height
```

変更矩形が小さいGIFでは、前節のフルサイズフレーム見積もりより多くのフレームを読み込める可能性がある。

正確な上限は実際のGIFに含まれる各 `ImageDesc.Width` および `ImageDesc.Height` に依存する。

### 18.5 再生開始後の常駐量

1bpp変換完了後は `DGifCloseFile()` を実行するため、`DGifSlurp()` が保持していた8bppデータは解放される。

再生中の主要画像データは、全フレーム分の1bppデータだけになる。

8MiBを1bppフレームだけに使用した場合の単純換算は次のとおりである。

| GIFサイズ | 1bpp／フレーム | 8MiB換算 |
|---|---:|---:|
| 800×600 | 60,000 B | 約139フレーム |
| 640×480 | 38,400 B | 約218フレーム |
| 512×384 | 24,576 B | 約341フレーム |

ただし、現在の `DGifSlurp()` と全フレーム一括変換方式では、その前に変換中ピークを通過する必要がある。

フルサイズフレームGIFの場合、実際に再生可能なフレーム数は原則として変換中の上限によって決まる。

### 18.6 画面保存バッファ

LUNA 1bppフレームバッファの画面保存容量は次のとおりである。

```text
256 bytes/line × 1024 lines
= 262,144 bytes
= 256KiB
```

画面保存バッファは `DGifCloseFile()` 後に確保するため、GIF変換中のピークには加算しない。

再生中には1bppフレームデータに加えて約256KiBが常駐する。

### 18.7 mmap領域

mmapしたフレームバッファ領域はデバイスメモリのマッピングであり、同じ262,144バイトの通常RAMを別途 `malloc()` するものではない。

ただし、ページテーブル等の小規模なカーネル管理領域は発生する。

### 18.8 実機での確認方法

`-p` または `-d` 使用時に、次の情報を出力できるようにすることを推奨する。

```text
GIF logical size
frame count
total RasterBits size
total 1bpp frame size
estimated peak image memory
saved framebuffer size
```

`RasterBits` の合計は次のように計算できる。

```c
uint64_t raster_total = 0;

for (i = 0; i < gif->ImageCount; i++) {
    raster_total +=
        (uint64_t)gif->SavedImages[i].ImageDesc.Width *
        gif->SavedImages[i].ImageDesc.Height;
}
```

これにより、実際のGIFがフルサイズフレーム型か差分矩形型かを含めて、より現実的なメモリ使用量を表示できる。

---

## 19. ソースファイル構成

初版では既存X11版への影響を抑えるため、次の構成を推奨する。

```text
monogifplay.c
    既存X11版

monogifplay-wscons.c
    wscons直接描画版

Makefile
    両方のターゲットを生成
```

初版ではGIF処理コードの一部重複を許容し、wscons版の実機動作確認を優先する。

動作確認後、必要に応じて次のように分割する。

```text
mono_gif.c
mono_gif.h
    MonoFrame
    GIF読み込み
    extract_mono_frames()
    時刻処理

display_x11.c
    X11表示処理

display_wscons.c
    wscons表示処理
```

---

## 20. ビルド仕様

wscons版ではX11ヘッダおよびX11ライブラリを使用しない。

必要な外部ライブラリはgiflibのみとする。

```make
PROGS = monogifplay monogifplay-wscons

WSCONS_SRCS = monogifplay-wscons.c
WSCONS_OBJS = ${WSCONS_SRCS:.c=.o}

CPPFLAGS += -Wall
CPPFLAGS += -DUNROLL_BITMAP_EXTRACT

CPPFLAGS += -I/usr/pkg/include
LDFLAGS  += -L/usr/pkg/lib -Wl,-R/usr/pkg/lib

monogifplay-wscons: ${WSCONS_OBJS}
	${CC} -o $@ ${CFLAGS} ${LDFLAGS} \
	    ${WSCONS_OBJS} -lgif
```

wscons版では次の指定は不要である。

```text
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

-I/usr/X11R7/include
-L/usr/X11R7/lib
-lX11
```

---

## 21. テスト項目

### 21.1 フレームバッファ情報

`-p` 指定時に次の情報を表示する。

```text
device
original mode
wsdisplay type
width
height
depth
stride
framebuffer offset
framebuffer size
mmap size
```

LUNA 1bppで想定される表示例は次のとおりである。

```text
device: /dev/ttyE0
mode: WSDISPLAYIO_MODE_EMUL
type: WSDISPLAY_TYPE_LUNA
size: 1280x1024
depth: 1
stride: 256
framebuffer offset: 8
framebuffer size: 262144
mmap size: 262152
```

### 21.2 ビット順

次の8画素パターンを順番に表示する。

```text
10000000
01000000
00100000
00010000
00001000
00000100
00000010
00000001
```

白画素が画面上で左から右へ移動することを確認する。

### 21.3 画素値の極性

次を確認する。

```text
全ビット0 = 黒
全ビット1 = 白
```

### 21.4 stride

640×480など、画面幅より小さい画像を表示し、各走査線が正常な位置に表示されることを確認する。

次の値を混同すると画面が崩れる。

```text
GIF src_stride = 640 / 8 = 80 bytes
LUNA fb_stride = 256 bytes
```

### 21.5 幅が8の倍数でないGIF

幅13画素などのGIFを表示し、GIF右端から次のバイト境界までの画素が破壊されないことを確認する。

### 21.6 `-c` 指定時

次を確認する。

```text
再生開始前に画面全体が白になる
GIF外領域が白のまま維持される
終了時に起動前画面へ戻る
```

### 21.7 `-c` 未指定時

次を確認する。

```text
再生開始前に画面全体を変更しない
GIF外領域に起動前画面が残る
終了時に起動前画面へ戻る
```

### 21.8 終了処理

次の終了方法ごとに、画面、termios、wsdisplayモードが復元されることを確認する。

```text
q
Ctrl-C
kill -TERM
SIGHUP
SIGQUIT
```

### 21.9 エラー経路

DUMBFBモード移行後の次のエラーでも、EMULモードへ戻ることを確認する。

```text
mmap失敗
画面保存用malloc失敗
termios設定失敗
描画範囲検査失敗
再生中の内部エラー
```

### 21.10 RAM使用量

800×600、640×480、512×384について、次を確認する。

```text
フルサイズフレームGIF
差分矩形フレームGIF
フレーム数を段階的に増加
ロード中のfree memory
DGifCloseFile()後のfree memory
再生中のfree memory
```

---

## 22. 既存GIF処理から継承する制約

現行 `extract_mono_frames()` は、透過画素または部分フレームの場合、直前の1bppフレームをコピーしてから対象部分を更新する。

Graphics Control Extensionから表示時間と透過色番号を取得するが、少なくとも次のDisposal Methodを完全には処理していない。

```text
Restore to background
Restore to previous
```

初版のGIF互換性は既存X11版と同等とする。

展示用GIFについては、次の条件を推奨する。

```text
各フレームが論理画面全体を持つ

または

Disposal Methodがunspecifiedもしくはdo not disposeで、
既存X11版で正常表示できることを事前確認する
```

---

## 23. 将来拡張

### 23.1 表示位置指定

```text
-x x
-y y
```

初期実装では `x` は8の倍数だけを許可する。

### 23.2 センタリング

```text
-C
```

`-c` を白クリアに使用するため、センタリングには大文字 `-C` など別のオプションを使用する。

```c
x = (fb_width  - gif_width)  / 2U;
y = (fb_height - gif_height) / 2U;

x &= ~7U;
```

X座標を8画素境界へ切り下げるため、厳密な中央位置から最大7画素左へずれる。

### 23.3 任意X座標

1バイト未満のX座標ずれについて、隣接バイト間のビットシフトと両端マスクを実装する。

```c
dst_bit_offset = x & 7U;
dst_byte       = x >> 3;
```

8画素境界の高速経路は別に維持する。

### 23.4 背景画像

将来、次のようなオプションを追加する。

```text
-b background-file
```

処理順序は次のとおりとする。

```text
1. 起動前画面保存
2. 必要なら白クリア
3. 背景画像を画面へ描画
4. アニメーションGIFを描画
5. 終了時に起動前画面を復元
```

背景画像形式、拡大縮小、配置方法、GIF透明画素との合成方法は別途設計する。

`-b` と `-c` を同時指定した場合は、白クリア後に背景画像を描画する。

### 23.5 GIF処理の共通化

X11版とwscons版の動作確認後、GIFデコードおよび1bpp変換処理を共通モジュールへ分離する。

---

## 24. 初版の確定仕様

```text
・NetBSD/luna68k専用
・WSDISPLAY_TYPE_LUNAかつdepth=1のみ
・/dev/ttyE0を標準デバイスとして使用
・-fまたはFRAMEBUFFERでデバイス変更可能
・起動時モードがEMULの場合のみ実行
・GIF処理は既存monogifplayと同等
・DGifSlurp()で全フレーム読み込み
・全フレームをMSB-first 1bppへ事前変換
・変換完了後にDGifCloseFile()でgiflib側データを解放
・表示位置は左上 (0, 0)
・GIF幅が8の倍数でない場合は右端をマスク
・GIFが画面より大きい場合はDUMBFB移行前にエラー終了
・strideはwsdisplay APIから取得
・LUNAフォールバック時のmmapオフセットは8バイト
・描画前に起動時の画面内容を保存
・デフォルトでは画面全体をクリアしない
・-c指定時のみ画面全体を白でクリア
・各フレームを行単位でVRAMへコピー
・GIF外領域は上書きしない
・q、SIGINT、SIGTERM、SIGHUP、SIGQUITで終了
・終了時に画面内容、termios、wsdisplayモードを復元
・X11ライブラリには依存しない
```

---

## 25. 改定履歴

### v2

- 再生開始前の白画面クリアを `-c` オプション化した。
- `-c` 未指定時は既存画面を維持する仕様とした。
- 将来の背景画像表示機能を考慮した処理順序を追加した。
- GIF透明画素と背景画像合成の制約を明記した。
- GIF変換完了後に `DGifCloseFile()` でgiflib側データを解放することを明記した。
- RAM見積もりを変換中ピークと再生中常駐量に分離した。
- NetBSD/luna68k起動後のフリーメモリ約12MBを前提とした。
- 800×600、640×480、512×384の現実的なフレーム数目安を追加した。
- 改定履歴を文書末尾へ配置した。
- 長時間・特定フレーム数の再生を最終目標とする記述を削除した。