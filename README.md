# PurpleMi Kernel #
# Based on Carlotta kernel project #
REDMI Note 9 Pro Enhanced Kernel (REDMI Note 9 Pro 增强型内核)

## 反馈bug或提建议 (如改进内核或者添加功能)

1.提交Issues

2.发送邮件至xkandemo666@outlook.com或xkandemo666@gmail.com


适用机型 (Supported Models)：REDMI Note 9 Pro（gauguinpro）

系统范围 (System Scope)：Android 12 - 16 QPR0（不限底子，实测ColorOS15能正常开机，LineageOS未测试）

原仓库 (Original Storage)：https://github.com/Fucking-Projekt/android_kernel_xiaomi_gauguin 

## 构建环境 (Build Environment)
系统 (System)：Ubuntu 26.04 LTS

Make: 4.4.1

Clang: 21.1.8

GCC: 15.2.0

编译使用的defconfig (defconfig used for compilation): gauguin_defconfig

## 内核信息与支持 (Kernel Information & Support)

| 内核版本(Kernel Version) | 
|----------------|
| 4.19.325-PurpleMiKernel-ForGauguinpro |

CPU调度器 (CPU Scheduling)：BORE v5.1.0-r2

ROOT方案 (ROOT Solution)：ReSukiSU (Inline Hook)

SusFS：是 (yes)

内存压缩算法支持 (Supported Memory Compression Algorithms)：LZ4, LZ4KD, ZSTD

默认内存压缩算法 (Default Memory Compression Algorithm)：LZ4KD

## 内置组件与功能 (Kernel-Integrated Components & Features)
| 功能 (Function) | 状态 (Status) |
|---------|-------------|
| **ReKernel** | ✅ |
| **DroidSpaces** | ✅ |
| **BBG (BaseBand Guard)** | ✅ |
| **NoMount** | ✅ |
| **Pstore Screen** | ❌（目前无法加入此功能，请等待作者想办法） |
> ✅:已启用(Enabled) ⚠️:正在测试(Testing) ❌：已禁用或未内置(Disabled/None Integrate)
> 
> 后续还会加入更多功能，敬请期待! 
>> More features are coming. Stay tuned!

## 版权 (Copyright)
- [ReSukiSU](https://github.com/ReSukiSU/ReSukiSU) - @ReSukiSU Development
- [SuSFS for Non-GKI](https://github.com/JackA1ltman/NonGKI_Kernel_Build_2nd) - @JackA1ltman
- [Re:Kernel](https://github.com/Sakion-Team/Re-Kernel) - @Sakion-Team
- [Baseband Guard](https://github.com/vc-teahouse/Baseband-guard) - Telegram @qdyKernel
- [Droidspaces](https://github.com/ravindu644/Droidspaces-OSS) - @ravindu644
- [NoMount](https://github.com/maxsteeel/nomount) - @maxsteeel
- [pstore-screen](https://github.com/cctv18/pstore-screen) - @cctv18

