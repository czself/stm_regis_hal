# 20_adc_dma - ADC + DMA 循环缓冲区

## 1. 本课到底在学什么

本课表面现象：PA1 接电位器，旋转时 `g_adc_buffer[0..15]` 持续刷新，主循环对 16 个值求平均后控制 PC13 LED。

真正要学的是 DMA 目标从单变量变成数组后的配置变化：

```text
上一课 18_dma_basic：
  ADC1->DR → DMA1_Channel1 → g_adc_value（单变量，MINC=0, CNDTR=1）

本课：
  ADC1->DR → DMA1_Channel1 → g_adc_buffer[0..15]（数组，MINC=1, CNDTR=16）
  CIRC=1 让 DMA 写完 16 个后回到数组开头继续覆盖
```

关键变化只有三个：`MINC` 从 0 变 1、`CNDTR` 从 1 变 16、`CMAR` 从变量地址变成数组首地址。其余配置（DIR、PINC、PSIZE/MSIZE、CIRC、CPAR、ADC 侧 CONT+DMA）和上一课完全相同。

本课还引入平均滤波：CPU 对 16 个采样值求平均，比单值更稳定。这是"DMA 负责采集、CPU 负责处理"的最小原型。

## 2. 本课学习目标

学完本课，你应该能做到：

- 解释为什么 DMA 目标从单变量变成数组后，`MINC` 必须从 0 改 1
- 说清楚 `MINC=1`、`CNDTR=16`、`CIRC=1` 三者怎样组成循环缓冲区
- 看懂 `g_adc_buffer` 为什么必须是 `volatile`
- 解释为什么 ADC 结果仍然来自 `ADC1->DR`，只是写入位置发生变化
- 把 HAL 版 `MemInc = DMA_MINC_ENABLE`、`HAL_ADC_Start_DMA(..., 16)` 对应回寄存器版
- 根据"数组只有第 0 个变化""LED 抖动""数值不更新"等现象定位错误层级

## 3. 本课目录结构

```text
20_adc_dma/
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
- PA1：模拟输入，接电位器中间端（两端接 3.3V/GND）
- PC13：板载 LED

PA1 输入电压应在 0~3.3V 范围内。调试器中可观察 `g_adc_buffer` 和平均值变化。

## 5. 先建立完整脑图

```text
1. 系统时钟 72MHz
2. PC13 推挽输出
3. PA1 模拟输入
4. 开 DMA1 时钟（AHBENR）
5. ★ DMA1_Channel1：CNDTR=16, MINC=1, CIRC=1, CMAR=g_adc_buffer
6. ADC1：CONT=1, DMA=1, 单通道 IN1, 校准
7. ★ 先使能 DMA → 再 SWSTART 启动 ADC
8. 主循环：求 16 个元素平均 → 比较阈值 → 控制 LED
```

第 5 步是本课全部新增变化（MINC/CNDTR/CMAR），其余和上一课相同。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在第 18/19 课已有完整解释，本课不再重复：

| 名词 | 一句话提醒 |
|------|-----------|
| `DMA1_Channel1` | ADC1 固定映射的 DMA 通道 |
| `CPAR = &ADC1->DR` | 外设地址，本课不变 |
| `CMAR` | 内存地址寄存器，本课指向数组首地址 |
| `CNDTR` | 传输数量，本课从 1 变 16 |
| `CCR` | 通道配置寄存器 |
| `DIR=0` | 外设→内存，本课不变 |
| `PINC=0` | 外设地址不自增，本课不变 |
| `CIRC=1` | 循环模式，上一课已开 |
| `PSIZE/MSIZE=16bit` | 半字搬运，本课不变 |
| `CR2.CONT` | ADC 连续转换 |
| `CR2.DMA` | ADC DMA 请求使能 |
| `TCIF / IFCR` | 传输完成标志 / 写 1 清标志 |
| `__HAL_LINKDMA` | HAL 句柄关联宏 |
| `volatile` | 防止编译器优化掉异步修改的变量 |

### 6.2 `DMA_CCR_MINC` 在本课的新用法

内存地址自增位，属于 DMA `CCR`。

**是什么**：打开后 DMA 每次传输后内存地址自动 +2（16 位宽度），指向下一个数组元素。

**干什么**：让 DMA 依次写入 `g_adc_buffer[0]`、`g_adc_buffer[1]`、...、`g_adc_buffer[15]`，而不是反复覆盖同一个位置。

**本课为什么需要**：上一课目标是单变量 `g_adc_value`，地址不需要移动，`MINC=0`。本课目标是数组 `g_adc_buffer[16]`，DMA 必须逐个元素写入，`MINC=1`。这是本课和上一课最核心的配置差异。

**配错会怎样**：不开 `MINC`，16 次搬运都覆盖 `g_adc_buffer[0]`，数组其他元素保持 0 或旧值，平均值严重偏低。

### 6.3 循环缓冲区

**是什么**：固定长度内存区域被 DMA 反复覆盖使用的方式。

**干什么**：DMA 依次写 `g_adc_buffer[0]` 到 `[15]`，写完后由 `CIRC=1` 自动回到 `[0]` 继续覆盖。数组始终保存最近 16 次采样。

**本课为什么需要**：ADC 连续转换不停产生数据，DMA 必须持续写数组。`CIRC=1` 让一轮结束后自动重载 `CNDTR` 并回到数组开头，否则数组只填一轮就停了。

**配错会怎样**：不开 `CIRC`，DMA 写完 16 个元素后停止，后续 ADC 转换结果没人搬，数组不再刷新。

注意：本课的循环缓冲区不是带读写指针的工程级环形队列。CPU 只是随时把 16 个元素求平均，不知道 DMA 当前写到第几个。入门实验可以接受；工程里要用半传输中断或双缓冲保证数据块完整。

### 6.4 `CNDTR = 16` 的含义

**是什么**：DMA 传输数量寄存器，本课写 16。

**干什么**：表示一轮 DMA 搬 16 个数据项（不是 16 字节）。配合 `CIRC=1`，搬完 16 个后自动重载。

**本课为什么需要**：数组有 16 个元素，DMA 一轮必须搬 16 次才能填满。上一课 `CNDTR=1` 只搬一个值。

**配错会怎样**：`CNDTR` 大于数组长度，DMA 越界写内存，破坏其他变量；`CNDTR` 小于数组长度，后部分元素永远不会被硬件写入。

`CNDTR` 必须在 DMA 通道关闭时配置。通道使能时改 `CNDTR`，行为不可预期。

### 6.5 `g_adc_buffer` 与 `volatile`

**是什么**：RAM 中的 ADC 采样数组，类型 `volatile uint16_t[16]`。

**干什么**：DMA 在后台持续写它，CPU 在主循环读取它求平均。它是"硬件采集"和"软件处理"的交界点。

**本课为什么需要**：DMA 硬件直接写 SRAM，CPU 没有执行任何"把 ADC 值放进数组"的语句。`volatile` 告诉编译器这个数组可能被当前代码看不见的硬件修改，每次读取都应该真的去内存取。

**配错会怎样**：去掉 `volatile`，高优化等级下编译器可能把数组值缓存在寄存器，CPU 读到旧值，平均值不反映最新采样。DMA 场景下比中断场景更严重，因为 DMA 写内存的频率更高。

### 6.6 平均滤波与 `uint32_t sum`

**是什么**：对 16 个 ADC 值求平均的软件算法。

**干什么**：降低单次采样噪声造成的 LED 抖动。

**本课为什么需要**：单值控制 LED 时，噪声可能导致阈值附近 LED 快速闪烁。平均值更稳定。

**配错会怎样**：用 `uint16_t` 求和，16 个 12 位最大值相加为 `16 × 4095 = 65520`，接近 `uint16_t` 上限 65535，可能溢出。用 `uint32_t` 更稳妥。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 已学步骤（快速过）

1. `system_clock_72mhz_init()` — HSE → PLL x9 → 72MHz
2. `led_pc13_init()` — PC13 推挽输出，初始高电平灭
3. `pa1_adc_input_init()` — PA1 模拟输入
4. `RCC->AHBENR |= DMA1EN` — 开 DMA1 时钟
5. `DMA1_Channel1->CCR &= ~EN` — 关通道进入可配置状态
6. `CPAR = &ADC1->DR` — 外设地址不变
7. ADC1 时钟/分频/规则组/采样时间/校准 — 同上一课
8. `CR2.CONT=1 + CR2.DMA=1` — 连续转换 + DMA 请求使能
9. 先 `CCR.EN=1` → 再 `SWSTART` — 启动顺序不变

### 7.2 新增：`CNDTR = ADC_BUFFER_SIZE`

```c
DMA1_Channel1->CNDTR = ADC_BUFFER_SIZE;  /* 16，不再是 1 */
```

上一课搬 1 个值到单变量，本课搬 16 个值到数组。DMA 内部计数器从 16 递减到 0，`CIRC=1` 时自动重载。

### 7.3 新增：`CMAR = g_adc_buffer`

```c
DMA1_Channel1->CMAR = (uint32_t)g_adc_buffer;  /* 数组首地址，不再是 &g_adc_value */
```

`g_adc_buffer` 在表达式中退化为数组首元素地址。DMA 从这个地址开始，配合 `MINC=1` 依次写后续元素。

### 7.4 新增：`CCR` 配置变化

```c
DMA1_Channel1->CCR &= ~(MEM2MEM | PL | MSIZE | PSIZE | MINC | PINC | CIRC | DIR);

DMA1_Channel1->CCR |= DMA_CCR_MINC;      /* ★ 内存地址自增（上一课没有） */
DMA1_Channel1->CCR |= DMA_CCR_PSIZE_0;   /* 外设 16 位（同上一课） */
DMA1_Channel1->CCR |= DMA_CCR_MSIZE_0;   /* 内存 16 位（同上一课） */
DMA1_Channel1->CCR |= DMA_CCR_CIRC;       /* 循环模式（同上一课） */
DMA1_Channel1->CCR |= DMA_CCR_PL_1;       /* 优先级高（同上一课） */
```

和上一课唯一区别：`MINC=1`。其余（DIR=0、PINC=0、PSIZE/MSIZE=16bit、CIRC=1、PL=高）完全相同。

本课 vs 上一课 DMA 配置对比：

| 配置项 | 上一课（单变量） | 本课（数组） |
|--------|-----------------|-------------|
| `CNDTR` | 1 | 16 |
| `CMAR` | `&g_adc_value` | `g_adc_buffer` |
| `MINC` | 0 | 1 |
| `CPAR` | `&ADC1->DR` | `&ADC1->DR`（不变） |
| `DIR/PINC/PSIZE/MSIZE/CIRC` | 同 | 同 |

### 7.5 主循环：求平均控制 LED

```c
avg_value = adc_buffer_average_get();

if (avg_value > 2048U) {
    GPIOC->BRR = GPIO_BRR_BR13;     /* LED 亮 */
} else {
    GPIOC->BSRR = GPIO_BSRR_BS13;   /* LED 灭 */
}
```

`adc_buffer_average_get()` 用 `uint32_t` 累加 16 个元素再除以 16。CPU 不参与每次搬运，只在需要时读取缓冲区并处理。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 已学步骤（快速过）

1. `HAL_Init()` + `system_clock_72mhz_init()` — SysTick + 72MHz
2. PC13 `GPIO_MODE_OUTPUT_PP`
3. PA1 `GPIO_MODE_ANALOG`
4. `__HAL_RCC_DMA1_CLK_ENABLE()`
5. `hdma_adc1.Instance = DMA1_Channel1`
6. `Direction = DMA_PERIPH_TO_MEMORY` → DIR=0
7. `PeriphInc = DMA_PINC_DISABLE` → PINC=0
8. `PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD` → PSIZE=16bit
9. `MemDataAlignment = DMA_MDATAALIGN_HALFWORD` → MSIZE=16bit
10. `Mode = DMA_CIRCULAR` → CIRC=1
11. `Priority = DMA_PRIORITY_HIGH` → PL=高
12. `HAL_DMA_Init()` + `__HAL_LINKDMA()`
13. `hadc1.Init`（单通道、连续转换、软件触发、右对齐）
14. `HAL_ADCEx_Calibration_Start()`
15. `sConfig`（Channel=1, Rank=1, SamplingTime=239.5）

### 8.2 新增：`MemInc = DMA_MINC_ENABLE`

```c
hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;   /* ★ 上一课是 DISABLE */
```

这是 HAL 版和上一课的唯一配置差异。目标是数组时必须启用；目标是单变量时应禁用。对应寄存器版 `CCR.MINC=1`。

### 8.3 新增：`HAL_ADC_Start_DMA` 长度参数

```c
HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_adc_buffer, ADC_BUFFER_SIZE);
```

和上一课的区别：目标地址从 `&g_adc_value` 变成 `g_adc_buffer`，长度从 1 变成 16。长度参数对应寄存器版 `CNDTR=16`。如果误写成 1，HAL 版退化成上一课单值刷新效果。

## 9. 两个版本怎么学

寄存器版抓住三个变化点：

```text
CNDTR: 1 → 16
CMAR: &g_adc_value → g_adc_buffer
MINC: 0 → 1
```

其余配置完全相同，不需要重新理解。

HAL 版抓住两个变化点：

```text
MemInc: DISABLE → ENABLE
Start_DMA 长度: 1 → 16
```

共同要点：**DMA 目标从单变量变成数组，核心就是打开 MINC 让地址自增、把 CNDTR 设成数组长度、把 CMAR 指向数组首地址。** 搬运路径不变（ADC1->DR → RAM），只是写入位置从固定一个点变成逐个前进。

## 10. 检验问题清单

### 10.1 本课和上一课 DMA 配置有哪些不同？

`CNDTR` 从 1 变 16，`CMAR` 从变量地址变成数组首地址，`MINC` 从 0 变 1。其余（DIR/PINC/PSIZE/MSIZE/CIRC/CPAR/ADC 侧）完全相同。

### 10.2 不开 `MINC` 会怎样？

16 次搬运都覆盖 `g_adc_buffer[0]`，数组其他元素保持 0 或旧值，平均值严重偏低。

### 10.3 不开 `CIRC` 会怎样？

DMA 写完 16 个元素后停止，后续 ADC 转换结果没人搬，数组不再刷新。电位器怎么转都不变。

### 10.4 `CNDTR` 大于数组长度会怎样？

DMA 越界写内存，破坏数组后面的变量，可能导致程序跑飞或数据异常。

### 10.5 `volatile` 去掉会怎样？

高优化等级下编译器可能把数组值缓存在寄存器，CPU 读到旧值，平均值不反映最新采样。

### 10.6 为什么用 `uint32_t` 求和？

16 个 12 位最大值相加为 `16 × 4095 = 65520`，接近 `uint16_t` 上限，可能溢出。`uint32_t` 更安全。

### 10.7 HAL 版 `MemInc = DMA_MINC_ENABLE` 对应寄存器版什么？

对应 `CCR.MINC = 1`。

### 10.8 `HAL_ADC_Start_DMA` 的长度参数对应寄存器版什么？

对应 `CNDTR = ADC_BUFFER_SIZE`。如果误写成 1，退化成上一课单值刷新。

## 11. 工程实现步骤

### 11.1 需求分析

让 ADC1 连续采样 PA1，DMA 自动把结果循环写入 16 元素数组，CPU 对数组求平均后控制 LED。要求 MINC 打开、CNDTR 等于数组长度、CIRC 打开让数组持续刷新。

### 11.2 硬件核查

电位器中间脚接 PA1，两端接 3.3V/GND，输入不超过 3.3V，共地。

### 11.3 寄存器路线

1. 时钟 72MHz、PC13 输出、PA1 模拟输入（同前课）
2. ADC1 时钟/分频/规则组/采样时间/校准（同前课）
3. `RCC->AHBENR |= DMA1EN`
4. 关 DMA 通道 → 配 `CNDTR=16`、`CPAR=&DR`、`CMAR=g_adc_buffer`
5. `CCR`：`MINC=1` + `PSIZE/MSIZE=16bit` + `CIRC=1` + `PL=高`
6. `CR2.CONT=1 + CR2.DMA=1`
7. 先 `CCR.EN=1` → 再 `SWSTART`

### 11.4 HAL 路线

1. HAL_Init + 时钟/PC13/PA1 ANALOG（同前课）
2. `__HAL_RCC_DMA1_CLK_ENABLE()`
3. `hdma_adc1.Init`：`MemInc=ENABLE`（关键变化）、其余同上一课
4. `HAL_DMA_Init()` + `__HAL_LINKDMA()`
5. `hadc1.Init.ContinuousConvMode = ENABLE`
6. `HAL_ADC_Start_DMA(&hadc1, g_adc_buffer, 16)`（长度从 1 变 16）

### 11.5 工程思维

本课展示了"DMA 负责采集、CPU 负责处理"的生产线模式。DMA 在后台持续把 ADC 数据排进数组，CPU 在前台对数组做软件处理（平均滤波）。很多数据采集系统（示波器、传感器 hub）都基于这个模式扩展：缓冲区更大、用半传输中断分块处理、双缓冲避免读写冲突。

### 11.6 常见工程陷阱

1. **MINC 没开** — 数组只有第 0 个元素被反复覆盖，其余为 0
2. **CIRC 没开** — 数组只刷新一轮就停
3. **CNDTR 和数组长度不匹配** — 越界写内存或部分元素不更新
4. **volatile 漏写** — 高优化下平均值不反映最新采样
5. **用 uint16_t 求和** — 16 个最大值累加可能溢出
6. **HAL 版 MemInc 仍用 DISABLE** — 复制上一课代码最容易漏改

## 12. 运行现象

电位器中间脚接 PA1，旋转电位器时：

- **电位器旋到 GND 端**：`g_adc_buffer` 各元素接近 0，平均值接近 0，PC13 LED **灭**
- **电位器旋到中间**：`g_adc_buffer` 各元素约 2048（~1.65V），平均值约 2048，PC13 LED **在此阈值切换**
- **电位器旋到 3.3V 端**：`g_adc_buffer` 各元素接近 4095，平均值接近 4095，PC13 LED **亮**

**和上一课的区别**：上一课 LED 在阈值附近可能因单次采样噪声快速闪烁；本课平均值更稳定，LED 切换更平滑。

**异常时**：
- 数组只有 `[0]` 变化，其余为 0：MINC 没开
- 数组变化一次后停：CIRC 没开
- 数组值随机异常：CMAR 地址写错或 PSIZE/MSIZE 不匹配
- LED 在阈值附近快速闪烁：可能 volatile 漏了或未用平均

## 13. 常见问题排查

### 13.1 数组只有第 0 个元素变化

`MINC` 没开。检查 CCR 是否包含 `DMA_CCR_MINC`（寄存器版）或 `MemInc = DMA_MINC_ENABLE`（HAL 版）。

### 13.2 数组变化一次后不再刷新

`CIRC` 没开。DMA 写完 16 个元素后停止。检查 CCR 是否包含 `DMA_CCR_CIRC`（寄存器版）或 `Mode = DMA_CIRCULAR`（HAL 版）。

### 13.3 数组值全部为 0

按层排查：DMA 时钟（AHBENR）→ CPAR 是否指向 &ADC1->DR → CCR.EN 是否使能 → CR2.DMA 是否设了 → SWSTART 是否启动。

### 13.4 LED 在阈值附近快速闪烁

单值噪声导致。确认是否用了 `adc_buffer_average_get()` 而不是直接读 `g_adc_buffer[0]`。如果用了平均仍闪烁，检查 `volatile` 是否漏写。

### 13.5 HAL 版 Start_DMA 返回错误

检查 `__HAL_LINKDMA` 是否写了，`hdma_adc1.Instance` 是否是 `DMA1_Channel1`。

## 14. 本课最核心的结论

1. DMA 目标从单变量变成数组，核心变化是 `MINC=1`、`CNDTR=数组长度`、`CMAR=数组首地址`
2. `MINC=1` 让 DMA 每次传输后内存地址自动前进到下一个数组元素
3. `CIRC=1` + `CNDTR=16` 组成循环缓冲区：写完 16 个后自动回到数组开头
4. `volatile` 在 DMA 场景下比中断场景更重要，因为 DMA 写内存频率更高
5. 平均滤波是"DMA 采集 + CPU 处理"的最简软件策略
6. 本课和上一课的 DMA 配置差异只有三处，其余完全相同

## 15. 阅读建议

先看寄存器版 `dma1_channel1_init()`，和上一课对比找三个变化点：`CNDTR`、`CMAR`、`MINC`。确认其余配置完全相同。

再看 `adc_buffer_average_get()`，理解为什么用 `uint32_t` 求和。

最后看 HAL 版，确认 `MemInc = DMA_MINC_ENABLE` 和 `Start_DMA` 长度参数是仅有的两个变化。

## 16. 扩展练习

1. 把 `MINC` 去掉，观察数组是否只有 `[0]` 变化
2. 把 `CIRC` 去掉，观察数组是否只刷新一轮
3. 把 `CNDTR` 改成 8，观察数组后半部分是否不更新
4. 把 `volatile` 去掉，开高优化，观察平均值是否不更新
5. 把缓冲区改成 32 个元素，修改 `ADC_BUFFER_SIZE` 和 `CNDTR`

## 17. 下一课预告

下一课：[21_uart_polling](../21_uart_polling/README.md)

从 ADC+DMA 转到 UART 轮询收发。USART1 不再只用 DMA 发送，而是通过轮询方式收发数据，理解串口通信的基本时序。