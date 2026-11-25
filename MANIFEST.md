# MANIFEST（2026-11-25）

生成命令：`scripts\make-delivery.py`（在开发工作区根目录执行）。

- 文件数：330，合计 35,256,691 字节
- 全树内容指纹（逐文件 `路径 + SHA-256` 再哈希）：`a8deb7a6eff72eb0d9b22901bc36db29e4f590c3bc91f4e3cfe7f7a27fb75acb`
- 第三方源码（FFmpeg / davs2 / uavs3d / nasm）**不在本包内**：引用与我们实际构建的那一份的哈希见 `THIRD-PARTY.md` 与 `deps\README.md`。
- 包内的自检样本：`samples\CCTV-4-short.avs`（广播片段，版权不属于本项目，见 `THIRD-PARTY.md` 第 4 节）；解码 canary `6c52e2bcd47d12ceee40532e1173eb0e`。
- 交付的二进制：`dist/MediaFoundationTransform/AVSVideoDecoder/bin/x64/Release/AVSVideoDecoder.dll`
  - 大小 5,328,896 字节，SHA-256 `7792c7e9419218ecc389d6005302aa7a0ff56a642734949a8ed21854ce673f3e`

## 按目录计数

| 目录 | 文件数 | 字节 |
|---|---|---|
| `(包根目录)` | 6 | 27,536 |
| `.git` | 190 | 10,451,019 |
| `MediaFoundationTransform` | 79 | 5,982,150 |
| `bin` | 19 | 16,315,904 |
| `deps` | 1 | 2,680 |
| `patches` | 2 | 1,610,251 |
| `samples` | 1 | 548,376 |
| `scripts` | 15 | 63,123 |
| `tools` | 17 | 255,652 |

下表列出所有可执行、可安装、可校验的产物（源码文件按上面的目录计数，全树逐文件哈希参与开头那条指纹）。校验某一个文件：`certutil -hashfile <文件> SHA256`。

| 文件 | 字节 | SHA-256 |
|---|---|---|
| `verify.cmd` | 4,368 | `3e2b9cc17f4144488f5eeaeb4f8bb1e86a709cec31dd81f48205e4aac8045ed8` |
| `MediaFoundationTransform/AVSVideoDecoder/bin/x64/Release/AVSVideoDecoder.dll` | 5,328,896 | `7792c7e9419218ecc389d6005302aa7a0ff56a642734949a8ed21854ce673f3e` |
| `bin/avsaudio-test.exe` | 4,177,920 | `ed11ca492c135f22271f647e6af753c5832075165713e17c9b40929edf174b05` |
| `bin/avsmft-register.exe` | 164,352 | `20c05305b8ad300c264fd6bf8dbd96f5adb4328c9ec105c0c25a519a92c62791` |
| `bin/avsmft-test.exe` | 5,230,080 | `73b055edacfff40de867229b5a756d3de73cd93c340b4c18706052ff128e8b83` |
| `bin/avssource-test.exe` | 218,624 | `cb366caa6d877e68336b8079b33cf3e9b1f70c9838fc5f6b90dafdf5b28ef560` |
| `bin/avsvk-test.exe` | 322,560 | `3735c953e448bddc8b3109777c76516bed9e8b6ff8ef4109439dc97640226e74` |
| `bin/cpu-isa.exe` | 142,336 | `ac5420f19ed2c24351b2cabe12668e8e5c6335370dba013bb0ed5f85b9225c76` |
| `bin/ffcaps.exe` | 4,074,496 | `e79ea0ae845e49e0b19b0a6c577b231de5ac997c98444532684e1082e23878e3` |
| `bin/mediaengine-probe.exe` | 156,672 | `29fcb63e9ae61306c1416816f39b4e3dbd0281da4fcde2f92a3124399e4de2a1` |
| `bin/mfplay-test.exe` | 181,760 | `61135a394a2ce740ff6d40db7ad204c7cf04a2d846edfeeb9dc3915170cc641d` |
| `bin/mfprobe.exe` | 169,472 | `4e47456410dfdea951107304e95bd4be5a349bf18c4ebddb1c6fd0a1feae78cb` |
| `bin/mfsession-test.exe` | 237,568 | `e68c4d4e24fade660f9c54b084f2459296a3b9f58c8b1b5faaab17b8d690bf4f` |
| `bin/mftenum.exe` | 181,760 | `0f4728acbcbb9b110871caf77cb5408d818389103c656b1173432900e7536c6e` |
| `bin/mftlifecycle-test.exe` | 164,864 | `b8328b9f5b02e6b55fbd86c4af58f004b5fed22cbebe5500e798091c943609c0` |
| `bin/probe-clock.exe` | 142,336 | `dc62a35c79ffc4adfce1602ec87666db799eeb0b4721cb011e82f1ac8bedceb4` |
| `bin/probe-evr.exe` | 143,872 | `9f6e5b6293007955e41902221dde9e5bb353e5cad5f29cdcb40d7dbb0ee68f0e` |
| `bin/probe-guid.exe` | 176,128 | `1a9e6a113e601c9db23190ca6c1a092e723deaf353c6ecae44d74ceb374999a2` |
| `bin/probe-service.exe` | 142,336 | `9edb162437dd012e21409b0420feb8dadf8c82b9714dd90671434edcbc8103ea` |
| `bin/regprobe.exe` | 141,312 | `4093fb9b3df4bb7968fcb1f79cfa42cdd1c0e5b53b6c6ab35681ddf03c3962fd` |
| `bin/typedump.exe` | 147,456 | `c8e2335bfb2a8ad16d78498a035b22e4dd8625fdbd224f07bca78f79d610be14` |
| `patches/ffmpeg-9.1-avs-dra-upstream-hunks.patch` | 7,023 | `3c9722305bea09cd3df538f00625b4b43aaa201f47a3b8995d4948ec40845d3d` |
| `patches/ffmpeg-9.1-avs-dra.patch` | 1,603,228 | `085fc479ffea51c15743645c88ac693176b99b1ba03dba019fa6fd807b786e56` |
| `scripts/build-deps.cmd` | 8,387 | `ffadea83f5518b43c361d9be422da4c898900065d5a82d2d931a5d6064e35589` |
| `scripts/build-ffmpeg.cmd` | 1,290 | `f20f8ce51e8e4cb9823146faaa767d8715bd337fe1823dbb2f1b84f85b2b524d` |
| `scripts/build-mft.cmd` | 7,343 | `501ba5e77ab51a7799662c0c5fe666621b6581b011e6a44d0fe4c8c11e419dd5` |
| `scripts/build_ffmpeg.sh` | 8,640 | `6d860eca53334537c24de5aae207ee9f3868f1c2347b3bf7889c6f643c6599d5` |
| `scripts/delivery-verify.cmd` | 4,368 | `3e2b9cc17f4144488f5eeaeb4f8bb1e86a709cec31dd81f48205e4aac8045ed8` |
| `scripts/dll-imports.py` | 4,979 | `7b573ebc2a19e74fd3f2acfbf0473895568f73b6950cbc3f01914c237e58f906` |
| `scripts/extract_cavs_dra_patch.py` | 2,719 | `d652ccc19f6d95b652903faa72a6581ce8309a54bdff6e19ef6f597a5d97c370` |
| `scripts/make-ffmpeg-patch.py` | 4,708 | `f512e90df99cfec88fa81f8e04eb0b3b3c348f33ba0bfdf05433d023c039becf` |
| `scripts/normalize_patch_paths.py` | 898 | `98c786335e2be63298ebfeb54aaec02da3a69094a1664ab386218b57cbe71911` |
| `scripts/patch-gate.cmd` | 2,378 | `0fa354c79c9cdc3e950a25d42fe58e0530d0b09d776705c8400afeaa8ea64618` |
| `scripts/patch_davs2_msvc.py` | 4,324 | `ad828bec2e9c8c4d874133615fbe8411b845d8413cbea66b439534ac3ca39dca` |
| `scripts/pkg-config.sh` | 4,414 | `57a2e8f030fbf8c111eb17d3531d4344b8cf68f9640f33b46f4890af70148861` |
| `scripts/split_ffmpeg_patch.py` | 1,330 | `53260f9af854b6649587dd598185658bf2c05c82fcabc3a037079168a0140e2a` |
| `scripts/vcenv.sh` | 2,136 | `3ba6aa908effa0fbc41a4c80528a6ef91de70db768c6395204df295957444855` |
