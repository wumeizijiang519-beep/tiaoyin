# 架构与实时性边界

## 信号路径

```
明确选择的输入端点
 → WASAPI capture（事件）
 → PCM/Float 解码、通道映射
 → 输入增益 → 可选 80 Hz 低切 → 三段 EQ → 噪声门 → 3:1 压缩
 → 有界音频桥接队列 / 自适应窗化 sinc 重采样
 → 耳返音量、静音平滑、±0.95 数字限幅
 → WASAPI render（事件） → 明确选择的输出端点
                              └→ SPSC 录音队列 → 独立磁盘线程 → PCM16 WAV
```

“旁通音效”绕过低切/EQ/噪声门/压缩，不绕过输入/输出音量和数字限幅。限幅在重采样之后，避免插值过冲未经约束送入输出。它是无前视数字硬限幅，不是 true-peak 限幅，也不能保证声压安全。

## 线程与生命周期

GUI 线程负责控件、文件选择、设备枚举、参数原子写入以及停止/启动控制。音频线程在自己的 COM MTA 中创建、使用和释放所有流服务；两个端点由同一线程处理。MMCSS 使用 `Pro Audio` 分类；如果提升失败仍可运行，但诊断会显示“不可用”。不修改系统电源计划、注册表驱动参数或系统默认设备。

音频线程等待停止/输出/输入事件。每次唤醒先读取已到达的输入，再写输出。独占采集只在输入事件就绪后处理一个完整缓冲；`GetNextPacketSize` 只用于共享采集。独占输出每次提交完整 ping-pong 缓冲；共享输出使用实际 padding，目标只维持一个引擎周期的输出数据，而不是填满整个缓冲容量。

正常音频循环不申请动态内存、不进行文件 I/O、不调用 GUI、不等待应用互斥锁、不 sleep。参数每包从无锁原子变量读取，电平原子发布；GUI 100 ms 更新一次。错误路径允许构造错误字符串并退出，不承诺错误处理本身满足实时调度。停止由独立事件唤醒；不在音频回调中停止自身。

当前单音频线程串行服务两端点，降低同步复杂度，但仍受 Windows 调度和驱动调用阻塞影响。代码结构不等于硬实时保证。启动时驱动协商也可能较慢；仅运行中的正常音频路径按低延迟目标设计。

## 格式与模式

共享模式以每个端点的实际混音格式打开，不伪装成 UI 选择的独占采样率。先查询 IAudioClient3 允许的最小/最大/基本周期，把请求向上量化到允许周期；失败时重新激活客户端并使用传统共享事件模式，诊断明确标记回退。RAW 属性失败也会明确显示未启用。

独占模式按用户选择的 44.1/48/96 kHz 探测驱动支持的 Float32 / PCM32 / PCM24 / PCM16，以及原生/双/单声道候选。遇到 `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` 时，读取驱动对齐帧数、释放旧 IAudioClient、重新激活后按对齐值初始化。不自动切换采样率、共享模式或设备。

输入/输出转换支持 Float32、PCM8/16/24/32、24-in-32 等左对齐有效位格式；异常 NaN/Inf 被归零。未支持的格式或超出 8–192 kHz / 1–32 通道范围时明确失败。仅处理输入前两路和输出前两路；输入单声道复制到双耳，单声道输出取 L/R 平均。

## 跨设备时钟与缓冲

设备名相似或来自同一 USB 产品，不足以证明两个端点同钟。所有 WASAPI 配对均启用一个输入/输出音频桥。名义比率为 `输入采样率 / 输出采样率`，并以平滑队列误差驱动 PI 控制器做 ±2000 ppm 范围的缓慢修正。

重采样采用 32-tap Blackman 窗化 sinc、1024 分数相位预计算表，并按名义比率设置抗混叠截止频率；有 16 个输入采样的前视需求。它适合低延迟语音耳返，不宣称达到专业离线采样率转换器的阻带指标。EQ 和压缩无额外块级前视，滤波器仍可能产生频率相关相位延迟。

队列只在单音频线程内使用。目标余量取较大端点周期（换算到输入帧）乘以 1–3 块。启动先积累足够帧，随后淡入；启动静音不计为运行欠载。输入不足时归零、计数并重新预填充；排队过长时丢弃陈旧帧、计数并淡入，宁可报告一次重同步，也不让耳返延迟无限增长。

这是抗小幅时钟漂移和有限调度抖动的工程处理，不能消除蓝牙缓存、硬件转换、USB 传输或驱动内置延迟。

## 录音

SPSC 队列仅用于音频线程与录音线程之间；有界容量 262,144 个双声道帧（48 kHz 时约 5.46 秒）。入队不覆盖消费者正在读取的数据，磁盘线程定期取出并写 PCM16 WAV。写入溢出/磁盘失败/RIFF 容量限制均停止录音，不在音频线程重试写盘。录音停止排空已入队数据并回写 WAV 长度。

## 指标含义

- **实际周期**：初始化后采用的引擎/驱动周期，不是 UI 请求值。
- **缓冲容量**：GetBufferSize 返回的容量，不等于始终排队的数据量。
- **驱动报告延迟**：GetStreamLatency 的返回值，不是模拟口声学实测。
- **软件排队**：应用桥接队列中的输入样本数量换算的近似时间。
- **处理负荷**：输入/输出处理用时相对于输出数据时长的近似占比，不是整个进程 CPU 占用，也不是硬件 DPC 指标。
- **队列欠载/重同步/采集不连续**：程序可观察到的具体事件；不宣称覆盖所有驱动 XRUN。

上述数字不相加伪装成“实测耳返延迟”。验收以物理测量和持续稳定性测试为准。

## 官方资料

- 爱调音产品工作流参考：https://www.aitiaoyin.com/
- Microsoft Low Latency Audio：https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio
- IAudioClient::Initialize：https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize
- GetSharedModeEnginePeriod：https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-getsharedmodeengineperiod
- InitializeSharedAudioStream：https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream
- GetNextPacketSize（仅共享模式）：https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getnextpacketsize
- W3C Audio EQ Cookbook（RBJ 双二阶公式）：https://www.w3.org/TR/audio-eq-cookbook/
