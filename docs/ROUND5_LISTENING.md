# 第五轮候选听测

这是一份待用户执行的检查表，不表示已经听测通过。

1. 退出旧工具并重启游戏，整包使用第五轮候选，不能混用 v1.2.0 的 DLL/后端（协议已改为 0x41534936）。
2. 运行 config.exe，选择自己的 ASIO 驱动；Output Volume 从较低值开始，Debug Log=1。默认不放 playback-trace.enabled。
3. 至少冷启动三次，等就绪后进入谱面。期望首次 PLAY 前不再出现恢复；若出现 `[ASIO recovery]`，保留 cause/stale/age_ms/trigger_epoch/trigger_tick。
4. Auto 游玩时暂停、继续、重试，切换选曲预览后退出。留意重复声、持续循环声、音量跳变及UI/BGM是否正常。

## 音量矩阵

建议固定同一谱面/皮肤与工具 Output Volume，降低设备监听音量。分别记录 ASIO 击打声和原生输出，不能只看滑块数值推断。

| 游戏 Hitsound | 游戏 Effect | ASIO 击打声观察 | 原生击打/UI观察 |
|---:|---:|---|---|
| 0% | 0% | 待记录 | 待记录 |
| 50% | 0% | 待记录 | 待记录 |
| 100% | 0% | 待记录 | 待记录 |
| 0% | 100% | 待记录 | 待记录 |
| 50% | 100% | 待记录 | 待记录 |
| 100% | 100% | 待记录 | 待记录 |

只读检查1115显示：HitsoundVolume绑定GameplayMixer.Volume，桥接器排除此用户增益；谱面/样本增益仍保留。这不代替矩阵听测，也不保证未知未来版本。

工具退出、故障或单通道拒绝后会回落原生；Hitsound=0%时对应原生回退将无声。测试原生对照时先退出游戏和工具，再独立启动游戏，避免在同一进程内反复注入。

## 提供哪些日志

退出游戏后提供本局 stdout、stderr、managed日志和同目录 asio_hook.log。新日志每行 `[t=...]` 使用同一系统启动毫秒基准。

重点检查：PLAY/started与missingSample、恢复的首次原因、`[ASIO lock] wait_max_us/hold_max_us/native_max_us/native`。锁耗时是Debug汇总，不是端到端延迟。短暂设备抖动与日志中的状态变更应对齐分析；不要手动暂停真实游戏线程来制造恢复。
