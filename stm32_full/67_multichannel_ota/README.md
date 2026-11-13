# 第 67 课：三通道 OTA（UART / CAN / LoRa）

## 1. 本课到底在学什么

本课表面现象是：同一个 Bootloader，既能在串口上收固件，也能在 CAN 上收，还能在 LoRa 上收；哪个通道先来数据，就锁定哪个通道，升级完成后统一跳到 `0x08008000`。

真正要学的是：**把“升级协议”和“物理传输”解耦**。前几课各自实现了一条链路：

```text
63：跳转与分区
64：UART 字节流 + OTA 协议 + Flash
65：CAN 过滤器/中断/远程帧
66：LoRa 收发
```

本课把它们组装成：

```text
                 +----------------+
PC --UART------->|                |
USB-CAN -------->|   Bootloader   |--> Flash app 区 --> 跳转
LoRa 节点 ------>|                |
                 +----------------+
                        |
                  同一套 OTA 协议
                  HELLO / DATA / END
```

OTA 协议（帧格式、CRC16、CRC32、序号、ACK）完全复用 64 课；本课新增的是**通道适配层**：

- UART：字节流直接喂协议解析器
- CAN：8 字节帧分片，按序号重组
- LoRa：一个包携带一段协议流，按包序号检查丢失

## 2. 本课学习目标

学完本课，你应该能回答：

1. 为什么通道选择只认“第一个收到数据的通道”？
2. CAN 一帧只有 8 字节，如何承载 41 字节的协议帧？
3. CAN 分片序号丢失时，为什么要重置协议状态机？
4. LoRa 包序号和 OTA 包序号有什么区别？
5. 为什么本课的 CHUNK 从 128 降到 32 字节？
6. ACK 如何从“发起通道”原路返回？
7. 多通道同时有数据会怎样？为什么不允许？
8. 升级中途某通道断开，Bootloader 如何避免跳转到半成品？
9. UART/CAN/LoRa 三个通道分别需要什么额外硬件？
10. 为什么说“协议不变、传输可变”是 OTA 架构的核心？

## 3. 本课目录结构

```text
67_multichannel_ota/
├── README.md
├── reg/
│   ├── platformio.ini
│   ├── ld/bootloader.ld
│   ├── ld/app.ld
│   ├── src/
│   │   ├── bootloader.c      # 三通道 + OTA 协议 + Flash
│   │   └── app.c             # v2.0 目标固件
│   └── tools/
│       └── ota_upload.py     # UART 上位机（chunk 32）
└── hal/
    ├── platformio.ini
    ├── ld/bootloader.ld
    ├── ld/app.ld
    ├── src/
    │   ├── bootloader.c
    │   └── app.c
    └── tools/
        └── ota_upload.py
```

```bash
pio run -d stm32_full/67_multichannel_ota/reg -e bootloader -t upload
pio run -d stm32_full/67_multichannel_ota/reg -e app

python stm32_full/67_multichannel_ota/reg/tools/ota_upload.py \
       --port /dev/ttyUSB0 \
       --file stm32_full/67_multichannel_ota/reg/.pio/build/app/firmware.bin
```

## 4. 实验硬件

| 通道 | 必需硬件 | 本课可验证程度 |
|------|---------|---------------|
| UART | USB-TTL | 完整可跑，推荐先跑这条 |
| CAN | TJA1050/SN65HVD230 + USB-CAN 分析仪，或第二块板 | 需要分析仪/第二节点 |
| LoRa | SX1278 + 第二块 LoRa 节点 | 需要第二个发送节点 |

- BluePill + ST-Link（烧 Bootloader 和首个 app）
- USB-TTL：PA9/PA10
- CAN：PA11/PA12 + 收发器，两端 120Ω
- LoRa：PA4~PA7 + PB0/PB1，3.3V 供电，必须接天线

本课默认三条通道都初始化：UART 一定可用；CAN 初始化成功但没接总线时只影响 CAN 通道；LoRa 没插模块时 `lora=0`，UART 仍能升级。

## 5. 先建立一个最基本的脑图

```text
上电
  -> UART / CAN / LoRa 全部初始化
  -> 5 秒内轮询三个通道
        UART 收到任意字节 -> channel=UART
        CAN FIFO0 收到帧 -> channel=CAN
        LoRa 收到包     -> channel=LoRa
  -> 锁定后只轮询该通道，其他通道不再参与
  -> 5 秒没有通道且 app 合法 -> 跳转 app

通道锁定后
  -> 解析器收 HELLO -> 擦除 -> ACK 原路返回
  -> 收 DATA -> 写 Flash -> ACK 原路返回
  -> 收 END -> CRC32 -> ACK -> 跳转

通道适配
  UART: [AA 55 ...] 直接喂
  CAN : ID 0x6A1，[seq][7B]，收端按 seq 拼接
  LoRa: 包 = [seq][<=63B]，收端按 seq 拼接
```

## 6. 核心名词解释

### 6.1 已学名词速查

| 名词 | 一句话提醒 |
|------|-----------|
| OTA 帧 / CRC16 / CRC32 | 64 课的协议与两级校验 |
| Flash 擦除/半字编程 | 38/64 课 |
| CAN 过滤器/FIFO0 | 65 课 |
| SX1278 寄存器/TX/RX | 66 课 |
| VTOR/MSP/跳转 | 63 课 |

### 6.2 `通道适配层` 是什么

通道适配层负责“把不同物理介质的差异磨平”，让上层协议只看到字节流：

- 收：把 UART 字节、CAN 分片、LoRa 包统一成 `ota_parser_feed(byte)`
- 发：把应答帧交给当前通道的发送函数

这样协议代码不需要知道数据是从哪来的，新增一个通道（例如 RS485、以太网）只需再加一组收发函数。

### 6.3 `通道锁定` 是什么

上电后三个通道都可能来数据。如果同时把它们的字节喂进同一个解析器，半包会互相穿插，永远拼不出完整帧。

本课的规则是：**第一个收到数据的通道获得升级会话**，其余通道忽略。PC/发送端必须持续重发 HELLO，直到被选中。

### 6.4 `CAN 分片` 是什么

CAN 数据帧最多 8 字节。本课约定：

```text
ID 0x6A1：主机 -> 目标
data[0]   ：分片序号 seq（0~255 循环）
data[1..7]：7 字节协议流
```

一个 41 字节的协议帧需要 6 个 CAN 帧。接收端按 seq 递增拼接，seq 不连续就重置解析器。

为什么不把 seq 放进协议帧：协议帧已经有自己的 SEQ（OTA 包序号），CAN 分片序号是传输层概念，两者不能混。

### 6.5 `LoRa 分片` 是什么

LoRa 是包式设备，本课约定：

```text
包 data[0]   ：包序号 seq
包 data[1..] ：最多 63 字节协议流
```

本课 CHUNK=32，所以一个协议帧最多 41 字节，能完整放进一个 LoRa 包。ACK 只有 10 字节，也放得下。

如果以后加大 CHUNK，就需要把协议帧拆成多个 LoRa 包，接收端同样按 seq 拼接。

### 6.6 `CHUNK=32` 是什么

64 课用 128 字节/包，因为 UART 是流式，长度无所谓。本课要兼容 LoRa 的 64 字节包（还要留 1 字节 seq），所以：

```text
最大协议帧 = 7 + 32 + 2 = 41 字节
LoRa 包     = 1 + 41 = 42 字节 <= 64
```

代价是 UART 升级变慢（包数变多），但三通道共用同一套参数，教学上更清晰。

### 6.7 `原路返回` 是什么

ACK 必须从“收到请求的同一个通道”发回：

- UART 请求 → UART 回
- CAN 请求 → CAN 回（同样分片）
- LoRa 请求 → LoRa 回（一个包）

如果走错通道，发送端永远等不到 ACK，表现为“升级卡在第 0 包”。

### 6.8 `空闲超时` 是什么

锁定通道后，如果 15 秒没有新数据：

- 升级未开始：若 app 合法则跳转
- 升级进行中：锁定 Flash，留在 Bootloader，等待重新 HELLO

原则仍然是：**app 区不完整时绝不跳转**。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

- 72MHz、SysTick、LED、USART1：63 课
- Flash 解锁/擦除/半字编程/CRC32：64 课
- CAN GPIO/初始化/过滤器/收发：65 课
- SX1278 复位/寄存器/收发：66 课
- OTA 解析器 10 状态机：64 课

### 7.2 新增步骤：通道选择

```c
while (g_channel == CH_NONE) {
    if (uart1_try_getc() >= 0) { g_channel = CH_UART; ... break; }
    if (can_poll_byte_input())  { g_channel = CH_CAN;  ... break; }
    if (lora_poll_byte_input()) { g_channel = CH_LORA; ... break; }
    if (超时 && app_is_valid()) jump_to_app();
}
```

### 7.3 新增步骤：CAN 分片发送

```c
while (offset < length) {
    uint8_t frame[8];
    uint8_t count = min(7, length - offset);
    frame[0] = g_can_tx_seq++;
    memcpy(&frame[1], data + offset, count);
    发送 ID 0x6A2，DLC=8
    offset += count;
}
```

应答帧 10 字节 → 2 个 CAN 帧。

### 7.4 新增步骤：CAN 分片接收

```c
seq = data[0];
if (seq != g_can_rx_seq) {
    ota_parser_init();   // 丢帧，放弃当前半包
}
g_can_rx_seq = seq + 1;
for (i = 1; i < 8; i++) ota_parser_feed(data[i]);
```

为什么重置解析器：丢一帧后，后面的字节无法拼回原帧；重置后从下一个 `AA 55` 重新同步，主机重传整包即可。

### 7.5 新增步骤：LoRa 接收

```c
if (!g_lora_rx_ready) lora_enter_rx();  // 只进入一次 RXCONT
if (DIO0 低) return 0;
flags = read(IRQ);
if (!(flags & RxDone) || (flags & CrcError)) { clear; return 0; }
len = read(RX_NB_BYTES);
current = read(FIFO_RX_CURRENT);
read_burst(FIFO, packet, len);
seq = packet[0];
if (seq != g_lora_rx_seq) ota_parser_init();
for (i = 1; i < len; i++) ota_parser_feed(packet[i]);
```

关键点：**不要在每次轮询时重新进入 RX 模式**。反复 Standby→RXCONT 会清掉 IRQ 标志，正在接收的包会丢失。

### 7.6 新增步骤：原路应答

```c
static void channel_send_bytes(const uint8_t *data, uint16_t len)
{
    if (g_channel == CH_UART) { 逐字节发送 }
    else if (g_channel == CH_CAN) { can_send_bytes(...); }
    else if (g_channel == CH_LORA) { lora_send_bytes(...); }
}
```

`ota_send_frame()` 只负责组帧，最后调用 `channel_send_bytes()`。协议层与传输层在这里解耦。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

- HAL UART/GPIO/SPI/CAN/Flash：前几课
- OTA 协议与 Flash 校验：64 课

本课用到的 HAL API 与寄存器对应关系：

| HAL API | 底层寄存器动作 |
|---------|---------------|
| `HAL_UART_Init/Transmit/Receive` | 写 `BRR/CR1`，轮询 `SR.TXE/RXNE` 后读写 `DR` |
| `HAL_CAN_Init` | 写 `MCR`、`BTR`（含 `LBKM`/`ABOM`） |
| `HAL_CAN_AddTxMessage` | 写 `TIR/TDTR/TDLR/TDHR` 并置 `TXRQ` |
| `HAL_CAN_GetRxMessage` | 读 `RIR/RDTR/RDLR/RDHR`，写 `RFOM0` |
| `HAL_SPI_Init/TransmitReceive` | 写 `CR1`（CPOL/CPHA/BR/SSM），轮询 `SR` 后读写 `DR` |
| `HAL_FLASHEx_Erase/Program` | 写 `PER/AR/STRT`、`PG`，半字编程 |

### 8.2 新增步骤：CAN 轮询

```c
if (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0) return 0;
HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &header, data);
if (header.StdId != 0x6A1) return 0;
```

本课不用 CAN 中断，因为 Bootloader 同时要轮询三个通道，轮询更直观。真实项目可以改成中断 + 队列。

### 8.3 新增步骤：LoRa 重新进入 RX

发送 ACK 后，LoRa 处于 TX 结束状态；HAL 版在 `lora_send_bytes()` 末尾判断：

```c
if (g_lora_rx_ready) lora_enter_rx();
```

否则发完 ACK 就再也收不到下一包。

### 8.4 新增步骤：跳转前反初始化

```c
HAL_UART_DeInit(&huart1);
HAL_CAN_Stop(&hcan);
HAL_SPI_DeInit(&hspi1);
```

对应寄存器版的“清 NVIC/停外设”。app 会重新初始化所有外设。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
三通道轮询 -> 通道锁定 -> 分片/重组 -> 同一解析器 -> 原路 ACK
```

HAL 版重点看：

```text
同样的架构 + HAL 外设 API + LoRa 发送后重新入 RX
```

协议部分两版完全一致；差异集中在通道初始化与收发封装。

## 10. 检验问题清单

### 10.1 为什么只锁定一个通道？

**答**：多个通道的字节流同时喂一个解析器会互相穿插，永远拼不出完整帧。

### 10.2 CAN 分片序号丢失怎么办？

**答**：重置协议解析器，丢弃当前半包，等主机重传整个 OTA 帧。

### 10.3 CAN 分片序号和 OTA SEQ 是一回事吗？

**答**：不是。前者是传输层分片序号，后者是 OTA 包序号。

### 10.4 为什么 CHUNK 降到 32？

**答**：41 字节协议帧 + 1 字节包序号 ≤ LoRa 64 字节包。

### 10.5 ACK 走错通道会怎样？

**答**：发送端永远等不到应答，升级卡住。

### 10.6 LoRa 每次轮询都重进 RX 会怎样？

**答**：清掉 IRQ 标志并打断接收，表现为大量丢包。

### 10.7 升级中途超时怎么办？

**答**：锁定 Flash，不跳转，留在 Bootloader 等重新 HELLO。

### 10.8 三个通道同时有数据会怎样？

**答**：只有一个通道被锁定，其他数据被忽略；发送端需重试。

### 10.9 CAN 通道需要什么额外硬件？

**答**：CAN 收发器（如 TJA1050）和总线终端电阻；发送端需要 USB-CAN 或第二个节点。

### 10.10 HAL 版的 UART/CAN/LoRa 收发分别对应寄存器版哪些操作？

**答**：UART 的 `HAL_UART_Transmit/Receive` 对应轮询 `USART1->SR.TXE/RXNE` 和读写 `DR`；CAN 的 `HAL_CAN_AddTxMessage/GetRxMessage` 对应写 `TIR/TDTR/TDLR/TDHR`、置 `TXRQ`，以及读 `RIR/RDTR/RDLR/RDHR` 后写 `RFOM0`；LoRa 的 `HAL_SPI_TransmitReceive` 对应寄存器版 `spi1_transfer()` 等 `TXE/RXNE` 后读写 `SPI1->DR`。

## 11. 工程实现步骤

### 11.1 需求分析

实现一个 Bootloader：三条物理通道都能接收同一套 OTA 协议，升级成功后跳转到 app。

### 11.2 硬件核查

先只跑 UART，确认协议和 Flash 正常；再接 CAN 收发器测 CAN 通道；最后接 LoRa 节点测无线通道。

### 11.3 寄存器路线

先写通道选择，再写三个 `poll` 函数，最后接 OTA 协议。每加一个通道都用串口打印 `channel=...`。

### 11.4 HAL 路线

HAL 版重点处理 CAN 初始化失败（没收发器不影响启动）和 LoRa 不存在时的降级。

### 11.5 工程思维

多通道 OTA 的核心是“协议稳定、传输可替换”。现场设备可能只有其中一种接口，Bootloader 不应因为另一种接口不存在而拒绝启动。

### 11.6 常见工程陷阱

- LoRa 反复重进 RX，丢包
- CAN 分片丢帧后不重置解析器
- ACK 没有原路返回
- 升级中途跳转到半成品 app
- CHUNK 超过 LoRa 包容量
- 通道选择阶段把调试日志当成升级数据（本课调试日志只在锁定后打印）

## 12. 运行现象

UART 通道升级：

```text
[OTA67] multi-channel bootloader: UART/CAN/LoRa
[OTA67] can=1 lora=1, chunk=32B, wait 5s
[OTA67] channel=UART
...
  进度 100%  ...
  校验通过，Bootloader 已跳转到新固件
```

板端随后打印：

```text
[APP-OTA67] v2.0 running at 0x08008000
```

CAN 通道：

```text
[OTA67] channel=CAN
```

LoRa 通道：

```text
[OTA67] channel=LoRa
```

异常表现：

- `can=0`：CAN 初始化失败（PA11/PA12 被占用或时钟没开）
- `lora=0`：SX1278 版本读不到，检查供电/SPI/天线
- 一直无通道：发送端没有在 5 秒窗口内发 HELLO
- 升级卡住：ACK 通道不对或分片序号错

## 13. 常见问题排查

### 13.1 UART 能升级，CAN 不行

先确认 CAN 收发器供电和 120Ω 终端；用 USB-CAN 分析仪看 0x6A1 是否真的发到总线。

### 13.2 LoRa 通道收不到 HELLO

确认发送节点与 Bootloader 的频率/同步字/SF/BW 一致；确认包格式是 `[seq][协议流]`。

### 13.3 升级到一半停止

看串口是否打印 `idle timeout`。检查发送端是否在等 ACK，而 ACK 走了别的通道。

### 13.4 CAN 计数增长但解析失败

分片 seq 不连续会导致解析器重置；用分析仪确认没有丢帧，或降低发送速率。

### 13.5 跳转后还是旧版本

确认 app 确实链接在 `0x08008000`，且 ST-Link 烧录的是 `-e app` 环境。

## 14. 本课最核心的结论

1. OTA 协议与物理通道解耦，是支持多通道的关键。
2. 通道选择只认第一个来数据的通道，避免半包互相污染。
3. CAN 需要分片/重组，LoRa 需要包序号检查。
4. ACK 必须原路返回。
5. CHUNK 要受最窄通道（LoRa 64 字节）约束。
6. 升级未完成时绝不跳转。
7. 任一通道缺失都不能阻止 Bootloader 启动。
8. 新通道只需实现 poll/send 两个适配函数。

## 15. 建议你现在怎么读这节课

先读 `channel_send_bytes()` 和主循环的通道选择，理解架构；再读 CAN 分片与 LoRa 包处理；最后对照 64 课确认 OTA 协议完全没变。

## 16. 扩展练习

1. 增加 RS485 通道，复用 UART 适配层。
2. 给 CAN 分片加 CRC，定位具体哪一片出错。
3. 把 LoRa 发送端做成第二块 BluePill 的固件。
4. 记录每个通道的升级耗时，比较三者差异。
5. 实现“通道优先级”：同时有数据时优先 UART。

## 17. 下一课预告

- 上一课：[66_lora_sx1278](../66_lora_sx1278/README.md)
- 下一课：本系列新增课程到此结束。后续可继续扩展：双分区 A/B 升级、固件签名、回滚。
