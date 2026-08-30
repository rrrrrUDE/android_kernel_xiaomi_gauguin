# 正在施工中 #
# PurpleMi Kernel #
# Based on Carlotta kernel project #
REDMI Note 9 Pro Enhanced Kernel(REDMI Note 9 Pro 增强型内核)

## 反馈bug或提建议（如改进内核或者添加功能）

1.提交Issues

2.发送邮件至xkandemo666@outlook.com或xkandemo666@gmail.com


适用机型（Supported Models）：REDMI Note 9 Pro（gauguinpro）

系统范围（System Scope）：Android 12 - 16 QPR0（不限底子，实测ColorOS15能正常开机，LineageOS未测试）

原仓库（Original Storage）：https://github.com/Fucking-Projekt/android_kernel_xiaomi_gauguin 

## 构建（Build）
编译环境(Build Environment)：Ubuntu 26.04 LTS
Make: 4.4.1
Clang: 21.1.8
GCC: 15.2.0
(只要是最新版的clang、gcc和make就能编译)

确认你的编译环境已经准备就绪后，执行以下命令

```
export SUBARCH=arm64
export ARCH=arm64
export LD=ld.lld
export CC=clang
export LLVM=1
export LLVM_IAS=1
```
执行后如果无输出，执行以下命令编译内核

```
make O=out gauguin_defconfig
make O=out -j2
```

## 内核信息与支持(Kernel Information & Support)

| 内核版本（Kernel Version） | 
|----------------|
| 4.19.325-PurpleMiKernel-ForGauguinpro |

CPU调度器（CPU Scheduling）：EAS+WALT（waltsched 分支/Branch）/ BORE v5.1.0-r2(mainline 分支/Branch)

ROOT方案（ROOT Solution）：ReSukiSU（Inline Hook）

SusFS：是（yes）

内存压缩算法支持(Supported Memory Compression Algorithms)：LZ4, LZ4KD, ZSTD

默认内存压缩算法(Default Memory Compression Algorithm)：LZ4KD

## 内置组件与功能（Kernel-Integrated Components & Features）
| 功能（Function） | 状态（Status） |
|---------|-------------|
| **ReKernel** | ✅ |
| **DroidSpaces** | ✅ |
| **BBG（BaseBand Guard）** | ✅ |
| **NoMount** | ✅ |
> ✅:已启用（Enabled） ⚠️:正在测试（Testing） ❌：已禁用或未内置（Disabled/None Integrate）
>
> 后续还会加入更多功能，敬请期待! 
>> More features are coming. Stay tuned!

