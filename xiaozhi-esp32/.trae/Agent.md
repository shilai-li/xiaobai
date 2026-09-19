# mr-esp32 项目上下文（Agent 快速索引）

> 本文档为 AI Agent 提供项目全貌：目的、功能、技术架构与实现方式。行号基于当前工作区代码，可能随提交漂移，以文件+函数名为准。

## 1. 项目目的

本项目是 **xiaozhi-esp32（小智 AI 聊天机器人）的深度定制分支**，面向自研硬件 **esp32c3-ci130x** 板卡，对接 **Moinai 云端**（`robotic-test.moinai.com`），构建一个通过 4G 蜂窝网络接入的语音对话机器人。

核心目标：低延迟、可被本地事件打断的流式语音交互（设备侧延迟已从 ~1.8s 优化到 ~0.2s）。

## 2. 硬件形态（esp32c3-ci130x 板卡）

| 部件 | 接口 | 引脚 | 职责 |
|---|---|---|---|
| ESP32-C3 | — | — | 主控：Opus 编解码、协议栈、状态机。4MB Flash，无 PSRAM，无 I2S 音频通路 |
| CI130X（启英泰伦） | UART0 @921600 8N1 | TX=GPIO0, RX=GPIO1 | 麦克风采集、硬件 VAD、唤醒词、离线命令 ASR（"小点声"等）、本地提示音、扬声器驱动、音量 |
| ML307（4G Cat.1） | UART1 @921600 | TX=GPIO2, RX=GPIO3 | 蜂窝网络：TCP/TLS/WebSocket |
| Boot 按钮 | GPIO | GPIO9 | 打断 topic 播报 |

关键分工：**音频进出、唤醒、VAD 全部在 CI130X 芯片内完成**；ESP32 侧无唤醒模型（`CONFIG_USE_ESP_WAKE_WORD=n`）、无 AFE 处理器（C3 不支持），Opus 编解码在 ESP32 的 AudioService 中完成。采样率 16kHz 单声道。

引脚/配置：`main/boards/esp32c3-ci130x/config.h`、`config.json`（自定义分区表 `partitions/v2/4m.csv`）。

## 3. 功能

- 流式语音对话：唤醒 → 流式上传 Opus → 云端 ASR + LLM + TTS → 流式播放
- 离线语音命令（CI130X 本地执行，如音量增减/静音），可撤销进行中的云端会话（cancel_turn）
- 连续对话（唤醒窗口内无需重复唤醒词，默认 30s）
- barge-in（播放中唤醒打断，复用健康 WebSocket 重新开始对话）
- topic 轮询播报（云端主动推送话题，HTTP 拉取）
- 设备端 MCP（JSON-RPC 2.0）：音量、亮度、主题、截图、重启、固件升级等工具
- 云端设备设置推送（VAD 参数、音量、唤醒窗口等 → NVS → UART 下发 CI130X）
- OTA 固件升级 + 资源包升级，Moinai 设备认证（HMAC-MD5）与激活/吊销/重激活
- 延迟追踪（12 阶段打点，日志单行报告）
- USB-Serial-JTAG 序列号写入（产测 provisioning，`main/main.cc`）

## 4. 技术架构

### 4.1 分层

```
┌─ 板级层 main/boards/ ──────────────────────────────┐
│  esp32c3-ci130x/（自定义板） common/ml307_board（蜂窝）│
│  common/board.h 基类：网络、音频、显示、LED 抽象        │
├─ 应用层 main/ ─────────────────────────────────────┤
│  application.cc（单例+主事件循环） device_state_machine │
│  mcp_server / ota / settings / moinai_device_settings│
│  latency_tracker.h                                  │
├─ 音频层 main/audio/ ───────────────────────────────┤
│  audio_service（中枢：3 任务+5 队列）                 │
│  codecs/ci130x_audio_codec + ci130x_protocol(CIAS)  │
│  processors/no_audio_processor（60ms 聚帧）           │
├─ 协议层 main/protocols/ ───────────────────────────┤
│  websocket_protocol（Socket.IO v4，实际使用）         │
│  mqtt_protocol（MQTT+UDP+AES-CTR，保留未启用）        │
├─ 组件 managed_components/ ─────────────────────────┤
│  78__esp-ml307（AT 指令/TCP/WebSocket） esp-sr 等     │
└─────────────────────────────────────────────────────┘
```

### 4.2 线程模型（核心设计）

全系统以 **Application 主任务为唯一串行化点**：

- 主循环基于 FreeRTOS EventGroup，15 个事件位（`application.h:22-36`）：SCHEDULE / SEND_AUDIO / WAKE_WORD_DETECTED / VAD_CHANGE / CLOCK_TICK / START/STOP_LISTENING / STATE_CHANGED 等
- `Application::Schedule()` 把回调压入 `main_tasks_` 队列 → 主循环取出执行（锁外执行防死锁）
- **所有触碰状态的操作**（CI130X UART 事件、WebSocket 回调、esp_timer）都经 Schedule 或事件位汇入主任务 → 保证线上顺序 `音频帧 → cancel_turn / vad_done`
- 上行发包也在主任务（`MAIN_EVENT_SEND_AUDIO` 循环，`application.cc:324-335`）

FreeRTOS 任务：`ci_rx_task`（优先级10，UART 解析）、`audio_input`(8)、`audio_output`(4)、`opus_codec`(2)、`ws_deferred_send`（延迟发送防 AT UART 自锁）、`sn_provision` 等。

### 4.3 设备状态机（`main/device_state.h` + `device_state_machine.cc`）

12 状态：Unknown / Starting / WifiConfiguring / Idle / Connecting / Listening / Speaking / Upgrading / Activating / AudioTesting / Revoked / FatalError。

要点：Listening --vad_done--> Connecting（等云端应答，不关 socket）；Speaking --> Listening/Idle；Revoked/FatalError 为终态。转换经 `IsValidTransition()` 校验 + 原子 store + 监听器通知。

## 5. 核心实现

### 5.1 CI130X 驱动与 CIAS 协议

文件：`main/audio/codecs/ci130x_audio_codec.{h,cc}`、`ci130x_protocol.h`

- **帧格式**：帧头 `A5 A5 5A 5A` + 16 字节头（[6-7] 命令字小端、[8-9] payload 长度）+ payload
- **命令字分组**：`0x01xx` 语音/配置（WAKEUP 0x0102、VAD_END 0x0103、PCM_MIDDLE 0x0105、VAD_START 0x0108、ASR_RESULT 0x0101、SET_VOLUME 0x0117）；`0x02xx` 播放（PLAY_START 0x0201、PLAY_GET 0x020A、PLAY_DATA 0x020B、PLAY_TTS_END 0x020C）；`0x4002` 状态提示音
- **接收**：`RxTask` 逐字节状态机；PCM payload 直写 24KB ring buffer（768ms 容量，容纳 VAD roll-back）。PCM 入队三重门控：会话开启 && 无云端播放 && 无本地播放，被拒字节按原因计数
- **录音 Read()**（:530-611）：实时节拍模式（按块时长设 deadline，不足补零）+ **burst 排空模式**（积压 >40ms 时一次性排空，消除固定延迟）
- **播放 Write()**（:629-725）：**PLAY_GET 信用制流控**——CI130X 每次授予 4096 字节窗口（上限 4*1920），主机预留足额信用才发 PLAY_DATA；1500ms 无新信用则重启播放会话（PCM 不丢）；首帧预授窗口压首字延迟
- **回合边界**：`BeginMicTurn()` 区分唤醒开闸（保留缓冲）与连续对话（先 flush 防上轮残留拼接）；`EnableInput(false)` 是唯一安全丢 PCM 的时机

### 5.2 音频管线（`main/audio/audio_service.{h,cc}`）

```
上行: (CI130X mic) →UART→ PCM ringbuf → audio_input(10ms块) → NoAudioProcessor(攒960样本=60ms)
      → encode_queue → opus_codec(esp_opus_enc) → send_queue(~267包/16s) → 主任务 SendAudio
下行: WebSocket → decode_queue(6s上限) → opus_codec(解码+重采样) → playback_queue(≤2)
      → audio_output(预缓冲: 对话200ms/topic 4000ms) → CI130X Write → PLAY_GET 流控 → 扬声器
```

- **60ms Opus 帧保证**：`OPUS_FRAME_DURATION_MS=60`，`NoAudioProcessor::Feed` 攒满 960 样本整帧输出；Read 永远返回完整块（补零）
- **时间戳链**：每 10ms 块记 `esp_timer` 时间戳（容量 64 deque），输出 60ms 帧时弹出首个作为 `mic_capture_time_us`，穿透到 `AudioStreamPacket` 用于延迟诊断
- **回合收尾三段式排空**（`HandleStopListeningEvent`，`application.cc:1644-1713`）：①等 mic 积压排空 → ②停语音处理 → ③排空发送队列 → 等编码空闲 → 再排空 → 发 vad_done
- 15 秒无音频自动关输入/输出（省电）

### 5.3 一次完整对话流程

```
唤醒(CI130X) → OnWakeup(board) → StartListening → [Idle→Connecting→Listening]
→ 发 start_talk + cloud_turn_active_=true + BeginMicTurn + EnableVoiceProcessing(true)
→ CI130X VAD start → 实时流式上行（每60ms: 451-元数据帧 + 二进制Opus帧）
→ VAD end → 三段式排空 → 发 vad_done → [Listening→Connecting]
→ 云端: asr_start/asr_done(空文本→回Idle重听)/llm_start/llm_done/tts_start
→ tts_start → BeginStreamingPlayback + [→Speaking]
→ 逐帧收 Opus → 解码 → 预缓冲达标 → PLAY_START/PLAY_DATA → 扬声器
→ tts_done(服务器发完) → 设备继续播缓冲 → CI130X PLAY_TTS_END(真正播完)
→ OnTtsEnd → FinishSpeakingTurn → [Speaking→Idle]（唤醒窗口仍开→继续Listening）
```

### 5.4 通信协议（`main/protocols/websocket_protocol.{h,cc}`）

实际使用 **WebSocket 上的 Socket.IO v4**（Engine.IO v4）：

- **握手**：`wss://...?EIO=4&transport=websocket` → Engine.IO Open → `"40{"Authorization":"Bearer <token>"}"` CONNECT（经延迟发送任务）→ Ping/Pong 心跳 → 等 `bot_ready`（协商采样率/帧长）。connect_error → 刷 token 重试一次
- **上行事件**（`42["bot_input",{id,timestamp,type,payload}]`）：`start_talk / vad_done / cancel_turn / mcp / tts`；音频为 `451-` 元数据帧 + 二进制 Opus 帧双帧连发
- **下行事件**（`bot_response`）：`asr_start/done, llm_start/done, tts_start(ed)/done, bot_text, user_text, llm(emotion), turn_cancelled`；`451-` 二进制预告后下一帧即 Opus TTS
- **turn_cancelled ack**：`status`（ok/no_active_turn）与 `cancelled_stage`（asr/llm/tts）**嵌在 message.data 下**（曾因从顶层解析出错而修复）
- token 来自 Moinai 设备认证（HMAC-MD5 签名，服务器 Date 头对时容错），缓存于 NVS `websocket` 命名空间，过期前 1 小时刷新

### 5.5 本地优先撤销链（cancel_turn 机制）

**`CancelCloudSession(reason, restart)`**（`esp32c3_ci130x_board.cc:147-219`，UART 线程调用）：
1. 抑制 VadStart 竞态 + 丢上行队列 + `BlockNextTts()`（丢弃后续 TTS 写入 + 强停播放）
2. 排空发送队列 → Schedule 到主任务
3. 主任务：`CancelActiveCloudTurn(reason)` → 按 restart 决定 RestartListening（复用 socket）或回 Idle

**`CancelActiveCloudTurn`**（`application.cc:1417-1432`）：
- 无条件 `LatencyTracker::AbortTurn()`（防跨轮打点污染）
- `cloud_turn_active_.exchange(false)` 原子幂等——每轮只发一次 cancel_turn
- 置位点：start_talk 发出时 / topic TTS 请求时；清除点：Idle 统一入口

**8 个调用点 / 4 种 reason**：

| 场景 | reason |
|---|---|
| 按钮/唤醒打断 topic 播报 | `topic_preempted` |
| 已醒再唤醒（barge-in）/ 伪会话 | `user_wakeup` |
| 离线命令（cmd_id=100 退出唤醒 / 音量等命令非 Speaking 时） | `local_command` |
| 唤醒窗口超时/命令退出（OnExitWakeup） | `wakeup_exit` |

本地命令 ASR：`cmd_id==100` 打断型（退出唤醒）；`cmd_id∈{3..6,9..10,302..314}` 非打断型（音量等，CI130X 芯片本地执行）。

### 5.6 延迟追踪（`main/latency_tracker.h`）

12 阶段打点（微秒原子时间戳）：`vad_start → listening_state → vad_end → mic_drain_done → encode_idle → vad_done_sent → tts_start → playback_ready → first_pcm_write → play_start → tts_stop → tts_end`，另有上行 tail（`mic_capture→uplink_sent`）。不上传云端，`ESP_LOGI("Latency", ...)` 单行报告；`ShouldBeginTurn()` 处理连续对话头尾相接归属。

### 5.7 ML307 AT 序列化（为何不能并发写）

ML307 单 UART 一问一答，`AtUart::SendCommandWithData` 持 `command_mutex_` 完成整个事务；WebSocket 帧级 `send_mutex_`；TCP 数据切 ≤730 字节 hex 块。**回调上下文禁发同步命令**（会自锁），Pong/CONNECT 走 `ws_deferred_send` 独立任务。应用层把上行音频与控制事件统一在主任务发送，保证线序。

### 5.8 MCP 设备端工具（`main/mcp_server.{h,cc}`）

JSON-RPC 2.0 / MCP 2024-11-05。工具：`self.get_device_status`、`self.audio_speaker.set_volume`(0-100)、`self.screen.set_brightness/set_theme`、`self.camera.take_photo`；UserOnly：`self.reboot`、`self.debug.get_moinai_device_settings_override`、`self.upgrade_firmware`、`self.screen.get_info/snapshot` 等。`tools/list` 分页（8000 字节上限）。

### 5.9 设置体系（`main/moinai_device_settings.{h,cc}` + `main/settings.cc`）

云端 HTTP GET `/api/v1/embeded/device/settings/{SN}` → NVS `moinai` 命名空间 → 读取时 clamp + 可选 `MOINAI_DEBUG_SETTINGS_OVERRIDE` → `Ci130xAudioCodec::ApplyStoredSettings()` 逐项 UART 下发（VAD 灵敏度/降噪/滤波帧数/结束触发/最长语音/唤醒窗口/休眠超时/多轮/全双工/音量/静音/VAD 停止播放）。刷新时机：激活前 + 每 30 分钟。

注意：MCP 音量（0-100，`SetOutputVolume` 映射 1-7）与云端 `ci_volume`（1-7，`SetCiVolume`）是两条通道。

### 5.10 OTA 与激活（`main/ota.cc`）

SN 来源：NVS `wifi/serial_number` → `CONFIG_MOINAI_SERIAL_NUMBER` → EFUSE。流程：CheckAssetsVersion（资源包）→ CheckVersion（固件，指数退避 ≤10 次）→ ShowActivationCode（数字播报）+ Activate 轮询 → PrepareMoinaiSession（token + 资格校验，未激活→重激活、未发货→Revoked）→ 建连 WebSocket → 上报基站位置 → Idle → 开始 topic 轮询。固件下载：顺序写 + 头部预校验 + `MarkCurrentVersionValid` 防回滚。

## 6. 关键文件索引

| 文件 | 职责 |
|---|---|
| `main/boards/esp32c3-ci130x/esp32c3_ci130x_board.cc` | 板级：事件回调、CancelCloudSession 8 调用点、看门狗、竞态防护 |
| `main/audio/codecs/ci130x_audio_codec.cc` | CI130X 驱动：UART 协议、PCM ringbuf、PLAY_GET 流控、BlockNextTts |
| `main/audio/codecs/ci130x_protocol.h` | CIAS V2.4 命令字定义与默认参数 |
| `main/audio/audio_service.cc` | 音频中枢：3 任务、编解码、队列、预缓冲 |
| `main/audio/processors/no_audio_processor.cc` | 10ms→60ms 聚帧 |
| `main/application.cc` | 单例主循环、状态机驱动、会话管理、cancel/排空逻辑 |
| `main/protocols/websocket_protocol.cc` | Socket.IO 协议、SendAudio 双帧、turn_cancelled 解析 |
| `main/latency_tracker.h` | 12 阶段延迟打点 |
| `main/mcp_server.cc` | 设备端 MCP 工具 |
| `main/ota.cc` | 认证、激活、固件/资源升级、topic/设置拉取 |
| `main/moinai_device_settings.cc` | 云端设置 NVS 持久化与合成 |
| `main/main.cc` | 入口 + USB 序列号产测控制台 |
| `main/boards/common/ml307_board.cc` | 蜂窝网络、时间同步、信号/状态 JSON |
| `managed_components/78__esp-ml307/` | AT UART / TCP / WebSocket 组件 |

## 7. 硬约束与工程约定（修改代码前必读）

1. **音频必须以 60ms Opus 帧流式上传，带准确时间戳**（帧首样本采集时刻）
2. **ML307 AT 不支持并发写**：所有网络操作必须序列化到主任务；协议回调上下文禁止同步发 AT 命令（自锁），需走延迟发送
3. **协议事件（start_talk / vad_done / cancel_turn）必须按序发送，不得重叠**；cancel_turn 后不得有帧尾随
4. **cancel_turn 的 reason 只能取 4 值**：`local_command / user_wakeup / topic_preempted / wakeup_exit`
5. **turn_cancelled 的 status/stage 嵌套在 message.data 下**解析
6. **本地命令撤销必须复用 CancelCloudSession 既有清理链**：suppress VadStart → DiscardCi130xPendingUplink → BlockNextTts → 排空 → Schedule → CancelActiveCloudTurn（幂等）
7. **会话取消必须幂等**：`cloud_turn_active_` 原子 exchange 防重复 cancel_turn；`CancelActiveCloudTurn` 开头必须 `LatencyTracker::AbortTurn()`
8. **音频发送队列在会话取消时必须排空并清空**；回合收尾走三段式排空（mic backlog → encode idle → send queue）
9. `cloud_turn_active_` 在 start_talk 与 topic TTS 两条路径都要置位；清除统一在 Idle 入口
10. 每次重新 StartListening 前须 `ClearTtsBlock()`（`EnableInput` 在已启用时会短路，不是可靠的丢弃开关）
11. 设备侧延迟优化按 A1（PCM burst 读）→ A3 → A2（流式上传+迟发命令撤销）→ A4（预缓冲调整）顺序推进

## 8. 构建信息

- ESP-IDF **>= 5.5.2**，目标芯片 esp32c3
- 板卡选择：`idf.py -B build -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" -D BOARD_TYPE=esp32c3-ci130x set-target esp32c3` 后 `idf.py build flash monitor`
- 分区表：`partitions/v2/4m.csv`（4MB Flash，v2 分区与 v1 不兼容，不可 OTA 跨越）
- 关键依赖：`78/esp-ml307 ~3.6.5`（AT/网络）、`espressif/esp_audio_codec ~2.4.1`（含 esp_opus）、`lvgl ~9.5.0`（本板无屏未实际使用）
