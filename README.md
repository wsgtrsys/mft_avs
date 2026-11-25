# AVS / AVS+ / AVS2 / AVS3 视频 + DRA 音频：Media Foundation 解码组件

生成日期 2026-11-25。本目录由开发工作区的 `scripts\make-delivery.py` 生成，内容自洽：
包内的源码可以构建出包内的 DLL，包内的补丁可以打到未修改的 FFmpeg 9.1 上。

第三方源码**不在包里**：FFmpeg、davs2、uavs3d 以**上游仓库 + 我们实际编译过的那一份的哈希**
引用（见 `deps\README.md` 与 `THIRD-PARTY.md`）。这样引用而不是打包别人的树，是因为
davs2 / uavs3d 我们用的是 master 分支快照，打包一份没有提交号的源码只会让"编的是哪一份"
变成猜测。

这份文件只写"在本包里能做到的事"和"实测到的边界"；每个数字后面都跟着复跑命令。
组件内部的设计说明与完整实测矩阵在 `MediaFoundationTransform\AVSVideoDecoder\README.md`
（随源码一起在本包里）。

## 1. 目录里有什么

| 路径 | 内容 | 性质 |
|---|---|---|
| `MediaFoundationTransform\AVSVideoDecoder\` | 解码器 MFT 源码（视频 + DRA 音频 + 自定义 media source + byte stream handler）、`.vcxproj`/`.sln` | 源码 |
| `MediaFoundationTransform\Common\` | MFT 示例工程共用的基类（`MFClassFactory.h`、`MFRegistry.h` 等），我们的工程直接 include 它们；另有 `MFTAsynchronous\`、`MFTDirectxAware\` 两个示例工程随树提供、不参与本组件构建；同目录带 `MediaFoundationTransform\LICENSE` | 第三方源码（public domain，见第 6 节） |
| `MediaFoundationTransform\AVSVideoDecoder\bin\x64\Release\AVSVideoDecoder.dll` | 交付的二进制（SHA-256 见 `MANIFEST.md`） | 构建产物 |
| `tools\`、`bin\*.exe` | 控制台验证工具（不注册即可驱动 MFT：`DllGetClassObject`） | 源码 + 构建产物 |
| `patches\ffmpeg-9.1-avs-dra.patch`、`patches\ffmpeg-9.1-avs-dra-upstream-hunks.patch` | 我们的补丁（完整补丁 / 只含可上游化的 hunk）。基线 FFmpeg 9.1 是引用，见第 4 节 | 补丁 |
| `scripts\build-*.cmd`、`scripts\build_ffmpeg.sh`、`scripts\vcenv.sh`、`scripts\pkg-config.sh`、`scripts\compat\` | 构建脚本（含 FFmpeg 在 MSVC 下需要的兼容头） | 构建脚本 |
| `scripts\patch-gate.cmd`、`scripts\make-ffmpeg-patch.py` 等 | 生成/校验补丁的工具（`PATCH_GATE=OK` 表示补丁与源码一致）——需要打过补丁的 `ffmpeg-9.1\` 工作树，因此只能在开发工作区跑；在包里请照第 4 节直接应用补丁 | 构建脚本 |
| `scripts\dll-imports.py` | 读交付 DLL 的 PE 导入表与资源目录，逐条确认加载器要的是什么 | 校验脚本 |
| `deps\README.md` | davs2 / uavs3d / nasm / FFmpeg 从哪里取、我们编译过的快照哈希、取到之后怎么构建 | 说明（第三方是引用） |
| `samples\CCTV-4-short.avs` | 一段广播节目的 AVS+ 裸流，只为了让 `verify.cmd` 开箱能做字节级自检 | 第三方内容（版权不属于本项目） |
| `LICENSE` | 本仓库自研源码的许可证：BSD-3 条款 | 许可证 |
| `scripts\make-delivery.py`、`scripts\delivery-check.py` | 本目录的定义与自检：前者是"什么属于交付包"的唯一清单，后者用同一份清单反查包里的一切 | 说明（生成器本身） |
| `MANIFEST.md`、`THIRD-PARTY.md` | 文件清单+哈希、第三方组件与许可证事实 | 说明 |
| `verify.cmd` | 不注册、不提权，直接检验这个包能不能解码（见第 3 节） | 工具 |

不放进本包的东西：第三方源码与它们的构建产物（引用，见上）、其余测试样本（`samples\`、`video\`，
广播内容的版权）、开发工作区的报告（`docs\`、`plan\`）与回归夹具。第 5 节的复跑命令里出现的
`scripts\*.py`（除 `dll-imports.py`）、`samples\`（除上面那一个）、`video\` 都属于开发工作区。

## 2. 只用编译产物部署

只需要一个文件：`MediaFoundationTransform\AVSVideoDecoder\bin\x64\Release\AVSVideoDecoder.dll`。
它不依赖额外的运行时 DLL——CRT 是静态链接（`AVSVideoDecoder.vcxproj:104`
`<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`），FFmpeg / davs2 / uavs3d 也是静态库
（`scripts\build_ffmpeg.sh:137` `--disable-shared --enable-static`）。

这条不是推测，是读出来的：`scripts\dll-imports.py` 解析交付 DLL 的导入表，2026-11-25 的
结果是 10 个模块，全部是系统模块（`KERNEL32`、`MFPlat`、`ole32`、`CRYPT32`、`Secur32`、
`USER32`、`WS2_32`、`ADVAPI32`、`bcrypt`、`ncrypt`），没有一个 CRT 或 FFmpeg DLL：

```
python scripts\dll-imports.py
```

同一个脚本还读资源目录：当前的 DLL **没有 `RT_VERSION` 资源**（只有 `RT_MANIFEST`），
所以注册之后文件本身说不出它是哪个构建——身份在 `MANIFEST.md` 的哈希里。这是交付形态的
缺口，不是解码缺陷。

注册到系统需要管理员（写 HKLM）：

```
bin\avsmft-register.exe "MediaFoundationTransform\AVSVideoDecoder\bin\x64\Release\AVSVideoDecoder.dll"
bin\mftenum.exe            REM 读回：应看到 {314FFBAE-5B41-4C95-9C19-4E7D586FACE3}
                           REM 以及 'AVS ' / 'AVS2' / 'AVS3' 三个子类型
```

系统播放器会走我们的是这些扩展名：`.avs`、`.cavs`、`.avs2`、`.avs3`。
`.ts` / `.m2ts` / `.mkv` 归系统自己的 handler（实测：以管理员写这些键返回 `0x80070005`），
把 `bin\typedump.exe <文件>` 跑一遍就能看到是谁认领了文件、报了什么 native type。
容器里的 AVS 目前可行的做法是把视频轨重封装成裸流（开发工作区里的 `scripts\repack-playable.py`）。

## 3. 不注册先自检

```
verify.cmd                            REM 用包里的样本 samples\CCTV-4-short.avs
verify.cmd <一个含 AVS 视频的文件> [帧数]
```

它用包里的 `bin\avsmft-test.exe` 与 `bin\mftlifecycle-test.exe` 做三件事：

1. 解码出真实画面（输出里有 `decoded N frame(s)`）；
2. 每类对象 10 次 create/use/destroy 周期不出错（输出里的
   `lifecycle check: 40 iteration(s), 0 failure(s)` 就是视频 MFT、音频 MFT、media source、
   byte stream handler 四类各 10 次；这一条保护的是宿主进程）；
3. 只有用包内样本时才做：整段解码的字节与记录的 MD5 一致
   （`CANARY=OK - every decoded byte matches the recorded md5 6c52e2bcd47d12ceee40532e1173eb0e`）。

判据写在脚本开头，输出 `VERIFY=OK` 才算过。全程不写注册表、不需要管理员。
第 3 步用 `-fmt i420`，因为记录的字节就是在这个平面上定义的；它把"这个包解出来的画面
与独立参考实现逐字节相同"变成任何人都能在克隆目录里跑的一句话。

2026-11-25 的实测（开发工作区，回归会话 11v，`scripts\delivery-check.py`）：
包里的 DLL 5,328,896 字节，对同一个样本输出同一个 MD5。

## 4. 从源码构建

前置：Visual Studio（含 `vcvars64.bat`）+ Windows SDK 10.0.26100.0；MSYS2（只用于
FFmpeg 的 configure/make，`scripts\build-ffmpeg.cmd` 里写的是 `C:\msys64`）；git。

```
REM 1) FFmpeg 9.1（引用，不在包里）+ 我们的补丁
git clone --branch n9.1 https://github.com/FFmpeg/FFmpeg ffmpeg-9.1
git apply -p1 patches\ffmpeg-9.1-avs-dra.patch      REM 补丁路径带 ffmpeg-9.1/ 前缀，所以 -p1 在其上一级执行
scripts\build-ffmpeg.cmd                            REM -> ff91-install\

REM 2) AVS2 / AVS3 的外部解码器（引用）：按 deps\README.md 取源码到 deps\src，然后
scripts\build-deps.cmd                              REM davs2 需要 nasm

REM 3) MFT 与控制台工具
scripts\build-mft.cmd                               REM -> ...\bin\x64\Release\AVSVideoDecoder.dll + bin\*.exe
```

少任何一步，`build-mft.cmd` 会先说出缺的是哪一步、去哪儿取，而不是让 `cl.exe` 报
"cannot open include file avcodec.h"。

补丁与基线的关系是**已实测**的：2026-11-25 在开发工作区，`patches\ffmpeg-9.1-avs-dra.patch`
对从发布 tarball `ffmpeg-9.1.tar.xz`（12,557,532 字节，SHA-256 见 `THIRD-PARTY.md`）解出的
源码树正向应用通过（20 个文件），应用后再反向校验也通过。**对 git 标签 `n9.1` 的同一检查尚未做过**，
所以第 1 步的第一步请按"需要先验证"对待：先 `git apply -p1 --check patches\ffmpeg-9.1-avs-dra.patch`。

`scripts\patch-gate.cmd` 是开发工作区里的补丁门控（它把"当前源码"与"未修改的 9.1"两头都
对一遍，期望 `PATCH_GATE=OK`）。它需要打过补丁的 `ffmpeg-9.1\` 工作树和 `pristine\ffmpeg-9.1\`，
本包里没有那棵树，所以在这里会停下来说明原因——包内的接收方用上面那两条命令就够了。

**包里的构建脚本能重建什么是读出来的，不是记要的**：`scripts\delivery-check.py` 从
`scripts\build-mft.cmd` 里的 `/Fe` 目标读出"包内构建会重建哪些 `bin\` 工具"，其余的列为
"随包提供的诊断工具"（源码在 `tools\`，但包里的脚本不编译它们）。本次（2026-11-25）它打印的是
`rebuilds 12 of the 19 tool(s) in bin/`，并把 7 个一次性探针列为随包提供。
另外 `avsvk-test` 虽然在构建脚本里，包内构建会打印跳过原因：它 include libavcodec 的内部头，
需要**打过补丁的 `ffmpeg-9.1\` 源码树**；本包只有引用，没有那棵树。
这个划分每次生成重新读一遍，所以它不会变成过时说法。

## 5. 实测能力与实测限制

除包内那一个样本外，测试素材不在本包内，下表命令在开发工作区里复跑；
`scripts\throughput-measure.py` 是唯一的吞吐入口（它同时打印交付帧数和 MFT 上报的丢帧数）。
日期均为 2026-11-25。

| 项目 | 实测 | 复跑命令 |
|---|---|---|
| AVS3 1080p 解码 | 169 帧/秒 = 源帧率的 5.65 倍，`dropped 0` | `scripts\throughput-measure.py "video\avs2_avs3_test_video\风电WindTurbines4_1080p_avs3.mkv" 200 1 default=` |
| AVS+ 720×576 解码 | 70 帧/秒 = 源帧率的 2.81 倍，`dropped 0` | `scripts\throughput-measure.py samples\CCTV-4.avs 700 1 default=` |
| AVS+ 1080i 广播流（`video\dfws.ts`，817 个存取单元） | **6.77 帧/秒 = 0.27 倍，不实时**；同一文件 FFmpeg 自带 native `cavs` 解码器 112 帧/秒（4.46 倍） | `scripts\throughput-measure.py video\dfws.ts 100000 1 default=` |
| AVS3 8K 10-bit HDR（`CCTV8K.AVS3.MP2.CB_10bit_HDR.avs3`，30 帧） | 默认策略 3.28 帧/秒 = 0.13 倍；`AVS_MFT_THREADS=1` 4.86 帧/秒 = 0.19 倍（方向复跑两次一致，倍率不稳定） | `scripts\throughput-measure.py video\playable\CCTV8K.AVS3.MP2.CB_10bit_HDR.avs3 30 3 default= threads1=AVS_MFT_THREADS=1` |
| 8K 不丢帧时的内存代价 | 30 帧期间峰值工作集 3151 MB（保留帧没有按字节的预算，这是已知缺陷） | `bin\avssource-test.exe video\playable\CCTV8K.AVS3.MP2.CB_10bit_HDR.avs3 -video -frames 30` |

已知的边界（写清楚，不遮掩）：

+ **画面结构（隔行/逐行）目前不可信**：AVS+ 广播样本里码流是场编码的（实测 `interlaced=1, top_field_first=0`），
  而我们的 media type 硬写 `MFVideoInterlace_Progressive`（`AVSMediaSource.cpp:824`、
  `AVSVideoDecoderMFT.cpp:617`），也没有样本级字段标志。这条是开发工作区里正在做的回归 11u
  （`scripts\interlace-test.py`，当前是红的）。在此之前不要依赖隔行处理。
+ **定位（seek）已实现但没有回归证据**：`AVSMediaSource.cpp:1123` 会按 `pvarStartPosition`
  调 `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)`，`GetCharacteristics` 在流可定位时上报
  `MFMEDIASOURCE_CAN_SEEK`（`AVSMediaSource.cpp:985-986`）；但工作区的回归里没有任何 seek 用例，
  也没有 `IMFMediaSource::SetPosition` 的实现。拖动进度条的实际行为请按"未验证"对待。
+ **构建里带了 H.264 解码器，但产品路径默认不用它**：`scripts\build_ffmpeg.sh:119` 是为了做归因对照
  （同一条 source/MFT/sink 只换子类型时，H.264 能走完而 AVS 不能）。要走它需要显式设
  `AVS_MFT_ALLOW_H264`（`AVSVideoDecoderMFT.cpp:384`）；默认关闭，H.264/HEVC 仍交给系统解码器。
+ **不接受 D3D manager**：`MFT_MESSAGE_SET_D3D_MANAGER` 返回 `MF_E_INVALIDREQUEST`
  （`AVSVideoDecoderMFT_Process.cpp:960`），所以没有零拷贝/DXVA 输出路径。
+ **音频**：音频侧只内置 DRA 解码器（`scripts\build_ffmpeg.sh:75`）。语料里实际出现的音频是
  AAC/AC-3/E-AC-3/MP2/MP3/DTS——这些由我们描述、交给系统解码器，本机 E-AC-3 没有可用解码器
  （`0xC00D5212`）。多音轨切换与"一条解不了的音频轨会不会让整段失败"未做端到端测量。
+ **`MF_MT_MPEG_SEQUENCE_HEADER` 的负例是红的**：`scripts\type-contract-test.py` 报告
  码流没有声明序列头时我们仍然发布它（`samples\CCTV-4-tiny.avs`）——未归因，未接入常态回归。
+ 多节目 TS 的时长（`video\dfws.ts` 两条节目时间线相差 38.96 秒）、`samples\CCTV-4.avs` 在
  `AVS_AEC_ZERO=1` 下最后两帧（下标 `[698, 699]`）与参考不同：这两条是已知缺陷，已在
  开发工作区里以确切形状钉住（回归 11f）。

## 6. 许可证

+ 本仓库里我们自己写的源码：BSD-3 条款，见 `LICENSE`。
+ `MediaFoundationTransform\Common\` 与随树的两个示例工程：Microsoft 的 MFT 示例，public domain（`MediaFoundationTransform\LICENSE`）。`Common\` 的基类编译进了 DLL（`StdAfx.h:69-73`），所以这条不是"随带文件"而是"在二进制里"。
+ 第三方组件（FFmpeg、davs2、uavs3d、nasm）：各自的许可证不变，位置与链接形态见
  `THIRD-PARTY.md`。其中 davs2 是 GPL-2.0，而它是**静态链接**进 `AVSVideoDecoder.dll` 的——
  这条决定发布形态，属于必须先确认的事（那份文件只是事实整理，不是法律意见）。
+ `samples\CCTV-4-short.avs` 是广播节目片段，版权不属于本项目，再分发授权未确认；
  删掉它只会让第 3 节的第 3 步需要换成你自己的文件。

## 7. 与开发工作区的关系

本包不是手工拷贝：生成它的 `scripts\make-delivery.py` 与自检它的
`scripts\delivery-check.py` 就在这个目录里，"什么属于交付包"只有这一份清单。
自检的内容是：源码是否与生成它的工作区一致、`.vcxproj` 引用的文件是否齐全、第三方引用是否
具体到能取回同一份、包里的 DLL 是否仍能产生与参考一致的字节。
**在克隆里要跑的是 `verify.cmd`**（不需要工作区、不注册、不提权）；
`delivery-check.py` 比较的是"工作区 vs 由它生成的 `dist/`"，那是开发工作区里的动作。
完整回归套件（`scripts\run-tests.cmd`，会话 11a–11v）也在开发工作区执行。

本目录同时就是 git 检出（发布到 `https://github.com/wsgtrsys/mft_avs`）：
重新打包时 `make-delivery.py` 会把 `\.git` 移到一边再放回来，所以重新生成不会丢历史；
`.gitignore` 与打包时删除的中间物是同一份清单。

在开发工作区根目录（不是 `dist\` 里）重新生成与校验这个包（开发工作区里的解释器是
`C:\Python310\python.exe`，把它换成上面任何一条 `python` 即可）：

```
python scripts\make-delivery.py --build    REM 打包 + 在包内构建 DLL
python scripts\delivery-check.py           REM 期望末行 DELIVERY=OK
python scripts\dll-imports.py dist\MediaFoundationTransform\AVSVideoDecoder\bin\x64\Release\AVSVideoDecoder.dll
```

最后一条读 PE 导入表与资源目录：它把"部署只需要这一个 DLL"从推断变成读出来的事实
（`delivery-check.py` 每次生成都会重新读一遍并打印；2026-11-25 的包：10 个模块，全部是系统模块）。
