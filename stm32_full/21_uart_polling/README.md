# 21_uart_polling - USART1 轮询收发

## 1. 本课到底在学什么

本课表面现象：串口助手收到欢迎信息；发送 `1`、`0`、`t` 后，开发板回显字符并控制 PC13 LED。

真正要学的是 USART 轮询收发链路：

```text
发送链路：
  CPU 写 USART1->DR
  → 等待 TXE=1 确认 DR 有空位
  → USART1 按 BRR 节拍产生串行波形
  → PA9 / USART1_TX
  → USB-TTL → 电脑串口助手

接收链路：
  电脑串口助手 → USB-TTL
  → PA10 / USART1_RX
  → USART1 按 BRR 节拍采样拼字节
  → 写入 DR，RXNE 置位
  → CPU 轮询 RXNE 并读取 DR
```

本课关键词是"轮询"：CPU 主动反复查看状态位（TXE/RXNE）。简单直观，但 CPU 要花时间等。下一课把接收改成中断，让 USART 收到数据后主动通知 CPU。

## 2. 本课学习目标

学完本课，你应该能做到：

- 说明 PA9、PA10 分别在 USART1 中承担什么角色
- 解释为什么 PA9 要配成复用推挽输出，PA10 配成输入
- 解释 `BRR=0x0271` 和 72MHz、115200 波特率之间的关系
- 说清楚 `TXE`、`RXNE`、`DR` 在收发中的硬件意义
- 区分阻塞轮询和非阻塞轮询
- 把 HAL 版 `HAL_UART_Transmit()`、`HAL_UART_Receive(..., 0)` 对应回 `TXE/RXNE` 轮询
- 根据无输出、乱码、收不到命令等现象排查

## 3. 本课目录结构

```text
21_uart_polling/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

## 4. 实验硬件

- STM32F103C8T6 BluePill
- ST-Link 下载器
- PA9（USART1_TX）→ 接 USB-TTL 的 RX
- PA10（USART1_RX）→ 接 USB-TTL 的 TX
- GND → USB-TTL 的 GND（必须共地）
- PC13：板载 LED
- 串口参数：115200，8N1

## 5. 先建立完整脑图

```text
1. 系统时钟 72MHz（PCLK2=72MHz → USART1 时钟源）
2. PC13 推挽输出
3. ★ 开 GPIOA / AFIO / USART1 时钟（APB2ENR）
4. ★ PA9 → 复用推挽输出（USART1_TX）
5. ★ PA10 → 浮空输入（USART1_RX）
6. ★ BRR = 0x0271（115200 @ 72MHz）
7. ★ CR1：TE + RE + UE
8. ★ 发送：轮询 TXE → 写 DR
9. ★ 接收：检查 RXNE → 读 DR（非阻塞）
10. 命令解析 → 控制 LED
```

第 3~9 步是本课新增内容。

## 6. 核心名词解释

### 6.1 已学名词速查

| 名词                | 一句话提醒                                                    |
| ------------------- | ------------------------------------------------------------- |
| `RCC->APB2ENR`    | APB2 外设时钟使能，本课开 IOPA/AFIO/USART1                    |
| `AFIO`            | 复用功能 I/O，第 10 课 EXTI 用过，本课用于 USART1_TX 复用输出 |
| `GPIO CRH/CRL`    | 引脚模式配置，PA9/PA10 在 CRH                                 |
| `BSRR / BRR`      | 置位/复位寄存器，控制 PC13 LED                                |
| `PC13 低电平点亮` | BluePill LED 极性，BRR=亮，BSRR=灭                            |
| `PCLK2 = 72MHz`   | APB2 时钟，USART1 时钟来源                                    |

### 6.2 `USART1` 与异步串口

**是什么**：STM32F103 的通用同步/异步串行外设。本课只用异步模式，所以也叫 UART。

**干什么**：把 CPU 写入的并行字节转换成 TX 线上的串行波形，把 RX 线上的串行波形还原成字节放进 DR。

**本课为什么需要**：之前所有课程中，开发板只能通过 LED 和按键与外界交互。USART1 让开发板和电脑交换字节，是后续调试输出、命令控制、printf 重定向、DMA 发送的基础。

**配错会怎样**：USART1 时钟不开，收发寄存器配置不生效；GPIO 不配成复用模式，USART 信号出不了引脚。

### 6.3 `PA9 / USART1_TX` 与 `PA10 / USART1_RX`

**是什么**：USART1 的默认发送/接收引脚。PA9 发，PA10 收。

**干什么**：PA9 由 USART1 发送器驱动电平输出；PA10 把外部串口电平送入 USART1 接收器。

**本课为什么需要**：PA9 必须配成复用推挽输出（`CNF9=10`），因为引脚电平由 USART1 控制，不是 GPIO 寄存器控制。PA10 配成浮空输入（`CNF10=01`），让外部 USB-TTL TX 信号自然进入。接线必须交叉：开发板 TX 接 USB-TTL RX，开发板 RX 接 USB-TTL TX。

**配错会怎样**：PA9 配成普通推挽（`CNF9=00`），USART1 信号出不了引脚，串口助手无输出；TX 接 TX、RX 接 RX，两个发送端接一起，没人接收。

### 6.4 `BRR` 与波特率计算

**是什么**：USART 波特率寄存器，控制发送和接收的位时间。

**干什么**：串口没有独立时钟线，双方只能约定"每一位持续多久"。STM32 靠 BRR 从 PCLK2 分频得到采样节奏，电脑靠你设置的 115200 解码。

**本课为什么需要**：USART1 时钟 = PCLK2 = 72MHz，目标 115200，16 倍过采样：

```text
USARTDIV = 72000000 / (16 × 115200) = 39.0625
整数部分 = 39 = 0x27 → DIV_Mantissa
小数部分 = 0.0625 × 16 = 1 → DIV_Fraction
BRR = (39 << 4) | 1 = 0x0271
```

**配错会怎样**：BRR 错或 PCLK2 不是 72MHz，串口助手显示乱码。这是串口无输出/乱码时最优先检查的项。

### 6.5 `TXE` 与 `RXNE`

**是什么**：USART 状态寄存器 `SR` 中的两个标志位。TXE = Transmit Data Register Empty，RXNE = Read Data Register Not Empty。

**干什么**：TXE=1 表示可以向 DR 写下一个字节；RXNE=1 表示 DR 里已有新接收字节。

**本课为什么需要**：发送前必须等 TXE=1，否则可能覆盖上一字节；接收时查 RXNE=1 再读 DR，读后硬件自动清 RXNE。这两个标志是轮询收发的核心。

**配错会怎样**：不等 TXE 就写 DR，上一字节丢失；不读 DR，RXNE 不清，后续数据溢出。

### 6.6 `DR` — 数据寄存器

**是什么**：USART 数据寄存器，一个名字、两个方向。

**干什么**：写 DR 启动发送路径；读 DR 取出接收字节并清 RXNE。

**本课为什么需要**：DR 是 CPU 和 USART 数据交换的唯一入口。发送时 `USART1->DR = byte`，接收时 `byte = USART1->DR`。

**配错会怎样**：接收时不读 DR，RXNE 永远不清，后续字节无法正常接收；发送时不等 TXE 就写 DR，字节丢失。

### 6.7 `CR1.TE / RE / UE`

**是什么**：USART 控制寄存器 1 的三个使能位。TE = 发送器使能，RE = 接收器使能，UE = USART 总使能。

**干什么**：TE 让发送器接管 TX 功能；RE 让接收器监听 RX；UE 是整个 USART 的总开关。

**本课为什么需要**：没有 UE，TE/RE 不生效。代码先写 TE|RE，再置 UE，是标准顺序：先配工作模式，再开外设。

**配错会怎样**：不开 UE，收发都不工作；只开 TE 不开 RE，只能发不能收。

### 6.8 轮询与非阻塞接收

**是什么**：轮询是 CPU 主动反复检查状态位。非阻塞接收是检查一次 RXNE 后立即返回。

**干什么**：本课发送时等 TXE（阻塞轮询），接收时查 RXNE 有则读、无则跳过（非阻塞轮询）。

**本课为什么需要**：发送是 CPU 主动的，知道要发多少字节，阻塞等待可接受；接收是被动等外部数据，不知道何时到来，非阻塞让主循环不被卡住。

**配错会怎样**：接收用阻塞轮询（`while (!RXNE)`），CPU 卡死在等数据，无法做其他任务。

### 6.9 `8N1` 帧格式

**是什么**：串口帧格式：8 个数据位、无校验、1 个停止位。

**干什么**：约定收发双方如何切分字节边界。电脑串口助手和 STM32 必须使用相同格式。

**本课为什么需要**：8N1 是最常用的串口格式，本课 CR1 不额外设置 M/PCE 位，CR2 不改 STOP 位，就是默认 8N1。

**配错会怎样**：格式不一致导致接收错误或乱码。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 已学步骤（快速过）

1. `system_clock_72mhz_init()` — HSE → PLL x9 → 72MHz，PCLK2=72MHz
2. `led_pc13_init()` — PC13 推挽输出，初始高电平灭

### 7.2 新增：开 GPIOA / AFIO / USART1 时钟

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
```

三个时钟缺一不可：GPIOA 让 PA9/PA10 配置寄存器可用，AFIO 让复用功能路径可用，USART1 让串口外设本体可用。只开 GPIOA 不开 USART1，PA9 模式能配但 USART 不会发送；只开 USART1 不配 GPIO，外设内部工作也出不了引脚。

### 7.3 新增：PA9 复用推挽输出

```c
GPIOA->CRH &= ~(GPIO_CRH_MODE9 | GPIO_CRH_CNF9);
GPIOA->CRH |= GPIO_CRH_MODE9;       /* MODE9=11, 50MHz */
GPIOA->CRH |= GPIO_CRH_CNF9_1;      /* CNF9=10, 复用推挽 */
```

`CNF9=10` 表示输出源来自 USART1_TX，不是 `GPIOA->ODR`。这是串口信号能从 PA9 出去的必要条件。

### 7.4 新增：PA10 浮空输入

```c
GPIOA->CRH &= ~(GPIO_CRH_MODE10 | GPIO_CRH_CNF10);
GPIOA->CRH |= GPIO_CRH_CNF10_0;     /* CNF10=01, 浮空输入 */
```

PA10 不需要复用配置，USART1 接收器自动读取引脚电平。浮空输入让外部 USB-TTL TX 信号无干扰进入。

### 7.5 新增：BRR = 0x0271

```c
USART1->BRR = 0x0271U;
```

115200 @ 72MHz 的编码值。前提是 `system_clock_72mhz_init()` 让 PCLK2 = 72MHz。如果时钟变了，BRR 必须重算。

### 7.6 新增：CR1 使能收发

```c
USART1->CR1 = USART_CR1_TE | USART_CR1_RE;   /* 先配 TE/RE */
USART1->CR1 |= USART_CR1_UE;                  /* 再开 UE */
```

先配工作模式再开总使能，是 USART 初始化标准顺序。本课不设 M/PCE/STOP，默认 8N1。

### 7.7 新增：发送单字节（轮询 TXE）

```c
while ((USART1->SR & USART_SR_TXE) == 0U) {}
USART1->DR = byte;
```

等 TXE=1 确认 DR 有空位，再写 DR。写入后硬件自动按 8N1 帧格式从 PA9 逐位输出。一个 8N1 字节在 115200 下约 86.8μs。

### 7.8 新增：非阻塞接收（检查 RXNE）

```c
if ((USART1->SR & USART_SR_RXNE) != 0U) {
    *byte = (uint8_t)USART1->DR;   /* 读 DR 清 RXNE */
    return 1U;
}
return 0U;
```

有数据就读 DR 并返回 1，没数据返回 0。读 DR 会自动清 RXNE，允许接收下一字节。主循环只在返回 1 时使用 `ch`，不会把旧字符误当新命令。

### 7.9 主循环：命令解析

```c
if (usart1_receive_byte_nonblocking(&ch) != 0U) {
    usart1_send_string("RX: ");
    usart1_send_byte(ch);
    usart1_send_string("\r\n");
    /* '1' → LED ON, '0' → LED OFF, 't'/'T' → TOGGLE */
}
```

回显是调试手段：看到 `RX: x` 可确认 RX 接收、命令解析和 TX 发送都工作。命令解析建立在 USART 收发链路正确之上。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 已学步骤（快速过）

1. `HAL_Init()` + `system_clock_72mhz_init()` — SysTick + 72MHz
2. PC13 `GPIO_MODE_OUTPUT_PP`，初始 SET 灭

### 8.2 新增：GPIO 配置

```c
/* PA9 → USART1_TX */
gpio.Pin = GPIO_PIN_9;
gpio.Mode = GPIO_MODE_AF_PP;        /* 复用推挽，对应 CNF9=10 */
gpio.Speed = GPIO_SPEED_FREQ_HIGH;
HAL_GPIO_Init(GPIOA, &gpio);

/* PA10 → USART1_RX */
gpio.Pin = GPIO_PIN_10;
gpio.Mode = GPIO_MODE_INPUT;        /* 输入，对应 CNF10=01 */
gpio.Pull = GPIO_NOPULL;
HAL_GPIO_Init(GPIOA, &gpio);
```

`GPIO_MODE_AF_PP` 对应寄存器版 `CNF9=10`，`GPIO_MODE_INPUT` 对应 `MODE10=00, CNF10=01`。

### 8.3 新增：UART 句柄与初始化

```c
huart1.Instance = USART1;
huart1.Init.BaudRate = 115200;                     /* → BRR 自动计算 */
huart1.Init.WordLength = UART_WORDLENGTH_8B;       /* → CR1.M=0 */
huart1.Init.StopBits = UART_STOPBITS_1;            /* → CR2.STOP=00 */
huart1.Init.Parity = UART_PARITY_NONE;             /* → CR1.PCE=0 */
huart1.Init.Mode = UART_MODE_TX_RX;                /* → CR1.TE+RE */
huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;       /* → CR3 无 RTS/CTS */
huart1.Init.OverSampling = UART_OVERSAMPLING_16;   /* → 16 倍过采样 */
HAL_UART_Init(&huart1);
```

HAL 不需要手算 BRR，根据 `BaudRate` 和当前 PCLK2 自动计算并写入。`HAL_UART_Init()` 内部依次写 BRR、CR1（TE/RE 但不设 UE）、CR2、CR3，最后 CR1 的 UE=1。

HAL 字段 → 寄存器映射：

| HAL 字段           | 写入的寄存器/位     |
| ------------------ | ------------------- |
| `BaudRate`       | `BRR`（自动计算） |
| `WordLength=8B`  | `CR1.M=0`         |
| `StopBits=1`     | `CR2.STOP=00`     |
| `Parity=NONE`    | `CR1.PCE=0`       |
| `Mode=TX_RX`     | `CR1.TE=1, RE=1`  |
| `HwFlowCtl=NONE` | `CR3.CTSE/RTSE=0` |

### 8.4 新增：`HAL_UART_Transmit`

```c
HAL_UART_Transmit(&huart1, (uint8_t *)str, (uint16_t)strlen(str), HAL_MAX_DELAY);
```

内部轮询 TXE/TC 完成发送。`HAL_MAX_DELAY` 表示愿意一直等到发完。长度用 `strlen()` 计算，不会把 `'\0'` 发出去。

对应寄存器版：`while (!TXE); DR = byte;` 的循环封装。

### 8.5 新增：`HAL_UART_Receive` 超时 0

```c
if (HAL_UART_Receive(&huart1, &ch, 1U, 0U) == HAL_OK) {
    /* 收到 1 字节 */
}
```

超时 0 = 立即检查 RXNE：有数据返回 `HAL_OK`，没数据返回 `HAL_TIMEOUT`。`HAL_TIMEOUT` 不是错误，而是"这一刻没有收到数据"。

对应寄存器版：`if (RXNE) { ch = DR; }`。

注意：不要和下一课的 `HAL_UART_Receive_IT()` 混淆。`HAL_UART_Receive()` 是当前函数调用里检查状态；`HAL_UART_Receive_IT()` 是配置中断后立即返回，数据到来时由 IRQ 处理。

## 9. 两个版本怎么学

寄存器版帮助你看清 USART 的基本状态机：

```text
发送：等 TXE → 写 DR → 硬件自动串行输出
接收：查 RXNE → 读 DR → 硬件自动清 RXNE
定速：BRR 决定位时间
使能：TE + RE + UE
```

HAL 版把状态机封装成句柄和函数，但本质仍然是轮询状态位。学习顺序：先理解寄存器版的 TXE/RXNE/DR/BRR 四要素，再看 HAL 版如何用结构体字段和 API 参数表达同样的配置。

## 10. 检验问题清单

### 10.1 PA9 为什么配成复用推挽输出？

因为引脚电平由 USART1 发送器控制，不是 GPIO 寄存器控制。配成普通推挽，USART1 信号出不了引脚。

### 10.2 BRR=0x0271 是怎么算出来的？

PCLK2=72MHz，115200 波特率，16 倍过采样：`USARTDIV = 72M / (16 × 115200) = 39.0625`，整数 39、小数 1，编码 `(39<<4)|1 = 0x0271`。

### 10.3 不等 TXE 就写 DR 会怎样？

可能覆盖上一字节尚未移出的数据，导致上一字节丢失或波形出错。

### 10.4 不读 DR 会怎样？

RXNE 不清，后续接收的字节无法正常进入 DR，可能溢出丢失。

### 10.5 不开 UE 会怎样？

TE/RE 配了但不生效，USART 不工作，收发都没有。

### 10.6 接收为什么用非阻塞而不是 `while (!RXNE)` 死等？

外部数据什么时候来不由 CPU 决定，死等会卡住主循环，无法做其他任务。非阻塞让 CPU 在没数据时继续转主循环。

### 10.7 `HAL_UART_Receive(..., 0U)` 的返回值 `HAL_TIMEOUT` 是错误吗？

不是。它表示"这一刻没有收到数据"，和 `HAL_ERROR` 不同。主循环只在 `HAL_OK` 时处理命令。

### 10.8 `HAL_UART_Transmit` 对应寄存器版什么操作？

对应 `while (!TXE); DR = byte;` 的循环。HAL 内部轮询 TXE/TC 完成发送。

## 11. 工程实现步骤

### 11.1 需求分析

用 USART1 实现 115200 8N1 轮询收发：开发板启动后发送欢迎信息，主循环非阻塞接收命令字符，回显并控制 LED。

### 11.2 硬件核查

PA9 接 USB-TTL RX，PA10 接 USB-TTL TX，GND 共地。接线交叉，不能 TX 接 TX。串口助手设 115200 8N1。

### 11.3 寄存器路线

1. 时钟 72MHz、PC13 输出（同前课）
2. `APB2ENR` 开 IOPA + AFIO + USART1
3. PA9：CRH 清 MODE9/CNF9 → 写 MODE9=11, CNF9=10（复用推挽）
4. PA10：CRH 清 MODE10/CNF10 → 写 CNF10=01（浮空输入）
5. `BRR = 0x0271`
6. `CR1 = TE | RE`，再 `CR1 |= UE`
7. 发送：`while (!TXE); DR = byte;`
8. 接收：`if (RXNE) { byte = DR; }`

### 11.4 HAL 路线

1. HAL_Init + 时钟/PC13（同前课）
2. PA9 `GPIO_MODE_AF_PP`，PA10 `GPIO_MODE_INPUT`
3. `__HAL_RCC_USART1_CLK_ENABLE()`
4. `huart1.Init`：BaudRate=115200, 8N1, TX_RX, 无流控
5. `HAL_UART_Init(&huart1)`
6. 发送：`HAL_UART_Transmit(&huart1, buf, len, HAL_MAX_DELAY)`
7. 接收：`HAL_UART_Receive(&huart1, &ch, 1, 0)` → 检查返回值

### 11.5 工程思维

轮询收发是最简单的串口通信方式，适合入门和低速场景。它的局限在于：发送时 CPU 被等 TXE 占用，接收时如果用阻塞轮询则 CPU 被卡死。工程中常用三种改进：中断接收（下一课）、DMA 发送（第 19 课已学）、DMA 收发（后续课程）。

### 11.6 常见工程陷阱

1. **TX 接 TX、RX 接 RX** — 两个发送端接一起，没人接收，串口无输出
2. **BRR 和实际时钟不匹配** — HSE 没起或 PCLK2 不是 72MHz，串口乱码
3. **PA9 配成普通推挽** — USART 信号出不了引脚
4. **不开 AFIO 时钟** — 复用功能路径不通
5. **不开 UE** — TE/RE 配了但不生效
6. **接收用阻塞轮询** — 主循环卡死
7. **HAL 版把 HAL_TIMEOUT 当错误** — 没收到字符时不断进 error_handler

## 12. 运行现象

串口助手设 115200 8N1，开发板上电后：

- **启动瞬间**：串口助手收到两行欢迎信息：
  ```text
  [reg] USART1 polling demo ready.
  Send '1' to LED ON, '0' to LED OFF, 't' to TOGGLE.
  ```
- **发送 `1`**：串口助手显示 `RX: 1` 和 `LED ON`，PC13 LED **亮**（低电平）
- **发送 `0`**：串口助手显示 `RX: 0` 和 `LED OFF`，PC13 LED **灭**（高电平）
- **发送 `t` 或 `T`**：串口助手显示 `RX: t` 和 `LED TOGGLE`，PC13 LED **翻转**
- **发送其他字符**：串口助手显示 `RX: x` 和 `Unknown command. Use 1 / 0 / t`

**异常时**：

- 串口完全无输出：查 TX/RX 接线是否交叉、PA9 是否复用推挽、USART1 时钟是否开
- 串口输出乱码：查 BRR 是否正确、串口助手波特率是否 115200、PCLK2 是否 72MHz
- 能发不能收：查 PA10 是否输入模式、RX 线是否接对、GND 是否共地
- LED 逻辑反了：PC13 低电平点亮，不是高电平

## 13. 常见问题排查

### 13.1 串口完全无输出

按层排查：USART1 时钟（APB2ENR bit14）→ PA9 复用推挽（CNF9=10）→ BRR 值 → CR1.TE+UE → USB-TTL 接线（PA9 接 RX）→ GND 共地。

### 13.2 串口输出乱码

BRR 和实际时钟不匹配。确认 PCLK2 = 72MHz（HSE 起振、PLL 配置正确），串口助手波特率 = 115200。

### 13.3 能发不能收

PA10 配置和接线。确认 PA10 是浮空输入（CNF10=01），PA10 接 USB-TTL 的 TX，GND 共地。在调试器中观察 `USART1->SR` 的 RXNE 位是否在你发送字符后置位。

### 13.4 回显正常但 LED 不亮

PC13 极性问题。BluePill 的 PC13 LED 是低电平点亮，确认 `led_on()` 写的是 BRR（拉低），不是 BSRR（拉高）。

## 14. 本课最核心的结论

1. USART 轮询收发的核心是两个状态位：TXE（可写 DR）和 RXNE（可读 DR）
2. PA9 必须配成复用推挽输出，让 USART1_TX 控制引脚电平
3. BRR 决定波特率，计算依赖 PCLK2 频率，时钟变了 BRR 必须重算
4. 发送用阻塞轮询（等 TXE）可接受，接收用非阻塞轮询避免卡死主循环
5. CR1 使能顺序：先 TE/RE，再 UE
6. HAL 版 `HAL_UART_Receive(..., 0)` 的 `HAL_TIMEOUT` 不是错误，是"没收到数据"

## 15. 阅读建议

先看寄存器版 `usart1_gpio_init()` 和 `usart1_init()`，理解三个时钟、两个引脚模式、BRR 计算和 CR1 使能顺序。

再看 `usart1_send_byte()` 和 `usart1_receive_byte_nonblocking()`，对比 TXE 阻塞等待和 RXNE 非阻塞检查的区别。

最后看 HAL 版，确认 `GPIO_MODE_AF_PP`、`BaudRate=115200`、`HAL_UART_Transmit`、`HAL_UART_Receive(..., 0)` 分别对应寄存器版的哪些操作。

## 16. 扩展练习

1. 把 BRR 改成 9600 对应的值，串口助手也改成 9600，观察是否正常
2. 把 PA9 配成普通推挽（CNF9=00），观察串口是否有输出
3. 把接收改成阻塞轮询（`while (!RXNE)`），观察主循环是否还能做其他事
4. 添加新命令：发送 `s` 后串口返回当前 ADC 采样值（结合第 18 课）
5. 把欢迎信息改成中文，观察串口助手是否需要设 UTF-8

## 17. 下一课预告

下一课：[22_uart_interrupt](../22_uart_interrupt/README.md)

把接收从轮询改成中断：RXNE 置位后触发 USART1 中断，CPU 不再主动轮询，而是被通知后处理数据。发送侧不变，接收效率大幅提升。
