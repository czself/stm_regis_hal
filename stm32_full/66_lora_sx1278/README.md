# 第 66 课：LoRa SX1278 通信

## 1. 本课到底在学什么

本课表面现象是：串口打印 SX1278 版本号，每轮先发一包 `PING nnn`，再进入最长 5 秒的接收等待，最后延时 2 秒；如果收到另一块板发来的 `PING`，自动回一包 `PONG` 并打印 RSSI/SNR。

真正要学的是：**如何用 SPI 配置一个射频芯片，并让数据真正发到空中**。链路是：

```text
SPI1 读写寄存器
  -> 硬复位 SX1278
  -> 读 RegVersion(0x42) 确认芯片在线
  -> Sleep 模式配置频率/带宽/扩频因子/同步字
  -> Standby -> 写 FIFO -> TX -> 等 DIO0/TxDone -> Standby
  -> RXCONT -> 等 DIO0/RxDone -> 读 FIFO/长度/RSSI/SNR -> Standby
```

LoRa 和 UART/CAN 最大的区别：它传的不是“线缆上的电平”，而是**扩频后的无线调制信号**。同样的寄存器值，收发双方必须完全一致，否则收到的只是噪声。

## 2. 本课学习目标

学完本课，你应该能回答：

1. SX1278 的 SPI 读/写命令最高位分别是什么？
2. 为什么上电必须先复位，再读 `RegVersion`？
3. 433MHz 的 `Frf` 为什么是 `0x6C4000`？公式是什么？
4. BW、SF、CR 分别影响什么？本课取值是多少？
5. 收发双方的同步字为什么必须一致？
6. TX 流程里 DIO0 被映射成什么事件？
7. RX 流程里为什么要先读 `RegIrqFlags` 再读 FIFO？
8. RSSI/SNR 如何从寄存器换算？
9. 为什么 SX1278 必须接天线？不接会怎样？
10. 单块板能完成哪些验证，不能完成哪些？

## 3. 本课目录结构

```text
66_lora_sx1278/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

```bash
pio run -d stm32_full/66_lora_sx1278/reg -t upload
pio run -d stm32_full/66_lora_sx1278/hal -t upload
```

## 4. 实验硬件

- BluePill
- SX1278 模块（Ra-01 / Ra-02 空板，433MHz）
- 433MHz 天线（必须接）
- USB-TTL（看日志）
- 第二块 BluePill + SX1278 用于双机收发测试（可选，但推荐）

接线：

| BluePill | SX1278 |
|----------|--------|
| PA4 | NSS |
| PA5 | SCK |
| PA6 | MISO |
| PA7 | MOSI |
| PB0 | RESET |
| PB1 | DIO0 |
| 3.3V | 3.3V |
| GND | GND |

注意：

- SX1278 是 3.3V 器件，IO 不耐 5V
- 发射瞬间电流可达 100mA 以上，USB 供电要足
- **不接天线发射可能损坏射频前端**
- 433MHz 频段使用需遵守当地无线电法规

## 5. 先建立一个最基本的脑图

```text
上电
  -> SPI1 初始化（9MHz，Mode0）
  -> PB0 低 10ms / 高 20ms 复位
  -> 读 RegVersion，应等于 0x12
  -> Sleep
       Frf = 0x6C4000（433MHz）
       PA_CONFIG = 0xFF（PA_BOOST，17dBm）
       MODEM_CONFIG1 = 0x72（BW125，CR4/5，显式头）
       MODEM_CONFIG2 = 0x74（SF7，CRC on）
       SYNC_WORD = 0x12
  -> Standby

发送
  -> DIO0 映射 TxDone
  -> FIFO_ADDR_PTR=0，写 payload
  -> PAYLOAD_LENGTH
  -> 清 IRQ
  -> OpMode=TX
  -> 等 DIO0 上升沿（最多 2s）
  -> 读 IRQ_FLAGS，确认 TxDone
  -> 清 IRQ，回 Standby

接收
  -> DIO0 映射 RxDone
  -> FIFO_RX_BASE=0
  -> 清 IRQ
  -> OpMode=RXCONT
  -> 等 DIO0（最多 5s）
  -> 读 IRQ_FLAGS，检查 RxDone/CRC
  -> 读 RX_NB_BYTES
  -> 读 FIFO_RX_CURRENT，设置 FIFO_ADDR_PTR
  -> 突发读 FIFO
  -> 读 RSSI/SNR
  -> 清 IRQ，回 Standby
```

## 6. 核心名词解释

### 6.1 已学名词速查

| 名词 | 一句话提醒 |
|------|-----------|
| `SPI1` | PA4~PA7，主机 Mode0，9MHz（30/31 课） |
| `CS/NSS` | 每笔事务拉低，结束后拉高（31 课） |
| `FIFO` | 芯片内部收发缓冲区，通过 `RegFifo` 读写（31 课类似概念） |
| `DIO0` | 芯片事件引脚，可由寄存器映射为 TxDone/RxDone（本课新用） |

### 6.2 `LoRa` 是什么

LoRa 是 Semtech 的扩频调制技术，用更大的带宽换取更远的距离和更强的抗干扰能力。它不等于 Wi-Fi/蓝牙，数据率低（几百 bps 到几十 kbps），适合小包、远距离、低功耗场景。

本课只做点对点透明传输，不涉及 LoRaWAN 协议栈。

### 6.3 `SX1278` 是什么

SX1278 是 433MHz 频段的 LoRa 收发芯片，SPI 接口，寄存器众多。模块（Ra-01）把芯片、晶振、匹配网络和天线座集成在一起。

本课直接操作寄存器，不依赖 RadioHead/LoRa 库，目的是看清每一步配置。

### 6.4 `RegVersion` 是什么

地址 `0x42`，固定返回 `0x12`。它是“芯片是否在线”的最小自检：

- 读到 `0x12`：SPI 接线、复位、供电基本正常
- 读到 `0x00`/`0xFF`：SPI 没通、模块没供电、复位没释放

### 6.5 `Frf` 是什么

载频寄存器，24 位，计算公式：

```text
Frf = fRF × 2^19 / 32MHz
433MHz: 433000000 × 524288 / 32000000 = 7094272 = 0x6C4000
```

拆分写入 `0x06/0x07/0x08` 三个字节。如果频率写错，接收端完全收不到，频谱仪上能看到载频偏移。

### 6.6 `BW / SF / CR` 是什么

- BW：带宽，本课 125kHz。带宽越大速率越高、灵敏度越低
- SF：扩频因子，本课 SF7。SF 越大距离越远、空中时间越长
- CR：编码率，本课 4/5。冗余越高抗干扰越强、速率越低

三者共同决定空中时间。收发双方必须完全一致。

### 6.7 `同步字` 是什么

`RegSyncWord` 用于区分不同网络。只有同步字相同的节点才会解调彼此的数据包。本课用私有值 `0x12`；LoRaWAN 用 `0x34`。

### 6.8 `DIO0` 是什么

SX1278 的多功能中断引脚，通过 `RegDioMapping1(0x40)` 映射事件：

- `0x40`：TxDone
- `0x00`：RxDone
- 其他值：RxTimeout/CAD 等

本课用轮询 DIO0，不占用 EXTI；实际工程可接 EXTI 降低 CPU 占用。

### 6.9 `IRQ_FLAGS` 是什么

地址 `0x12`，位定义：

| 位 | 含义 |
|----|------|
| 0x80 | RxTimeout |
| 0x40 | RxDone |
| 0x20 | PayloadCrcError |
| 0x10 | ValidHeader |
| 0x08 | TxDone |
| 0x04 | CadDone |
| 0x02 | FhssChangeChannel |
| 0x01 | CadDetected |

读完后必须写 `0xFF` 清除，否则下一次事件不会触发。

### 6.10 `RSSI / SNR` 是什么

- RSSI：接收信号强度，HF 端口换算 `RSSI = RegPktRssi - 157`（dBm）
- SNR：信噪比，`RegPktSnr / 4`（dB）

本课打印时用带符号整数，避免负数显示成很大的正数。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

- 72MHz、SysTick、LED、USART1：与前几课相同
- SPI1 GPIO/初始化/单字节交换：31 课相同

### 7.2 新增步骤：寄存器读写的方向位

```c
/* 写 */
spi1_transfer(address & 0x7F);
spi1_transfer(value);

/* 读 */
spi1_transfer(address | 0x80);
value = spi1_transfer(0x00);
```

最高位为 0 是写，为 1 是读。突发读写同样遵守这个规则。

如果写反：写操作变成读，配置全部不生效；读操作变成写，可能把寄存器改成随机值。

### 7.3 新增步骤：复位与版本检查

```c
GPIOB->BRR = GPIO_BRR_BR0;   // RESET 低
delay_ms(10);
GPIOB->BSRR = GPIO_BSRR_BS0; // RESET 高
delay_ms(20);
version = lora_read_reg(0x42);
if (version != 0x12) { 报错并停住 }
```

20ms 是等内部振荡器和基准稳定。复位不充分时，版本号可能偶尔读对、偶尔读错。

### 7.4 新增步骤：发送流程

```c
lora_set_opmode(STANDBY);
lora_write_reg(0x40, 0x40);       // DIO0 = TxDone
lora_write_reg(0x0D, 0x00);       // FIFO 指针
lora_write_burst(0x00, data, len);
lora_write_reg(0x22, len);
lora_write_reg(0x12, 0xFF);       // 清标志
lora_set_opmode(TX);
等 DIO0 高
flags = lora_read_reg(0x12);
确认 flags & 0x08
清标志，回 STANDBY
```

为什么必须回 Standby：SX1278 在 TX 结束后不会自动回 Standby，继续写配置可能被忽略。

### 7.5 新增步骤：接收流程

```c
lora_set_opmode(STANDBY);
lora_write_reg(0x40, 0x00);       // DIO0 = RxDone
lora_write_reg(0x0F, 0x00);       // RX base
lora_write_reg(0x22, 64);         // 最大长度
lora_write_reg(0x12, 0xFF);
lora_set_opmode(RXCONT);
等 DIO0
flags = lora_read_reg(0x12);
if (!(flags & 0x40) || (flags & 0x20)) { 丢弃 }
len = lora_read_reg(0x13);
current = lora_read_reg(0x10);
lora_write_reg(0x0D, current);
lora_read_burst(0x00, buf, len);
```

为什么读 `RX_CURRENT`：FIFO 的起始位置由最近一次包决定，直接把指针设到 `current` 才能读到正确数据。

### 7.6 新增步骤：RSSI/SNR 打印

```c
rssi = (int16_t)lora_read_reg(0x1A) - 157;
snr  = (int8_t)lora_read_reg(0x19) / 4;
```

用 `int16_t`/`int8_t` 接收，避免负数被当成大正数。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

- `HAL_SPI_Init`、`HAL_SPI_TransmitReceive`：31 课
- `HAL_GPIO_Init`、`HAL_GPIO_WritePin/ReadPin`：01 课

### 8.2 新增步骤：片选与寄存器

```c
HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
spi1_transfer(address & 0x7F);
spi1_transfer(value);
HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
```

HAL 没有“SX1278 驱动”，这些流程必须自己写。

### 8.3 新增步骤：DIO0 轮询

```c
while ((HAL_GetTick() - start) < 2000) {
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_SET) break;
}
```

对应寄存器版读 `GPIOB->IDR`。HAL 版同样要注意：DIO0 是电平事件，读完 IRQ 后要清零并切回 Standby。

### 8.4 新增步骤：SPI 速率

```c
hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
```

APB2 72MHz / 8 = 9MHz，低于 SX1278 的 10MHz 上限。用 `/2`（36MHz）会读到不稳定数据。

`HAL_SPI_Init()` 的 `CLKPolarity=LOW`、`CLKPhase=1EDGE` 对应 `SPI1->CR1.CPOL=0/CPHA=0`；`NSS=SPI_NSS_SOFT` 对应 `SSM=1`；`BaudRatePrescaler=8` 对应 `BR[2:0]=010`。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
SPI 方向位 -> 复位 -> 版本 -> 频率/调制参数 -> TX/RX 状态机 -> IRQ 清除
```

HAL 版重点看：

```text
同一套寄存器序列 + HAL_SPI/HAL_GPIO 的封装
```

LoRa 的难点不在 HAL，而在寄存器值和时序；两版可以逐函数对照。

## 10. 检验问题清单

### 10.1 SPI 读写的最高位分别是什么？

**答**：写为 0（`addr & 0x7F`），读为 1（`addr | 0x80`）。

### 10.2 为什么先复位再读版本？

**答**：复位让芯片进入确定状态；不复位时 SPI 可能读到随机值，配置也不可靠。

### 10.3 `Frf=0x6C4000` 怎么来的？

**答**：`433e6 × 2^19 / 32e6 = 7094272 = 0x6C4000`。

### 10.4 收发双方哪些参数必须一致？

**答**：频率、带宽、扩频因子、编码率、同步字、包头模式、CRC 开关。

### 10.5 DIO0 映射 0x40 表示什么？

**答**：TxDone。

### 10.6 忘记清 IRQ 标志会怎样？HAL 版对应哪一步？

**答**：IRQ 标志写 1 才清除，不清会让下一次 TxDone/RxDone 无法触发，表现为“只能收发一次”。HAL 版仍要调用 `lora_write_reg(REG_IRQ_FLAGS, 0xFF)`，对应寄存器版直接写 `RegIrqFlags`；HAL 只封装 SPI 传输，不替你清射频中断。

### 10.7 不接天线会怎样？

**答**：发射能量反射回功放，可能损坏射频前端；至少通信距离极短且不稳定。

### 10.8 单板能验证什么？

**答**：SPI 通信、版本号、配置写入、TX 完成；不能验证收包和空中链路。

### 10.9 RSSI 为什么可能是负数？

**答**：信号强度以 dBm 表示，通常小于 0；必须用有符号类型接收。

### 10.10 为什么接收要读 `RX_CURRENT`？

**答**：它给出本次包在 FIFO 中的起始地址，直接读 FIFO 可能读到旧数据。

## 11. 工程实现步骤

### 11.1 需求分析

实现最小 LoRa 点对点：一块板周期发 `PING`，收到 `PING` 回 `PONG`，串口打印收发状态和信号质量。

### 11.2 硬件核查

先只连 SPI 和电源，读版本号；版本正确后再接天线做发射。不要在没有天线时长时间发射。

### 11.3 寄存器路线

先写寄存器读写和版本检查，再写 TX，最后写 RX。每步都用串口打印，避免“射频问题”和“SPI 问题”混在一起。

### 11.4 HAL 路线

HAL 版重点确认 SPI 模式和速率；GPIO 片选必须手动控制，不能用硬件 NSS。

### 11.5 工程思维

LoRa 项目最重要的是参数规划：频率、SF、BW、同步字一旦确定，所有节点必须一致。空中速率低，协议要尽量短小，避免大包。

### 11.6 常见工程陷阱

- SPI 读写方向位写反
- 忘记复位或复位时间不足
- 没接天线就发射
- 收发同步字/带宽/SF 不一致
- 读完 IRQ 不清标志
- 用无符号类型读 RSSI/SNR
- 5V 直接接模块 IO

## 12. 运行现象

单板（只接一个 SX1278）：

```text
[LORA] SX1278 433MHz BW125 SF7 CR4/5
[LORA] init ok, version=18
[LORA] tx PING 0
[LORA] tx done
```

单板时每轮约 7 秒（发送 + 5 秒接收超时 + 2 秒延时）；有对端时收到回包后约 2 秒一轮。

双板（都烧本课固件，天线接好）：

```text
[LORA] tx PING 3
[LORA] tx done
[LORA] rx PING 3
[LORA] rssi=-72dBm snr=8dB
[LORA] tx PING 4
...
```

另一块板会收到 `PONG` 并打印。RSSI 通常在 -30 ~ -120dBm 之间，距离越远越负。

异常表现：

- `init failed, version=0`：SPI 没通或模块没供电
- `init failed, version=255`：MISO 悬空/模块未插好
- `tx timeout`：DIO0 没接或射频未启动
- 双板收不到：频率/同步字/SF/BW 不一致，或天线没接

## 13. 常见问题排查

### 13.1 版本号读不到 0x12

先查 3.3V 和 GND，再查 NSS/SCK/MOSI/MISO 是否接反；用逻辑分析仪看 SPI 是否有波形。

### 13.2 能发不能收

确认两块板的 `SYNC_WORD`、`MODEM_CONFIG1/2` 完全一致；确认接收方 DIO0 映射为 RxDone。

### 13.3 收到 CRC 错误

检查 BW/SF/CR 是否一致；降低 SPI 速率重试；确认天线匹配。

### 13.4 RSSI 显示异常大

用了无符号类型。改成 `int16_t` 并减 157。

### 13.5 发射后复位/死机

供电不足。发射瞬间电流大，换更短的 USB 线或加 100µF 电容。

## 14. 本课最核心的结论

1. SX1278 通过 SPI 寄存器配置，读命令最高位为 1，写为 0。
2. 上电必须复位并读 `RegVersion=0x12`。
3. `Frf = fRF × 2^19 / 32MHz`，433MHz 对应 `0x6C4000`。
4. BW/SF/CR/同步字收发双方必须完全一致。
5. TX/RX 都由 DIO0 事件驱动，IRQ 标志必须写 1 清除。
6. 接收要先读 `RX_NB_BYTES` 和 `RX_CURRENT`，再突发读 FIFO。
7. 不接天线不能发射。
8. 单板只能验证到“发送完成”，收包必须两个节点。

## 15. 建议你现在怎么读这节课

先读 `lora_init()`，把每个寄存器值和第 6 章的名词对应起来；再读 `lora_send()` 和 `lora_receive()`，用纸笔画出 DIO0 的时序；最后对照 HAL 版确认 SPI 封装差异。

## 16. 扩展练习

1. 把 SF 改成 SF9，比较空中时间和 RSSI。
2. 把带宽改成 250kHz，观察速率和灵敏度变化。
3. 用 EXTI 替代 DIO0 轮询。
4. 增加包序号和超时重传，做成可靠传输。
5. 两块板做 RSSI 随距离变化的记录实验。

## 17. 下一课预告

- 上一课：[65_can_advanced](../65_can_advanced/README.md)
- 下一课：[67_multichannel_ota](../67_multichannel_ota/README.md)：把 UART、CAN、LoRa 三个通道接到同一个 Bootloader，做多通道 OTA。
