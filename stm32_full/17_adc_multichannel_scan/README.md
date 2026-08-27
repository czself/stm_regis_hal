# 17_adc_multichannel_scan - ADC 多通道扫描

## 1. 本课到底在学什么

本课表面现象：两个电位器分别接 PA0 和 PA1，调试器里 `g_adc0` 跟 PA0 电压走、`g_adc1` 跟 PA1 电压走；当 PA0 电压高于 PA1 时 PC13 翻转。

真正要学的是 ADC 多通道扫描。前面两课只采一个通道，规则组里只有一个成员；本课打开 `SCAN`，把规则组长度设为 2，第 1 次转换采通道 0（PA0），第 2 次转换采通道 1（PA1）。软件读 `DR` 的顺序必须和规则组顺序一致。

**核心变化：规则组从"1个位置"变成"2个位置"，ADC 按队列依次转换，结果从同一个 DR 依次出来。** 这是 DMA 多通道的前置知识——下一课 DMA 会自动把 DR 依次搬到数组，不用 CPU 手动读。

## 2. 本课学习目标

1. `SCAN` 打开后 ADC 行为有什么变化？
2. `SQR1.L=1` 为什么表示 2 个转换？
3. `SQ1=0, SQ2=1` 怎么决定 PA0/PA1 的采样顺序？
4. 第一次读 DR 为什么是通道 0？第二次为什么是通道 1？
5. rank 配错后变量和引脚为什么对不上？
6. 连续转换模式（`CONT`）在这里的作用？
7. 为什么多通道扫描适合配合 DMA？

## 3. 本课目录结构

```text
17_adc_multichannel_scan/
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
- **两个电位器**（推荐 10kΩ），接法：
  - 电位器 1：中间脚 → PA0，两端 → 3.3V/GND
  - 电位器 2：中间脚 → PA1，两端 → 3.3V/GND
- PC13 板载 LED

**PA0/PA1 输入都不能超过 3.3V。外部模拟源必须共地。**

## 5. 先建立一个最基本的脑图

```text
1. 系统时钟 72MHz
2. PC13 推挽输出
3. PA0/PA1 都配成模拟输入
4. ADC1 时钟 PCLK2/6 = 12MHz
5. ★ CR1.SCAN = 1（扫描模式）
6. ★ CR2.CONT = 1（连续转换）
7. ★ SQR1.L = 1（2 个转换）
8. ★ SQR3: SQ1=0(先采PA0), SQ2=1(再采PA1)
9. SMPR2: CH0/CH1 采样时间
10. 校准 → SWSTART 启动
11. 第一次等 EOC → 读 DR → g_adc0（PA0 结果）
12. 第二次等 EOC → 读 DR → g_adc1（PA1 结果）
13. CONT=1 → ADC 自动开始下一轮扫描
```

第 5~8 步是本课新增。第 11~12 步是"读 DR 顺序必须和规则组顺序一致"的关键。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在第 15~16 课已详细讲过，本课不再重复：

| 名词 | 一句话提醒 |
|------|-----------|
| `ADC1_IN0 / PA0` | PA0 第二功能是 ADC1 通道 0，配模拟输入 |
| `ADC1_IN1 / PA1` | PA1 第二功能是 ADC1 通道 1，配模拟输入 |
| `ADCPRE = PCLK2/6` | 12MHz，不能超 14MHz |
| `ADON / RSTCAL / CAL` | 上电 → 复位校准 → 执行校准 |
| `EXTTRIG / SWSTART` | 允许触发 + 软件启动转换 |
| `EOC` | 转换完成标志，每完成一个通道置位一次 |
| `DR` | 数据寄存器，12 位结果，读 DR 清 EOC |
| `SMPR2` | 通道 0~9 采样时间寄存器 |
| `SQR1.L` | 规则组长度，写"转换数-1" |
| `SQR3.SQ1` | 规则组第 1 个转换通道号 |
| `GPIO_MODE_ANALOG` | HAL 模拟输入模式 |
| `HAL_ADC_Start()` | 启动 ADC 转换 |
| `HAL_ADC_PollForConversion()` | 等待 EOC |
| `HAL_ADC_GetValue()` | 读 DR |

本课新增重点在下面。

### 6.2 `SCAN`（扫描模式）是什么

位于 `ADC1->CR1`。单通道时 ADC 只转 SQ1 那一个通道就停；`SCAN=1` 后 ADC 按 SQ1→SQ2→...→SQ(L+1) 依次转换所有位置，每个位置完成都会置一次 `EOC`。

不开 `SCAN`，ADC 不会按多个 rank 扫描，第二个通道读数不可靠。

### 6.3 `CONT`（连续转换模式）是什么

位于 `ADC1->CR2`。`CONT=1` 时，一轮规则组扫描完成后 ADC 自动开始下一轮，不需要软件重新启动。

本课用 `CONT=1` + `SCAN=1`，效果是 ADC 持续循环"采 PA0 → 采 PA1 → 采 PA0 → ..."。

如果 `CONT=0`，一轮扫描完就停，需要软件再写 `SWSTART`。

### 6.4 `SQR3.SQ2` 是什么

规则组第 2 个转换位置。本课 `SQ2=1` 表示第 2 次采通道 1（PA1）。

`SQ1` 和 `SQ2` 一起决定了扫描顺序：先 PA0 再 PA1。配反了变量就和引脚对不上。

### 6.5 `Rank` 是什么

HAL 对规则组序列位置的称呼。`ADC_REGULAR_RANK_1` = 第 1 个转换位置（对应 SQ1），`ADC_REGULAR_RANK_2` = 第 2 个转换位置（对应 SQ2）。

HAL 版用两次 `HAL_ADC_ConfigChannel()` 分别配 rank 1 和 rank 2。rank 配错时，变量读到的通道顺序会错。

### 6.6 `NbrOfConversion` 是什么

HAL 的规则组转换数量字段。本课设为 2，对应寄存器版 `SQR1.L=1`（2-1=1）。

如果仍是 1，HAL 只按一个 rank 扫描，第二个通道不会采。

### 6.7 多通道扫描时 DR 怎么工作

ADC 只有一个 `DR`。扫描模式下，每完成一个通道转换，结果就放入 `DR` 并置 `EOC`。软件必须在下一个通道转换完成前读走 `DR`，否则会被覆盖。

这就是为什么多通道扫描适合 DMA——DMA 能自动把每个 DR 值搬到数组对应位置，不会漏。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

1. `system_clock_72mhz_init()` — HSE 8MHz → PLL x9 → 72MHz
2. `pc13_led_init()` — PC13 推挽输出，初始高电平灭
3. 开 GPIOA + ADC1 时钟
4. PA0/PA1 模拟输入（清 MODE0/CNF0/MODE1/CNF1）
5. ADC 时钟 PCLK2/6 = 12MHz
6. 校准（RSTCAL → CAL）

### 7.2 新增：打开扫描和连续转换

```c
ADC1->CR1 = ADC_CR1_SCAN;
ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_CONT;
ADC1->CR2 |= ADC_CR2_EXTTRIG;
```

`SCAN=1` 让 ADC 按规则组序列依次转换多个通道。`CONT=1` 让一轮完成后自动开始下一轮。`EXTTRIG` 允许软件触发。

### 7.3 新增：配置采样时间

```c
ADC1->SMPR2 = ADC_SMPR2_SMP0 | ADC_SMPR2_SMP1;
```

通道 0 和 1 都设最长采样时间（239.5 cycles），适合电位器。

### 7.4 新增：规则组长度和顺序

```c
ADC1->SQR1 = ADC_SQR1_L_0;                    /* L=1 → 2个转换 */
ADC1->SQR3 = (0U << 0) | (1U << 5);           /* SQ1=0, SQ2=1 */
```

`L=1` 表示 2 个转换（写的是转换数减 1）。`SQ1=0` 先采 PA0，`SQ2=1` 再采 PA1。

### 7.5 启动扫描

```c
ADC1->CR2 |= ADC_CR2_SWSTART;
```

因为 `CONT=1`，启动一次后 ADC 会持续循环扫描。

### 7.6 主循环：两次 EOC 两次 DR

```c
g_adc0 = adc_wait_and_read();    /* 第1次EOC → DR → PA0结果 */
g_adc1 = adc_wait_and_read();    /* 第2次EOC → DR → PA1结果 */

if (g_adc0 > g_adc1) {
    pc13_toggle();
}
delay_cycles(720000U);
```

读取顺序必须和规则组顺序一致。第一次读到的 DR 是 SQ1（通道 0/PA0），第二次是 SQ2（通道 1/PA1）。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

1. `HAL_Init()` + 时钟 72MHz
2. PC13 `GPIO_MODE_OUTPUT_PP`
3. PA0/PA1 `GPIO_MODE_ANALOG`
4. `__HAL_RCC_ADC1_CLK_ENABLE` + `RCC_ADCPCLK2_DIV6`
5. `HAL_ADCEx_Calibration_Start()`
6. `HAL_ADC_Start()`

### 8.2 新增：扫描和连续转换

```c
hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;       /* → CR1.SCAN=1 */
hadc1.Init.ContinuousConvMode = ENABLE;           /* → CR2.CONT=1 */
hadc1.Init.NbrOfConversion = 2U;                  /* → SQR1.L=1 */
```

三个字段分别对应扫描模式、连续转换、规则组长度。

### 8.3 新增：两次 ConfigChannel 配 rank

```c
/* Rank 1 → SQ1 = 通道 0（PA0）*/
ch.Channel = ADC_CHANNEL_0;
ch.Rank = ADC_REGULAR_RANK_1;
ch.SamplingTime = ADC_SAMPLETIME_55CYCLES_5;
HAL_ADC_ConfigChannel(&hadc1, &ch);

/* Rank 2 → SQ2 = 通道 1（PA1）*/
ch.Channel = ADC_CHANNEL_1;
ch.Rank = ADC_REGULAR_RANK_2;
HAL_ADC_ConfigChannel(&hadc1, &ch);
```

每次调用写一组 SQR + SMPR 位。Rank 1 先采，Rank 2 后采，顺序不能乱。

### 8.4 主循环：两次 Poll/GetValue

```c
g_adc0 = adc_poll_and_read();    /* Rank 1 → PA0 */
g_adc1 = adc_poll_and_read();    /* Rank 2 → PA1 */

if (g_adc0 > g_adc1) {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
}
HAL_Delay(300);
```

`PollForConversion` 每次等一个 rank 的 EOC，`GetValue` 读 DR。两次调用分别拿到 PA0 和 PA1 的结果。

## 9. 两个版本怎么学

寄存器版抓住规则组序列：

```text
SCAN + CONT + SQR1.L=1 + SQR3(SQ1=0,SQ2=1) → 两次EOC/DR读取
```

HAL 版抓住 rank 配置和读取：

```text
NbrOfConversion=2 + Rank1=CH0 + Rank2=CH1 → 两次Poll/GetValue
```

共同要点：**变量赋值顺序必须和规则组/rank 顺序严格一致，否则变量和引脚对不上。**

## 10. 检验问题清单

### 10.1 `SQR1.L=1` 为什么表示 2 个转换？

**答**：`L` 写的是转换数减 1。2 个转换时 `L=2-1=1`。

### 10.2 第一次读 DR 为什么是 PA0？

**答**：`SQ1=0`，规则组第 1 个转换通道是 0，即 PA0。

### 10.3 rank 配反会怎样？

**答**：`g_adc0` 实际存的是 PA1 的值，`g_adc1` 存的是 PA0 的值。变量含义和引脚对不上。

### 10.4 不开 `SCAN` 会怎样？

**答**：ADC 不会按多个 rank 扫描，第二个通道结果不可靠。

### 10.5 `CONT=1` 有什么作用？

**答**：一轮 PA0→PA1 扫描完成后自动开始下一轮，不需要软件重新启动。

### 10.6 为什么多通道扫描适合 DMA？

**答**：多个结果连续从同一个 DR 出来，CPU 手动读容易漏或错位。DMA 自动按顺序搬到数组。

### 10.7 如果只读一次 DR 会怎样？

**答**：只拿到 rank 1 的结果，rank 2 的结果留在 DR 会被下一轮覆盖或丢失。

### 10.8 HAL 版 `NbrOfConversion` 仍为 1 会怎样？

**答**：HAL 只按一个 rank 配 SQR，第二个 ConfigChannel 写的 rank 2 不生效，只采一个通道。

## 11. 工程实现步骤

### 11.1 需求分析

连续采样 PA0 和 PA1 两个模拟输入，分别存入 `g_adc0/g_adc1`。要求两个引脚模拟输入正确、扫描模式打开、规则组长度和顺序正确、读取顺序正确。

### 11.2 硬件核查

两个电位器分别接 PA0/PA1，两端接 3.3V/GND。输入不超过 3.3V，共地。

### 11.3 寄存器路线

1. 时钟 72MHz、PC13 输出、PA0/PA1 模拟输入（同前课）
2. ADC1 时钟/分频/校准（同前课）
3. `CR1.SCAN=1, CR2.CONT=1`
4. `SMPR2` 配 CH0/CH1 采样时间
5. `SQR1.L=1, SQR3: SQ1=0, SQ2=1`
6. `SWSTART` 启动
7. 主循环：两次等 EOC → 两次读 DR → 比较控制 LED

### 11.4 HAL 路线

1. HAL_Init + 时钟/PC13/PA0+PA1 ANALOG（同前课）
2. `ScanConvMode=ENABLE, ContinuousConvMode=ENABLE, NbrOfConversion=2`
3. Rank 1 = ADC_CHANNEL_0, Rank 2 = ADC_CHANNEL_1
4. 校准 + `HAL_ADC_Start()`
5. 主循环：两次 Poll/GetValue → 比较控制 LED

### 11.5 工程思维

多通道扫描最重要的是**顺序意识**：规则组写进去的顺序决定 DR 出来的顺序，变量赋值必须对齐。通道多了、速度快了，手动轮询容易漏，这时该用 DMA。

### 11.6 常见工程陷阱

1. **rank 配错** — 变量和引脚对不上
2. **没开 SCAN** — 只能正确读一个通道
3. **读取次数不够** — 只读一次 DR，第二个通道结果丢失
4. **模拟输入悬空** — 读数乱跳
5. **NbrOfConversion 没改** — HAL 只按一个 rank 扫描

## 12. 运行现象

两个电位器分别接 PA0 和 PA1：

- **PA0 电位器旋到 3.3V，PA1 电位器旋到 GND**：`g_adc0` 接近 4095，`g_adc1` 接近 0，`g_adc0 > g_adc1` 成立，PC13 持续翻转（寄存器版约每 10ms 翻一次，HAL 版每 300ms 翻一次）
- **PA0 电位器旋到 GND，PA1 电位器旋到 3.3V**：`g_adc0` 接近 0，`g_adc1` 接近 4095，`g_adc0 > g_adc1` 不成立，PC13 保持当前状态不翻转
- **两个电位器旋到相同位置**：`g_adc0 ≈ g_adc1`，PC13 偶尔翻转（因为两个通道值在阈值附近抖动）

用调试器观察 `g_adc0` 和 `g_adc1`：旋转 PA0 电位器时只有 `g_adc0` 变化，旋转 PA1 电位器时只有 `g_adc1` 变化。如果两个值同时变化或对不上，说明 rank 配反了。

## 13. 常见问题排查

### 13.1 两个值都不变化

检查 PA0/PA1 是否有模拟输入、是否配成模拟输入、ADC 是否启动（SWSTART 或 HAL_ADC_Start）。

### 13.2 两个变量和电位器对应反了

`SQR3` 或 HAL rank 配反了。rank 1 应是通道 0（PA0），rank 2 应是通道 1（PA1）。

### 13.3 第二个通道读数异常

没开扫描模式，或规则组长度不是 2，或只读了一次 DR。

### 13.4 读数抖动大

模拟输入悬空、没共地、采样时间太短。寄存器版用 239.5 cycles；HAL 版用 55.5 cycles，可改长。

## 14. 本课结论

1. `SCAN=1` 让 ADC 按规则组序列依次转换多个通道
2. `SQR1.L` 写转换数减 1，`SQR3` 写每个位置采哪个通道
3. DR 每次只给出当前转换结果，软件必须按顺序读取
4. 变量赋值顺序必须和规则组/rank 顺序严格一致
5. `CONT=1` 让一轮扫描完成后自动开始下一轮
6. HAL 的 rank 对应底层规则组序列位置
7. 多通道扫描和 DMA 是天然搭档——下一课就学

## 15. 阅读建议

先画一个两格序列：rank 1 = PA0，rank 2 = PA1。

然后看寄存器版 `SQR1/SQR3`，确认这个序列怎么写进 ADC。再看主循环两次 EOC/DR 读取，确认顺序对齐。

最后看 HAL 版两次 `ConfigChannel` 和两次 `Poll/GetValue`，理解 rank 和读取的对应关系。

## 16. 扩展练习

1. 交换 PA0/PA1 的 rank，观察 `g_adc0/g_adc1` 含义变化
2. 把 HAL 版采样时间改成 239.5 cycles，比较读数稳定性
3. 增加第三个通道 PA2，思考 `SQR1.L` 和读取次数怎么改
4. 故意只读一次 DR，观察第二个变量是否更新
5. 思考用 DMA 时，数组下标和 rank 如何对应

## 17. 下一课预告

上一课：[16_adc_interrupt](../16_adc_interrupt/README.md)

下一课：[18_dma_basic](../18_dma_basic/README.md)

下一课学习 DMA 基础。DMA 可以在外设和内存之间自动搬运数据，为 ADC 多通道 DMA 采样打基础。