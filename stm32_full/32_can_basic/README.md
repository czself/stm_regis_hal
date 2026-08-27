# 第 32 课：CAN 基础

## 1. 本课到底在学什么

本课表面现象是：STM32 的 CAN1 工作在内部回环模式，周期性发送一帧标准数据帧，再从 FIFO0 读回来校验；校验成功时 PC13 LED 翻转，失败时 LED 点亮。

真正要学的是 CAN 和 UART/SPI/I2C 完全不同：

```text
UART/SPI/I2C 更像“字节怎么传”
CAN 更像“报文怎么在总线上竞争、过滤、接收”
```

本课使用内部回环，不需要外部 CAN 收发器和 CANH/CANL。报文仍会走 bxCAN 控制器内部的完整路径：

```text
CPU 写发送邮箱
  -> CAN1 内部发送路径
  -> 回环到接收路径
  -> 过滤器判断是否接收
  -> FIFO0 暂存报文
  -> CPU 读取 FIFO0
  -> 校验 ID、DLC、Data
```

所以本课不是“外部 CAN 总线通信”，而是先把 bxCAN 控制器内部结构讲清楚。

## 2. 本课学习目标

学完本课，你应该能回答：

1. CAN 报文里的 ID、DLC、Data 分别是什么？
2. 为什么 CAN 需要过滤器，而 UART/SPI 通常没有这个概念？
3. 内部回环模式到底绕过了什么、保留了什么？
4. `MCR.INRQ` 和 `MSR.INAK` 为什么成对出现？
5. `BTR` 如何配置出 500kbps？
6. 标准帧 ID 为什么写入发送邮箱时要左移 21 位？
7. FIFO0 中的报文读完后为什么必须释放？
8. HAL 版的 `CAN_TxHeaderTypeDef` 对应哪些发送邮箱寄存器？

## 3. 本课目录结构

```text
32_can_basic/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接操作 bxCAN 的 MCR、MSR、BTR、过滤器、发送邮箱和 FIFO。  
`hal/` 使用 `CAN_HandleTypeDef`、`CAN_FilterTypeDef`、`CAN_TxHeaderTypeDef` 和 `CAN_RxHeaderTypeDef`。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- LED：PC13
- CAN 引脚初始化：PA12 = CAN_TX，PA11 = CAN_RX

本课使用 CAN 内部回环，不需要外部 CAN 收发器，也不需要 CANH/CANL 总线。代码仍初始化 PA11/PA12，是为了让后续真实总线实验可以平滑过渡。

## 5. 先建立一个最基本的脑图

```text
系统时钟 72MHz
  -> APB1 = 36MHz
  -> CAN1 位时序基于 PCLK1

CAN1 初始化
  -> 打开 GPIOA/AFIO/CAN1 时钟
  -> PA12 复用推挽，PA11 输入
  -> 进入初始化模式
  -> BTR 配成 500kbps + 内部回环
  -> 过滤器 0 全放行并分配到 FIFO0
  -> 退出初始化模式

发送接收
  -> 邮箱 0 写入 ID=0x123、DLC=2、Data
  -> TXRQ 请求发送
  -> 内部回环到接收路径
  -> 过滤器放行
  -> FIFO0 有报文
  -> 读取 RIR/RDTR/RDLR/RDHR
  -> 释放 FIFO0
  -> 校验后控制 LED
```

这节课的重点是 bxCAN 控制器结构，不是外部 CAN 物理层。真实总线还需要收发器、终端电阻和多个节点。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在前面课程已完整讲解，本课只用不重复展开：

| 名词 | 一句话提醒 |
|------|-----------|
| `system_clock_72mhz_init()` | 配置 HSE+PLL 产生 72MHz，APB1=36MHz 给 CAN1 提供位时序基准 |
| `SysTick` | 内核定时器，本课配置为 1ms 中断，驱动 `g_ms_ticks` 和 `HAL_Delay()` |
| `delay_ms()` | 基于 `g_ms_ticks` 的毫秒延时 |
| `GPIOC->CRH` | GPIOC 高 8 位引脚配置寄存器，PC13 的 MODE13/CNF13 在这里 |
| `GPIOC->BSRR` / `GPIOC->BRR` | 位设置/位复位寄存器，BSRR 拉高、BRR 拉低引脚 |
| `GPIOA->CRH` | GPIOA 高 8 位引脚配置寄存器，PA11/PA12 在 MODE11~12/CNF11~12 位段 |
| `RCC->APB2ENR` | APB2 外设时钟使能寄存器，GPIOA、AFIO 的时钟在这里打开 |
| `RCC->APB1ENR` | APB1 外设时钟使能寄存器，CAN1 的时钟在这里打开 |
| `FLASH->ACR` | Flash 访问控制寄存器，72MHz 需要等 2 个 Flash 等待周期 |

### 6.2 `CAN` 是什么

CAN 是 Controller Area Network，控制器局域网。

它属于车载和工业常用的报文总线。节点发送的是“带 ID 的报文”，不是简单字节流。总线上的所有节点都能看到报文，再根据 ID 决定是否关心。

如果用 UART 的点对点思维理解 CAN，就会忽略 ID、仲裁、过滤器、FIFO 这些核心概念。

### 6.3 `bxCAN` 是什么

bxCAN 是 STM32F1 内置的 CAN 控制器模块。

它属于芯片外设层，包含发送邮箱、接收 FIFO、过滤器、位时序配置和错误管理。代码里的 `CAN1->MCR`、`CAN1->BTR`、`CAN1->sTxMailBox` 都属于这个控制器。

### 6.4 `内部回环模式` 是什么

内部回环模式让 CAN 控制器把发送路径内部接回接收路径。

它绕过外部收发器和 CANH/CANL，但保留发送邮箱、过滤器、FIFO 等控制器路径。这样可以在没有外设模块时学习 CAN 控制器。

如果切到普通模式却没有收发器和总线，发送可能无法正常完成。

### 6.5 `标准帧 ID` 是什么

标准帧 ID 是 11 位报文标识符。本课使用 `0x123`。

它属于 CAN 协议层，既表示报文含义，也参与总线仲裁。数值越小，仲裁优先级越高。

本课只校验 ID 是否等于 `0x123`，不涉及多节点仲裁。

### 6.6 `DLC` 是什么

`DLC` 是 Data Length Code，数据长度码。

经典 CAN 一帧最多 8 字节数据。本课 `DLC=2`，表示只使用 data[0] 和 data[1]。

如果 DLC 填错，接收端即使收到报文，也会认为数据长度和预期不一致。

### 6.7 `发送邮箱` 是什么

bxCAN 有 3 个发送邮箱，用来暂存待发送报文。

发送邮箱寄存器包括：

- `TIR`：ID、IDE、RTR、TXRQ
- `TDTR`：DLC
- `TDLR/TDHR`：数据字节

本课使用邮箱 0。写好报文后设置 `TXRQ`，CAN 控制器开始发送。

### 6.8 `接收 FIFO0` 是什么

FIFO0 是 CAN 接收报文暂存队列。

报文通过过滤器后进入 FIFO0。CPU 读取后必须写 `RFOM0` 释放当前槽位，否则 FIFO 计数不会减少。

如果不释放 FIFO，后续报文可能堆满并丢失。

### 6.9 `过滤器` 是什么

过滤器是 bxCAN 的硬件筛选机制。

CAN 总线上所有节点都可能收到很多 ID 的报文，但应用通常只关心其中一部分。过滤器可以在硬件层决定哪些 ID 进入 FIFO。

本课配置全放行：过滤器 0、32 位尺度、掩码模式、掩码为 0、分配到 FIFO0。

### 6.10 `初始化模式` 是什么

CAN 的关键配置必须在初始化模式下完成。

寄存器版设置 `MCR.INRQ=1` 请求进入，等待 `MSR.INAK=1` 确认。配置完成后清 `INRQ`，等待 `INAK=0`。

如果没进入初始化模式就改 BTR，配置可能不生效。

### 6.11 `BTR` 是什么

`BTR` 是 Bit Timing Register，位时序寄存器。

本课配置：

- PCLK1 = 36MHz
- BRP = 4
- TS1 = 13 tq
- TS2 = 4 tq
- 总 tq = 1 + 13 + 4 = 18

速率：

```text
36MHz / (4 * 18) = 500kbps
```

`BTR.LBKM=1` 同时打开内部回环。

### 6.12 `HAL_CAN_AddTxMessage()` 是什么

HAL 发送报文 API。它把 `CAN_TxHeaderTypeDef` 和数据缓冲区写入发送邮箱，并设置发送请求。

它对应寄存器版写 `TIR/TDTR/TDLR/TDHR` 和 `TXRQ`。

### 6.13 `HAL_CAN_GetRxMessage()` 是什么

HAL 接收报文 API。它从 FIFO 读取报文头和数据，并释放 FIFO 槽位。

它对应寄存器版读 `RIR/RDTR/RDLR/RDHR` 后写 `RFOM0`。

这个 API 的“释放 FIFO”很关键。CAN FIFO 不是普通变量，硬件最多暂存有限帧数；CPU 读完当前帧后必须告诉硬件这个槽位可以释放。寄存器版要写 `RFOM0`，HAL 版在 `HAL_CAN_GetRxMessage()` 内部完成。

如果你只读取寄存器内容但不释放 FIFO，`FMP0` 不会减少，新报文最终会被丢弃。本课内部回环每秒一帧不容易压满 FIFO，但真实总线报文密集时，这个错误会很快暴露。

### 6.14 `CAN 收发器` 是什么

CAN 收发器是把 STM32 的 CAN_TX/CAN_RX 逻辑电平转换为 CANH/CANL 差分物理信号的外部芯片，例如 TJA1050、SN65HVD230。

本课使用内部回环，所以不需要收发器，也不需要 120 欧终端电阻。但这只验证 bxCAN 控制器内部路径，不验证真实总线物理层。切到普通模式后，没有收发器和正确终端，CAN 控制器即使配置正确也无法和外部节点通信。

## 7. 寄存器版代码讲解

### 7.1 已学步骤（快速过）

以下步骤在前 30 课已多次出现，本课用列表方式快速过：

- `system_clock_72mhz_init()`：FLASH 等 2 周期 → 开 HSE → 等 HSE 稳定 → 配 PLL 9 倍频 → 等 PLL 稳定 → 切 SYSCLK 到 PLL。结果为 72MHz，PCLK1=36MHz。
- `systick_init()`：SysTick LOAD=71999，使能中断和计数器。`SysTick_Handler` 每 1ms 递增 `g_ms_ticks`。
- `delay_ms()`：基于 `g_ms_ticks` 的忙等延时。
- `led_pc13_init()`、`led_on()`、`led_off()`、`led_toggle()`：开 GPIOC 时钟 → 配 PC13 推挽输出 → 通过 BRR/BSRR 控 LED。
- `can_gpio_init()`：开 GPIOA、AFIO、CAN1 时钟；PA12 配复用推挽（CRH MODE12=11, CNF12=10）；PA11 配浮空输入（CNF11=01）。内部回环虽不依赖引脚，但保留此初始化便于以后切到真实总线。

### 7.2 本课新增步骤概览

本课新增的是 bxCAN 控制器配置与收发流程：

```text
can1_enter_init_mode()
  → CAN1->MCR.INRQ=1, 等待 MSR.INAK=1
  → CAN1->BTR = BRP=4, TS1=13, TS2=4, LBKM=1  // 500kbps + 回环
  → can1_filter_init_accept_all()               // 全放行
  → can1_leave_init_mode()                      // 清 INRQ, 等 INAK=0
  → 写发送邮箱 (TIR/TDTR/TDLR/TDHR)
  → TXRQ 请求发送
  → 等待 FIFO0.FMP0 > 0
  → 读 RIR/RDTR/RDLR/RDHR
  → RF0R.RFOM0 释放 FIFO
  → 校验 ID/DLC/Data → 控 LED
```

### 7.3 进入初始化模式

```c
CAN1->MCR |= CAN_MCR_INRQ;
while ((CAN1->MSR & CAN_MSR_INAK) == 0U) {}
```

`INRQ` 是软件请求，`INAK` 是硬件确认。只有硬件确认后，才能安全配置 BTR。

### 7.4 配置 `MCR`

本课保留最简单配置，只处于初始化请求状态，不启用时间触发、自动唤醒等扩展功能。

这有助于先看清发送邮箱、过滤器和 FIFO。

### 7.5 配置 `BTR`

```c
CAN1->BTR = CAN_BTR_LBKM |
            (3U << CAN_BTR_BRP_Pos) |
            (12U << CAN_BTR_TS1_Pos) |
            (3U << CAN_BTR_TS2_Pos);
```

寄存器字段存的是“实际值减 1”：

- BRP 字段 3 -> 实际 4
- TS1 字段 12 -> 实际 13 tq
- TS2 字段 3 -> 实际 4 tq

加上同步段 1 tq，总共 18 tq，得到 500kbps。`LBKM` 打开内部回环。

### 7.6 配置过滤器全放行

代码进入过滤器初始化，关闭过滤器 0，选择 32 位尺度、掩码模式、FIFO0，`FR1=0`、`FR2=0`，再激活过滤器。

掩码全 0 表示所有 ID 都匹配，所以回环报文能进入 FIFO0。

### 7.7 退出初始化模式

清 `MCR.INRQ`，等待 `MSR.INAK=0`。这表示 CAN 控制器进入正常工作状态。

### 7.8 等待发送邮箱空

`CAN_TSR_TME0` 表示邮箱 0 空闲。只有空闲时才能写入新报文。

如果邮箱一直不空，发送路径可能卡住或前一帧没有完成。

### 7.9 写发送邮箱 ID

```c
CAN1->sTxMailBox[0].TIR = ((uint32_t)std_id << 21);
```

标准帧 ID 位于 `TIR` 的 bit31~21，所以 11 位 ID 要左移 21 位。IDE/RTR 保持 0，表示标准数据帧。

### 7.10 写 DLC 和数据

`TDTR` 写数据长度，`TDLR/TDHR` 写最多 8 字节数据。本课只校验前 2 字节。

### 7.11 设置 `TXRQ`

```c
CAN1->sTxMailBox[0].TIR |= CAN_TI0R_TXRQ;
```

这一步请求 CAN 控制器发送邮箱 0 中的报文。内部回环模式下，报文会进入接收路径。

### 7.12 等待 FIFO0 有报文

`RF0R.FMP0` 表示 FIFO0 中有多少帧。大于 0 说明过滤器放行的报文已经进入 FIFO0。

### 7.13 读取 FIFO0 报文

代码读取 `RIR/RDTR/RDLR/RDHR`，解析 ID、DLC 和数据。标准帧 ID 需要右移 21 位取回。

### 7.14 释放 FIFO0

```c
CAN1->RF0R |= CAN_RF0R_RFOM0;
```

这一步告诉硬件当前报文已经处理完。否则 FIFO0 的报文计数不会减少。

### 7.15 主循环校验

代码校验 ID、DLC、data[0]、data[1]。成功则 LED 翻转，失败则 LED 点亮。

## 8. HAL 版代码讲解

### 8.1 已学步骤（快速过）

- `HAL_Init()`：初始化 HAL 基础环境，包括 HAL Tick（SysTick）。
- `system_clock_72mhz_init()`（HAL 版）：使用 `RCC_OscInitTypeDef` + `RCC_ClkInitTypeDef` 配置 HSE+PLL，PCLK1=36MHz。
- `led_pc13_init()`（HAL 版）：`GPIO_InitTypeDef` 配 PC13 推挽输出，对应寄存器版 CRH/MODE13/CNF13。
- `can_gpio_init()`（HAL 版）：PA12 配 `GPIO_MODE_AF_PP`（复用推挽），PA11 配 `GPIO_MODE_INPUT`。
- `SysTick_Handler()`：调用 `HAL_IncTick()`，为 `HAL_Delay()` 提供时间基准。

### 8.2 `CAN_HandleTypeDef hcan`

`hcan.Instance = CAN1` 绑定 CAN1。`hcan.Init` 字段描述位时序、模式和自动功能。

### 8.3 `Prescaler / TimeSeg1 / TimeSeg2`

HAL 字段：

- `Prescaler=4`
- `TimeSeg1=13TQ`
- `TimeSeg2=4TQ`
- `SyncJumpWidth=1TQ`

对应寄存器版 BTR，得到 500kbps。

### 8.4 `Mode = CAN_MODE_LOOPBACK`

该字段对应 `BTR.LBKM=1`。它启用内部回环，不需要外部收发器。

### 8.5 `HAL_CAN_Init()`

该函数进入初始化模式，写 MCR/BTR 等寄存器，再按 HAL 状态机完成初始化。

### 8.6 `CAN_FilterTypeDef`

过滤器字段对应寄存器版过滤器配置：

- `FilterBank=0`
- `FilterMode=IDMASK`
- `FilterScale=32BIT`
- `FilterId/Mask=0`
- `FilterFIFOAssignment=FIFO0`
- `FilterActivation=ENABLE`

### 8.7 `HAL_CAN_Start()`

启动 CAN 工作，退出睡眠/初始化相关状态，让 CAN 控制器开始处理发送和接收。

### 8.8 `CAN_TxHeaderTypeDef`

发送头字段对应邮箱寄存器：

- `StdId` -> TIR 的 STID
- `IDE` -> 标准/扩展帧
- `RTR` -> 数据帧/远程帧
- `DLC` -> TDTR 的 DLC

### 8.9 `HAL_CAN_AddTxMessage()`

把发送头和数据写入一个空邮箱，并返回使用的邮箱编号。对应寄存器版写邮箱并置 `TXRQ`。

### 8.10 `HAL_CAN_GetRxFifoFillLevel()`

查询 FIFO0 是否有报文。对应读取 `RF0R.FMP0`。

### 8.11 `HAL_CAN_GetRxMessage()`

从 FIFO0 取出报文头和数据，并释放 FIFO。对应读取接收邮箱寄存器并写 `RFOM0`。

## 9. 两个版本真正应该怎么学

寄存器版按 bxCAN 数据路径读：

```text
BTR/过滤器 -> 发送邮箱 -> TXRQ -> FIFO0 -> RFOM0
```

HAL 版按结构体读：

```text
CAN_HandleTypeDef.Init -> BTR/MCR
CAN_FilterTypeDef -> 过滤器寄存器
CAN_TxHeaderTypeDef -> 发送邮箱
CAN_RxHeaderTypeDef -> 接收邮箱
```

CAN 的难点不是“调用发送函数”，而是理解报文 ID、过滤器、FIFO 和位时序。

## 10. 检验问题清单

### 10.1 内部回环模式需要外部 CAN 收发器吗？

**答**：不需要。报文在 CAN 控制器内部从发送路径回到接收路径，但仍经过邮箱、过滤器和 FIFO。

### 10.2 为什么 CAN 需要过滤器？

**答**：CAN 是共享报文总线，节点会看到很多 ID 的报文。过滤器能在硬件层筛选需要的 ID，减少 CPU 负担。

### 10.3 `BTR` 如何得到 500kbps？

**答**：PCLK1=36MHz，BRP=4，总 tq=1+13+4=18，所以速率为 36MHz/(4*18)=500kbps。

### 10.4 标准帧 ID 为什么左移 21 位？

**答**：因为 bxCAN 发送邮箱 `TIR` 中标准 ID 位于 bit31~21，11 位 ID 必须移动到该字段位置。

### 10.5 读完 FIFO0 为什么要释放？

**答**：否则 FIFO0 当前槽位仍被占用，报文计数不减少，后续报文可能堆满丢失。

### 10.6 HAL 版 `StdId` 对应哪个寄存器字段？

**答**：对应发送邮箱 `TIR` 的 STID 字段，也就是标准帧 ID 字段。

### 10.7 本课全放行过滤器的关键是什么？

**答**：掩码模式下掩码全 0，任何 ID 都匹配，并且过滤器分配到 FIFO0。

### 10.8 为什么真实 CAN 总线还需要收发器？

**答**：STM32 的 CAN_TX/CAN_RX 是逻辑信号，真实 CANH/CANL 差分物理层需要外部 CAN 收发器转换。

## 11. 工程实现步骤

### 11.1 需求分析

本课先验证 bxCAN 控制器内部收发路径。选择内部回环是为了不引入收发器、终端电阻和多节点问题。

### 11.2 硬件核查

只需 BluePill 和 ST-Link。PC13 用于观察结果。PA11/PA12 初始化了，但本课不接外部 CAN 模块。

### 11.3 寄存器路线

打开 CAN1 时钟，进入初始化模式，配置 BTR 和过滤器，退出初始化模式，写发送邮箱，读 FIFO0，释放 FIFO。

### 11.4 HAL 路线

填 `hcan.Init`，调用 `HAL_CAN_Init()`；填过滤器结构体并配置；`HAL_CAN_Start()` 后发送并轮询 FIFO0 接收。

### 11.5 工程思维

CAN 调试要分层：先内部回环，再静默/回环，再外部收发器单节点，再多节点真实总线。不要一开始就把所有变量混在一起。

### 11.6 常见工程陷阱

BTR 位时序算错、过滤器没激活、FIFO 读完不释放、ID 位移错、没启动 CAN、普通模式下没收发器，都会导致没有报文或校验失败。

还有一个常见误区是把内部回环成功等同于真实总线成功。内部回环绕过了 CANH/CANL、电平转换、终端电阻、其他节点 ACK 和总线仲裁，所以它只能证明控制器配置、邮箱、过滤器和 FIFO 逻辑正确。

真实总线排查要额外看物理层：收发器供电、TX/RX 连接、CANH/CANL 是否接反、两端 120 欧终端、所有节点波特率是否一致，以及总线上是否至少有一个节点能 ACK。

## 12. 运行现象

正常情况下，PC13 每秒翻转一次。若 CAN 初始化失败、发送失败、FIFO 收不到报文或校验不一致，LED 会点亮。

内部回环不需要外部 CANH/CANL，因此接不接收发器不影响本课现象。

## 13. 常见问题排查

### 13.1 LED 一直亮

说明初始化、发送、接收或校验某一步失败。先用调试器看是否 `can1_init()` 返回失败，再看 FIFO0 是否有报文。

### 13.2 FIFO0 一直没有报文

检查是否启用内部回环、过滤器是否激活并全放行、发送邮箱是否真的设置了 `TXRQ`。

### 13.3 ID 校验不一致

检查标准 ID 左移/右移 21 位是否正确，IDE 是否为标准帧而不是扩展帧。

### 13.4 HAL 发送失败

检查 `HAL_CAN_Start()` 是否成功、是否有空发送邮箱、`hcan.Instance` 是否为 CAN1。

### 13.5 切到真实总线后不通

本课内部回环不验证物理层。真实总线要检查 CAN 收发器、CANH/CANL、120 欧终端电阻、波特率一致和节点 ACK。

## 14. 本课最核心的结论

1. CAN 是报文总线，核心单位是带 ID 的帧，不是裸字节。
2. 内部回环适合先验证 bxCAN 控制器内部路径。
3. CAN 配置关键参数必须在初始化模式下设置。
4. `BTR` 决定位时序，当前配置得到 500kbps。
5. 过滤器决定报文能否进入 FIFO。
6. 发送邮箱和接收 FIFO 是 bxCAN 收发路径的两个核心缓冲区。
7. HAL CAN 结构体字段都能映射到底层寄存器和邮箱字段。

## 15. 建议你现在怎么读这节课

先把第 5 章的内部路径画出来，再读过滤器和 FIFO。然后对照寄存器版发送邮箱和接收 FIFO 的位移操作，最后读 HAL 版结构体字段映射。

## 16. 扩展练习

1. 把标准 ID 改成 `0x321`，观察校验逻辑。
2. 把过滤器从全放行改成只接收 `0x123`。
3. 修改 DLC 为 4，增加 data[2]/data[3] 校验。
4. 接入 CAN 收发器，切换普通模式做真实两节点实验。

## 17. 下一课预告

- 上一课：[31_spi_w25q64](../31_spi_w25q64/README.md)
- 下一课：[33_nvic_priority](../33_nvic_priority/README.md)
