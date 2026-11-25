# 第三方组件与许可证（事实整理，不是法律意见）

本仓库（`mft_avs`）里我们自己写的源码按 **BSD-3 条款**发布（见 `LICENSE`）。
但**交付的 `AVSVideoDecoder.dll` 链接了第三方组件**，它们各自的许可证不因本仓库的
LICENSE 而改变。链接方式决定义务，所以每条都写清证据与出处。

## 1. 这个包不含第三方源码，只含引用

| 组件 | 上游 | 我们实际构建的那一份 | 许可证 |
|---|---|---|---|
| FFmpeg 9.1（打我们的补丁） | https://github.com/FFmpeg/FFmpeg ，标签 `n9.1`（提交 `74e4393595cf2b5857f0da925c4e0dd74ea306e5`） | 发布 tarball `ffmpeg-9.1.tar.xz`：12,557,532 字节，SHA-256 `1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433` | LGPL-2.1+（构建带 `--enable-gpl` 时为 GPL） |
| davs2（AVS2 解码） | https://github.com/pkuvcl/davs2 | master 分支快照 tarball：534,423 字节，SHA-256 `1d688e4832822c779053fa69d2dab2651a3f82461e2b74c67d9bae2ef55da134` | **GPL-2.0**（上游 `COPYING`） |
| uavs3d（AVS3 解码） | https://github.com/uavs3/uavs3d | master 分支快照 tarball：629,324 字节，SHA-256 `f5288ffed9f540a0300fc040b971cb910bf676e2a8441c5aee6495280c0683e8` | BSD 3 条款风格（上游 `COPYING`） |
| nasm（构建工具，不参与链接） | https://www.nasm.us/ | `deps\bin\nasm.exe` | BSD-2 条款 |

获取方式与构建命令写在 `deps\README.md`（davs2 / uavs3d / nasm）与
`scripts\build-ffmpeg.cmd`、`scripts\build_ffmpeg.sh`（FFmpeg 9.1 + 我们的补丁）。

为什么记录**快照哈希**而不是 tag：davs2 与 uavs3d 我们用的是 master 分支的快照，
名字里只有 `master`、没有提交号；哈希能唯一确定编译过的那份源码，tag 不能。
FFmpeg 有正式标签，所以同时给出标签与 tarball 哈希。

## 2. 链接形态（这是结论的前提，不是细节）

+ `scripts\build-deps.cmd` 把 davs2、uavs3d 构建成**静态库** `deps\lib\davs2.lib`、
  `deps\lib\uavs3d.lib`。
+ `MediaFoundationTransform\AVSVideoDecoder\StdAfx.h:35-36` 用
  `#pragma comment(lib, "davs2")` / `#pragma comment(lib, "uavs3d")` 把它们与我们的代码
  链成同一个 `AVSVideoDecoder.dll`。
+ `scripts\build_ffmpeg.sh:133`：只要 `deps/lib/davs2.lib` 存在就加 `--enable-gpl`。
+ 运行时不需要额外的 DLL：`scripts\dll-imports.py` 读导入表，输出的每一个模块都是
  系统模块（没有 CRT、没有 FFmpeg/davs2/uavs3d 的 DLL）。

因此：**「一个 BSD 许可证的仓库」与「一个静态链接了 GPL-2.0 组件的二进制」是两件事。**
本仓库的源码可以按 BSD 发布；把 `AVSVideoDecoder.dll` 作为闭源组件分发则需要先确认
davs2 的 GPL-2.0 义务。这一条需要人来定，不是我能替它下结论的地方。

## 3. 补丁与移植代码的出处

+ `patches\ffmpeg-9.1-avs-dra.patch` 是对 FFmpeg 源码树的改动，随 FFmpeg 的许可证走。
+ 其中 `libavcodec/libcavs.c`、`libavcodec/libdra.c` 等是按一份公开的第三方移植
  （`maliwen2015/ffmpeg_cavs_dra`）为基础移植/维护的，文件头保留了出处声明：

```
/*****************************************************************************
* libcavs.c: the source file of avs/avs+ decoder 
* Copyright (c) 2021 maliwen2015/ffmpeg_cavs_dra
*****************************************************************************
*/
```

+ FFmpeg `LICENSE.md` 开头（核对用，不替代全文）：

```
# License
Most files in FFmpeg are under the GNU Lesser General Public License version 2.1
or later (LGPL v2.1+). Read the file `COPYING.LGPLv2.1` for details. Some other
files have MIT/X11/BSD-style licenses. In combination the LGPL v2.1+ applies to
FFmpeg.
Some optional parts of FFmpeg are licensed under the GNU General Public License
```

## 4. 测试样本（不是我们的作品）

`samples\CCTV-4-short.avs`（548,376 字节）是一段广播节目的 AVS+ 基本流片段，
放进包里的唯一目的是让 `verify.cmd` 开箱就能做字节级自检：

```
verify.cmd samples\CCTV-4-short.avs 30
```

它的内容版权不属于本项目，来源与再分发授权**未确认**。如果对外发布时不能确认授权，
删掉这个文件即可——`verify.cmd` 接受任意一个含 AVS 视频的文件作为参数，
字节 canary（`6c52e2bcd47d12ceee40532e1173eb0e`）只对这一个文件成立。
