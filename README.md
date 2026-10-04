# PurpleMi Kernel

适用机型 (Supported Models)：REDMI Note 9 Pro（gauguinpro）

系统范围 (System Scope)：Android 12 - 16 QPR0
>⚠️ 注意：本次更新后将会暂停更新一段时间，将会准备合并linux-4.19.y-cip-rt上游 + 新的BPF，尽情期待...
>
>⚠️ Note: Updates will be paused for a while following this release as we prepare to merge the upstream linux-4.19.y-cip-rt + New BPF, Stay tuned...
## 内核信息与支持 (Kernel Information & Support)

| 内核版本 (Kernel Version) | 
|----------------|
| 4.19.325-PurpleMiKernel-ForGauguinpro |

| 内核特性 (Kernel Feature) | Placeholder |
|---------|-------------|
| **Landlock 沙箱机制 (Landlock Sandbox Framework)** | ✅ Work well |
| **调度器 (Scheduler)** | 🚧 WALT+EAS(Backporting and Moving to sched_ext(scx)) |
| **io_uring 支持 (io_uring Support)** | 🚧 BACKPORT Stage... |
| **BBRv3 TCP 拥塞控制算法 (BBRv3 TCP Congestion Control)** | 🚧 Still BACKPORT Stage... |
> 后续版本将持续同步更多上游内核特性、性能优化与安全增强，敬请期待！
> 
> Future releases will continue to bring more upstream kernel features, performance improvements, and security enhancements. Stay tuned!

## 内置组件与功能 (Kernel-Integrated Components & Features)
### 基础 (Basic)
ROOT方案 (ROOT Solution)：BakaSU 

SusFS支持（SuSFS Support）：是 (yes)
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

