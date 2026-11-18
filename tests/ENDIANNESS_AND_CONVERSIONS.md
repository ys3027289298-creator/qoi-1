# QOI 字节序假设定位与隐式类型转换清单

针对问题：同一 QOI 码流在小端/大端机器上解码结果不一致。

## 1. 字节序假设定位结论

### 1.1 唯一的端序敏感点：`qoi_rgba_t` 联合体的整字比较

修改前：

```c
typedef union {
    struct { unsigned char r, g, b, a; } rgba;
    unsigned int v;
} qoi_rgba_t;
```

代码用整字比较判定像素相等，例如：

```c
if (px.v == px_prev.v)              /* 编码器 */
if (index[index_pos].v == px.v)     /* 颜色索引表命中 */
```

这在两种机器上语义不同：

| 内存字节 (RGBA) | 小端机 `px.v` | 大端机 `px.v` |
|---|---|---|
| `88 12 45 FF` | `0xFF451288` | `0x881245FF` |

虽然同机编解码自洽（编码端、解码端用同一种解释），但它还叠加了两个可移植性缺陷：

1. `unsigned int` 宽度由实现决定（C 标准只保证 ≥16 位）；若不是 32 位，比较会越界读/截断；
2. 联合体若存在填充字节，未初始化的填充位参与 `==` 比较，结果未定义。

### 1.2 本就端序安全的部分（无需改逻辑）

- `qoi_read_32` / `qoi_write_32`：逐字节拼装，`a<<24 | b<<16 | c<<8 | d`，与主机字节序无关；
- 所有 chunk 标签与通道运算都基于 `unsigned char`（整数提升为 `int` 后运算），值的含义与端序无关；
- 像素缓冲区是字节数组，按下标 `px_pos+0..3` 访问，不做整体多字节加载。

### 1.3 修复方案（`qoi.h`）

1. 像素改为四字节显式通道结构体，删除联合体整字：

```c
typedef struct { uint8_t r, g, b, a; } qoi_rgba_t;
typedef char qoi_rgba_size_check[(sizeof(qoi_rgba_t) == 4) ? 1 : -1];
```

2. 像素相等改为逐字节比较函数 `qoi_rgba_eq()`，任何机器、任何填充策略结果一致；
3. 提供 `qoi_rgba_pack()/unpack()` 作为**有文档约定的**打包形式（R 在最高字节），仅用移位按字节组合，端序无关（当前编解码路径已不再依赖打包值，保留它用于需要 32 位键的场景）；
4. 魔量由字符常量改为显式十六进制 `0x716f6966`，避免多字节字符常量的实现定义行为；
5. 所有 32 位量改用 `<stdint.h>` 的 `uint32_t`（`QOI_MAGIC`、`qoi_read_32/write_32`、`header_magic`、`QOI_PIXELS_MAX`）。

## 2. 隐式类型转换全清单

命令（gcc 13，覆盖收窄与有符号/无符号两类）：

```
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic \
    -Wconversion -Wsign-conversion -Wshadow
```

基线对照：原始 `qoi.h` 27 条告警，修改后 23 条（无新增类别，仅 19→15 条收窄转换减少）。常规构建标志（`-Wall -Wextra -Wpedantic`）下**零告警**；下列每一条的值域都被上游守卫约束，不构成实际缺陷，列出以便审计。

### 2.1 整数提升（integer promotion，本身安全，端序无关）

所有 `uint8_t`/`unsigned char` 在参与算术前提升为 `int`：

- 哈希 `C.r*3 + C.g*5 + C.b*7 + C.a*11`：最大 255×(3+5+7+11)=6630，int 内无溢出，再 `& 63` 落回 0..63；
- 编码器差值（现已显式写 `(int)px.r - (int)px_prev.r`，结果 -255..255）；
- 解码器 `px.r += ((b1>>4)&0x03) - 2` 等：右侧 int，回绕后靠赋值截断回 uint8，这是无符号回绕，良定义且与端序无关；
- chunk 字节运算 `b1 & 0xc0`、`b1 >> 4` 等。

### 2.2 收窄转换（int → 8 位无符号，共 15 处）

| 位置 | 表达式 | 实际值域 | 安全性 |
|---|---|---|---|
| qoi.h 编码器 `bytes[p++]=QOI_OP_RUN\|(run-1)` ×2 | run∈1..62 | 0xc0..0xfd，单字节 | 安全 |
| 编码器 `QOI_OP_INDEX\|index_pos` | `&63` 之后 | 0x00..0x3f | 安全 |
| 编码器 `QOI_OP_DIFF\|...` | vr/vg/vb∈-2..1，+2 后 0..3 | 0x40..0x7f | 安全 |
| 编码器 `QOI_OP_LUMA\|(vg+32)` | vg∈-32..31 | 0x80..0xbf | 安全 |
| 编码器第二字节 `(vg_r+8)<<4\|(vg_b+8)` | 两项均 0..15 | 0x00..0xff | 安全 |
| 解码器 `px.r/g/b/a += ...`（6 处，DIFF/LUMA/RGB/RGBA） | 无符号回绕 | 0..255 | 安全，规范要求的回绕语义 |

### 2.3 有符号/无符号混用与长度换算（共 8 处）

| 位置 | 转换 | 说明与既有约束 |
|---|---|---|
| 编码器 `max_size = width*height*(channels+1)+...` | size_t 运算结果 → int | 受 `height < QOI_PIXELS_MAX/width` 守卫，最坏约 20 亿，int 可容纳；这是上游既有的 2GB 上限设计 |
| 编码器 `QOI_MALLOC(max_size)` | int → size_t | max_size 恒正 |
| 编码器 `px_len = width*height*channels` | uint32 乘积 → int | 同上守卫 |
| 解码器 `px_len = width*height*channels` | uint32 → int，回传 malloc 时 int → size_t | 同上 |
| 解码器 `chunks_len = size - (int)sizeof(qoi_padding)` | size_t → int | 入口已要求 `size ≥ 14+8` |
| `qoi_write` 中 `fwrite(..., size, ...)` | int → size_t | size 为正 |
| `qoi_read` 中 `size = ftell(f)` | long → int | 上游既有 2GB 限制；文件更大时本实现本就不支持 |
| `qoi_read` 中 `QOI_MALLOC(size)`、`fread(...,size,...)` | int → size_t；size_t 返回值 → int(bytes_read) | 同上；`bytes_read != size` 比较为有符号/无符号混合，但两侧均非负 |

### 2.4 顺带修复的实现定义/未定义行为

- **`signed char` 溢出（原实现）**：原编码器把 8 位回绕差值存入 `signed char vr/vg/vb`，`px.rgba.r - px_prev.rgba.r` 的结果可能超出 -128..127（如 0-255=-255），signed char 存不下属实现定义行为。已改为 `int`，彻底消除。
- **函数参数求值顺序（测试路径发现的真实 UB）**：模拟大端解码器最初写成
  `alt_be_make(stream[pos++], stream[pos++], stream[pos++], ...)`，
  同一表达式内多个无顺序点的 `pos++` 是未定义行为，编译器实际逆序求值导致 R/B 通道互换，表象与"大端解码错误"完全一致。已改为先按序读入具名局部变量再传参。这条教训也说明：**通道顺序必须靠具名变量/下标固定，不能靠参数求值顺序**。
- 多字节字符常量 `'q'<<24 ...` → 显式 `uint32_t` 十六进制。

## 3. 两套（实为三套）编解码路径交叉验证

| 路径 | 文件 | 像素表示 | 设计目的 |
|---|---|---|---|
| PROD | `qoi.h` | 显式四字节结构体 + `qoi_rgba_eq` | 修复后的生产代码 |
| ALT | `tests/qoi_alt.h` | 四张扁平 `uint8_t` 表，**全程不构成任何 32 位像素字** | 结构完全不同的独立实现，杜绝同一错误思路 |
| BE | `tests/qoi_be.h` | 打包 32 位字，所有多字节内存访问经 `alt_be_load32/store32` 字节反转 | 在小端主机上**模拟大端机视图** |

### 3.1 大端模拟原理

大端机把内存首字节当作字的最高有效字节。`alt_be_load32(p) = p[0]<<24|p[1]<<16|p[2]<<8|p[3]` 正是该视图；像素字约定为 `R<<24|G<<16|B<<8|A`，与真实大端机原生 4 字节加载同一 RGBA 字节序列得到的整数完全相同。该路径保留了旧代码"以整字为中心"的写法，因此若生产代码仍残留端序逻辑依赖，必然在此暴露。

### 3.2 每张图的检查项

1. 三个编码器输出**逐字节相同**的 QOI 码流（含长度）；
2. 3×3 交叉解码矩阵：每条码流经每个解码器，输出像素 `memcmp` 逐像素一致；
3. 解码结果与原始源像素逐字节一致（往返无损）；
4. 模拟大端解码 == 主机原生解码，逐像素一致；
5. 头字段（width/height/channels）一致；
6. 畸形输入（截断头、坏魔量、chunk 截断、NULL 参数）三路径失败行为一致，不崩溃。

### 3.3 测试语料

- 边界图：纯色（含 62 长度 RUN）、0↔255 极端回绕、±1..2 小差值（DIFF）、平滑渐变（LUMA）、64 项索引表全覆盖、1×1、单行、单列；RGB 与 RGBA 各一套；
- 10,000 张确定性伪随机图（xorshift32，种子固定可复现）：尺寸 1..48 见方、RGB/RGBA 随机、三种内容模式（纯噪声/随机游程/有界随机游走），确保全部 chunk 类型被覆盖；
- 畸形/截断输入组。

## 4. 验证结果

```
== 10,000 random images (three code paths, 3x3 decode matrix) ==
  10000 images, 230429 checks, 0 FAILURES
```

- 总检查 230,429 项，零差异；
- 构建矩阵：c99 / gnu99 / c11 / c17 × -O0/-O1/-O2/-O3 共 16 个 C 构建，外加 C++11 构建，全部零失败；
- 全程开启 `-fsanitize=address,undefined -fno-sanitize-recover=all`，无任何 ASan/UBSan 报告；
- libFuzzer 对生产 `qoi_decode` 持续 20 秒、约 298 万次随机输入，无崩溃/无 sanitizer 告警；
- 生产 `qoi.h` 在 `-Wall -Wextra -Wpedantic` 下 gcc/clang 均零告警。

复现：

```
cd tests && make test     # 主验证（含 ASan/UBSan）
cd tests && make matrix   # 17 个构建的完整矩阵
```
