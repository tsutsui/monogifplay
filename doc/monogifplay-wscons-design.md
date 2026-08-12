# monogifplay wscons直接描画版 実装設計書 v8

> v8改訂: 第1フレームは合成済みフル画面1bpp、第2フレーム以降は元GIFの更新矩形に対応する合成済み部分1bppデータを可能な場合に格納し、再生時も部分矩形だけをVRAMへ転送する。
>
> GIF Disposal Methodの扱いはv7から変更せず、`Restore to background` および `Restore to previous` の完全対応は引き続き対象外とする。

## 1. 目的

既存の `monogifplay` が持つGIF読み込み・モノクロ変換処理を基礎として、NetBSD/luna68k の wsdisplay フレームバッファへ直接描画する `monogifplay-wscons` を追加する。

初版の最優先事項は、2026年8月1日の展示で使用する wscons版を確実に実装・試験することである。既存X11版の内部構造変更は初版の対象外とし、X11版の既存動作と試験範囲を維持する。

一方、wscons版で導入するGIF変換処理およびgiflibデータの逐次解放はX11版にも適用可能であるため、現行wscons実装内でも、将来共通モジュールへ移動できる境界を設定して関数設計する。

プログラム名は次のとおりとする。

```text
monogifplay-wscons
```

v8では、v7で定義した静止背景画像表示機能と起動画面内容の任意保存・復元を継承する。実行時のGIFデコードを避けるため、背景は専用の1bpp形式へ事前変換し、別コマンドで生成する。

```text
gif2monobg
```

`monogifplay-wscons` は専用背景形式の検査と表示だけを担当し、`gif2monobg` はGIF静止画像の読込み、二値化、専用背景形式の生成を担当する。コマンドの責務は分離するが、背景形式I/Oおよび将来のGIF変換共通化を可能にするI/F境界を初版から定義する。

## 2. 初版の開発範囲

### 2.1 実装対象

v8の実装時に追加・変更するファイルは次のとおりとする。

```text
Makefile
monogifplay-wscons.c
gif2monobg.c
monobg_format.h
monobg_format.c
```

既存の `monogifplay.c` は変更しない。

`monobg_format.h` および `monobg_format.c` は、背景ファイルの形式定義、ヘッダの符号化・復号、ファイルサイズ検査、読込み・書込みを担当し、`monogifplay-wscons` と `gif2monobg` が共用する。

GIFから1bppへの変換処理については、展示前に既存X11版まで含む変更を行わない。`gif2monobg.c` は静止GIF変換を独立実装するが、将来 `mono_gif.c` へ移動できる関数I/Fに分離する。

### 2.2 試験対象

v8で新規に実施する機能試験は次を対象とする。

```text
monogifplay-wscons
    専用背景ファイルの検査
    背景の行単位VRAM描画
    -bと既存オプションの組合せ
    既定の起動画面内容保存なし動作
    -r指定時の可視画面保存・復元
    画面内容を保存しない場合も画面モードを復元
    第1フレームのフル画面格納
    第2フレーム以降の可変長部分矩形格納
    部分矩形だけのVRAM更新
    v7と同一の合成済み表示結果

gif2monobg
    静止GIFの寸法検査
    カラーから1bppへの二値化
    専用背景形式の生成

monobg_format
    ヘッダ符号化・復号
    不正形式の拒否
    ファイルサイズの完全一致検査
```

Makefile変更後も既存 `monogifplay` が従来どおりビルドできることを確認するが、X11版へのメモリ最適化適用や内部リファクタリングは行わない。

### 2.3 共通化への方針

v8では処理を次の層に分ける。

1. 将来X11版と共通化可能なGIF変換層
2. wscons版固有のアニメーションフレーム格納層
3. wsdisplay初期化・描画・復元層
4. 背景ファイル形式I/O層
5. 静止GIFから背景データを生成する変換コマンド層

背景ファイル形式I/O層は新規機能で二つのコマンドから利用するため、初版から `monobg_format.c` として共通化する。

一方、既存X11版とwscons版のGIF変換共通化は展示後の別変更とする。v8では、`monogifplay-wscons.c` 内の `mono_render_frame()` と、`gif2monobg.c` 内の静止画像変換処理が、後から同一の `mono_gif.c` へ移動できるよう、表示先・ファイル形式・wscons型を参照しない関数境界を設ける。

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

`gif2monobg` はwsconsに依存せず、標準Cライブラリとgiflibだけを使用する。NetBSD上でのビルドを必須とし、Linux等のgiflib利用可能な開発ホストでもビルド可能なソース構成を目標とする。生成対象は初版ではLUNA 1bpp画面に固定する。

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
- 第1フレームは論理画面全体、第2フレーム以降は可能な場合に更新矩形だけを格納する
- 可変長の全1bppフレームデータを単一の匿名 `mmap()` 領域へ格納する
- 変換中だけ論理画面全体の1bpp作業バッファを1枚保持する
- 画像データとフレーム管理情報を分離する
- フレーム管理情報は小さな記述子配列へ集約する
- フレームデータの位置を生ポインタではなくpool内offsetで保持する
- 変換済みのgiflibフレームデータを順次解放する
- 起動画面内容は既定では保存せず、アニメーション用RAMを優先する
- `-r`指定時だけ可視1bpp領域を別の匿名マッピングへ保存する
- 再生中に不要なページをVMがswapへ退避できる構成とする

### 5.2 フレーム記述子を利用した可変長格納

v7までは各フレームを論理画面全体の合成済み1bpp画像として格納していたため、全フレームの `data_size` は同一であった。

v8では、初版から用意していたフレーム記述子の `data_offset`、`data_size`、`line_bytes`、`format` を使用し、フレームごとに異なるサイズのpayloadを格納する。

```c
bitmap_pool + frame->data_offset
```

第1フレームはループ先頭でGIF表示領域全体を確定させる必要があるため、常に論理画面全体の合成済み1bpp画像とする。

第2フレーム以降は、元GIFの `SavedImage.ImageDesc` 矩形をX方向の外側8画素境界まで広げた領域を候補とする。候補payloadがフルフレームより小さい場合だけ部分形式を選択し、小さくならない場合は従来のフル形式を使用する。

```c
stored_byte_left = update_left / 8U;
stored_line_bytes =
    ((update_left & 7U) + update_width + 7U) / 8U;
stored_size = stored_line_bytes * update_height;
```

X方向へ広げた画素は、変換用の合成済み作業バッファから取得する。そのため元GIF更新矩形外の端数画素にも直前までの正しい合成結果が入り、透明マスクを追加せずバイト単位でVRAMへ転送できる。

この方式は、元GIFの生ラスターをそのまま保存する差分形式ではない。各payloadは `mono_render_frame()` が生成した合成済み1bpp画像の一部であり、v7と同じ表示結果を維持する。

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
    1枚の合成用作業バッファを使用
    フルまたは部分1bppフレームを可変長匿名mmapプールへ保持

X11版の将来案
    1枚の作業バッファを使い回し、変換直後にPixmapへ転送
```

したがって、フレーム記述子、可変長フレームプール、offset管理、部分矩形抽出、`madvise()` はwscons固有とする。GIF1フレームの変換、更新矩形の取得、delay取得、giflib所有解放だけを将来共通化可能な境界に置く。

### 5.5 背景画像読込みの一時メモリ

アニメーションの1bppフレームプールが物理RAMを可能な限り使用する構成では、一度しか使用しない背景画像のためにpayload全体と同じ匿名メモリを確保しない。

MonoBGのヘッダとファイルサイズはDUMBFB移行前に検査するが、payloadは全体をメモリへ保持せず、1行分の小さなバッファへ順次読み込みながらVRAMへ転送する。

LUNAでは背景payload全体が163840バイトであるのに対し、1行分は160バイトである。全体バッファ方式はファイルキャッシュとは別に163840バイトの書込み済み匿名メモリを一時的に発生させ、メモリ逼迫時にはアニメーションフレームのページアウトを誘発する可能性がある。解放後も、すでにページアウトされたアニメーションページが自動的に元の常駐状態へ戻るとは限らない。

行単位ストリーミングでも背景ファイルのページがファイルキャッシュへ入る可能性はあるが、payload全体の匿名コピーを重ねないため、追加メモリ圧力は小さい。ファイルキャッシュはクリーンなファイルページであり、匿名フレームページのようにswapへ書き出さずに再取得可能である。

背景読込み速度より、アニメーション再生開始時のフレームプール常駐状態を優先する。背景読込みは起動時の1回だけであるため、1024行分のread処理は許容する。


### 5.6 起動画面内容の保存に関する非機能要件

アニメーション用1bppフレームプールが物理RAMを可能な限り使用する条件では、終了時にしか使用しない起動画面保存領域を既定で確保しない。

画面内容の保存・復元と、wsdisplay画面モードの復元は別の責務として扱う。

```text
常時実施
    stdinのtermios復元
    VRAMのmunmap
    wsdisplay画面モードの起動時モードへの復元
    wsdisplay fdのclose

-r指定時だけ実施
    DUMBFB移行直後の可視画面内容の保存
    終了時の可視画面内容の復元
```

既定動作では起動画面内容を保存しない。この場合、終了時に画面モードは復元するが、表示画素の内容は復元しない。終了後の画面内容は未規定とし、最終アニメーションフレーム、背景、または再開したコンソール表示が混在する可能性を許容する。

`-r`指定時もVRAM stride内の非表示paddingは保存しない。保存対象は可視1bpp画素だけとする。

```c
visible_line_bytes = ((size_t)display->width + 7U) / 8U;
saved_fb_size = visible_line_bytes * display->height;
```

LUNA 1bppでは次となる。

```text
visible_line_bytes = 160 bytes
saved_fb_size       = 160 × 1024
                    = 163840 bytes
```

従来仕様の `stride × height = 262144` バイト保存に比べ、保存指定時の匿名メモリを98304バイト削減する。既定動作では保存領域を確保せず、追加匿名メモリは0バイトとする。


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

v8では第1フレームを論理画面全体、後続フレームを可能な場合に更新矩形相当の部分payloadとして格納するため、この情報を格納範囲とVRAM転送位置の決定に使用する。`update_*` 自体は元GIFの矩形を保持し、X方向に広げた格納範囲は `update_left`、`update_width`、`line_bytes` から導出する。

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

wscons版では、論理画面全体の1bpp作業バッファを1枚確保し、第1フレームでは `previous == NULL`、第2フレーム以降では同じ作業バッファを `bitmap` と `previous` の両方へ渡す。各フレームの合成後に、フレーム記述子が指定するフルまたは部分領域をbitmap poolへコピーする。

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

### 7.1 v8のフレーム形式

v8で実装する格納形式は次の2種類とする。

```c
enum {
    WSCONS_FRAME_FULL_1BPP = 0,
    WSCONS_FRAME_PARTIAL_1BPP
};
```

`WSCONS_FRAME_FULL_1BPP` は、GIF論理画面全体の合成済みMSB-first 1bpp画像を保持する。

`WSCONS_FRAME_PARTIAL_1BPP` は、元GIF更新矩形をX方向の外側8画素境界まで広げた、合成済みMSB-first 1bpp矩形を保持する。payload内には透明値を持たず、すべてのビットがVRAMへ書き込める確定画素である。

第1フレームは常に `WSCONS_FRAME_FULL_1BPP` とする。第2フレーム以降は部分payloadがフルpayloadより小さい場合だけ `WSCONS_FRAME_PARTIAL_1BPP` とし、それ以外はフル形式へ戻す。

未知のformatは `ENOTSUP` でエラーとする。

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
    将来拡張用フラグ。v8では0

reserved
    将来拡張および構造体整列用。v8では0
```

フル形式では次の値とする。

```c
frame->data_size = animation->info.frame_bytes;
frame->line_bytes = animation->info.line_bytes;
frame->format = WSCONS_FRAME_FULL_1BPP;
```

部分形式では次の値とする。

```c
frame->line_bytes =
    ((frame->gif.update_left & 7U) +
     frame->gif.update_width + 7U) / 8U;
frame->data_size =
    frame->line_bytes * frame->gif.update_height;
frame->format = WSCONS_FRAME_PARTIAL_1BPP;
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

### 7.4 フレーム記述子配列と事前レイアウト

```c
frames = calloc(info.frame_count, sizeof(*frames));
```

`DGifSlurp()` 後、全 `SavedImage.ImageDesc` を事前走査して各フレームの格納形式とサイズを決める。

```text
第1フレーム
    常にFULL

第2フレーム以降
    X方向を8画素境界まで広げた部分サイズを計算
    部分サイズ < info.frame_bytes ならPARTIAL
    それ以外はFULL
```

各offsetは可変長payloadを隙間なく順番に配置して求める。

```c
pool_size = 0;

for (i = 0; i < info.frame_count; i++) {
    wscons_frame_layout(&frames[i], info,
        &gif->SavedImages[i].ImageDesc, i == 0);
    frames[i].data_offset = pool_size;
    pool_size += frames[i].data_size;
}
```

加算と乗算時には `SIZE_MAX` によるオーバーフローを検査する。各ImageDescがGIF論理画面内に収まることもDUMBFB移行前に検査する。

### 7.5 フレームプール

全フレームの可変長データを次の単一匿名マッピングへ格納する。

```c
bitmap_pool = mmap(NULL, bitmap_pool_size,
    PROT_READ | PROT_WRITE,
    MAP_ANON | MAP_PRIVATE,
    -1, 0);
```

マッピング確保直後に全領域を初期化しない。フレーム変換時に実際のpayloadだけを書き込む。

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

### 7.7 合成用作業バッファ

`wscons_extract_mono_frames()` の開始時に、論理画面全体の1bpp作業バッファを1枚だけ確保する。

```c
canvas = malloc(animation->info.frame_bytes);
```

作業バッファは0で初期化し、全フレーム変換完了後またはエラー時に解放する。再生中には保持しない。

第1フレームでは次のように呼び出す。

```c
mono_render_frame(gif, info, 0, canvas, NULL, &frame->gif);
```

第2フレーム以降はインプレース合成する。

```c
mono_render_frame(gif, info, i, canvas, canvas, &frame->gif);
```

これにより、前フレームのフル画像をbitmap poolに保持しなくても、v7と同じ前画面引き継ぎを実現する。

### 7.8 合成結果の格納

`wscons_store_composited_frame()` は、合成済みcanvasから記述子のformatに応じたpayloadをbitmap poolへコピーする。

フル形式では論理画面全体を1回コピーする。

```c
memcpy(data, canvas, info.frame_bytes);
```

部分形式では、格納開始バイトと各行のコピー元を次で求める。

```c
src_byte = frame->gif.update_left / 8U;
src = canvas
    + (frame->gif.update_top + y) * info.line_bytes
    + src_byte;
```

各行 `frame->line_bytes` バイトだけをコピーする。X方向へ広げられた端数画素もcanvas上の合成済み値であり、そのままpayloadへ含める。

### 7.9 `wscons_extract_mono_frames()`

各フレームについて次の順序で処理する。

```text
1. 記述子から事前決定済みformat、size、offsetを取得する
2. mono_render_frame()で全画面canvasをインプレース更新する
3. delayと元GIF更新矩形をframes[i].gifへ保存する
4. wscons_store_composited_frame()でフルまたは部分payloadを格納する
5. mono_release_saved_image()を呼び出す
6. 進捗と変換時間を更新する
```

`mono_render_frame()` の合成意味はv7から変更しない。Disposal Methodも引き続き参照しない。

### 7.10 再生時の参照と変換完了後

再生ループはdelayを次のように取得する。

```c
delay = animation->frames[i].gif.delay;
```

描画処理はformat、offset、size、line bytes、更新矩形を検査した上でデータを取得する。フレーム番号から固定長アドレスを計算しない。

全フレーム変換完了後、プールを読み取り専用化する。

```c
mprotect(bitmap_pool, bitmap_pool_size, PROT_READ);
```

次に順次アクセスのヒントを設定する。

```c
madvise(bitmap_pool, bitmap_pool_size, MADV_SEQUENTIAL);
```

フレームpayloadは再生順に低位アドレスから高位アドレスへ配置する。明示的な `MADV_DONTNEED` はフレームプールには使用しない。

### 7.11 部分形式の制約

部分payloadは元GIFの生ラスターではなく、合成済みcanvasの切り出しである。したがって透明画素用マスクは不要であり、部分矩形内の全ビットをVRAMへ書き込む。

GIF Disposal Methodの `Restore to background` および `Restore to previous` はv7と同様に処理しない。これらを正規対応する場合は、前フレームのdispose操作と現フレーム描画を複数更新として表現するか、合成前後の差分矩形を生成する別設計が必要であり、v8の対象外とする。

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
monogifplay-wscons [-C] [-c] [-d] [-p] [-r] [-f device]
    [-b background-file] [-x x-position] [-y y-position] gif-file
```

```text
-C
    GIF論理画面をフレームバッファ内で中央配置する。
    X方向は8画素境界へ切り下げる。

-b background-file
    専用MonoBG形式の背景画像を、アニメーション開始前に
    フレームバッファ全面へ表示する。

-c
    再生開始前に画面全体を白でクリアする。
    未指定時は既存画面を維持する。

-d
    GIF読み込みおよびフレーム変換の処理時間を表示する。
    -pも暗黙に有効とする。

-p
    進捗、画像情報、フレームバッファ情報を表示する。

-r
    DUMBFB移行直後の可視画面内容を保存し、終了時に復元する。
    未指定時は画面内容を保存・復元しない。
    画面モードとtermiosは指定の有無にかかわらず復元する。

-f device
    使用するwsdisplayデバイスを指定する。

-x x-position
    GIF論理画面左上のX座標を画素単位で指定する。
    初版では8の倍数だけを許可する。

-y y-position
    GIF論理画面左上のY座標を画素単位で指定する。
    任意の非負整数を許可する。
```

デバイスの決定順序は次とする。

```text
1. -f device
2. 環境変数 FRAMEBUFFER
3. /dev/ttyE0
```

`-b` と `-c` は同時指定不可とする。背景画像は画面全体を置き換えるため、背景描画直前の白クリアには意味がないためである。併用時はDUMBFBへ移行する前にusageエラーとする。

### 9.1 配置指定の優先順位

配置は軸ごとに決定する。

```text
1. -xまたは-yによる明示指定
2. -Cによる中央配置
3. 既定値0
```

したがって、次の指定を許可する。

```text
-C
    X/Yとも中央配置

-C -x 0
    Xは左端、Yだけ中央配置

-C -y 0
    Xだけ中央配置、Yは上端

-C -x 0 -y 0
    明示指定を優先するため左上配置
```

オプションの記載順には依存しない。全オプションを解析した後に最終位置を決定する。

### 9.2 座標値の解析と未指定値

`-x` および `-y` の保持変数は符号付き整数とし、初期値 `-1` を未指定値として使用する。

```c
long requested_x = -1;
long requested_y = -1;
```

指定された値は、既存X11版のオプション解析と同様に、各 `case` 内で `strtol()` により10進整数として解析する。

```c
requested_x = strtol(optarg, &endptr, 10);
if (*endptr != '\0' || requested_x < 0)
    usage();
```

`-y` も同様とする。数値以外の文字を含む値および負数はusageエラーとする。

X座標の8画素境界、画面内に収まるかどうか、および実際に使用可能な座標範囲は、GIF論理画面サイズとフレームバッファサイズが確定した後の最終位置決定処理でまとめて検査する。オプション解析時にはこれらを重複して検査しない。

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
    size_t visible_line_bytes;
    size_t saved_fb_size;
    uint8_t *map_base;
    uint8_t *fb_base;
    uint8_t *saved_fb;
    struct termios original_termios;
    bool termios_changed;
    bool stdin_is_tty;
} WsDisplay;
```

### 10.2 フレームバッファ情報の取得

情報取得は次の順序で行う。

```text
open(device, O_RDWR)
WSDISPLAYIO_GMODE
WSDISPLAYIO_GTYPE
WSDISPLAYIO_GET_FBINFOを試行
```

`WSDISPLAYIO_GET_FBINFO` が成功し、取得値が有効な場合は、次の情報を使用する。

```text
width
height
depth
stride
fb_offset
```

`WSDISPLAYIO_GET_FBINFO` が未定義、ioctlが失敗、または取得値が無効な場合は、次のioctlへフォールバックする。

```text
WSDISPLAYIO_GINFO
    width
    height
    depth

WSDISPLAYIO_LINEBYTES
    stride
```

起動時モードが `WSDISPLAYIO_MODE_EMUL` でない場合は実行しない。

### 10.3 LUNA固有のmmapオフセット補完

`WSDISPLAYIO_GINFO` および `WSDISPLAYIO_LINEBYTES` からは、mmap領域先頭から実フレームバッファ先頭までのオフセットを取得できない。

そのため、前節のフォールバック経路であり、かつ `WSDISPLAYIO_GTYPE` で `WSDISPLAY_TYPE_LUNA` を確認済みの場合に限り、LUNA固有値として次を設定する。

```c
fb_offset = 8;
```

フレームバッファサイズおよびmmapサイズは、ioctlから取得した `stride` と `height` を使用して計算する。

```c
fb_size = stride * height;
map_size = fb_offset + fb_size;
```

幅、高さ、深度、strideは固定値として設定せず、`WSDISPLAYIO_GINFO` および `WSDISPLAYIO_LINEBYTES` の結果を使用する。

LUNA 1bpp実機で想定される取得値と計算結果は次のとおりである。

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
2. -bと-cの排他条件を検査
3. wsdisplayデバイスをopen
4. wsdisplay情報を取得・検査
5. visible_line_bytesと、-r指定時のsaved_fb_sizeを計算・検査
6. -b指定時は背景ファイルをopen
7. -b指定時は背景ヘッダ、実ファイルサイズ、表示環境との一致を検査
8. -b指定時は1行分の背景読込みバッファを確保
9. GIFアニメーションファイルをopen
10. GIF論理画面サイズを検査
11. MonoGifInfoを初期化
12. 表示X/Y位置を決定し、画面内に収まることを検査
13. DGifSlurp()
14. 全ImageDescを事前走査し、各フレームのFULL/PARTIAL形式とpayload sizeを決定
15. WsconsFrame記述子配列へ可変長offsetを設定
16. 可変長全フレームデータ用の単一匿名mmapプールを確保
17. 論理画面全体の1bpp作業バッファを1枚確保
18. wscons_extract_mono_frames()で全フレームを合成・格納
19. 各記述子へdelayと元GIF更新矩形を保存
20. 各変換後にgiflibフレームデータを逐次解放
21. 作業バッファを解放
22. DGifCloseFile()
23. フレームプールを読み取り専用化
24. フレームプールへMADV_SEQUENTIALを指定
25. シグナルハンドラを設定
26. WSDISPLAYIO_MODE_DUMBFBへ変更
27. VRAMをmmap
28. -r指定時だけ可視画面内容を行単位で保存
29. -r指定時の保存領域へMADV_DONTNEEDを指定
30. stdinのtermiosを必要に応じて変更
31. -b指定時は背景payloadを1行ずつ読込み、直ちにVRAMへ描画
32. 背景ファイルをcloseし、1行分のバッファを解放
33. -c指定時は画面全体を白でクリア
34. アニメーション再生開始
```

GIFファイルの読込みとアニメーション変換、背景ファイルのヘッダ・サイズ・表示環境検査はDUMBFB移行前に完了させる。

背景payload自体はDUMBFB移行前には読み込まない。背景ファイルは検査後もopenしたままとし、ファイル位置をpayload先頭に置いておく。VRAMをmmapし、`-r`指定時には起動画面内容を保存した後、1行分を読み込んでは対応するVRAM行へ転送する。

payload読込み中にI/Oエラー、予期しないEOF、シグナルによる終了が発生した場合は共通クリーンアップへ移行する。`-r`指定時は可視画面内容も復元する。`-r`未指定時は画面内容を復元せず、termiosとwsdisplay画面モードだけを復元する。

## 12. 起動画面内容の任意保存・復元

### 12.1 既定動作

`-r`を指定しない場合、起動画面内容を保存する匿名マッピングを確保しない。

終了時またはDUMBFB移行後のエラー時には、次を実行する。

```text
stdinのtermiosを復元
VRAMマッピングをmunmap
wsdisplay画面モードを起動時モードへ復元
wsdisplay fdをclose
```

画面内容は復元しない。終了後の表示画素内容は未規定とする。

### 12.2 `-r`指定時の保存領域

`-r`指定時だけ、可視1bpp画面を保存する匿名マッピングを確保する。

```c
saved_fb = mmap(NULL, saved_fb_size,
    PROT_READ | PROT_WRITE,
    MAP_ANON | MAP_PRIVATE,
    -1, 0);
```

`visible_line_bytes` と `saved_fb_size` は次で計算する。

```c
visible_line_bytes =
    ((size_t)display->width + 7U) / 8U;

saved_fb_size =
    visible_line_bytes * display->height;
```

積算時にはオーバーフローを検査し、`display->stride < visible_line_bytes` の場合はエラーとする。

### 12.3 行単位の保存

VRAM stride内の非表示paddingを保存せず、可視画素だけを行単位で保存する。

```c
for (y = 0; y < display->height; y++) {
    memcpy(saved_fb + (size_t)y * visible_line_bytes,
        display->fb_base + (size_t)y * display->stride,
        visible_line_bytes);
}
```

保存後は読み取り専用化し、終了まで参照しない領域として次を指定する。

```c
mprotect(saved_fb, saved_fb_size, PROT_READ);
madvise(saved_fb, saved_fb_size, MADV_DONTNEED);
```

`mprotect()` または `madvise()` の失敗は画面保存内容の正当性を失わせない場合、警告として処理を継続してよい。`MADV_FREE` は使用しない。

### 12.4 終了時の復元

`saved_fb` が有効な場合だけ、復元前に次を指定する。

```c
madvise(saved_fb, saved_fb_size, MADV_WILLNEED);
```

その後、可視画素だけを行単位でVRAMへ書き戻す。

```c
for (y = 0; y < display->height; y++) {
    memcpy(display->fb_base + (size_t)y * display->stride,
        saved_fb + (size_t)y * visible_line_bytes,
        visible_line_bytes);
}
```

復元後に保存領域を `munmap()` する。

VRAM stride内の非表示paddingは保存・復元しない。`-r`が保証するのは可視画面内容の復元である。

### 12.5 状態管理

コマンドライン解析ではローカルな真偽値として `restore_screen` を保持する。

保存領域確保後の実状態は `saved_fb != MAP_FAILED` で判断する。クリーンアップでは、オプション指定値ではなく実際に保存領域が確保されたかを確認して復元する。

```c
if (display->saved_fb != MAP_FAILED)
    wsdisplay_restore_visible(display);
```

保存領域確保前のエラーでは画面内容復元を試みない。

## 13. 初期画面の設定


### 13.1 背景未指定時

`-b`を指定せず、`-c`を指定した場合だけ、VRAMのmmap後、`-r`指定時には起動画面内容を保存した後で画面全体を白でクリアする。

```c
memset(fb_base, 0xff, fb_size);
```

`-b` と `-c` のいずれも指定しない場合は全面を変更しない。GIF表示領域外には起動前の画面内容が残る。

### 13.2 背景指定時

`-b`指定時はVRAMのmmap後、`-r`指定時には起動画面内容を保存した後で、専用背景画像の可視画素payloadを1行ずつファイルから読み込み、対応するVRAM行へ直ちに描画する。

背景はフレームバッファの表示幅・高さと完全一致するため、画面の可視領域全体を置き換える。VRAM stride内の非表示paddingは変更しない。

背景payload全体を保持する一時バッファは作成しない。背景描画完了後は背景ファイルをcloseし、1行分の読込みバッファを解放する。再生中には背景ファイルおよび背景データを保持しない。

### 13.3 アニメーションとの関係

第1アニメーションフレームはGIF論理画面全体をVRAMへ転送する。第2フレーム以降は、元GIF更新矩形に対応する合成済み部分payloadだけを転送する。

第1フレームでGIF表示矩形全体が確定するため、背景はGIF表示矩形の外側に残る。GIF矩形内の透明画素はv7と同様にGIFの前画面を維持する意味であり、背景を透過表示する合成はv8の対象外とする。

## 14. 表示位置の決定

### 14.1 配置情報

表示位置は再生中に変化しないため、`main()` または小さな配置構造体で次を保持する。

```c
typedef struct {
    unsigned int x;
    unsigned int y;
} DisplayPosition;
```

フレーム記述子には表示位置を重複して保持しない。全フレームが同じGIF論理画面位置へ表示されるためである。

### 14.2 中央配置の計算

GIFがフレームバッファ内に収まることを確認した後、中央位置を次で求める。

```c
center_x = (display->width  - gif->width)  / 2U;
center_y = (display->height - gif->height) / 2U;
```

初版のX方向描画はバイト境界に限定するため、中央X座標を8画素境界へ切り下げる。

```c
center_x &= ~7U;
```

したがって、厳密な中央位置より最大7画素左へずれる場合がある。Y方向は厳密な整数中央位置を使用する。

### 14.3 最終位置の決定

```c
position.x = requested_x >= 0 ? requested_x :
    center_requested ? center_x : 0;

position.y = requested_y >= 0 ? requested_y :
    center_requested ? center_y : 0;
```

`requested_x` または `requested_y` が0以上なら、その軸は明示指定済みと判断する。負値の場合は未指定であり、`-C` が指定されていれば中央位置、指定されていなければ0を使用する。明示指定を表す別のフラグは保持しない。

### 14.4 表示範囲の検査

減算時のunderflowと加算時のoverflowを避けるため、先にGIF寸法を検査してから次を使用する。

```c
if (gif_width > display_width ||
    gif_height > display_height)
    return -1;

if (position.x > display_width - gif_width ||
    position.y > display_height - gif_height)
    return -1;
```

範囲外の場合はDUMBFBへ移行する前にエラー終了する。

`-p` 指定時は最終的に採用した表示位置を出力する。

```text
position: 320,272
```

## 15. VRAM描画

`wsdisplay_blit_frame()` は表示位置を引数として受け取る。

```c
static int
wsdisplay_blit_frame(const WsDisplay *display,
    const WsconsAnimation *animation,
    int frame_number,
    unsigned int dst_x,
    unsigned int dst_y);
```

関数内でも防御的に次を検査する。

```c
(dst_x & 7U) == 0

dst_x <= display->width  - animation->info.width
dst_y <= display->height - animation->info.height
```

`WsconsFrame` のformat、offset、data size、line bytesおよび更新矩形を検査する。未知のformatはエラーとする。

### 15.1 フルフレーム描画

`WSCONS_FRAME_FULL_1BPP` はv7と同じ処理で論理画面全体を行単位転送する。

```c
dst = display->fb_base
    + (size_t)(dst_y + y) * display->stride
    + dst_x / 8U;
```

幅が8の倍数でない場合は最終バイトをread-modify-writeし、GIF右端より外側の画素を保持する。

### 15.2 部分フレーム描画

`WSCONS_FRAME_PARTIAL_1BPP` の格納開始位置は次で求める。

```c
byte_left = frame->gif.update_left / 8U;
```

各行の転送先は次とする。

```c
dst = display->fb_base
    + (size_t)(dst_y + frame->gif.update_top + y)
      * display->stride
    + dst_x / 8U
    + byte_left;
```

転送行数は `update_height`、1行の転送量は `frame->line_bytes` とする。

X方向の左端は格納時に8画素境界へ切り下げられているため、ビットシフトおよび左端マスクを必要としない。元GIF矩形外へ広がった画素は合成済みcanvasから取得済みであり、そのまま書き込む。

部分矩形がGIF論理画面の最終バイトへ達し、論理画面幅が8の倍数でない場合だけ、最終バイトをread-modify-writeしてGIF右端外のフレームバッファ画素を保持する。

### 15.3 ループ先頭

第1フレームは常にフル形式であるため、最終フレームから先頭へ戻る際もGIF表示矩形全体が再設定される。部分フレームは必ず直前までの再生結果へ適用する前提とし、途中フレームから単独表示する機能はv8の対象外とする。

## 16. 再生ループ

全フレームを順番に表示し、最終フレーム後は先頭へ戻る。

各フレームの表示時間は `animation.frames[i].gif.delay` から取得する。

GIF内のループ回数指定は初版では参照しない。

VRAM転送時間はフレーム表示時間に含める。

描画がdelayを超過した場合もフレームを飛ばさず、直ちに次フレームを表示する。

## 17. 入力とシグナル

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

## 18. クリーンアップ

終了時は次の順序で処理する。

```text
1. saved_fbが有効ならMADV_WILLNEEDを指定
2. saved_fbが有効なら可視画面内容を行単位でVRAMへ復元
3. stdinのtermiosを復元
4. VRAMマッピングをmunmap
5. wsdisplayモードを起動時モードへ復元
6. saved_fbが有効ならmunmap
7. wsdisplay fdをclose
8. bitmap_poolをmunmap
9. WsconsFrame記述子配列をfree
```

`-r`未指定時は `saved_fb == MAP_FAILED` のままであり、手順1、2、6を行わない。画面モードとtermiosの復元は `-r` の有無にかかわらず実施する。

初期化途中でも共通クリーンアップを安全に呼び出せるよう、各資源の有効状態を個別に管理する。

DUMBFB移行後は直接 `exit()` または `err()` を呼び出さず、必ず共通クリーンアップを経由する。

## 19. RAM使用量


### 19.1 前提

LUNAの物理RAM 16MBに対し、NetBSD/luna68k起動後のフリーメモリを約12MBとする。

主要画像データを物理RAM内で扱う保守的な目安として8MiBを使用する。swapが存在する場合は、それを超える匿名フレームプールも許容する。

### 19.2 `DGifSlurp()` 直後

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

### 19.3 変換中

変換中は可変長フレームプールに加え、論理画面全体の1bpp作業バッファを1枚だけ保持する。

```text
作業バッファ: ceil(width / 8) × height
```

各フレームの8bpp `RasterBits` は変換後に逐次解放する。第1フレームはフル1bppを追加し、第2フレーム以降は更新矩形相当の部分payload、または縮小できない場合だけフルpayloadを追加する。

作業バッファは全フレーム変換後に解放され、再生中のRAMには含まれない。

### 19.4 再生中

v8のフレームプールサイズは次の合計となる。

```text
第1フレーム
    full_frame_bytes

第2フレーム以降
    min(full_frame_bytes,
        aligned_update_line_bytes × update_height)
```

全ImageDescが論理画面全体の場合はv7と同じサイズになる。元GIFが部分矩形で最適化されている場合は、保存RAMとVRAM書き込み量を同時に削減できる。

480×420、229フレームの場合、v7の最大プールは次である。

```text
60 × 420 × 229 = 5770800 bytes
```

v8の実際のサイズは各ImageDescに依存し、`-p` または `-d` の `1bpp pool` 表示で確認する。

フレームプールは匿名マッピングであるため、物理RAMに収まらない非アクティブページはswapへ退避可能である。既定動作では起動画面保存領域を確保しない。`-r`指定時だけ可視画面163840バイトを保存する。

### 19.5 フレーム記述子の管理領域

32ビットluna68kで `WsconsFrame` が28バイトとなる場合、記述子配列の概算は次のとおりである。

| フレーム数 | 記述子配列 |
|---:|---:|
| 100 | 約2.7KiB |
| 300 | 約8.2KiB |
| 600 | 約16.4KiB |

v3のdelay配列4バイト／フレームと比較した増加量は約24バイト／フレームであり、600フレームでも約14.1KiBである。

実際の構造体サイズはコンパイラABIに依存するため、診断表示または試験では `sizeof(WsconsFrame)` を確認する。

この管理領域は画像データに比べて十分小さく、フレームプールのページアウト可能性や再生可能フレーム数へ与える影響は実質的に無視できる。

### 19.6 背景画像の一時メモリ

LUNA 1280×1024の背景payloadは次のサイズである。

```text
line_bytes   = 1280 / 8 = 160 bytes
payload_size = 160 × 1024 = 163840 bytes
```

v8ではpayload全体の163840バイトをユーザー空間へ読み込まない。背景表示のために確保するデータバッファは1行分の160バイトだけとする。

640×480の1bppアニメーションフレームは38400バイトであるため、163840バイトは約4.3フレーム分に相当する。800×600では約2.7フレーム分である。物理RAMをフレームプールがほぼ使い切る条件では、一時的であってもpayload全体の匿名バッファを追加することは無視できない。

行単位ストリーミングでもファイルシステムのページキャッシュが背景ファイルを保持する可能性はあるため、追加RAM使用量が厳密に160バイトだけになるとは限らない。ただし、payload全体の匿名コピーを重ねないため、全読込み方式よりメモリ圧力は明確に小さい。

背景ファイルには順次1回だけアクセスする。実装環境で利用可能な場合は、`posix_fadvise()` の `POSIX_FADV_SEQUENTIAL` および転送完了後の `POSIX_FADV_DONTNEED` を性能上のヒントとして使用してよい。これらの失敗は背景表示の正当性へ影響しないため、エラー終了条件にはしない。

### 19.7 起動画面保存領域

既定動作では起動画面内容を保存しないため、保存用匿名メモリは0バイトである。

`-r`指定時は可視画素だけを保存する。

```text
visible_line_bytes = 1280 / 8 = 160 bytes
saved_fb_size       = 160 × 1024
                    = 163840 bytes
```

従来のstride全体保存は262144バイトであったため、`-r`指定時でも98304バイト削減する。

アニメーションフレーム換算では163840バイトは次に相当する。

```text
640×480 1bpp
    38400 bytes/frame
    約4.3フレーム

800×600 1bpp
    60000 bytes/frame
    約2.7フレーム
```

フレームプールが物理RAMをほぼ使い切る場合、この差は無視できない。展示デモでは画面内容復元を必要としないため、既定の復元なしを使用する。


## 20. MonoBG背景ファイル形式

### 20.1 形式の目的

背景ファイルは、フレームバッファの可視画素を1bpp MSB-firstで格納した専用形式とする。形式名を `MonoBG`、推奨拡張子を `.mbg` とする。拡張子は検査条件に使用しない。

payloadはVRAMのベタダンプではない。wsdisplayの `fb_offset` およびVRAM stride内のpaddingを含めず、可視画素だけを行方向に隙間なく格納する。

LUNAでは次のサイズとなる。

```text
画面幅                  1280 pixels
画面高さ                1024 lines
可視1行                 160 bytes
payload                  163840 bytes
VRAM stride              256 bytes
VRAM stride×height       262144 bytes
```

### 20.2 v1ヘッダ

v1ヘッダは32バイト固定とする。多バイト整数はすべてbig-endianで格納する。C構造体をそのまま `write()` してはならない。

| offset | size | field | v1の値・意味 |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `MONOBG\r\n` |
| 8 | 2 | version | `1` |
| 10 | 2 | header_size | `32` |
| 12 | 2 | width | 可視幅、LUNAでは1280 |
| 14 | 2 | height | 可視高さ、LUNAでは1024 |
| 16 | 2 | depth | `1` |
| 18 | 2 | pixel_format | `1` = MSB-first、1=white、0=black |
| 20 | 4 | line_bytes | `ceil(width / 8)` |
| 24 | 4 | payload_size | `line_bytes × height` |
| 28 | 4 | reserved | `0` |

magicの8バイトは次の値とする。

```text
4d 4f 4e 4f 42 47 0d 0a
 M  O  N  O  B  G CR LF
```

### 20.3 payload

payloadは上から下、各行は左から右の順に格納する。

```text
bit 7 = 行内の左端画素
bit 6 = 次の画素
...
bit 0 = 8番目の画素
```

画素値は次とする。

```text
1 = white
0 = black
```

幅が8の倍数でない場合、最終バイトの可視範囲外となる下位ビットは1で埋める。LUNAの幅1280は8の倍数である。

### 20.4 ファイルサイズ

ファイル全体のサイズは次と完全一致しなければならない。

```text
header_size + payload_size
```

LUNA v1形式では次となる。

```text
32 + 163840 = 163872 bytes
```

短いファイルだけでなく、末尾に余分なデータがあるファイルもエラーとする。

### 20.5 v1検査条件

読込み時は次をすべて検査する。

```text
magic == "MONOBG\r\n"
version == 1
header_size == 32
width > 0
height > 0
depth == 1
pixel_format == 1
line_bytes == ceil(width / 8)
payload_size == line_bytes × height
reserved == 0
実ファイルサイズ == header_size + payload_size
```

サイズ計算ではoverflowを検査する。未知のversion、pixel format、非0のreservedはエラーとする。

チェックサムはv1には含めない。magic、version、全寸法、payload size、実ファイルサイズの検査で、展示用途の誤ファイルおよび切断ファイルを検出する。

## 21. 背景ファイルの読込みと描画

### 21.1 データ構造

背景形式I/O層では、ファイル全体を保持する `MonoBgImage` ではなく、逐次読込み状態を表す `MonoBgReader` を使用する。

```c
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t pixel_format;
    uint32_t line_bytes;
    uint32_t payload_size;
} MonoBgInfo;

typedef struct {
    MonoBgInfo info;
    int fd;
    uint32_t next_row;
} MonoBgReader;
```

初期状態では `fd = -1`、`next_row = 0` とする。

プレイヤーは別途、次の1行分の読込みバッファだけを所有する。

```c
uint8_t *background_line;
```

`background_line` のサイズは `reader.info.line_bytes` とする。LUNAでは160バイトである。

`MonoBgImage` は `gif2monobg` が変換結果をファイルへ書き出す用途にだけ使用し、`monogifplay-wscons` の背景読込みには使用しない。

### 21.2 表示環境との一致検査

`monogifplay-wscons` はヘッダ検査後、次を要求する。

```text
background.width  == display.width
background.height == display.height
background.depth  == display.depth
background.pixel_format == MONOBG_PIXEL_MSB_WHITE_ONE
display.stride >= background.line_bytes
```

ファイルにはVRAM strideを保存しない。strideは実行時にwsdisplayから取得した値を使用する。

### 21.3 DUMBFB移行前のopenと検査

背景ファイルのopenと検査は、wsdisplay情報取得後、アニメーションGIFの `DGifSlurp()` より前に行う。

`monobg_reader_open()` は次を行う。

```text
1. O_RDONLYでopen
2. fstat
3. 通常ファイルであることを確認
4. 32バイトのヘッダを完全にread
5. ヘッダをdecodeしてMonoBgInfoを検査
6. st_sizeがheader_size + payload_sizeと完全一致することを確認
7. ファイル位置をpayload先頭に置いたまま成功を返す
```

形式、サイズ、表示環境の不一致はDUMBFB移行前にエラー終了する。

payload全体はこの段階では読み込まない。ヘッダ検査成功後に `background_line` を確保し、背景ファイルのfdはopenしたまま保持する。

背景ファイルをopenしたままアニメーションGIFを変換するが、保持する追加資源はfd、`MonoBgInfo`、1行分バッファだけである。

### 21.4 行単位の読込みとVRAM描画

VRAMのmmap後、`-r`指定時には起動画面内容を保存した後で、背景を次のように1行ずつ転送する。

```c
for (y = 0; y < reader->info.height; y++) {
    if (monobg_reader_read_row(reader,
      background_line, reader->info.line_bytes) != 0)
        goto cleanup;

    dst = display->fb_base
        + (size_t)y * display->stride;
    memcpy(dst, background_line, reader->info.line_bytes);
}
```

`monobg_reader_read_row()` は `EINTR` を処理し、指定された `line_bytes` を完全に読み込むまで `read()` を繰り返す。予期しないEOFまたはI/Oエラーは失敗とする。

LUNAでは1024回の160バイト読込みとVRAM転送となる。stride込み262144バイトを転送せず、可視画素163840バイトだけをVRAMへ書き込む。

各行の残り96バイトのVRAM paddingは変更しない。`-r`指定時の終了時復元でも可視160バイトだけを書き戻し、paddingは復元対象としない。

背景は起動時に1回だけ描画するため、1024回のreadシステムコールよりも、一時匿名メモリを163840バイト削減することを優先する。実機測定で背景初期化時間が問題になる場合は、複数行を収める小さな固定上限バッファへ拡張できるが、payload全体の読込みへは戻さない。

### 21.5 読込み途中の失敗

ヘッダと実ファイルサイズを事前検査しても、媒体エラー、ファイル内容の変更、予期しないEOF等によりpayload読込みが途中で失敗する可能性は残る。

背景読込み失敗時は共通クリーンアップへ移行する。

```text
-r指定時
    保存済みの可視画面内容を復元してからEMULモードへ戻す

-r未指定時
    画面内容は復元せず、termiosと画面モードだけを復元する
```

`-r`未指定時には部分的に描画された背景が残る可能性があるが、アニメーション用RAMを優先する既定動作として許容する。

DUMBFB移行後の背景読込み失敗では直接 `err()`、`errx()`、`exit()` を呼び出さない。

### 21.6 所有と解放

`MonoBgReader` はopenした背景ファイルfdを所有する。`monobg_reader_close()` はfdが有効な場合だけcloseし、`fd = -1`、`next_row = 0` へ戻す。

`background_line` はプレイヤーが所有し、通常経路では背景転送直後、エラー経路では共通クリーンアップから解放する。

背景転送完了後、必要に応じて `POSIX_FADV_DONTNEED` を性能上のヒントとして指定してからファイルをcloseする。助言の失敗は警告表示の対象にはできるが、エラー終了条件にはしない。

再生開始時には背景ファイルfdも背景読込みバッファも残さない。

## 22. `gif2monobg` 背景変換コマンド

### 22.1 コマンドライン

```text
gif2monobg [-d] [-p] gif-file background-file
```

```text
-d
    GIF読込み、変換、ファイル書込みの処理時間を表示する。
    -pも暗黙に有効とする。

-p
    入力GIF情報、出力形式、進捗を表示する。
```

入力ファイルと出力ファイルはそれぞれ1個必須とする。出力ファイルは `O_WRONLY | O_CREAT | O_EXCL` で新規作成し、既存ファイルがある場合はエラーとする。これにより入力ファイルや既存成果物の誤上書きを避ける。

### 22.2 初版の変換対象

初版はLUNA 1bpp背景専用とし、入力GIFの論理画面寸法について次を要求する。

```text
SWidth  == 1280
SHeight == 1024
ImageCount == 1
```

アニメーションGIFはエラーとする。

単一の `SavedImage.ImageDesc` が論理画面全体より小さいことは許容する。出力論理画面全体を白で初期化し、`Left`、`Top`、`Width`、`Height` に従って画像を配置する。画像矩形が論理画面外へ出る場合はエラーとする。

透明画素は初期値の白を維持する。GIFの背景色インデックスは出力背景色として使用せず、未描画領域は常に白とする。

### 22.3 カラーマップ

フレーム固有カラーマップが存在する場合はそれを使用し、存在しない場合はグローバルカラーマップを使用する。どちらも存在しない場合、またはRasterBitsのインデックスがカラーマップ範囲外の場合はエラーとする。

### 22.4 二値化

`monogifplay` および `monogifplay-wscons` と同じ判定を使用する。

```c
brightness = red * 299 + green * 587 + blue * 114;
white = brightness > 128000;
```

出力はMSB-first、1=white、0=blackとする。

### 22.5 出力生成

出力payloadを全面白で初期化する。

```c
line_bytes = (width + 7U) / 8U;
payload_size = line_bytes * height;
memset(pixels, 0xff, payload_size);
```

GIFの不透明画素だけを対象位置へ二値化して設定する。

変換完了後、`MonoBgInfo` を設定し、`monobg_write_file()` で32バイトヘッダとpayloadを書き出す。short writeと `EINTR` を処理して全バイトを書き込み、`close()` のエラーも検出する。`fsync()` は必須としない。

### 22.6 終了状態

```text
成功              EXIT_SUCCESS
usageエラー       EXIT_FAILURE
GIF読込みエラー   EXIT_FAILURE
形式・寸法エラー  EXIT_FAILURE
出力エラー        EXIT_FAILURE
```

部分的に作成された出力ファイルは、失敗時に可能であればunlinkする。

## 23. 共通モジュールとI/F境界

### 23.1 初版から共有する背景形式I/O

`monobg_format.h` は背景形式の定数、型、公開関数を定義する。

```c
#define MONOBG_HEADER_SIZE 32
#define MONOBG_VERSION 1
#define MONOBG_PIXEL_MSB_WHITE_ONE 1

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t pixel_format;
    uint32_t line_bytes;
    uint32_t payload_size;
} MonoBgInfo;

typedef struct {
    MonoBgInfo info;
    uint8_t *pixels;
} MonoBgImage;

typedef struct {
    MonoBgInfo info;
    int fd;
    uint32_t next_row;
} MonoBgReader;

void monobg_image_init(MonoBgImage *image);
void monobg_image_destroy(MonoBgImage *image);

void monobg_reader_init(MonoBgReader *reader);
void monobg_reader_close(MonoBgReader *reader);

int monobg_info_init(MonoBgInfo *info,
    unsigned int width, unsigned int height);

int monobg_header_encode(uint8_t header[MONOBG_HEADER_SIZE],
    const MonoBgInfo *info);
int monobg_header_decode(const uint8_t header[MONOBG_HEADER_SIZE],
    MonoBgInfo *info);

int monobg_reader_open(const char *path, MonoBgReader *reader);
int monobg_reader_read_row(MonoBgReader *reader,
    uint8_t *line, size_t line_size);

int monobg_write_file(const char *path, const MonoBgImage *image);
```

`MonoBgImage` は変換コマンドが生成したpayload全体を保持してファイルへ書き出すための型とする。`MonoBgReader` はプレイヤーが背景ファイルを逐次読込みするための型であり、payloadポインタを持たない。

`monobg_reader_open()` はヘッダと実ファイルサイズを検査し、fdをpayload先頭に位置付ける。`monobg_reader_read_row()` は次の1行を完全に読み込み、`next_row` を更新する。

`monobg_format.c` はgiflib、X11、wsconsの型を参照しない。標準C/POSIXファイルI/Oだけを使用する。

### 23.2 プレイヤー固有境界

`monogifplay-wscons.c` には次の処理を置く。

```c
static int monobg_validate_display(const MonoBgInfo *info,
    const WsDisplay *display);
static int wsdisplay_stream_background(const WsDisplay *display,
    MonoBgReader *reader, uint8_t *line);
```

表示環境との一致検査、背景readerからの行読込み、VRAM strideを使用した転送、途中失敗時のクリーンアップ移行はwscons固有であり、`monobg_format.c` へ入れない。

### 23.3 変換コマンド固有境界

`gif2monobg.c` では、GIF読込みと静止画像変換を次の責務へ分ける。

```c
static int gif_background_validate(const GifFileType *gif,
    unsigned int target_width, unsigned int target_height);
static int gif_background_render(const GifFileType *gif,
    MonoBgImage *background);
```

`gif_background_render()` は出力ファイルを開かず、`MonoBgImage` の可視画素payloadを生成するだけとする。ファイル出力は `monobg_write_file()` が担当する。

### 23.4 将来のGIF変換共通化

展示後、次の表示バックエンド非依存処理を `mono_gif.h`／`mono_gif.c` へ移動する。

```text
RGB輝度判定
カラーマップから白黒bit cache生成
MSB-first 1bpp画素設定
静止SavedImageの論理画面合成
アニメーション1フレームの合成
Graphics Control Extension処理
giflib所有データの解放
```

想定I/Fは次のとおりとする。

```c
typedef struct {
    unsigned int width;
    unsigned int height;
    size_t line_bytes;
    size_t size;
    uint8_t *data;
} MonoBitmap;

int mono_gif_render_still(GifFileType *gif,
    MonoBitmap *bitmap);
int mono_gif_render_frame(GifFileType *gif,
    const MonoGifInfo *info,
    int frame_number,
    uint8_t *bitmap,
    const uint8_t *previous,
    MonoGifFrameInfo *frame_info);
```

初版の `gif_background_render()` と既存 `mono_render_frame()` は、上記へ移動する際に呼出し側の所有構造を変更しなくて済むよう、出力先バッファを呼出し側が所有する設計とする。

### 23.5 X11版との将来共有

将来の共通化後も、最終格納方式は分離する。

```text
monogifplay
    MonoBitmap 1枚を作業領域として使い回す
    XPutImage()でPixmapへ転送

monogifplay-wscons
    MonoBitmap相当の作業バッファを1枚使用
    フルまたは部分payloadを可変長mmapプールへ保持

gif2monobg
    MonoBitmap 1枚をMonoBG payloadとしてファイルへ出力
```

コマンドは統合しない。共有するのはソース内のGIF変換プリミティブおよび背景形式I/Oであり、CLIと実行責務は分離したままとする。

## 24. Makefile

v8では次の3ターゲットを生成する。

```make
PROGS = monogifplay monogifplay-wscons gif2monobg
```

依存関係は次のとおりとする。

```text
monogifplay
    monogifplay.o
    X11
    giflib

monogifplay-wscons
    monogifplay-wscons.o
    monobg_format.o
    giflib

gif2monobg
    gif2monobg.o
    monobg_format.o
    giflib
```

`monobg_format.o` はX11、giflib、wsconsへ依存しない。

`monogifplay-wscons` にはX11のヘッダ検索パス、ライブラリ検索パス、`-lX11` を付加しない。`gif2monobg` にもX11およびwsconsの依存を付加しない。

既存 `monogifplay.c` のソース内容は変更しない。

## 25. v8のソース配置

```text
monogifplay.c
    既存X11版。変更しない。

monogifplay-wscons.c
    アニメーションGIF変換
    wsconsフレーム格納
    wsdisplay管理
    MonoBG表示環境検査
    背景VRAM描画
    再生制御

gif2monobg.c
    静止GIF読込み
    LUNA寸法検査
    1bpp背景payload生成
    変換コマンドのCLI

monobg_format.h
monobg_format.c
    MonoBG形式定義
    big-endianヘッダ符号化・復号
    プレイヤー向け逐次reader
    変換ツール向けファイル書込み
    readerおよびimageの所有管理
```

`monogifplay-wscons.c` 内では、従来どおり将来共通化可能なGIF変換関数とwscons固有処理を分離する。

`gif2monobg.c` 内でも、GIFからpayloadを生成する関数はCLI、ファイル名、wscons型を参照しない。

## 26. 将来のリファクタリング案

展示動作確認後、必要に応じて次の構成へ移行する。

```text
mono_gif.h
mono_gif.c
    MonoGifInfo
    MonoGifFrameInfo
    MonoBitmap
    RGB二値化
    カラーマップ処理
    mono_gif_render_still()
    mono_gif_render_frame()
    giflib所有解放

monobg_format.h
monobg_format.c
    v6から継続して共用

monogifplay.c
    X11バックエンド
    1枚のwork bitmapを使い回してPixmapへ転送

monogifplay-wscons.c
    WsconsFrame
    WsconsAnimation
    合成用フル画面work bitmap
    可変長全フレームmmapプール
    MonoBG背景表示
    wsdisplay処理

gif2monobg.c
    変換CLI
    mono_gif_render_still()の呼出し
    monobg_write_file()の呼出し
```

X11版への共通化は別変更として設計レビュー、実装、X11版回帰試験を行う。

CLIは将来も別コマンドのままとする。`monogifplay-wscons` へ変換モードを追加せず、ソース内の変換プリミティブだけを共有する。

## 27. テスト項目

### 27.1 既存wscons再生機能

```text
monogifplay-wsconsのビルド
既存monogifplayのビルド
UNROLL_BITMAP_EXTRACT有効・無効
GIF論理画面サイズ検査
部分フレーム・透過フレーム変換
変換済みRasterBits等の逐次解放
DGifCloseFile()で二重解放しない
単一mmapプールへの可変長全フレーム格納
第1フレームが常にFULL形式であること
第2フレーム以降のPARTIAL/FULL選択
X方向を8画素境界へ広げたline bytes計算
部分payloadが縮小にならない場合のFULL fallback
各WsconsFrameのoffset・size・line bytesの妥当性
フレームデータ参照時のpool範囲検査
MonoGifFrameInfoのdelayと元GIF更新矩形
MSB-firstビット順
LUNAの画素極性
stride 256による行描画
FULLフレームの全画面描画
PARTIALフレームの更新矩形だけの描画
非バイト境界ImageDesc左端の切り下げ
非バイト境界ImageDesc右端の切り上げ
幅が8の倍数でないGIFの右端マスク
部分矩形が論理画面最終バイトへ達する場合の右端マスク
v7相当の全画面合成結果とのフレーム単位一致
既定位置(0,0)
-x、-y、-Cの各配置
-c指定時と未指定時
qおよび各シグナルによる終了
既定動作で起動画面保存領域を確保しない
-r未指定時もtermiosとwsdisplayモードを復元
-r未指定時の終了後画面内容が未規定であること
-r指定時の可視画面保存・復元
-r指定時にstride paddingを保存・復元しない
swap使用時のループ再生
```

### 27.2 MonoBG形式

```text
正常なv1ヘッダのencode/decode
big-endian整数の既知バイト列との一致
magic不一致
version不一致
header_size不一致
depth不一致
pixel_format不一致
line_bytes不一致
payload_size不一致
reserved非0
サイズ計算overflow
短いファイル
末尾余分データ
MonoBgImageのpayload所有とdestroy
MonoBgReaderのopen/read_row/close lifecycle
next_rowがheightを超える読込みの拒否
```

LUNA用正常ファイルについて次を確認する。

```text
width        1280
height       1024
line_bytes   160
payload_size 163840
file_size    163872
```

### 27.3 `gif2monobg`

```text
1280×1024単一GIFの変換
寸法不一致の拒否
ImageCountが0または2以上の拒否
部分ImageDescの白背景への配置
ImageDesc範囲外の拒否
透明画素が白のままであること
ローカルカラーマップ優先
グローバルカラーマップfallback
カラーマップなしの拒否
インデックス範囲外の拒否
輝度閾値の境界
MSB-first出力
1=white、0=black
既存出力ファイルの拒否
出力ファイルwrite/close失敗
失敗時の新規部分ファイル削除
```

### 27.4 背景表示

```text
-bによる正常背景表示
-bと-cの併用拒否
背景寸法とdisplay寸法の不一致拒否
depth・pixel format不一致拒否
display.stride < line_bytesの拒否
通常ファイル以外の拒否
DUMBFB移行前のヘッダ・実ファイルサイズエラー終了
payload全体の一時バッファを確保しない
1行分バッファだけを確保する
1024行を順次readして160バイトずつVRAMへ転送
readの短い返却を完全読込みまで処理
EINTRの再試行
payload途中EOF・I/Oエラー時に共通クリーンアップへ移行
-r指定時のpayload途中失敗で可視画面を復元
-r未指定時のpayload途中失敗で画面内容を復元しない
各行のstride paddingを変更しない
背景描画後のfd closeと1行分バッファ解放
GIF表示矩形外に背景が残ること
GIF表示矩形内はアニメーションで上書きされること
-r指定時に可視画面だけを復元
```

### 27.5 共通化境界

```text
monobg_format.cがgiflib、X11、wscons型を参照しない
monobg_format.cがVRAM strideを仮定しない
monobg_reader_open()がpayload全体を確保しない
monobg_reader_read_row()がwsdisplay型を参照しない
gif_background_render()がファイルI/Oを行わない
wsdisplay_stream_background()がMonoBGヘッダ解析を行わない
mono_render_frame()が背景形式を参照しない
既存monogifplay.cを変更していない
```

### 27.6 起動画面内容の保存・復元

```text
-r未指定時にsaved_fbを確保しない
-r未指定時の追加保存メモリが0バイト
-r指定時のsaved_fb_sizeがvisible_line_bytes×height
LUNAでsaved_fb_sizeが163840バイト
-r指定時に1024行×160バイトを保存
-r指定時に1024行×160バイトを復元
stride内の非表示96バイトを保存・復元しない
保存後のmprotect(PROT_READ)
MADV_DONTNEED失敗時の継続
復元前のMADV_WILLNEED失敗時の継続
保存領域確保失敗時にVRAM内容を変更せず、VRAMをunmapしてEMULモードへ復元
保存途中またはDUMBFB後のエラーで有効資源だけを解放
-r未指定時もEMULモードへ復元
-r指定時の通常終了、q、各シグナル、背景読込み失敗で可視画面を復元
SIGKILLでは復元できないことを仕様上許容
```

### 27.7 v8対象外

```text
X11版への新メモリ設計適用
X11版の1枚work bitmap方式
mono_gif.cへの実際の共通化
アニメーションGIF透明画素と背景の合成
LUNA以外の背景target profile
MonoBGチェックサム
GIF Disposal Methodの完全対応
部分フレームの途中フレーム単独表示
```

## 28. 想定実装変更規模

### 28.1 v6背景機能

v6で定義した背景形式と変換コマンドは、位置指定だけを追加したv5より変更量が大きい。ただし、既存アニメーションフレーム格納と再生ループの変更は小さい。

| 項目 | 追加・変更行数の目安 |
|---|---:|
| `monobg_format.h/.c` | 180～280行 |
| `gif2monobg.c` | 250～400行 |
| `monogifplay-wscons.c` 背景検査・逐次読込み・描画 | 110～190行 |
| Makefile | 15～30行 |
| 合計 | 約555～900行 |

行数の大部分はファイル形式検査、エラー処理、静止GIF変換である。既存 `mono_render_frame()`、フレームプール、再生タイミング処理の設計変更は不要である。


### 28.2 v7起動画面保存・復元オプション

v7の追加変更は `monogifplay-wscons.c` に限定できる。

| 項目 | 追加・変更行数の目安 |
|---|---:|
| `-r`オプション解析・usage | 5～15行 |
| visible size計算と構造体フィールド | 10～20行 |
| 行単位保存・復元関数 | 35～70行 |
| 初期化・クリーンアップ条件分岐 | 15～35行 |
| 合計 | 約65～140行 |

背景形式、`gif2monobg`、アニメーション変換、フレームプール、再生ループの仕様変更は不要である。


### 28.3 v8部分矩形格納・描画

v8の変更は `monogifplay-wscons.c` に限定し、Makefile、背景形式、`gif2monobg`、起動画面保存・復元には変更を加えない。

| 項目 | 追加・変更行数の目安 |
|---|---:|
| format追加と事前レイアウト | 45～80行 |
| 合成用作業バッファと部分payload格納 | 70～120行 |
| PARTIALのVRAM描画 | 55～90行 |
| 合計 | 約220～310行 |

`mono_render_frame()` の画素変換本体、背景処理、位置計算、再生タイミング、シグナル処理は変更しない。

## 29. 将来機能

- 任意ビット位置へのアニメーション描画
- 背景画像とGIF透明画素の合成
- 生のGIF差分ラスターを保持する透明マスクまたは2bpp・run形式
- 複数矩形更新を表現するフレームformat
- X11版とのGIF変換コード共通化
- GIF Disposal Methodの完全対応
- `gif2monobg` のLUNA以外のtarget profile
- MonoBGの追加pixel format
- 必要性が確認された場合のpayload checksum

背景とGIF透明画素を合成する場合は、現在の合成済みフル／部分payloadでは透明情報が失われるため、透明マスクまたは背景を初期合成元にする追加設計を行う。

## 30. v8の確定仕様

```text
・既存monogifplay.cは変更しない
・monogifplay-wsconsはアニメーション再生と背景表示を担当
・gif2monobgは別コマンドとして静止GIFを背景形式へ変換
・monogifplay-wsconsへ変換モードを追加しない
・背景形式I/Oはmonobg_format.h/.cとして初版から共有
・GIF変換プリミティブのX11/wscons/変換ツール共通化は展示後
・MonoBG v1は32バイト固定ヘッダ
・ヘッダ整数はbig-endian
・magicはMONOBG CR LF
・payloadは可視画素だけを格納
・fb_offsetおよびVRAM stride paddingはファイルに含めない
・pixel formatはMSB-first、1=white、0=black
・LUNA payloadは160×1024=163840バイト
・LUNA MonoBGファイル全体は163872バイト
・ファイルサイズはheader_size+payload_sizeと完全一致必須
・チェックサムはv1に含めない
・背景幅、高さ、depthは実行時displayと完全一致必須
・VRAM転送先strideはwsdisplayから取得
・背景ヘッダと実ファイルサイズはDUMBFB移行前に検査
・背景payload全体はユーザー空間へ読み込まない
・背景ファイルは検査後もopenしたままpayload先頭で保持
・VRAM mmap後、-r指定時には可視画面を保存してから背景を逐次転送
・背景を1024行×160バイトで逐次read・VRAM転送
・背景読込みバッファは1行分だけ
・VRAMの各行paddingは背景描画時に変更しない
・背景転送完了後にfdをcloseし1行分バッファを解放
・背景payload途中のI/O失敗時は共通クリーンアップへ移行
・-r指定時の背景I/O失敗では可視画面を復元
・-r未指定時の背景I/O失敗では画面内容を復元しない
・-bで背景を指定
・-bと-cは同時指定不可
・背景はGIF矩形外に残る
・GIF矩形内の透明背景合成はv8対象外
・gif2monobg入力は1280×1024、ImageCount=1に限定
・部分ImageDescは白い論理画面へ合成
・透明画素および未描画領域は白
・二値化は既存monogifplayと同じ係数・閾値
・gif2monobgはwsconsおよびX11へ依存しない
・起動画面内容は既定では保存・復元しない
・-r指定時だけ可視1bpp画面を保存・復元
・画面モードとtermiosは-rの有無にかかわらず復元
・-r未指定時の終了後画面内容は未規定
・-r指定時の保存サイズはvisible_line_bytes×height
・LUNAでの保存サイズは163840バイト
・VRAM stride paddingは保存・復元しない
・第1アニメーションフレームは常に合成済みFULL 1bpp
・第2フレーム以降は元ImageDescをX方向8画素境界へ広げた領域を候補とする
・候補payloadがフルより小さい場合だけPARTIAL 1bppを使用
・縮小にならないフレームはFULL 1bppへfallback
・PARTIAL payloadは生のGIFラスターではなく合成済みcanvasの切り出し
・透明マスクは保持しない
・変換中はフル画面1bpp canvasを1枚だけ使用
・再生時はPARTIAL矩形だけをVRAMへ転送
・第1フレームのFULL描画で各ループ先頭を再初期化
・Disposal Methodの扱いはv7から変更しない
・Restore to background/previousの完全対応はv8対象外
```

## 31. 改定履歴

### v8

- 第1フレームを常に合成済みフル1bppとして格納する仕様とした。
- 第2フレーム以降は元GIF更新矩形をX方向8画素境界へ広げ、縮小可能な場合に合成済み部分payloadだけを格納する仕様とした。
- フレーム記述子のoffset、size、line bytes、formatを使用した可変長pool配置を実装対象とした。
- 変換中に論理画面全体の1bpp作業バッファを1枚だけ使用し、インプレース合成後にpayloadを切り出す仕様とした。
- `WSCONS_FRAME_PARTIAL_1BPP` を追加した。
- 再生時に部分矩形だけをVRAMへ転送する仕様とした。
- GIF論理画面右端の非表示ビットはread-modify-writeで保持する仕様とした。
- v7の合成結果とDisposal Method非対応を維持し、透明マスクおよび完全なdispose処理は追加しないこととした。

### v7

- 起動画面内容の保存・復元を `-r` オプションとして定義した。
- 既定では起動画面内容を保存・復元しない仕様へ変更した。
- 画面内容復元と、termios・wsdisplay画面モード復元を別責務として定義した。
- `-r` 未指定時もtermiosと画面モードは必ず復元する仕様とした。
- `-r` 未指定時の終了後画面内容を未規定とした。
- `-r` 指定時だけ起動画面保存用匿名マッピングを確保する仕様とした。
- 保存対象を `stride × height` 全体から可視1bpp画素へ変更した。
- `visible_line_bytes = ceil(width / 8)` を導入した。
- LUNAでの保存量を262144バイトから163840バイトへ削減した。
- VRAM stride内の非表示paddingを保存・復元しない仕様とした。
- 可視画面の保存・復元を行単位処理として定義した。
- 保存領域への `MADV_DONTNEED` と復元前の `MADV_WILLNEED` を継承した。
- 背景逐次読込み失敗時の動作を `-r` の指定有無で分けた。
- 初期化順序、クリーンアップ順序、RAM見積もり、テスト項目を更新した。
- v7設計レビュー後、背景逐次読込みと起動画面任意復元を実装した。

### v6

- `-b` による専用背景画像表示を追加した。
- 背景変換を `gif2monobg` 別コマンドとして定義した。
- プレイヤーへ変換モードを追加しない方針を確定した。
- 背景ファイル形式をMonoBG v1として定義した。
- 32バイト固定big-endianヘッダの全フィールドを定義した。
- payloadをVRAM stride込みではなく可視画素だけとした。
- LUNA payloadを163840バイト、全ファイルを163872バイトとした。
- 背景を行単位でVRAMへ転送し、stride paddingを変更しない仕様とした。
- `fb_offset` を背景ファイルへ含めないことを明記した。
- 背景ヘッダと実ファイルサイズをDUMBFB移行前に検査する仕様とした。
- 背景payload全体の一時読込みを取りやめ、1行分バッファによる逐次読込みへ変更した。
- 背景ファイルをpayload先頭でopenしたまま保持し、起動画面保存後に読込み・描画する仕様とした。
- 背景読込み途中の失敗時に起動画面を復元するエラー処理を定義した。
- 背景転送後にfdと1行分バッファを解放する所有規則を定義した。
- `-b` と `-c` を排他指定とした。
- 背景とアニメーション透明画素の合成を対象外とした。
- `gif2monobg` の入力寸法、単一画像制約、白背景合成を定義した。
- カラー二値化を既存monogifplayと同一仕様とした。
- `monobg_format.h/.c` を初版から共有する構成とした。
- 将来の `mono_gif.h/.c` 共通化に向けた関数I/F境界を定義した。
- Makefileを3ターゲット構成へ更新した。
- 背景形式、変換コマンド、背景表示のテスト項目を追加した。

### v5

- `-x` および `-y` による表示位置指定を追加した。
- `-C` による中央配置を追加した。
- X座標は初版では8画素境界に限定した。
- 中央X座標を8画素境界へ切り下げる仕様を追加した。
- `-C` と `-x`／`-y` の併用時に明示指定した軸を優先する仕様を追加した。
- オプション記載順に依存せず、解析完了後に最終位置を決定する仕様とした。
- 座標値の数値解析とエラー条件を定義した。
- underflowおよびoverflowを避ける表示範囲検査を定義した。
- `wsdisplay_blit_frame()` に表示X/Y座標を渡す仕様へ変更した。
- VRAM行先頭計算へX/Y位置を反映した。
- `-p` 出力へ最終表示位置を追加した。
- 配置関連のテスト項目を追加した。
- v4実装からの想定変更量を追加した。
- 任意ビット位置X描画は変更量と試験量が増えるため引き続き将来機能とした。

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
