# deps —— 第三方解码器源码从哪里来

这个目录**不在交付包里**（包里只有源码与脚本）。`scripts\build-mft.cmd` 需要
`deps\lib\davs2.lib`、`deps\lib\uavs3d.lib` 和 `deps\include`，它们由
`scripts\build-deps.cmd` 从下面的上游源码构建出来。

## 1. davs2（AVS2 解码器，GPL-2.0）

```
https://github.com/pkuvcl/davs2
```

展开到 `deps\src\davs2-master`。本项目实际构建时用的是这个仓库 master 分支的一份快照：

| 快照文件 | 字节 | SHA-256 |
|---|---|---|
| `deps\dl\davs2.tar.gz` | 534,423 | `1d688e4832822c779053fa69d2dab2651a3f82461e2b74c67d9bae2ef55da134` |

快照名里只有 `master`，没有提交号，所以这里记录的是**快照哈希**而不是某个 tag——
哈希能唯一确定我们编译过的那份源码，tag 不能。许可证见仓库根目录 `COPYING`（GPL-2.0）。

`davs2` 的 x86 asm 内核需要 nasm（`deps\bin\nasm.exe` 或在 PATH 里）：

```
https://www.nasm.us/     （BSD-2 条款，只是构建工具，不参与链接）
```

## 2. uavs3d（AVS3 解码器，BSD 风格）

```
https://github.com/uavs3/uavs3d
```

展开到 `deps\src\uavs3d-master`。实际构建用的同样是 master 分支快照：

| 快照文件 | 字节 | SHA-256 |
|---|---|---|
| `deps\dl\uavs3d.tar.gz` | 629,324 | `f5288ffed9f540a0300fc040b971cb910bf676e2a8441c5aee6495280c0683e8` |

许可证见仓库根目录 `COPYING`（BSD 3 条款风格），版权行列出三家机构：
`Peking University Shenzhen Graduate School`、`Peng Cheng Laboratory`、
`Guangdong Bohua UHD Innovation Corporation`。

## 3. 构建

```bat
scripts\build-deps.cmd          :: 8 bit（默认）：deps\lib\davs2.lib + uavs3d.lib
scripts\build-deps.cmd 10       :: 10 bit：真实广播与 HDR 素材需要这个
```

两个库都只按一种位深编译；换位深要重跑这个脚本，然后重新链接 MFT 与工具
（`scripts\build-mft.cmd`），FFmpeg 本身不需要重建（它只用头文件）。

## 4. FFmpeg 基线（另一个依赖，装在仓库根目录）

```
https://github.com/FFmpeg/FFmpeg        标签 n9.1（提交 74e4393595cf2b5857f0da925c4e0dd74ea306e5）
```

本项目构建 FFmpeg 9.1 时用的是发布 tarball `ffmpeg-9.1.tar.xz`（12,557,532 字节，
SHA-256 `1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433`），
然后打上 `patches\ffmpeg-9.1-avs-dra.patch`。**已实测**：补丁能应用到从该 tarball 解出的
源码树（`git apply -p1 --check` 与正向应用都通过，见 `plan` 与 `docs`）。
针对 git 标签 `n9.1` 的同一检查**尚未做过**——所以这里只给出引用，不断言它已通过。
