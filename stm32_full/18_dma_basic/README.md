# 18_dma_basic - ADC + DMA 单值采样

## 1. 本课到底在学什么

本课表面现象：PA1 接电位器，旋转时 `g_adc_value` 自动刷新，超过 2048 时 PC13 亮、低于时灭。主循环里看不到任何读 ADC 寄存器的代码。

真正要学的是 DMA 数据通路：

```text
PA1 模拟电压 → ADC1_IN1 → ADC1 连续转换 → DR 出新结果
→ ADC 发 DMA 请求 → DMA1_Channel1 自动读 DR → 写入 RAM g_adc_value
→ CPU 只读内存变量控制 LED
```

**核心变化：CPU 从"亲自搬数据"变成"数据已经被搬好了，直接用"。** 前几课 CPU 要么轮询 EOC，要么进中断读 DR；本课开始，DMA 在后台把 ADC 数据搬到内存，CPU 只消费内存里的数据。

## 2. 本课学习目标

1. 为什么 ADC1 在 F103 上固定用 `DMA1_Channel1`？
2. `CPAR`、`CMAR`、`CNDTR`、`CCR` 分别决定 DMA 的哪部分行为？
3. `CONT` 和 `DMA` 位为什么要同时出现？
4. 为什么 DMA 要先使能、ADC 再 SWSTART？
5. `CIRC=1` 不开会怎样？`MINC=1` 误开会怎样？
6. `__HAL_LINKDMA` 不写会怎样？
7. `volatile` 在这里为什么比中断课更重要？

## 3. 本课目录结构

```text
18_dma_basic/
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
- 电位器（推荐 10kΩ），接法：中间脚 → PA1，两端 → 3.3V/GND
- PC13 板载 LED

**PA1 输入不能超过 3.3V。外部模拟源必须共地。**

## 5. 先建立一个最基本的脑图

```text
1. 系统时钟 72MHz
2. PC13 推挽输出
3. PA1 模拟输入
4. ★ 开 DMA1 时钟（AHBENR，不是 APB2ENR）
5. ★ 配 DMA1_Channel1：CPAR=&ADC1->DR, CMAR=&g_adc_value, CNDTR=1
6. ★ CCR：CIRC=1, PSIZE=16bit, MSIZE=16bit, PINC=0, MINC=0
7. ADC1 时钟 PCLK2/6 = 12MHz，规则组通道 1，采样 239.5 cycles
8. ★ CR2.CONT=1（连续转换）+ CR2.DMA=1（允许 DMA 请求）
9. ADC 上电校准
10. ★ 先使能 DMA 通道（CCR.EN=1）→ 再 SWSTART 启动 ADC
11. 主循环只读 g_adc_value → 控制 LED
```

第 4~6 步是 DMA 配置，第 8 步是 ADC 侧的 DMA 请求使能，第 10 步是启动顺序。这三块是本课全部新增。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在第 15~17 课已详细讲过，本课不再重复：

| 名词 | 一句话提醒 |
|------|-----------|
| `ADC1_IN1 / PA1` | PA1 第二功能是 ADC1 通道 1，配模拟输入 |
| `ADCPRE = PCLK2/6` | 12MHz，不能超 14MHz |
| `SQR1.L=0, SQR3.SQ1=1` | 规则组只采 1 个通道，就是通道 1 |
| `SMPR2.SMP1=111` | 239.5 cycles 采样时间 |
| `ADON / RSTCAL / CAL` | 上电 → 复位校准 → 执行校准 |
| `EXTTRIG / SWSTART` | 允许触发 + 软件启动转换 |
| `EOC` | 转换完成标志 |
| `DR` | 数据寄存器，12 位结果 |
| `CONT` | 连续转换模式，CR2 bit 1 |
| `GPIO_MODE_ANALOG` | HAL 模拟输入模式 |
| `volatile` | 防止编译器优化掉异步修改的变量 |

本课新增重点在下面。

### 6.2 `DMA` 是什么

Direct Memory Access，直接存储器访问。它是芯片内部的硬件控制器，不属于某个单一外设。

DMA 控制"数据搬运行为"：从哪个地址读、写到哪个地址、搬多少个、地址是否自增、搬完停止还是循环。本课 DMA 只做一件事：把 `ADC1->DR` 的值搬到 `g_adc_value`。

配置错时常见现象：`g_adc_value` 一直为 0、保持旧值、或内存被写坏。

### 6.3 `DMA1_Channel1` 是什么

F103 的 DMA 请求和通道有**固定映射**。ADC1 的 DMA 请求硬连线接到 DMA1_Channel1，不是软件可选的。

误用其他通道，代码能编译，但 ADC 请求到不了那个通道，`g_adc_value` 不会更新。

### 6.4 `CPAR` 是什么

Channel Peripheral Address Register，外设地址寄存器。保存 DMA 读数据的源地址。

本课 `CPAR = (uint32_t)&ADC1->DR`，取的是 DR 的**地址**，不是 DR 的值。写错则 DMA 从错误地址取数，结果随机或不变化。

### 6.5 `CMAR` 是什么

Channel Memory Address Register，内存地址寄存器。保存 DMA 写数据的目标地址。

本课 `CMAR = (uint32_t)&g_adc_value`。写错则 ADC 数据被写到错误 RAM 位置，可能导致其他变量被改坏或程序跑飞。

### 6.6 `CNDTR` 是什么

Channel Number of Data Register，传输数量寄存器。决定一轮 DMA 搬多少个数据单元。

本课 `CNDTR = 1`，只搬 1 个半字。配合循环模式，每次 ADC 请求来时这个 1 被反复装载。设太大而内存没有对应空间会越界写内存。

### 6.7 `CCR` 是什么

Channel Configuration Register，通道配置寄存器（注意不是定时器的 CCR）。控制 DMA 主要行为：方向、循环模式、地址自增、数据宽度、优先级、使能。

本课关键设置：`CIRC=1`（循环）、`PSIZE=MSIZE=16bit`、`PINC=MINC=0`、`DIR=0`（外设→内存）、`PL=高`。

### 6.8 `CIRC`（循环模式）是什么

`CCR.CIRC` 位。`CIRC=1` 时，一轮传输完成后自动重载 `CNDTR` 继续响应下一次请求。

本课 ADC 连续转换，DMA 必须循环接收。不开 `CIRC`，`g_adc_value` 只更新第一次，之后电位器怎么转都不变了。这是 ADC+DMA 初学最容易误判成 ADC 没工作的点。

### 6.9 `PSIZE` / `MSIZE` 是什么

外设端/内存端数据宽度。ADC 结果 12 位存在 16 位 DR 里，所以两端都按半字（16bit）搬运。

设成 8 位会截断结果；设成 32 位会多读/多写相邻内存。两端宽度应保持一致。

### 6.10 `PINC` / `MINC` 是什么

外设地址/内存地址自增控制。本课源地址始终是 `ADC1->DR`，目标始终是 `g_adc_value`，所以两个都不开。

误开 `MINC`，DMA 第一次写 `g_adc_value`，后面写到后续地址，变量只变一次，还可能破坏别的内存。下一课多通道 DMA 会打开 `MINC` 写数组。

### 6.11 `CR2.DMA` 位是什么

ADC 的 DMA 请求使能位（CR2 bit 8）。`DMA=1` 后，ADC 每次转换完成会向 DMA 控制器发请求。

这是 ADC 和 DMA 之间的"握手开关"。不设它，DMA 通道配得再对也收不到 ADC 请求，`g_adc_value` 不更新。

### 6.12 `__HAL_LINKDMA` 是什么

HAL 的句柄关联宏。`__HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1)` 把 ADC 句柄的 `DMA_Handle` 成员指向 DMA 句柄。

不写这步，`HAL_ADC_Start_DMA()` 不知道用哪个 DMA 通道，会返回错误。

### 6.13 `HAL_ADC_Start_DMA` 是什么

HAL 启动 ADC+DMA 的函数。接收 ADC 句柄、目标内存地址、长度。内部做了：配 DMA 地址和计数 → 使能 DMA 通道 → 打开 ADC DMA 请求 → 启动 ADC 转换。

对应寄存器版的组合动作：CCR.EN + CR2.DMA + CR2.CONT + SWSTART。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

1. `system_clock_72mhz_init()` — HSE 8MHz → PLL x9 → 72MHz
2. `led_pc13_init()` — PC13 推挽输出，初始高电平灭
3. `pa1_adc_input_init()` — PA1 模拟输入
4. ADC1 时钟 PCLK2/6 = 12MHz，规则组通道 1，采样 239.5 cycles
5. ADC 上电校准（ADON → RSTCAL → CAL）

### 7.2 新增：开 DMA1 时钟

```c
RCC->AHBENR |= RCC_AHBENR_DMA1EN;
```

DMA1 挂 AHB 总线，使能位在 `AHBENR`，不是 GPIO/ADC 常用的 `APB2ENR`。漏掉这句，DMA 寄存器写了也不工作。

### 7.3 新增：配 DMA 前先关通道

```c
DMA1_Channel1->CCR &= ~DMA_CCR_EN;
```

通道使能时部分配置寄存器不能改。先清 EN 让通道停在可配置状态。

### 7.4 新增：配 DMA 五要素

```c
DMA1_Channel1->CNDTR = 1U;                              /* 搬 1 个 */
DMA1_Channel1->CPAR  = (uint32_t)&ADC1->DR;             /* 从哪读 */
DMA1_Channel1->CMAR  = (uint32_t)&g_adc_value;          /* 写到哪 */
```

三个地址/计数寄存器决定搬运路径。`CPAR` 取的是 DR 的地址不是值。

### 7.5 新增：配 CCR 行为

```c
DMA1_Channel1->CCR &= ~(DMA_CCR_MEM2MEM | DMA_CCR_PL |
    DMA_CCR_MSIZE | DMA_CCR_PSIZE | DMA_CCR_MINC |
    DMA_CCR_PINC | DMA_CCR_CIRC | DMA_CCR_DIR);

DMA1_Channel1->CCR |= DMA_CCR_CIRC;      /* 循环模式 */
DMA1_Channel1->CCR |= DMA_CCR_PSIZE_0;   /* 外设 16bit */
DMA1_Channel1->CCR |= DMA_CCR_MSIZE_0;   /* 内存 16bit */
DMA1_Channel1->CCR |= DMA_CCR_PL_1;      /* 优先级高 */
```

先清再设，防止残留配置干扰。`DIR=0`（默认）= 外设→内存。`CIRC=1` 让 DMA 持续接收。`PINC=MINC=0` 因为地址都固定。

### 7.6 新增：ADC 侧 CONT + DMA

```c
ADC1->CR2 |= ADC_CR2_CONT;    /* 连续转换 */
ADC1->CR2 |= ADC_CR2_DMA;     /* 允许 DMA 请求 */
```

`CONT` 让 ADC 持续产生数据，`DMA` 让每次结果产生后通知 DMA 搬运。一个负责"持续生产"，一个负责"请求搬运"，缺一个链路都不完整。

### 7.7 新增：启动顺序——先 DMA 后 ADC

```c
DMA1_Channel1->CCR |= DMA_CCR_EN;                        /* 先开 DMA */
ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_SWSTART;         /* 再启动 ADC */
```

先让 DMA 进入响应状态，再让 ADC 开始转换。顺序反了，第一笔 ADC 数据出来时 DMA 还没准备好，会丢失。

### 7.8 主循环只读内存变量

```c
if (g_adc_value > 2048U) {
    GPIOC->BRR = GPIO_BRR_BR13;     /* LED 亮 */
} else {
    GPIOC->BSRR = GPIO_BSRR_BS13;   /* LED 灭 */
}
```

没有 EOC 轮询，没有读 DR。CPU 只读 `g_adc_value`——DMA 已经在后台持续刷新它。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

1. `HAL_Init()` + 时钟 72MHz
2. PC13 `GPIO_MODE_OUTPUT_PP`
3. PA1 `GPIO_MODE_ANALOG`
4. `__HAL_RCC_ADC1_CLK_ENABLE` + `RCC_ADCPCLK2_DIV6`
5. `hadc1.Init`（单通道、连续转换、软件触发、右对齐）
6. `HAL_ADCEx_Calibration_Start()`
7. `sConfig`（Channel=1, Rank=1, SamplingTime=239.5）

### 8.2 新增：DMA 句柄配置

```c
hdma_adc1.Instance = DMA1_Channel1;                              /* 硬件通道 */
hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;                 /* → DIR=0 */
hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;                     /* → PINC=0 */
hdma_adc1.Init.MemInc = DMA_MINC_DISABLE;                        /* → MINC=0 */
hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;    /* → PSIZE=01 */
hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;       /* → MSIZE=01 */
hdma_adc1.Init.Mode = DMA_CIRCULAR;                              /* → CIRC=1 */
hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;                     /* → PL=10 */
```

每个字段对应 CCR 的一个位域。HAL 只是把寄存器位组合变成枚举，填错一样会写到底层寄存器。

### 8.3 新增：HAL_DMA_Init + LINKDMA

```c
HAL_DMA_Init(&hdma_adc1);                        /* 写 CCR */
__HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);   /* 关联句柄 */
```

`HAL_DMA_Init` 把上面字段写进 DMA 寄存器。`__HAL_LINKDMA` 让 ADC 句柄知道用哪个 DMA 通道。不关联则 `Start_DMA` 找不到通道。

### 8.4 新增：HAL_ADC_Start_DMA 一键启动

```c
HAL_ADC_Start_DMA(&hadc1, (uint32_t *)&g_adc_value, 1U);
```

这一句等于寄存器版的：配 DMA 地址/计数 → 使能 DMA 通道 → 打开 CR2.DMA → SWSTART。参数地址或长度错，搬运目标就错。

### 8.5 主循环只读内存变量

```c
if (g_adc_value > 2048U) {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);  /* 亮 */
} else {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);    /* 灭 */
}
```

和寄存器版一样，CPU 不碰任何 ADC 寄存器。

## 9. 两个版本怎么学

寄存器版抓住 DMA 五要素 + 启动顺序：

```text
CPAR=&ADC1->DR + CMAR=&g_adc_value + CNDTR=1 + CCR(CIRC|PSIZE|MSIZE) + 先DMA后ADC
```

HAL 版抓住三个关键动作：

```text
DMA_HandleTypeDef 配置 → __HAL_LINKDMA 关联 → HAL_ADC_Start_DMA 一键启动
```

共同要点：**ADC 负责"产生数据"，DMA 负责"搬数据"，CPU 负责"消费已经在内存里的数据"。三者各司其职。**

## 10. 检验问题清单

### 10.1 为什么 ADC1 固定用 DMA1_Channel1？

**答**：F103 芯片内部硬连线映射，不是软件可选的。查参考手册 DMA 请求映射表可确认。

### 10.2 `CIRC=1` 不开会怎样？

**答**：DMA 搬一次就停，`g_adc_value` 只更新第一次，之后电位器怎么转都不变。

### 10.3 `CR2.DMA` 位不设会怎样？

**答**：ADC 不会向 DMA 发请求，DMA 通道配得再对也收不到触发，`g_adc_value` 不更新。

### 10.4 启动顺序反了会怎样？

**答**：先 SWSTART 再开 DMA，第一笔 ADC 数据出来时 DMA 还没使能，会丢失。

### 10.5 误开 `MINC` 会怎样？

**答**：DMA 第一次写 `g_adc_value`，后续写到后续地址，变量只变一次，还可能破坏别的内存。

### 10.6 `__HAL_LINKDMA` 不写会怎样？

**答**：`HAL_ADC_Start_DMA()` 找不到 DMA 通道，返回错误。

### 10.7 `volatile` 去掉会怎样？

**答**：编译器可能把 `g_adc_value` 缓存在寄存器，主循环读不到 DMA 写的最新值。DMA 场景下比中断场景更严重，因为 DMA 写内存的频率更高。

### 10.8 `CPAR` 写成 `ADC1->DR` 的值而不是地址会怎样？

**答**：DMA 把那个值当地址去读，读到的数据完全随机，`g_adc_value` 不反映 ADC 结果。

## 11. 工程实现步骤

### 11.1 需求分析

让 ADC1 连续采样 PA1，DMA 自动把结果搬到内存变量，CPU 只读变量控制 LED。要求 DMA 通道映射正确、搬运路径正确、循环模式开启、ADC 侧 DMA 请求使能、启动顺序正确。

### 11.2 硬件核查

电位器中间脚接 PA1，两端接 3.3V/GND，输入不超过 3.3V，共地。

### 11.3 寄存器路线

1. 时钟 72MHz、PC13 输出、PA1 模拟输入（同前课）
2. ADC1 时钟/分频/规则组/采样时间/校准（同前课）
3. `RCC->AHBENR |= DMA1EN`（注意是 AHB 不是 APB2）
4. 关 DMA 通道 → 配 CNDTR/CPAR/CMAR/CCR
5. `CR2.CONT=1 + CR2.DMA=1`
6. 先 `CCR.EN=1` → 再 `SWSTART`

### 11.4 HAL 路线

1. HAL_Init + 时钟/PC13/PA1 ANALOG（同前课）
2. `__HAL_RCC_DMA1_CLK_ENABLE()`
3. `hdma_adc1.Init` 配方向/自增/宽度/循环/优先级
4. `HAL_DMA_Init()` + `__HAL_LINKDMA()`
5. `hadc1.Init.ContinuousConvMode = ENABLE`
6. `HAL_ADC_Start_DMA(&hadc1, &g_adc_value, 1)`

### 11.5 工程思维

DMA 的本质是"让硬件代替 CPU 做搬运"。ADC+DMA 是 STM32 中最经典的组合之一：ADC 不断产生数据，DMA 不断搬走，CPU 完全解放。后续多通道扫描+DMA 更是标配——CPU 手动读多个通道容易漏，DMA 按顺序搬到数组不会错。

### 11.6 常见工程陷阱

1. **DMA 时钟没开** — 写在 APB2ENR 而不是 AHBENR
2. **CIRC 没开** — 变量只更新一次
3. **CR2.DMA 没设** — ADC 不发请求
4. **启动顺序反** — 第一笔数据丢失
5. **MINC 误开** — 写坏相邻内存
6. **__HAL_LINKDMA 漏写** — Start_DMA 返回错误
7. **volatile 漏写** — 主循环读不到最新值

## 12. 运行现象

电位器中间脚接 PA1，旋转电位器时：

- **电位器旋到 GND 端**：`g_adc_value` 接近 0，PC13 LED **灭**
- **电位器旋到中间**：`g_adc_value` 约 2048（对应 ~1.65V），PC13 LED **在此阈值切换**
- **电位器旋到 3.3V 端**：`g_adc_value` 接近 4095，PC13 LED **亮**

**关键区别于前课**：主循环里没有任何 `while(!EOC)` 轮询、没有读 `ADC1->DR`、没有中断回调。CPU 只执行 `if (g_adc_value > 2048)` 这一行判断。用调试器观察 `g_adc_value`，旋转电位器时应看到 0~4095 平滑变化，更新频率约 40kHz（12MHz ADC 时钟 / 252 转换周期），远快于轮询或中断方式。

如果 `g_adc_value` 始终为 0：检查 DMA 时钟（AHBENR）、CPAR/CMAR 地址、CIRC 模式、CR2.DMA 位。如果只更新一次就不变了：CIRC 没开。

## 13. 常见问题排查

### 13.1 g_adc_value 始终为 0

按层排查：DMA 时钟开了没（AHBENR）→ CPAR 指向 &ADC1->DR 了没 → CCR.EN 使能了没 → CR2.DMA 设了没 → SWSTART 启动了没。

### 13.2 g_adc_value 只更新一次

CIRC 没开。DMA 搬一次就停了，后续 ADC 数据没人搬。

### 13.3 g_adc_value 值随机或异常

CPAR 写成了 DR 的值而不是地址，或 PSIZE/MSIZE 不匹配。检查 `CPAR = (uint32_t)&ADC1->DR` 是否取了地址。

### 13.4 HAL 版 Start_DMA 返回错误

检查 `__HAL_LINKDMA` 是否写了，`hdma_adc1.Instance` 是否是 `DMA1_Channel1`。

### 13.5 主循环读不到最新值

`volatile` 漏了。编译器优化后把变量缓存在寄存器，DMA 写了内存但 C 代码读的是旧值。

## 14. 本课结论

1. DMA 是硬件搬运工，把数据从外设寄存器搬到内存，CPU 不参与
2. F103 中 ADC1 固定映射到 DMA1_Channel1
3. DMA 五要素：CPAR（源）、CMAR（目标）、CNDTR（数量）、CCR（行为）、EN（使能）
4. `CIRC=1` 让 DMA 循环搬运，配合 ADC 连续转换持续工作
5. `CR2.DMA=1` 是 ADC 和 DMA 的握手开关，不设则 ADC 不发请求
6. 启动顺序：先使能 DMA → 再启动 ADC，否则第一笔数据丢失
7. HAL 的 `__HAL_LINKDMA` 关联句柄，`HAL_ADC_Start_DMA` 一键启动

## 15. 阅读建议

先看寄存器版 `dma1_channel1_init()`，理解 DMA 五要素怎么配。再看 `adc1_init()` 里的 `CONT+DMA` 两行，理解 ADC 侧怎么发请求。最后看 `adc1_dma_start()` 的启动顺序。

HAL 版重点看 `hdma_adc1.Init` 每个字段对应哪个 CCR 位，以及 `__HAL_LINKDMA` 为什么不是装饰代码。

## 16. 扩展练习

1. 把 `CIRC` 去掉，观察 `g_adc_value` 是否只更新一次
2. 误开 `MINC`，观察变量和相邻内存的变化
3. 把启动顺序反过来（先 SWSTART 再开 DMA），观察第一笔数据是否丢失
4. 把 `CPAR` 改成 `ADC1->DR` 的值而不是地址，观察结果
5. 思考：如果 DMA 搬到数组而不是单个变量，`CNDTR` 和 `MINC` 怎么改？

## 17. 下一课预告

上一课：[17_adc_multichannel_scan](../17_adc_multichannel_scan/README.md)

下一课：[19_dma_memory_uart_cases](../19_dma_memory_uart_cases/README.md)

下一课学习 DMA 的更多使用场景：内存到内存搬运、内存到 UART 发送等，扩展对 DMA 方向和触发源的理解。