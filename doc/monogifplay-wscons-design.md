# monogifplay wscons直接描画版 実装設計書 v4

## 1. 目的

既存の `monogifplay` が持つGIF読み込み・モノクロ変換処理を基礎として、NetBSD/luna68k の wsdisplay フレームバッファへ直接描画する `monogifplay-wscons` を追加する。

初版の最優先事項は、2026年8月1日の展示で使用する wscons版を確実に実装・試験することである。既存X11版の内部構造変更は初版の対象外とし、X11版の既存動作と試験範囲を維持する。

一方、wscons版で導入するGIF変換処理およびgiflibデータの逐次解放はX11版にも適用可能であるため、初版の単一ソース内でも、将来共通モジュールへ移動できる境界を設定して関数設計する。

プログラム名は次のとおりとする。

```text
monogifplay-wscons
```

## 2. 初版の開発範囲

### 2.1 実装対象

初版で追加・変更するファイルは次のとおりとする。

```text
Makefile
monogifplay-wscons.c
```

既存の `monogifplay.c` は変更しない。

### 2.2 試験対象

初版で新規に実施する機能試験および実機試験は、原則として `monogifplay-wscons` を対象とする。

Makefile変更後も既存 `monogifplay` が従来どおりビルドできることは確認するが、X11版へのメモリ最適化適用や内部リファクタリングは行わない。

### 2.3 将来共通化への方針

初版ではソースファイルを分割せず、`monogifplay-wscons.c` の単一ソースとする。

ただし、関数およびデータ構造を次の3層に分ける。

1. 将来X11版と共通化可能なGIF変換層
2. wscons版固有のフレーム格納層
3. wsdisplay初期化・描画・復元層

初版の実機動作確認後、必要に応じて共通化を別変更として実施する。

## 3. 対象環境

初版では次の環境に限定する。

```text
OS              NetBSD
アーキテクチャ  luna68k
wsdisplay type  WSDISPLAY_TYPE_LUNA
画面深度        1bpp
標準デバイス    /dev/ttyE0
```

次の条件を満たさない場合はエラー終了する。

```c
display.type == WSDISPLAY_TYPE_LUNA
display.depth == 1
gif_width  <= display.width
gif_height <= display.height
```

他のwsdisplayフレームバッファは、VRAMビット順、画素値の極性、stride、mmapオフセット等が異なる可能性があるため初版では対象外とする。

## 4. 既存X11版から継承する動作

次のGIF処理仕様を既存X11版から継承する。

- `DGifSlurp()` によるGIF全体の読み込み
- GIF論理画面全体への部分フレーム合成
- 透過画素で直前フレームの画素を維持する処理
- RGB輝度による1bpp二値化
- MSB-first形式への変換
- Graphics Control Extensionからの表示時間取得
- 表示時間が0の場合の既定値75ms
- `UNROLL_BITMAP_EXTRACT` による32画素単位の変換最適化

初版のGIF互換性は既存X11版と同等とする。Disposal Methodの `Restore to background` および `Restore to previous` の完全対応は初版の対象外とする。

## 5. 初版で変更するメモリ設計

### 5.1 目的

wscons版では再生元となる全1bppフレームをクライアントプロセスが保持する必要がある。

再生可能フレーム数を増やすため、次を実施する。

- フレームごとの画像データ用 `malloc()` を行わない
- 全1bppフレームを単一の匿名 `mmap()` 領域へ格納する
- 画像データとフレーム管理情報を分離する
- フレーム管理情報は小さな記述子配列へ集約する
- フレームデータの位置を生ポインタではなくpool内offsetで保持する
- 変換済みのgiflibフレームデータを順次解放する
- 起動画面保存領域を別の匿名マッピングとする
- 再生中に不要なページをVMがswapへ退避できる構成とする

### 5.2 フレーム記述子を導入する理由

初版の格納形式は全フレーム同一サイズの論理画面全体1bpp画像である。この条件だけなら、次の式でフレームアドレスを求めることができる。

```c
bitmap_pool + (size_t)frame_number * frame_bytes
```

ただし、この方式は次の前提に依存する。

- 全フレームが同一サイズ
- 全フレームが同一形式
- フレーム間にアラインメント用の空き領域がない
- フレームごとの更新矩形や格納サイズを管理しない

将来、GIF元データの更新矩形だけをVRAMへ転送する機能、可変長の差分フレーム形式、透明マスク、圧縮形式等を追加する場合、暗黙の固定長計算では格納管理を全面的に変更する必要がある。

このため初版からフレーム記述子を導入し、データ位置を明示的なpool内offsetとして保持する。初版のoffset配置自体は固定長であるが、再生側は固定長計算へ依存しない。

### 5.3 offsetを採用する理由

フレーム固有情報へ生ポインタを格納する方法も可能であるが、初版ではpool内offsetを使用する。

```c
data = animation->bitmap_pool + frame->data_offset;
```

offset方式の利点は次のとおりである。

- `mmap()` の実アドレスに依存しない
- `data_offset + data_size <= bitmap_pool_size` を検査できる
- 将来の可変長配置やアラインメント変更へ対応しやすい
- 将来ファイルマッピングへ変更する場合にも利用できる
- 32ビットluna68kでは通常、生ポインタと同じ4バイトである

1フレームの表示につき必要なアドレス加算は1回程度であり、フレーム全体のVRAMコピー量に対して実行コストは無視できる。

### 5.4 X11版との関係

giflibデータの逐次解放と1フレーム単位のモノクロ変換は、将来X11版にも共通化可能である。

ただし最終格納方式は異なる。

```text
wscons版
    全1bppフレームを匿名mmapプールへ保持

X11版の将来案
    1枚の作業バッファを使い回し、変換直後にPixmapへ転送
```

したがって、フレーム記述子、全フレームプール、offset管理、`madvise()` はwscons固有とする。GIF1フレームの変換、更新矩形の取得、delay取得、giflib所有解放だけを将来共通化可能な境界に置く。

## 6. 将来共通化可能なGIF変換層

### 6.1 `MonoGifInfo`

表示バックエンドに依存しないGIF論理画面情報を保持する。

```c
typedef struct {
    unsigned int width;
    unsigned int height;
    size_t line_bytes;
    size_t frame_bytes;
    int frame_count;
} MonoGifInfo;
```

`line_bytes` と `frame_bytes` は次の式で求める。

```c
line_bytes = ((size_t)width + 7U) / 8U;
frame_bytes = line_bytes * height;
```

積算時には `SIZE_MAX` によるオーバーフローを検査する。

GIFの画面寸法およびフレーム矩形は16ビット値で保持するため、幅と高さが `UINT16_MAX` を超える場合はエラーとする。GIF形式上の画面寸法も16ビットである。

### 6.2 `MonoGifFrameInfo`

1フレームのGIF由来メタデータを、表示バックエンドに依存しない構造体として保持する。

```c
typedef struct {
    uint32_t delay;
    uint16_t update_left;
    uint16_t update_top;
    uint16_t update_width;
    uint16_t update_height;
} MonoGifFrameInfo;
```

`update_*` は `SavedImage.ImageDesc` に記録された元のGIF更新矩形である。

初版では各フレームを論理画面全体の合成済み1bpp画像として格納するが、元の更新矩形を失わず保持する。これにより、将来は格納形式を変更せず、合成済みフルフレームから更新矩形だけをVRAMへ転送する最適化を検討できる。

### 6.3 `mono_gif_info_init()`

```c
static int
mono_gif_info_init(MonoGifInfo *info,
    unsigned int width,
    unsigned int height,
    int frame_count);
```

責務は次のとおりとする。

- 幅、高さ、フレーム数の検査
- 16ビットGIF寸法範囲の検査
- `line_bytes` の算出
- `frame_bytes` の算出
- サイズ計算時のオーバーフロー検査

表示バックエンドの資源は扱わない。

### 6.4 `mono_render_frame()`

```c
static int
mono_render_frame(GifFileType *gif,
    const MonoGifInfo *info,
    int frame,
    uint8_t *bitmap,
    const uint8_t *previous,
    MonoGifFrameInfo *frame_info);
```

1フレームをGIF論理画面全体のMSB-first 1bpp画像へ変換し、GIF由来のフレームメタデータを `frame_info` へ格納する。

責務は次のとおりとする。

- `SavedImage` とカラーマップの検査
- Graphics Control Extensionの取得
- delayの決定
- 元GIF更新矩形の保存
- 部分フレームおよび透過フレームの前画面引き継ぎ
- RGB輝度による白黒化
- MSB-first 1bppデータ生成

表示バックエンドへの格納、Pixmap生成、wsdisplay描画、メモリ解放、進捗表示は行わない。

### 6.5 `bitmap` と `previous` の契約

`bitmap` は出力先の1フレーム領域である。

`previous` は前フレームの合成済み1bpp画像であり、第1フレームでは `NULL` とする。

部分フレームまたは透過フレームの場合は次の処理を行う。

```c
if (previous == NULL)
    memset(bitmap, 0, info->frame_bytes);
else if (bitmap != previous)
    memcpy(bitmap, previous, info->frame_bytes);
```

この契約により、将来X11版では1枚の作業バッファを `bitmap` と `previous` の両方に渡してコピーを省略できる。

wscons版では、フレーム記述子から求めた現在フレームと直前フレームの格納先を渡す。

### 6.6 `mono_release_saved_image()`

```c
static void
mono_release_saved_image(SavedImage *img);
```

バックエンドが1bpp変換結果とフレームメタデータを確定した後、そのフレームのgiflib所有データを解放する。

解放対象は次のとおりとする。

```text
RasterBits
フレーム固有ColorMap
フレーム固有ExtensionBlocks
```

解放後は次の状態へ戻す。

```text
RasterBits          NULL
ColorMap            NULL
ExtensionBlocks     NULL
ExtensionBlockCount 0
```

グローバルカラーマップは後続フレームが参照する可能性があるため、この関数では解放しない。

この関数はレンダリング関数から分離し、次の所有遷移を明確にする。

```text
giflib所有の8bppフレーム
    ↓ mono_render_frame()
バックエンド所有の1bpp結果とMonoGifFrameInfo
    ↓ 格納成功後
mono_release_saved_image()
```

## 7. wscons固有のフレーム格納層

### 7.1 初版のフレーム形式

初版で実装する格納形式は次の1種類とする。

```c
enum {
    WSCONS_FRAME_FULL_1BPP = 0
};
```

`WSCONS_FRAME_FULL_1BPP` は、GIF論理画面全体の合成済みMSB-first 1bpp画像を保持する。

将来別形式を追加する場合、既存形式の意味を変更せず、新しいformat値を追加する。

### 7.2 `WsconsFrame`

```c
typedef struct {
    MonoGifFrameInfo gif;
    size_t data_offset;
    size_t data_size;
    size_t line_bytes;
    uint8_t format;
    uint8_t flags;
    uint16_t reserved;
} WsconsFrame;
```

各メンバーの意味は次のとおりとする。

```text
gif
    delayおよび元GIF更新矩形

data_offset
    bitmap_pool先頭からフレームデータまでのoffset

data_size
    フレームデータの格納バイト数

line_bytes
    格納データの1行当たりバイト数

format
    格納データ形式

flags
    将来拡張用フラグ。初版では0

reserved
    将来拡張および構造体整列用。初版では0
```

初版の `WSCONS_FRAME_FULL_1BPP` では次の値とする。

```c
frame->data_size = animation->info.frame_bytes;
frame->line_bytes = animation->info.line_bytes;
frame->format = WSCONS_FRAME_FULL_1BPP;
```

### 7.3 `WsconsAnimation`

```c
typedef struct {
    MonoGifInfo info;
    WsconsFrame *frames;
    uint8_t *bitmap_pool;
    size_t bitmap_pool_size;
} WsconsAnimation;
```

フレーム固有情報は `frames[]` に集約し、独立したdelay配列は持たない。

### 7.4 フレーム記述子配列

```c
frames = calloc(info.frame_count, sizeof(*frames));
```

初版ではフレーム記述子を順番に初期化し、固定長のフルフレーム領域を隙間なく割り当てる。

```c
pool_size = 0;

for (i = 0; i < info.frame_count; i++) {
    frames[i].data_offset = pool_size;
    frames[i].data_size = info.frame_bytes;
    frames[i].line_bytes = info.line_bytes;
    frames[i].format = WSCONS_FRAME_FULL_1BPP;

    pool_size += frames[i].data_size;
}
```

加算時には `SIZE_MAX` によるオーバーフローを検査する。

初版の結果は `i * frame_bytes` と同じになるが、再生側と変換側はこの式を使用せず、記述子に設定されたoffsetを参照する。

### 7.5 フレームプール

全フレームデータを次の匿名マッピングへ格納する。

```c
bitmap_pool = mmap(NULL, bitmap_pool_size,
    PROT_READ | PROT_WRITE,
    MAP_ANON | MAP_PRIVATE,
    -1, 0);
```

マッピング確保直後に全領域を初期化しない。フレーム変換時に対象ページだけを書き込む。

フレーム記述子配列は通常の `calloc()` 領域とし、画像データとは別に管理する。

### 7.6 フレームデータ参照

フレームデータの参照前に、次の範囲検査を行う。

```c
frame->data_offset <= animation->bitmap_pool_size

frame->data_size <=
    animation->bitmap_pool_size - frame->data_offset
```

検査成功後、実アドレスを次のように求める。

```c
data = animation->bitmap_pool + frame->data_offset;
```

変換用の書き込み可能アクセサと、再生用の読み取り専用アクセサを分離する。

```c
static uint8_t *
wscons_frame_data(WsconsAnimation *animation,
    WsconsFrame *frame);

static const uint8_t *
wscons_frame_const_data(const WsconsAnimation *animation,
    const WsconsFrame *frame);
```

### 7.7 `wscons_extract_mono_frames()`

```c
static int
wscons_extract_mono_frames(GifFileType *gif,
    WsconsAnimation *animation);
```

各フレームについて次の順序で処理する。

```text
1. frames[i]から格納形式、サイズ、offsetを取得・検査
2. offsetからプール内の出力先を求める
3. 前フレーム記述子から前フレームのアドレスを求める
4. mono_render_frame()を呼び出す
5. delayと元GIF更新矩形をframes[i].gifへ保存する
6. 変換結果がプールへ確定したことを確認する
7. mono_release_saved_image()を呼び出す
8. 進捗と変換時間を更新する
```

この関数はwscons版の全フレーム格納方針を担当する。将来X11版では、同じ `mono_render_frame()` を使用する別のループを実装する。

### 7.8 再生時の参照

再生ループはdelayを次のように取得する。

```c
delay = animation->frames[i].gif.delay;
```

描画処理は、フレーム番号から直接データアドレスを計算せず、`frames[i]` のformat、offset、size、line bytesを検査した上でデータを取得する。

初版の描画関数が対応するformatは `WSCONS_FRAME_FULL_1BPP` だけである。未知のformatは `ENOTSUP` でエラーとする。

### 7.9 変換完了後

全フレーム変換完了後、プールを読み取り専用化する。

```c
mprotect(bitmap_pool, bitmap_pool_size, PROT_READ);
```

失敗時は警告を出して再生を継続する。

次に順次アクセスのヒントを設定する。

```c
madvise(bitmap_pool, bitmap_pool_size, MADV_SEQUENTIAL);
```

フレームデータは初版では低位アドレスから高位アドレスへ順番に配置・再生する。通過済みページをVMが低優先度にし、必要に応じてswapへ退避できることを期待する。

明示的な `MADV_DONTNEED` はフレームプールには使用しない。

### 7.10 将来の部分更新

初版でも各記述子に元GIF更新矩形を保存するが、VRAM描画は論理画面全体を転送する。

将来の第1段階として、格納形式を `WSCONS_FRAME_FULL_1BPP` のまま維持し、合成済みフルフレーム内の `update_*` 矩形だけをVRAMへ転送できる。この方式ではフレームプールサイズは減らないが、VRAM書き込み量を削減できる。

将来の第2段階として、更新矩形だけを可変長で格納する場合は、新しいformatを追加する。その場合は事前走査で各フレームのpayload sizeを計算し、各 `data_offset` を割り当てた後、単一の匿名 `mmap()` を確保できる。

透明画素を含む差分データでは、黒画素と「書き換えない画素」を区別する必要があるため、次のいずれかを別途設計する。

- 1bpp画素データと1bpp書き込みマスク
- 黒・白・透明を表現する2bpp形式
- 不透明区間または変更区間を表すrun形式

また、GIF Disposal Methodの完全対応も別途必要となる。フレーム記述子方式はこれらの拡張先を提供するが、初版では実装しない。

## 8. giflibデータの解放時期

### 8.1 `DGifSlurp()` 直後

全フレームの `RasterBits`、拡張ブロック、カラーマップ等がgiflib側に存在する。

### 8.2 フレーム変換中

フレーム `i` の1bpp結果および `MonoGifFrameInfo` が `frames[i]` とプールへ確定した直後に、そのフレームの `RasterBits`、ローカルカラーマップ、拡張ブロックを解放する。

前フレームの合成結果は1bppプールに存在するため、変換済みフレームの8bppデータは不要である。

### 8.3 `DGifCloseFile()`

全フレーム変換後に通常どおり `DGifCloseFile()` を呼び出す。

個別解放済みメンバーはNULLまたは空状態へ戻しておき、`DGifCloseFile()` には未解放の残りのgiflib資源を解放させる。

変換途中でエラーになった場合も、処理済みフレームは空状態、未処理フレームはgiflib所有のままとし、共通クリーンアップから `DGifCloseFile()` を呼び出せるようにする。

## 9. コマンドライン仕様

```text
monogifplay-wscons [-c] [-d] [-p] [-f device] gif-file
```

```text
-c
    再生開始前に画面全体を白でクリアする。
    未指定時は既存画面を維持する。

-d
    GIF読み込みおよびフレーム変換の処理時間を表示する。
    -pも暗黙に有効とする。

-p
    進捗、画像情報、フレームバッファ情報を表示する。

-f device
    使用するwsdisplayデバイスを指定する。
```

デバイスの決定順序は次とする。

```text
1. -f device
2. 環境変数 FRAMEBUFFER
3. /dev/ttyE0
```

## 10. wsdisplay管理

### 10.1 `WsDisplay`

```c
typedef struct {
    int fd;
    const char *device;
    unsigned int original_mode;
    unsigned int type;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int stride;
    bool used_extended_info;
    bool mode_changed;
    size_t fb_offset;
    size_t fb_size;
    size_t map_size;
    uint8_t *map_base;
    uint8_t *fb_base;
    uint8_t *saved_fb;
    struct termios original_termios;
    bool termios_changed;
    bool stdin_is_tty;
} WsDisplay;
```

### 10.2 情報取得順序

```text
open(device, O_RDWR)
WSDISPLAYIO_GMODE
WSDISPLAYIO_GTYPE
WSDISPLAYIO_GET_FBINFOを試行
失敗時:
    WSDISPLAYIO_GINFO
    WSDISPLAYIO_LINEBYTES
```

起動時モードが `WSDISPLAYIO_MODE_EMUL` でない場合は実行しない。

### 10.3 LUNAフォールバック値

`WSDISPLAYIO_GET_FBINFO` が使用できない場合、LUNAのフレームバッファオフセットを8バイトとする。

```c
fb_offset = 8;
fb_size = stride * height;
map_size = fb_offset + fb_size;
```

LUNA 1bppで想定する値は次のとおりである。

```text
width      1280
height     1024
depth      1
stride     256
fb_offset  8
fb_size    262144
map_size   262152
```

## 11. 初期化順序

```text
1. コマンドライン解析
2. wsdisplayデバイスをopen
3. wsdisplay情報を取得・検査
4. GIFファイルをopen
5. GIF論理画面サイズを検査
6. DGifSlurp()
7. MonoGifInfoを初期化
8. WsconsFrame記述子配列を確保し、各フレームのoffsetと格納形式を設定
9. 全フレームデータ用の単一匿名mmapプールを確保
10. wscons_extract_mono_frames()で全フレームを変換
11. 各記述子へdelayと元GIF更新矩形を保存
12. 各変換後にgiflibフレームデータを逐次解放
13. DGifCloseFile()
14. フレームプールを読み取り専用化
15. フレームプールへMADV_SEQUENTIALを指定
16. シグナルハンドラを設定
17. WSDISPLAYIO_MODE_DUMBFBへ変更
18. VRAMをmmap
19. 起動画面を保存
20. 保存画面へMADV_DONTNEEDを指定
21. stdinのtermiosを必要に応じて変更
22. -c指定時だけ全面を白クリア
23. 再生開始
```

GIF読み込みと変換はDUMBFB移行前に完了させる。

## 12. 起動画面の保存

起動時画面は別の匿名マッピングへ保存する。

```c
saved_fb = mmap(NULL, fb_size,
    PROT_READ | PROT_WRITE,
    MAP_ANON | MAP_PRIVATE,
    -1, 0);

memcpy(saved_fb, fb_base, fb_size);
```

保存後は読み取り専用化し、終了まで参照しない領域として次を指定する。

```c
mprotect(saved_fb, fb_size, PROT_READ);
madvise(saved_fb, fb_size, MADV_DONTNEED);
```

`MADV_FREE` は使用しない。

終了時は `MADV_WILLNEED` を指定した後、VRAMへ書き戻す。ページインによる終了時遅延は再生速度へ影響しないため許容する。

## 13. 画面クリア

`-c` 指定時だけ、起動画面保存後に画面全体を白でクリアする。

```c
memset(fb_base, 0xff, fb_size);
```

`-c` 未指定時は全面クリアしない。GIF表示領域外には起動前の画面内容が残る。

この仕様により、将来、背景画像を表示してからGIFを重ねる処理を追加できる。

## 14. VRAM描画

表示位置は初版では `(0, 0)` 固定とする。

`wsdisplay_blit_frame()` は `WsconsFrame` のformat、offset、data size、line bytesを検査し、初版では `WSCONS_FRAME_FULL_1BPP` だけを描画する。未知のformatはエラーとする。

初版では `MonoGifFrameInfo.update_*` を描画範囲の削減には使用せず、論理画面全体を転送する。

各行の転送先は、GIF幅ではなく取得したVRAM strideから計算する。

```c
dst = fb_base + (size_t)y * display->stride;
```

幅が8の倍数の場合は行単位で `memcpy()` する。

幅が8の倍数でない場合は最終バイトをread-modify-writeし、GIF右端より外側の画素を保持する。

```c
mask = (uint8_t)(0xffU << (8U - rem_bits));

dst[full_bytes] =
    (dst[full_bytes] & (uint8_t)~mask) |
    (src[full_bytes] & mask);
```

## 15. 再生ループ

全フレームを順番に表示し、最終フレーム後は先頭へ戻る。

各フレームの表示時間は `animation.frames[i].gif.delay` から取得する。

GIF内のループ回数指定は初版では参照しない。

VRAM転送時間はフレーム表示時間に含める。

描画がdelayを超過した場合もフレームを飛ばさず、直ちに次フレームを表示する。

## 16. 入力とシグナル

stdinがttyの場合は `ICANON` と `ECHO` を解除する。`ISIG` は維持する。

`select()` でフレーム期限とstdin入力を待機し、`q` 入力で終了する。

次のシグナルを処理する。

```text
SIGINT
SIGTERM
SIGHUP
SIGQUIT
```

シグナルハンドラは `sig_atomic_t` の終了フラグだけを設定する。

## 17. クリーンアップ

終了時は次の順序で処理する。

```text
1. saved_fbへMADV_WILLNEEDを指定
2. 起動画面をVRAMへ復元
3. stdinのtermiosを復元
4. VRAMマッピングをmunmap
5. wsdisplayモードを起動時モードへ復元
6. saved_fbをmunmap
7. wsdisplay fdをclose
8. bitmap_poolをmunmap
9. WsconsFrame記述子配列をfree
```

初期化途中でも共通クリーンアップを安全に呼び出せるよう、各資源の有効状態を個別に管理する。

DUMBFB移行後は直接 `exit()` または `err()` を呼び出さず、必ず共通クリーンアップを経由する。

## 18. RAM使用量

### 18.1 前提

LUNAの物理RAM 16MBに対し、NetBSD/luna68k起動後のフリーメモリを約12MBとする。

主要画像データを物理RAM内で扱う保守的な目安として8MiBを使用する。swapが存在する場合は、それを超える匿名フレームプールも許容する。

### 18.2 `DGifSlurp()` 直後

フルサイズフレームの場合の主要データは概ね次となる。

```text
frame_count × width × height
```

`DGifSlurp()` 完了直後には全 `RasterBits` が存在するため、フルサイズフレームGIFでは通常この時点が最大の物理メモリ負荷となる。

| GIFサイズ | RasterBits／フレーム | 8MiB単純換算 | 実用目安 |
|---|---:|---:|---:|
| 800×600 | 480,000 B | 17フレーム | 約16フレーム |
| 640×480 | 307,200 B | 27フレーム | 約26フレーム |
| 512×384 | 196,608 B | 42フレーム | 約40フレーム |

### 18.3 変換中

フルサイズフレームでは1フレーム変換ごとに、8bpp `RasterBits` を解放し、1bppフレームをプールへ追加する。

```text
解放: width × height
追加: ceil(width / 8) × height
```

そのため、通常は変換が進むほど主要データ量が減少する。

差分矩形が非常に小さいGIFでは、1bpp論理画面全体の追加量がRasterBits解放量を上回り、変換終了付近がピークになる可能性がある。

### 18.4 再生中

1bppフレーム1枚のサイズは次のとおりである。

| GIFサイズ | 1bpp／フレーム | 8MiB換算 |
|---|---:|---:|
| 800×600 | 60,000 B | 約139フレーム |
| 640×480 | 38,400 B | 約218フレーム |
| 512×384 | 24,576 B | 約341フレーム |

フレームプールは匿名マッピングであるため、物理RAMに収まらない非アクティブページはswapへ退避可能である。

次のループで退避済みフレームを再び参照するとページインが発生するが、ユーザー要件として許容する。

起動画面保存領域256KiBは再生中に参照しないため、優先的にページアウト可能とする。


### 18.5 フレーム記述子の管理領域

32ビットluna68kで `WsconsFrame` が28バイトとなる場合、記述子配列の概算は次のとおりである。

| フレーム数 | 記述子配列 |
|---:|---:|
| 100 | 約2.7KiB |
| 300 | 約8.2KiB |
| 600 | 約16.4KiB |

v3のdelay配列4バイト／フレームと比較した増加量は約24バイト／フレームであり、600フレームでも約14.1KiBである。

実際の構造体サイズはコンパイラABIに依存するため、診断表示または試験では `sizeof(WsconsFrame)` を確認する。

この管理領域は画像データに比べて十分小さく、フレームプールのページアウト可能性や再生可能フレーム数へ与える影響は実質的に無視できる。

## 19. Makefile

初版では次の2ターゲットを生成する。

```make
PROGS = monogifplay monogifplay-wscons
```

依存ライブラリをターゲットごとに分離する。

```text
monogifplay
    X11
    giflib

monogifplay-wscons
    giflib
```

`monogifplay-wscons` にはX11のヘッダ検索パス、ライブラリ検索パス、`-lX11` を付加しない。

既存 `monogifplay.c` のソース内容は変更しない。

## 20. 初版のソース内配置

単一の `monogifplay-wscons.c` 内で、関数を次の順序に配置する。

```text
共通候補の基本処理
    size_mul()
    size_add()
    mono_gif_info_init()

wsconsフレーム格納
    wscons_frame_range_valid()
    wscons_frame_data()
    wscons_frame_const_data()
    wscons_animation_init()
    wscons_animation_allocate()
    wscons_animation_finish_loading()
    wscons_animation_destroy()

共通候補のGIF変換
    pixels_to_word_alignment()
    mono_render_frame()
    mono_release_saved_image()

wscons全フレーム変換制御
    wscons_extract_mono_frames()

wsdisplay処理
    wsdisplay_init()
    wsdisplay_open_and_query()
    wsdisplay_enter_dumbfb()
    wsdisplay_cleanup()
    wsdisplay_blit_frame()

再生制御
    シグナル処理
    入力待機
    main()
```

物理的には単一ファイルだが、表示バックエンド非依存関数がwsdisplay構造体やX11型を参照しないようにする。

## 21. 将来のリファクタリング案

初版の展示動作確認後、必要に応じて次の構成へ分割する。

```text
mono_gif.h
mono_gif.c
    MonoGifInfo
    MonoGifFrameInfo
    サイズ計算
    mono_gif_info_init()
    mono_render_frame()
    mono_release_saved_image()

monogifplay.c
    既存X11バックエンド
    将来は1枚のwork bitmapを使い回してPixmapへ転送

monogifplay-wscons.c
    WsconsFrame
    WsconsAnimation
    全フレームmmapプール
    madvise
    wsdisplay処理
```

この共通化は、wscons版初版とは別の変更として設計レビュー、実装、X11版回帰試験を行う。

共通化時にも、次はバックエンドごとに分離する。

```text
X11版
    Pixmap配列
    XImage/XPutImage
    Xイベント処理

wscons版
    匿名mmapフレームプール
    MADV_SEQUENTIAL
    VRAM直接描画
    起動画面保存・復元
```

## 22. テスト項目

### 22.1 初版必須試験

```text
monogifplay-wsconsのビルド
既存monogifplayのビルド
UNROLL_BITMAP_EXTRACT有効・無効
GIF論理画面サイズ検査
部分フレーム・透過フレーム変換
変換済みRasterBits等の逐次解放
DGifCloseFile()で二重解放しない
単一mmapプールへの全フレーム格納
各WsconsFrameのoffset・size・line bytesの妥当性
フレームデータ参照時のpool範囲検査
MonoGifFrameInfoのdelayと元GIF更新矩形
フレーム記述子経由の変換結果がv3固定長計算版と一致
未知のフレームformatをENOTSUPで拒否
MSB-firstビット順
LUNAの画素極性
stride 256による行描画
幅が8の倍数でないGIFの右端マスク
-c指定時と未指定時
qおよび各シグナルによる終了
画面・termios・wsdisplayモードの復元
swap使用時のループ再生
```

### 22.2 共通化境界の確認

初版内でも次を確認する。

```text
mono_render_frame()がwsdisplay型を参照しない
mono_render_frame()がフレーム格納方式を仮定しない
MonoGifFrameInfoがwscons型を含まない
mono_release_saved_image()が格納処理から分離されている
WsconsFrameとWsconsAnimationがX11型を含まない
wscons固有madviseが共通候補関数へ混入していない
```

### 22.3 初版対象外

次は初版の試験範囲に含めない。

```text
X11版への新メモリ設計適用
X11版の1枚work bitmap方式
共通ソース分割後のX11/wscons両バックエンド回帰試験
```

## 23. 将来機能

- 表示X/Y位置指定
- センタリング
- 任意ビット位置への描画
- 背景画像ファイルの表示
- 背景画像とGIF透明画素の合成
- `update_*` 矩形だけをVRAMへ転送する部分更新
- 可変長差分フレームformat
- 差分フレーム用透明マスクまたは2bpp・run形式
- X11版とのGIF変換コード共通化
- GIF Disposal Methodの完全対応

背景画像機能の処理順序は次を想定する。

```text
1. 起動画面保存
2. 必要なら白クリア
3. 背景画像を描画
4. GIFを描画
5. 終了時に起動画面を復元
```

## 24. 初版の確定仕様

```text
・初版の実装・実機試験対象はwscons版
・既存monogifplay.cは変更しない
・初版の追加ソースはmonogifplay-wscons.c単一ファイル
・将来共通化可能なGIF変換境界を単一ソース内に設定
・MonoGifInfoは表示バックエンド非依存
・MonoGifFrameInfoはdelayと元GIF更新矩形を保持
・mono_render_frame()は1フレームの1bpp変換と共通メタデータ生成を担当
・mono_release_saved_image()は変換結果確定後の所有解放を担当
・WsconsFrameはoffset・size・line bytes・format・共通メタデータを保持
・WsconsAnimationはwscons固有の記述子配列と全フレーム格納を担当
・全1bppフレームを単一匿名mmap領域へ格納
・フレームデータ位置は生ポインタでなくpool内offsetで保持
・フレーム参照時にoffsetとsizeをpool範囲内か検査
・初版のformatはWSCONS_FRAME_FULL_1BPPのみ
・初版のoffset配置は固定長だが再生側はframe番号による暗黙計算を行わない
・元GIF更新矩形は保持するが初版では全面描画する
・変換済みgiflibフレームデータを逐次解放
・全変換後にDGifCloseFile()を実行
・フレームプールを読み取り専用化
・フレームプールへMADV_SEQUENTIALを指定
・起動画面保存領域へMADV_DONTNEEDを指定
・NetBSD/luna68k、WSDISPLAY_TYPE_LUNA、1bpp専用
・標準デバイスは/dev/ttyE0
・表示位置は左上(0,0)
・デフォルトでは全面クリアしない
・-c指定時のみ全面を白クリア
・終了時に画面、termios、wsdisplayモードを復元
・X11版への最適化適用と共通ソース分割は別変更とする
```

## 25. 改定履歴

### v4

- delay配列だけを持つ固定長暗黙アドレス計算方式を廃止した。
- `MonoGifFrameInfo` を追加し、delayと元GIF更新矩形を共通メタデータとして保持する仕様とした。
- `WsconsFrame` 記述子を追加した。
- 各フレームにpool内offset、格納サイズ、line bytes、format、flagsを保持する仕様とした。
- 生ポインタではなくpool内offsetを採用する理由を明記した。
- 初版のformatとして `WSCONS_FRAME_FULL_1BPP` を定義した。
- 初版では固定長フルフレームを隙間なく配置するが、変換・再生側はframe番号からアドレスを暗黙計算しない仕様とした。
- フレームデータ参照時にoffsetとsizeのpool範囲検査を行う仕様とした。
- `mono_render_frame()` の出力をdelay単体から `MonoGifFrameInfo` へ変更した。
- 元GIF更新矩形を初版から保持し、将来の部分VRAM転送に利用できる設計とした。
- 将来の可変長差分フレームも単一mmapで扱う事前offset割当方式を記載した。
- 差分格納には透明マスク等の追加形式とDisposal Method対応が必要であることを明記した。
- フレーム記述子のRAMオーバーヘッド見積もりを追加した。
- クリーンアップをdelay配列解放からフレーム記述子配列解放へ変更した。
- テスト項目へ記述子、範囲検査、更新矩形、format検査を追加した。

### v3

- 初版の優先対象を2026年8月1日展示用wscons版に限定した。
- 既存X11版のソース変更と新規回帰試験を初版対象外とした。
- 初版は `monogifplay-wscons.c` 単一ソースとする方針を明記した。
- 単一ソース内に将来共通化可能なGIF変換境界を設定した。
- `MonoGifInfo` を表示バックエンド非依存構造として定義した。
- `mono_gif_info_init()` の責務を定義した。
- `mono_render_frame()` を1フレーム変換の共通候補関数として定義した。
- `bitmap` と `previous` の契約を定義し、将来のX11版1枚作業バッファ方式に対応可能とした。
- `mono_release_saved_image()` を所有遷移の共通候補関数として分離した。
- `WsconsAnimation` と単一匿名mmapプールをwscons固有設計として定義した。
- `wscons_extract_mono_frames()` をwscons固有の全フレーム格納制御として定義した。
- X11版とwscons版で共通化する範囲と、バックエンド固有として残す範囲を明記した。
- 将来の `mono_gif.c`、X11バックエンド、wsconsバックエンドへの分割案を追加した。
- 共通化は展示後の別変更として設計レビューと回帰試験を行う方針とした。
- v2で定義した画面クリア、画面復元、RAM最適化、エラー処理等の仕様を継承した。
