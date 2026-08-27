# 19_dma_memory_uart_cases - DMA 内存拷贝与 USART1 发送

## 1. 本课到底在学什么

本课表面现象：程序每隔一段时间通过 USART1 发送字符串 `DMA UART demo\n`，同时 PC13 LED 翻转一次。

真正要学的是 DMA 的两个典型用法：

```text
用法 1：内存到内存
g_src[] -> DMA1_Channel1 -> g_dst[]

用法 2：内存到外设
g_dst[] -> DMA1_Channel4 -> USART1->DR -> PA9 TX 引脚
```

上一课 ADC+DMA 是"外设到内存"：ADC 产生数据，DMA 写入变量。本课换两个方向看 DMA：先让 DMA 在两块 RAM 之间拷贝，再让 DMA 把 RAM 中的字符串喂给 USART1 发送。

核心直觉：DMA 不是 ADC 专属工具，它是芯片内部的数据搬运控制器；不同外设请求、不同方向、不同通道，会形成不同数据路径。

本课还首次使用 USART1。USART1 在本课只做发送，不接收，配置较简单（波特率 + TE + UE + DMAT）。USART 的完整学习在后续课程。

## 2. 本课学习目标

学完本课，你应该能做到：

- 区分 DMA 的内存到内存传输和 USART 发送 DMA
- 解释为什么内存拷贝使用 `DMA1_Channel1`，而 USART1_TX 使用 `DMA1_Channel4`
- 说清楚 `MEM2MEM`、`DIR`、`MINC`、`PINC` 在两个案例中分别怎么配
- 解释 `USART_CR3_DMAT` 为什么是 USART1 发送 DMA 的关键开关
- 看懂 `TCIF1` 和 `TCIF4` 分别表示哪一路 DMA 完成
- 看懂 HAL 版中 `HAL_UART_Transmit_DMA()` 如何借助 `hdmatx` 句柄启动 DMA1_Channel4
- 根据串口无输出、只输出一次、输出乱码等现象排查对应层级

## 3. 本课目录结构

```text
19_dma_memory_uart_cases/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

寄存器版直接配置 DMA1_Channel1、DMA1_Channel4 和 USART1。HAL 版用 `memcpy()` 完成内存拷贝，用 `HAL_UART_Transmit_DMA()` 完成 USART1 DMA 发送。两个版本不是逐句完全相同，但学习目标相同：理解 DMA 搬运路径如何随使用场景变化。

## 4. 实验硬件

- STM32F103C8T6 BluePill
- ST-Link 下载器
- PA9：USART1_TX，接 USB-TTL 的 RX
- GND：开发板 GND 和 USB-TTL GND 必须共地
- PC13：板载 LED，每轮发送后翻转
- 串口参数：115200 波特率，8 数据位，无校验，1 停止位

本课只发送不接收，不需要连接 PA10。若串口助手无输出，先确认 USB-TTL 方向：开发板 PA9 要接到转换器 RX。

## 5. 先建立完整脑图

```text
1. 系统时钟 72MHz
2. PC13 推挽输出（心跳灯）
3. PA9 复用推挽输出（USART1_TX）
4. USART1：BRR=625, CR3.DMAT=1, CR1.TE+UE=1
5. 开 DMA1 时钟（AHBENR）
6. ★ DMA1_Channel1：MEM2MEM 拷贝 g_src → g_dst
7. ★ DMA1_Channel4：内存到外设 g_dst → USART1->DR
8. 主循环：先拷贝 → 再发送 → 翻转 LED → 延时
```

第 6 步是本课第一个新用法（内存到内存），第 7 步是第二个新用法（内存到外设）。两段 DMA 共用 DMA1，但通道、方向、触发方式完全不同。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在第 18 课已有完整解释，本课不再重复：

| 名词 | 一句话提醒 |
|------|-----------|
| `DMA1` | DMA 控制器，时钟在 AHBENR |
| `DMA1_Channel1` | 上一课用于 ADC1，本课用于 MEM2MEM |
| `CPAR` | 外设地址寄存器，MEM2MEM 中当源地址用 |
| `CMAR` | 内存地址寄存器 |
| `CNDTR` | 传输数量 |
| `CCR` | 通道配置寄存器（方向/自增/宽度/模式/使能） |
| `DIR` | 方向位：0=外设→内存，1=内存→外设 |
| `MINC` | 内存地址自增 |
| `TCIF / IFCR` | 传输完成标志 / 写 1 清标志 |
| `__HAL_LINKDMA` | HAL 句柄关联宏 |
| `HAL_DMA_Init` | HAL DMA 初始化 |

### 6.2 `DMA_CCR_MEM2MEM` 是什么

内存到内存模式位，属于 DMA `CCR`。

**是什么**：打开后 DMA 不依赖外设请求，直接在两块内存之间搬运。

**干什么**：让 DMA 以最快速度把源地址的数据复制到目标地址，不需要等任何外设触发。

**本课为什么需要**：第一段 DMA 要把 `g_src` 拷贝到 `g_dst`，两端都是 RAM，没有外设参与触发。不开 `MEM2MEM`，DMA1_Channel1 没有外设请求源驱动这次拷贝，`g_dst` 不会得到期望字符串。

**配错会怎样**：不开 `MEM2MEM`，内存拷贝不会执行；在 USART 发送通道误开 `MEM2MEM`，DMA 行为会偏离 USART 请求节奏，发送链路异常。

### 6.3 `DMA_CCR_PINC` 在本课的新用法

外设地址自增位，属于 DMA `CCR`。

**是什么**：打开后 DMA 每次传输后外设地址自动 +1/+2/+4（取决于数据宽度）。

**干什么**：让外设端地址逐个数据前进。

**本课为什么需要**：MEM2MEM 拷贝中，代码把 `CPAR` 当作源内存地址使用，所以打开 `PINC` 让源地址逐字节前进。USART1_TX 中，外设地址固定为 `USART1->DR`，所以不能打开 `PINC`。

**配错会怎样**：MEM2MEM 不开 `PINC`，源地址不前进，只拷贝第一个字节到目标的所有位置；USART 发送误开 `PINC`，DMA 会把后续字节写到 `USART1->DR` 后面的地址，串口输出异常甚至影响其他寄存器。

### 6.4 `DMA1_Channel4` 与 USART1_TX 映射

**是什么**：STM32F103 中 USART1_TX 的 DMA 请求固定映射到 DMA1_Channel4。

**干什么**：USART1 发送器每需要下一个字节，就通过 DMA 请求让 Channel4 把内存中的下一个字节写入 `USART1->DR`。

**本课为什么需要**：第二段 DMA 要把 `g_dst` 中的字符串发给 USART1，必须用 Channel4 才能收到 USART1 的 DMA 请求。

**配错会怎样**：用错通道，USART1 的 TX DMA 请求不会驱动该通道，串口助手通常看不到输出。

### 6.5 `USART_CR3_DMAT` 是什么

USART1 的 DMA 发送使能位，属于 `CR3` 寄存器。

**是什么**：控制 USART1 发送侧是否向 DMA 发请求。

**干什么**：打开后，USART1_TX 在需要数据时主动请求 DMA 写入 `DR`。

**本课为什么需要**：只有这个位打开，USART1_TX 才会在发送器空时请求 DMA1_Channel4 写 `DR`。没有它，DMA 通道配置好了、USART 也使能了，但发送 DMA 不会被触发，串口无输出。

**配错会怎样**：忘记设置 `DMAT`，串口无输出；这是 USART+DMA 最容易漏的一步。

### 6.6 `HAL_UART_Transmit_DMA()` 与 `__HAL_LINKDMA(&huart1, hdmatx, hdma_tx)`

**是什么**：`HAL_UART_Transmit_DMA()` 是 HAL 的 UART DMA 发送函数；`__HAL_LINKDMA` 是句柄关联宏。

**干什么**：`HAL_UART_Transmit_DMA()` 接收 UART 句柄、缓冲区地址和长度，内部配置 DMA1_Channel4 的地址和计数，打开 USART TX DMA 请求，并启动发送。`__HAL_LINKDMA` 把 `huart1.hdmatx` 指向 `hdma_tx`，让发送函数找到该用哪个 DMA 通道。

**本课为什么需要**：HAL 版通过这两步完成 USART1 DMA 发送。不关联句柄，`HAL_UART_Transmit_DMA()` 不知道用哪个 DMA 通道，会返回错误。

**配错会怎样**：缺少 `__HAL_LINKDMA`，发送函数返回 HAL_ERROR；HAL 版还需要 DMA 和 USART 中断完成收尾（`DMA1_Channel4_IRQHandler` + `USART1_IRQHandler`），少了它们，第一轮可能发出，但第二轮因 `huart1` 仍 BUSY 而启动失败。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 已学步骤（快速过）

1. `system_clock_72mhz_init()` — HSE → PLL x9 → 72MHz
2. `pc13_led_init()` — PC13 推挽输出，初始高电平灭
3. 开 GPIOA + AFIO 时钟，PA9 配成复用推挽输出（`CRH` 中 MODE9=10, CNF9=10）
4. 开 DMA1 时钟（`RCC->AHBENR |= DMA1EN`）

### 7.2 新增：USART1 初始化

```c
RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
USART1->BRR = 72000000U / 115200U;
USART1->CR3 = USART_CR3_DMAT;
USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
```

三步缺一不可：

- `BRR`：波特率。PCLK2=72MHz，115200 波特率，`BRR ≈ 625`。配错则串口乱码或无输出。
- `CR3.DMAT`：USART 侧的 DMA 请求开关。没有它，USART 不会向 DMA 要数据。
- `CR1.TE + CR1.UE`：打开发送器 + 使能 USART。不使能则整个 USART 不工作。

### 7.3 新增：`dma_mem_copy()` — MEM2MEM 拷贝

```c
DMA1_Channel1->CCR = 0U;                          /* 先关通道 */
DMA1->IFCR = DMA_IFCR_CTCIF1;                     /* 清残留标志 */

DMA1_Channel1->CPAR  = (uint32_t)g_src;           /* MEM2MEM 中 CPAR=源地址 */
DMA1_Channel1->CMAR  = (uint32_t)g_dst;           /* CMAR=目标地址 */
DMA1_Channel1->CNDTR = sizeof(g_src);             /* 搬 16 字节 */

DMA1_Channel1->CCR = DMA_CCR_MINC   |             /* 内存地址自增 */
                      DMA_CCR_PINC   |             /* 源地址自增（CPAR 当源用） */
                      DMA_CCR_DIR    |             /* DIR=1：从 CPAR 到 CMAR */
                      DMA_CCR_MEM2MEM |            /* 内存到内存模式 */
                      DMA_CCR_PL_0   |             /* 优先级中 */
                      DMA_CCR_EN;                  /* 使能 */
```

MEM2MEM 中 `CPAR` 和 `CMAR` 的角色与上一课不同：上一课 `CPAR` 是外设地址（ADC1->DR），本课 `CPAR` 是源内存地址（g_src）。`DIR=1` 配合 `MEM2MEM` 表示从 `CPAR` 指向的位置搬到 `CMAR` 指向的位置。

`PINC=1` 是本课的特殊用法：因为 `CPAR` 当源地址用，源地址也要逐字节前进，所以打开 `PINC`。上一课 ADC+DMA 中 `PINC=0`，因为外设地址（DR）固定。

```c
while ((DMA1->ISR & DMA_ISR_TCIF1) == 0U) {}     /* 等传输完成 */
DMA1->IFCR = DMA_IFCR_CTCIF1;                     /* 清标志 */
```

等 Channel1 传输完成后再继续，确保 `g_dst` 内容正确后才交给 USART 发送。

### 7.4 新增：`dma_uart_send()` — USART TX DMA

```c
DMA1_Channel4->CCR = 0U;                          /* 先关通道 */
DMA1->IFCR = DMA_IFCR_CTCIF4;                     /* 清残留标志 */

DMA1_Channel4->CPAR  = (uint32_t)&USART1->DR;     /* 外设端：DR 地址固定 */
DMA1_Channel4->CMAR  = (uint32_t)g_dst;           /* 内存端：g_dst */
DMA1_Channel4->CNDTR = sizeof(g_dst);             /* 搬 16 字节 */

DMA1_Channel4->CCR = DMA_CCR_MINC |               /* 内存地址自增 */
                      DMA_CCR_DIR  |               /* DIR=1：内存到外设 */
                      DMA_CCR_PL_0 |               /* 优先级中 */
                      DMA_CCR_EN;                  /* 使能 */
```

和 MEM2MEM 的关键区别：

| 配置项 | MEM2MEM (Channel1) | USART TX (Channel4) |
|--------|-------------------|---------------------|
| `MEM2MEM` | 开 | 关 |
| `PINC` | 开（CPAR 当源用） | 关（DR 地址固定） |
| `MINC` | 开 | 开 |
| `DIR` | 1（CPAR→CMAR） | 1（CMAR→CPAR） |
| 触发方式 | 不等外设请求 | USART1_TX 请求驱动 |

没有 `MEM2MEM`，发送节奏由 USART 外设请求控制；没有 `PINC`，每个字节都写同一个 `USART1->DR`。

```c
while ((DMA1->ISR & DMA_ISR_TCIF4) == 0U) {}     /* 等传输完成 */
DMA1->IFCR = DMA_IFCR_CTCIF4;                     /* 清标志 */
```

DMA 完成表示 16 个字节已经交给 USART 数据寄存器，不一定等同于最后一个停止位已从引脚发完，但对本课的周期发送和延时来说足够。

### 7.5 主循环

```c
while (1) {
    dma_mem_copy();
    dma_uart_send();
    pc13_toggle();
    delay_cycles(7200000U);
}
```

每轮先拷贝再发送，确保串口发送的数据来自本轮刚拷贝好的 `g_dst`。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 已学步骤（快速过）

1. `HAL_Init()` + `system_clock_72mhz_init()` — SysTick + 72MHz
2. PC13 `GPIO_MODE_OUTPUT_PP`
3. `__HAL_RCC_GPIOA_CLK_ENABLE()`，PA9 `GPIO_MODE_AF_PP`
4. `__HAL_RCC_DMA1_CLK_ENABLE()`

### 8.2 新增：UART 句柄配置

```c
huart1.Instance = USART1;
huart1.Init.BaudRate = 115200;
huart1.Init.WordLength = UART_WORDLENGTH_8B;
huart1.Init.StopBits = UART_STOPBITS_1;
huart1.Init.Parity = UART_PARITY_NONE;
huart1.Init.Mode = UART_MODE_TX;
huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
huart1.Init.OverSampling = UART_OVERSAMPLING_16;
HAL_UART_Init(&huart1);
```

这些字段最终让 `HAL_UART_Init()` 写 USART 的 `BRR/CR1/CR2/CR3` 等寄存器。`Mode = UART_MODE_TX` 对应 `CR1.TE=1`。

### 8.3 新增：DMA TX 句柄配置

```c
hdma_tx.Instance = DMA1_Channel4;
hdma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;        /* → DIR=1 */
hdma_tx.Init.PeriphInc = DMA_PINC_DISABLE;             /* → PINC=0 */
hdma_tx.Init.MemInc = DMA_MINC_ENABLE;                 /* → MINC=1 */
hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;  /* → PSIZE=8bit */
hdma_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;      /* → MSIZE=8bit */
hdma_tx.Init.Mode = DMA_NORMAL;                        /* 非循环 */
hdma_tx.Init.Priority = DMA_PRIORITY_LOW;              /* → PL=00 */
HAL_DMA_Init(&hdma_tx);
```

每个字段对应 CCR 的一个位域。`DMA_MEMORY_TO_PERIPH` 对应 `DIR=1`；`DMA_PINC_DISABLE` 因为 DR 地址固定；`DMA_MINC_ENABLE` 因为要依次读取 `g_dst` 的每个字节。

### 8.4 新增：`__HAL_LINKDMA` 关联句柄

```c
__HAL_LINKDMA(&huart1, hdmatx, hdma_tx);
```

把 `huart1.hdmatx` 指向 `hdma_tx`。之后 `HAL_UART_Transmit_DMA()` 通过 `huart1.hdmatx` 找到 DMA1_Channel4。不写这句，发送函数返回错误。

### 8.5 新增：DMA/USART 中断收尾

```c
HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 1, 0);
HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
HAL_NVIC_SetPriority(USART1_IRQn, 1, 1);
HAL_NVIC_EnableIRQ(USART1_IRQn);
```

```c
void DMA1_Channel4_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_tx); }
void USART1_IRQHandler(void)        { HAL_UART_IRQHandler(&huart1); }
```

这不是为了让字节开始发送，而是为了让 HAL 在 DMA 搬运完成、USART 最后一位真正发完后更新内部状态。少了它们，第一轮可能发出，但第二轮因 `huart1` 仍 BUSY 而启动失败。

### 8.6 新增：`memcpy()` 与 `HAL_UART_Transmit_DMA()`

```c
memcpy(g_dst, g_src, sizeof(g_dst));
HAL_UART_Transmit_DMA(&huart1, g_dst, sizeof(g_dst));
```

HAL 版用 `memcpy()` 完成内存拷贝，不是 DMA MEM2MEM。这说明 HAL 版重点放在 USART DMA 发送上。数据路径仍然是 `g_src → g_dst → USART1`，但第一段由 CPU/库函数完成，第二段由 DMA 完成。

`HAL_UART_Transmit_DMA()` 内部会设置 DMA 源地址为 `g_dst`，目标外设为 `USART1->DR`，长度为 16，并打开 USART TX DMA 请求。

## 9. 两个版本怎么学

寄存器版抓住两段 DMA 的配置差异：

```text
MEM2MEM：CPAR=源, CMAR=目标, PINC+MINC, MEM2MEM=1, 不等外设请求
USART TX：CPAR=&DR, CMAR=g_dst, MINC only, MEM2MEM=0, USART 请求驱动
```

HAL 版抓住三个关键动作：

```text
DMA_HandleTypeDef 配置 → __HAL_LINKDMA 关联 → HAL_UART_Transmit_DMA 启动
```

共同要点：**DMA 的搬运路径由 CPAR/CMAR/DIR/MEM2MEM 决定，触发方式由 MEM2MEM 和外设请求决定。** 同一个 DMA 控制器，不同配置形成完全不同的数据路径。

## 10. 检验问题清单

### 10.1 为什么内存拷贝用 Channel1，USART 发送用 Channel4？

Channel4 是 USART1_TX 的固定 DMA 请求映射。MEM2MEM 不依赖外设请求，理论上任何空闲通道都行，本课选 Channel1 是因为本课没有 ADC。

### 10.2 MEM2MEM 拷贝中 `PINC` 为什么打开？

MEM2MEM 中 `CPAR` 当源地址用，源地址要逐字节前进，所以 `PINC=1`。USART TX 中 `CPAR` 是 `USART1->DR`，地址固定，所以 `PINC=0`。

### 10.3 忘记设置 `CR3.DMAT` 会怎样？

DMA 通道配置好了，USART 也使能了，但 USART 不会向 DMA 发请求。串口无输出。这是 USART+DMA 最容易漏的一步。

### 10.4 USART 发送通道误开 `MEM2MEM` 会怎样？

DMA 不等 USART 请求就一口气把数据写完，发送节奏偏离 USART 位时间，串口输出乱码或异常。

### 10.5 USART 发送通道不开 `MINC` 会怎样？

DMA 一直读 `g_dst[0]`，串口重复输出第一个字符（'D'），而不是完整字符串。

### 10.6 HAL 版 `__HAL_LINKDMA` 不写会怎样？

`HAL_UART_Transmit_DMA()` 找不到 DMA 通道，返回 `HAL_ERROR`。

### 10.7 HAL 版缺少 DMA/USART 中断处理会怎样？

第一轮可能发出，但第二轮因 `huart1` 仍处于 `HAL_UART_STATE_BUSY_TX` 而启动失败。

### 10.8 `HAL_UART_Transmit_DMA()` 对应寄存器版哪些操作？

对应：配 DMA1_Channel4 的 CPAR/CMAR/CNDTR/CCR → 使能 DMA 通道 → 打开 USART CR3.DMAT。本质上是把 `dma_uart_send()` 的配置和启动封装成一个函数调用。

## 11. 工程实现步骤

### 11.1 需求分析

用 DMA 完成两个任务：RAM 到 RAM 拷贝、RAM 到 USART1 发送。要求通道映射正确、方向和自增配置匹配场景、USART 侧 DMAT 使能、MEM2MEM 只在内存拷贝时打开。

### 11.2 硬件核查

PA9 接 USB-TTL RX，GND 共地。串口助手设 115200/8N1。确认 USB-TTL 方向：PA9 是 TX，要接转换器 RX。

### 11.3 寄存器路线

1. 时钟 72MHz、PC13 输出（同前课）
2. PA9 复用推挽输出
3. USART1：BRR + CR3.DMAT + CR1.TE+UE
4. `RCC->AHBENR |= DMA1EN`
5. Channel1 MEM2MEM：CPAR=g_src, CMAR=g_dst, CNDTR=16, PINC+MINC+DIR+MEM2MEM+EN
6. 等 TCIF1，清 IFCR
7. Channel4 USART TX：CPAR=&DR, CMAR=g_dst, CNDTR=16, MINC+DIR+EN
8. 等 TCIF4，清 IFCR

### 11.4 HAL 路线

1. HAL_Init + 时钟/PC13/PA9 AF_PP
2. `__HAL_RCC_DMA1_CLK_ENABLE()`
3. `huart1.Init` 配波特率/8N1/TX only
4. `hdma_tx.Init` 配方向/自增/宽度/模式/优先级
5. `HAL_DMA_Init()` + `__HAL_LINKDMA()`
6. 使能 DMA1_Channel4 和 USART1 中断
7. 主循环：`memcpy()` + `HAL_UART_Transmit_DMA()`

### 11.5 工程思维

DMA 的本质是"让硬件代替 CPU 做搬运"。上一课是外设→内存，本课扩展到内存→内存和内存→外设。三个方向的核心区别在于：谁触发（外设请求还是 MEM2MEM）、地址怎么走（自增还是固定）、CPAR/CMAR 谁是源谁是目标。理解了这三个变量，任何 DMA 场景都能配。

### 11.6 常见工程陷阱

1. **CR3.DMAT 漏设** — USART 不发 DMA 请求，串口无输出
2. **USART TX 通道误开 MEM2MEM** — 发送节奏异常
3. **USART TX 通道误开 PINC** — DMA 写到 DR 后面的地址
4. **USART TX 不开 MINC** — 重复发送第一个字节
5. **HAL 版缺少 DMA/USART 中断** — 第二轮发送失败
6. **HAL 版 __HAL_LINKDMA 漏写** — Transmit_DMA 返回错误
7. **PA9 接到 USB-TTL TX 而非 RX** — 方向反了，串口无输出

## 12. 运行现象

串口助手（115200/8N1）每隔约 1 秒收到一行 `DMA UART demo`（末尾有换行），PC13 LED 每轮发送后翻转一次（亮约 100ms，灭约 900ms）。

**正常时**：串口助手持续显示 `DMA UART demo`，每行间隔约 1 秒；PC13 LED 周期性闪烁。

**异常时**：
- 串口完全无输出：查 PA9 接线方向、USART1 时钟、CR3.DMAT、DMA 通道映射
- 只输出一次就停：HAL 版查 DMA/USART 中断是否使能
- 重复输出 'D' 字符：MINC 没开
- 输出乱码：波特率配置错或 MEM2MEM 误开

## 13. 常见问题排查

### 13.1 串口完全无输出

按层排查：PA9 接线方向（TX→RX）→ USART1 时钟 → BRR 波特率 → CR1.TE+UE → CR3.DMAT → DMA1 时钟 → Channel4 映射 → CCR.EN。

### 13.2 串口只输出一次

寄存器版：检查主循环是否持续调用 `dma_uart_send()`。HAL 版：检查 `DMA1_Channel4_IRQHandler` 和 `USART1_IRQHandler` 是否实现，否则 `huart1` 状态不会恢复 READY。

### 13.3 串口重复输出第一个字符

`MINC` 没开，DMA 一直读 `g_dst[0]`。检查 Channel4 的 CCR 是否包含 `DMA_CCR_MINC`。

### 13.4 串口输出乱码

波特率配置错（BRR 计算和实际 PCLK2 不匹配），或 USART TX 通道误开 `MEM2MEM`。先确认系统时钟确实是 72MHz，再检查 BRR 值。

### 13.5 PC13 不闪

程序没跑起来或卡在某个 `while` 等待中。检查 DMA 通道是否正确使能，TCIF 标志是否能正常置位。

## 14. 本课最核心的结论

1. DMA 有三个方向：外设→内存（上一课）、内存→内存（本课 MEM2MEM）、内存→外设（本课 USART TX）
2. `MEM2MEM` 让 DMA 不依赖外设请求，直接在 RAM 之间搬运
3. `CPAR/CMAR` 的角色随方向变化：MEM2MEM 中 CPAR 是源、CMAR 是目标；USART TX 中 CPAR 是外设地址、CMAR 是内存源
4. `PINC` 在 MEM2MEM 中打开（源地址要前进），在 USART TX 中关闭（DR 地址固定）
5. `CR3.DMAT` 是 USART 侧的 DMA 请求开关，不设则 USART 不发请求
6. DMA 通道映射是硬件固定的：USART1_TX 必须用 DMA1_Channel4
7. HAL 版需要 DMA/USART 中断完成状态收尾，否则第二轮发送失败

## 15. 阅读建议

先看寄存器版 `dma_mem_copy()` 和 `dma_uart_send()`，对比两段 DMA 的 CCR 配置差异。重点理解：MEM2MEM 开不开、PINC 开不开、CPAR/CMAR 谁是源谁是目标。

再看 HAL 版，把 `hdma_tx.Init` 每个字段翻译回 CCR 位，确认 `__HAL_LINKDMA` 不是装饰代码。

最后对比 `memcpy()` 和 DMA MEM2MEM：前者是 CPU 搬，后者是硬件搬，功能结果一样但底层实现不同。

## 16. 扩展练习

1. 注释掉 `CR3.DMAT`，观察串口是否无输出
2. USART TX 通道不开 `MINC`，观察串口是否重复输出 'D'
3. 把 `dma_mem_copy()` 改成 `memcpy()`，观察功能是否不变
4. 把 Channel4 误配成 Channel2，观察串口是否无输出
5. HAL 版注释掉 `DMA1_Channel4_IRQHandler`，观察第二轮发送是否失败

## 17. 下一课预告

下一课：[20_adc_dma](../20_adc_dma/README.md)

回到 ADC+DMA，但这次是多通道扫描 + DMA 循环搬运到数组。DMA 方向仍是外设→内存，但 `MINC` 要打开，`CNDTR` 要等于通道数，`CIRC` 要打开让数组持续刷新。