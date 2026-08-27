# 15_adc_polling - ADC 轮询采样

## 1. 本课到底在学什么

本课表面现象：PA1 接电位器中间脚，旋转电位器时 PC13 LED 在电压超过约 1.65V 时亮、低于时灭。

真正要学的是 ADC 轮询采样链路：模拟电压从 PA1 进入 ADC1 通道 1，经过采样保持和 12 位转换得到 0~4095 的数字值，CPU 轮询 `EOC` 标志等待完成，再读 `DR`。

这是从"数字 0/1"进入"模拟量采集"的第一课。前面 GPIO 只能读高低电平；ADC 能读出连续电压对应的数字量。下一课把轮询改成中断。

## 2. 本课学习目标

1. 为什么 PA1 要配模拟输入而不是普通输入？
2. 12 位 ADC 结果为什么是 0~4095？2048 对应多少伏？
3. ADC 时钟为什么不能直接用 72MHz？本课怎么分频的？
4. 规则组 `SQR1/SQR3` 怎么决定采哪个通道？
5. 采样时间 239.5 cycles 是什么意思？太短会怎样？
6. `ADON`、`RSTCAL`、`CAL`、`SWSTART`、`EOC`、`DR` 各自干什么？
7. 轮询方式的缺点是什么？
8. HAL 版 Start/Poll/GetValue 分别对应寄存器版哪一步？

## 3. 本课目录结构

```text
15_adc_polling/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接配 PA1 模拟输入、ADC1 规则组、采样时间、校准、轮询读取。

`hal/` 用 `ADC_HandleTypeDef` + `ADC_ChannelConfTypeDef` + Start/Poll/GetValue 完成同样功能。

## 4. 实验硬件

- STM32F103C8T6 BluePill
- ST-Link 下载器
- 电位器（推荐 10kΩ）
- PC13 板载 LED
- 可选：示波器或万用表

电位器接法：

```text
电位器一端 → 3.3V
电位器另一端 → GND
电位器中间脚 → PA1
```

**PA1 输入电压不能超过 3.3V，否则可能损坏芯片。** 外部模拟源必须和 STM32 共地。

## 5. 先建立一个最基本的脑图

```text
1. 系统时钟 72MHz
2. PC13 推挽输出（心跳/阈值指示）
3. PA1 配成模拟输入
4. 打开 ADC1 时钟，ADCPRE = PCLK2/6 → 12MHz
5. 规则组长度 1，SQ1 = 通道 1
6. 通道 1 采样时间 239.5 cycles
7. ADON 上电 → 等待稳定 → RSTCAL → CAL 校准
8. 主循环：SWSTART 启动转换 → 轮询 EOC → 读 DR → 阈值判断 → LED
```

关键点：第 4 步 ADC 时钟不能超 14MHz；第 6 步采样时间太短结果会抖；第 8 步轮询期间 CPU 什么也干不了。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在前面的课已详细讲过，本课不再重复：

| 名词 | 一句话提醒 |
|------|-----------|
| `RCC->APB2ENR` | APB2 外设时钟使能，本课开 GPIOA/GPIOC/ADC1 |
| `GPIOA->CRL` | 控制 PA0~PA7 模式，本课配 PA1 模拟输入 |
| `GPIOC->CRH` | 控制 PC8~PC15 模式，本课配 PC13 推挽输出 |
| `BSRR/BRR` | 置位/复位寄存器，控制 PC13 高低电平 |
| `PCLK2` | APB2 时钟，72MHz，ADC 输入时钟来源 |

本课新增重点在下面。

### 6.2 `ADC` 是什么

ADC 是 Analog-to-Digital Converter，模数转换器。它把连续模拟电压转换成离散数字值。

STM32F103 的 ADC 是 12 位，结果 0~4095。0 对应 0V，4095 对应参考电压 VREF+（约 3.3V）。

本课第一次接触模拟外设，之前所有课都是数字 0/1。配错的话读到的数字和实际电压对不上。

### 6.3 `ADC1_IN1 / PA1` 是什么

PA1 的第二功能是 ADC1 的通道 1 输入（ADC1_IN1）。同一个引脚在 ADC 功能下把模拟电压送到 ADC1 通道 1。

代码里 PA1 配模拟输入、规则组选通道 1，两边必须一致。电位器接 PA0 但代码采通道 1，读到的就不是电位器电压。

### 6.4 模拟输入模式是什么

GPIO 的一种模式，F103 中对应 `MODE=00`、`CNF=00`。它会断开数字输入路径（施密特触发器），让模拟电压直接进入 ADC 采样电容。

如果配成普通输入，数字输入电路会干扰模拟信号，采样不准。

### 6.5 `ADCPRE` 是什么

ADC 预分频器，位于 `RCC->CFGR`，决定 PCLK2 进入 ADC 前除以多少。

本课 `ADCPRE = PCLK2/6`，72MHz/6 = 12MHz。F103 ADC 最大时钟约 14MHz，超过则转换结果不稳定。

### 6.6 规则组 / `SQR1` / `SQR3` 是什么

规则组是 ADC 的常规转换序列——一个"要采哪些通道"的列表。

- `SQR1.L`：规则组通道数减 1。本课 `L=0`，只转换 1 个通道。
- `SQR3.SQ1`：规则组第 1 个转换通道号。本课 `SQ1=1`，即通道 1。

如果通道号和 PA1 不对应，读到的就不是电位器电压。

### 6.7 `SMPR2` / 采样时间是什么

`SMPR2` 是采样时间寄存器 2，控制通道 0~9 的采样时间。本课通道 1 用 `SMP1=111`，即 239.5 个 ADC 周期。

采样时间是采样电容充电的时间。太短则电容没充满，结果偏低或抖动。电位器源阻抗较高，需要较长采样时间。

总转换时间 = 采样时间 + 12.5 固定周期。本课 239.5 + 12.5 = 252 周期，12MHz 下约 21μs。

### 6.8 `ADON` / `RSTCAL` / `CAL` 是什么

都在 `ADC1->CR2` 中：

- `ADON`：ADC 上电。第一次置 1 给 ADC 上电，后续配合触发启动转换。没上电则校准和转换都不能进行。
- `RSTCAL`：复位校准。置 1 后硬件自动清零，轮询等清零即可。
- `CAL`：执行校准。置 1 后硬件自动清零。校准测量内部偏移并修正，不校准结果会有几个 LSB 偏移。

### 6.9 `EXTTRIG` / `SWSTART` 是什么

都在 `ADC1->CR2` 中：

- `EXTTRIG`：允许外部/软件触发规则组转换。不设则 `SWSTART` 不生效。
- `SWSTART`：软件启动规则组转换。置 1 后 ADC 开始一次转换。

如果 `SWSTART` 不生效，`EOC` 不会置位，轮询会卡住。

### 6.10 `EOC` / `DR` 是什么

- `EOC`：End Of Conversion，转换完成标志，位于 `ADC1->SR`。硬件置 1 表示 `DR` 中有新数据。
- `DR`：数据寄存器，12 位结果在 bit 0~11。读取 `DR` 后 F103 自动清除 `EOC`。

### 6.11 `ADC_HandleTypeDef` / `ADC_ChannelConfTypeDef` 是什么

HAL 的两个核心结构体：

- `ADC_HandleTypeDef`：ADC 句柄，包含 `Instance`（ADC1）、`Init`（整体模式）、状态和锁。对应 ADC1 整体配置。
- `ADC_ChannelConfTypeDef`：通道配置，包含 `Channel`、`Rank`、`SamplingTime`。对应 `SQR3` 和 `SMPR2`。

### 6.12 轮询采样是什么

CPU 启动 ADC 转换后，一直检查 `EOC` 直到置位。等待期间 CPU 不能做其他事。本课用轮询是因为链路最直观；下一课用中断改进。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

1. `system_clock_72mhz_init()`：HSE 8MHz → PLL x9 → 72MHz，APB2 不分频
2. `led_pc13_init()`：开 GPIOC 时钟，PC13 推挽输出，初始高电平灭
3. 开 GPIOA 时钟

### 7.2 PA1 配成模拟输入

```c
GPIOA->CRL &= ~(GPIO_CRL_MODE1 | GPIO_CRL_CNF1);
```

清零 `MODE1/CNF1` 后，PA1 进入模拟输入模式（`MODE=00, CNF=00`）。断开数字输入路径，模拟电压直接进 ADC 采样电容。

### 7.3 打开 ADC1 时钟并设置分频

```c
RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
RCC->CFGR &= ~RCC_CFGR_ADCPRE;
RCC->CFGR |= RCC_CFGR_ADCPRE_DIV6;
```

ADC1 挂 APB2，先开时钟。`ADCPRE_DIV6` 让 ADC 时钟 = 72MHz/6 = 12MHz，低于 14MHz 限制。超过限制则转换不准。

### 7.4 配置规则组：长度和通道

```c
ADC1->SQR1 &= ~ADC_SQR1_L;       /* L=0，转换 1 个通道 */
ADC1->SQR3 &= ~ADC_SQR3_SQ1;
ADC1->SQR3 |= 1U;                  /* SQ1=1，第 1 个转换通道是通道 1 */
```

### 7.5 配置采样时间

```c
ADC1->SMPR2 &= ~ADC_SMPR2_SMP1;
ADC1->SMPR2 |= ADC_SMPR2_SMP1;    /* SMP1=111，239.5 cycles */
```

`ADC_SMPR2_SMP1` 宏展开就是 SMP1 三位全 1（bit 3~5），即 239.5 cycles。电位器源阻抗较高，需要长采样时间让电容充满。

### 7.6 ADC 上电、校准

```c
ADC1->CR2 |= ADC_CR2_ADON;        /* 上电 */
delay(1000U);                       /* 等待内部稳定 */
ADC1->CR2 |= ADC_CR2_RSTCAL;      /* 复位校准 */
while ((ADC1->CR2 & ADC_CR2_RSTCAL) != 0U) {}
ADC1->CR2 |= ADC_CR2_CAL;         /* 执行校准 */
while ((ADC1->CR2 & ADC_CR2_CAL) != 0U) {}
```

顺序不能乱：先上电等稳定，再复位校准等完成，再执行校准等完成。跳过校准结果会有偏移。

### 7.7 主循环：启动转换 → 轮询 EOC → 读 DR

```c
ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_SWSTART;  /* 启动 */
while ((ADC1->SR & ADC_SR_EOC) == 0U) {}          /* 等完成 */
return (uint16_t)ADC1->DR;                          /* 读结果 */
```

`EXTTRIG` 允许触发，`SWSTART` 产生触发事件。`EOC` 置位说明转换完成，读 `DR` 取走数据并自动清 `EOC`。

如果 `SWSTART` 不生效（比如忘了 `EXTTRIG`），`EOC` 永远不置位，程序卡死。

### 7.8 阈值控制 LED

```c
if (adc_value > 2048U) {
    GPIOC->BRR = GPIO_BRR_BR13;    /* PC13 低电平，LED 亮 */
} else {
    GPIOC->BSRR = GPIO_BSRR_BS13;  /* PC13 高电平，LED 灭 */
}
```

2048 约等于 12 位半量程，对应约 1.65V。BluePill PC13 低电平点亮。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

1. `HAL_Init()` + 时钟配置到 72MHz（RCC_OscInitTypeDef / RCC_ClkInitTypeDef）
2. PC13 配成 `GPIO_MODE_OUTPUT_PP`，初始 `GPIO_PIN_SET`
3. PA1 配成 `GPIO_MODE_ANALOG`
4. `__HAL_RCC_ADC1_CLK_ENABLE()` 开 ADC1 时钟
5. `__HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6)` 设 ADC 分频

### 8.2 `hadc1.Init` 配 ADC 整体模式

```c
hadc1.Instance = ADC1;
hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;       /* 不扫描，单通道 */
hadc1.Init.ContinuousConvMode = DISABLE;           /* 单次转换 */
hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;  /* 软件触发 */
hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;        /* 右对齐 */
hadc1.Init.NbrOfConversion = 1;                    /* 规则组 1 个通道 */
```

对应寄存器版 `CR1/CR2/SQR1` 的整体配置。`ScanConvMode=DISABLE` 对应不扫描，`ContinuousConvMode=DISABLE` 对应单次转换，`ExternalTrigConv=ADC_SOFTWARE_START` 对应用 `SWSTART` 触发。

### 8.3 `HAL_ADCEx_Calibration_Start()`

对应寄存器版 `RSTCAL` + `CAL` 流程。F1 的 ADC 校准不是 `HAL_ADC_Init()` 自动完成的，必须显式调用。

### 8.4 `ADC_ChannelConfTypeDef` 配通道

```c
sConfig.Channel = ADC_CHANNEL_1;                      /* 通道 1 */
sConfig.Rank = ADC_REGULAR_RANK_1;                    /* 规则组第 1 位 */
sConfig.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;    /* 239.5 cycles */
```

对应寄存器版 `SQR3.SQ1=1` + `SMPR2.SMP1=111`。

### 8.5 轮询三步曲

```c
HAL_ADC_Start(&hadc1);                          /* → SWSTART */
HAL_ADC_PollForConversion(&hadc1, 100U);        /* → while(!EOC)，带 100ms 超时 */
adc_value = HAL_ADC_GetValue(&hadc1);           /* → 读 DR */
```

- `HAL_ADC_Start()`：清状态标志 + 设 `ADON` + 设 `SWSTART`
- `HAL_ADC_PollForConversion()`：轮询 `EOC`，100ms 超时保护（转换本身只需约 21μs）
- `HAL_ADC_GetValue()`：底层就是读 `ADC1->DR`

## 9. 两个版本怎么学

寄存器版抓住 ADC 底层顺序：

```text
PA1 模拟输入 → ADC 时钟分频 → 规则组通道 → 采样时间 → 上电校准 → SWSTART → EOC → DR
```

HAL 版抓住三类对象：

```text
ADC_HandleTypeDef  → ADC 整体模式（对应 CR1/CR2/SQR1）
ADC_ChannelConfTypeDef → 通道和采样时间（对应 SQR3/SMPR2）
Start/Poll/GetValue → 轮询流程（对应 SWSTART/EOC/DR）
```

## 10. 检验问题清单

### 10.1 为什么 PA1 要配模拟输入而不是普通输入？

**答**：普通输入经过施密特触发器，会干扰模拟信号。模拟输入断开数字路径，让电压直接进 ADC 采样电容。

### 10.2 ADC 值 2048 大约对应多少伏？

**答**：约 `2048/4095 × 3.3V ≈ 1.65V`。

### 10.3 为什么 ADC 时钟不能直接用 72MHz？

**答**：F103 ADC 最大时钟约 14MHz，超过则内部比较器来不及稳定，转换不准。本课用 PCLK2/6 = 12MHz。

### 10.4 忘了校准会怎样？

**答**：结果可能有几个 LSB 的偏移，精度要求高的场景更明显。

### 10.5 采样时间太短会怎样？

**答**：采样电容没充满就开始转换，结果偏低且抖动。电位器源阻抗高，更需要长采样时间。

### 10.6 忘了设 `EXTTRIG` 就写 `SWSTART` 会怎样？

**答**：`SWSTART` 不生效，`EOC` 永远不置位，轮询卡死。

### 10.7 读 `DR` 后会发生什么？

**答**：得到 12 位转换结果；F103 上读 `DR` 后 `EOC` 自动清除，准备下一次转换。

### 10.8 HAL 版哪个 API 对应等待 `EOC`？

**答**：`HAL_ADC_PollForConversion()`，底层轮询 `EOC` 并带超时。

## 11. 工程实现步骤

### 11.1 需求分析

读取 PA1 模拟电压，用半量程阈值控制 PC13。要求 PA1 模拟输入正确、ADC1 时钟分频正确、规则组选通道 1、采样时间足够、校准完成、轮询读取成功。

### 11.2 硬件核查

确认电位器两端接 3.3V 和 GND，中间脚接 PA1。输入电压不超过 3.3V。外部模拟源必须和 STM32 共地。

### 11.3 寄存器路线

1. 系统时钟 72MHz
2. PC13 推挽输出
3. PA1 模拟输入
4. 开 ADC1 时钟，ADCPRE = PCLK2/6
5. SQR1.L=0，SQR3.SQ1=1
6. SMPR2.SMP1=111
7. ADON 上电，等待稳定
8. RSTCAL → CAL 校准
9. 主循环：EXTTRIG+SWSTART → 等EOC → 读DR → 阈值判断

### 11.4 HAL 路线

1. HAL_Init() + 时钟 72MHz
2. PC13 OUTPUT_PP
3. PA1 ANALOG
4. __HAL_RCC_ADC1_CLK_ENABLE + ADCPCLK2_DIV6
5. hadc1.Init → HAL_ADC_Init()
6. HAL_ADCEx_Calibration_Start()
7. sConfig → HAL_ADC_ConfigChannel()
8. 主循环：Start → PollForConversion → GetValue → 阈值判断

### 11.5 工程思维

ADC 采样不是"读一个引脚"那么简单，涉及模拟输入模式、时钟分频、采样时间、通道序列、校准、触发和完成标志。轮询适合入门和低速采样；采样频率高或 CPU 有其他任务时，应考虑中断或 DMA。

### 11.6 常见工程陷阱

1. **PA1 没配模拟输入** — 数字输入电路干扰采样，结果不准
2. **ADC 时钟超 14MHz** — 转换结果不稳定
3. **电位器接错引脚或没共地** — 读到错误电压或满量程
4. **采样时间太短** — 电位器场景结果抖动
5. **忘记校准** — 结果有偏移

## 12. 运行现象

电位器中间脚接 PA1，旋转电位器时：

- **电位器旋到 GND 端**：PA1 电压接近 0V，`adc_value` 接近 0，PC13 LED 灭
- **电位器旋到中间**：PA1 电压约 1.65V，`adc_value` 约 2048，PC13 LED 在此阈值切换
- **电位器旋到 3.3V 端**：PA1 电压接近 3.3V，`adc_value` 接近 4095，PC13 LED 亮

用调试器观察 `adc_value` 变量，旋转电位器时应看到 0~4095 平滑变化。PC13 LED 在 `adc_value` 跨过 2048 时切换亮灭。

如果 `adc_value` 始终为 0：检查 PA1 接线和模拟输入配置。如果始终接近 4095：检查 PA1 是否悬空或被拉高。

## 13. 常见问题排查

### 13.1 ADC 值一直是 0

先查 PA1 是否有输入电压，电位器中间脚是否接到 PA1。再查 GPIOA 时钟、PA1 模拟输入、ADC1 时钟、规则组通道是否是 1。

### 13.2 ADC 值一直接近 4095

PA1 可能被接到 3.3V 或悬空被拉高。确认电位器接法和共地。

### 13.3 ADC 值抖动很大

检查模拟输入线是否太长、是否共地、供电是否稳定。可加硬件滤波电容或软件多次平均。本课已用最长采样时间。

### 13.4 程序卡在等待转换完成

寄存器版检查 `EXTTRIG/SWSTART` 是否设置、ADC 是否上电校准完成。HAL 版检查 `HAL_ADC_Start()` 返回值和 `PollForConversion()` 是否超时。

## 14. 本课结论

1. ADC 把 PA1 模拟电压转换成 0~4095 的 12 位数字值
2. PA1 必须配模拟输入，断开数字路径才能正确采样
3. ADC 时钟必须从 PCLK2 分频到 14MHz 以下，本课 12MHz
4. 规则组决定采哪个通道，采样时间决定电容充电多久
5. 校准能减少偏移误差，F103 必须显式执行
6. 轮询流程：SWSTART → 等EOC → 读DR，等待期间 CPU 什么也干不了
7. HAL 的 Start/Poll/GetValue 对应 SWSTART/EOC/DR

## 15. 阅读建议

先从硬件接线读起：电位器中间脚 → PA1。

然后看寄存器版 `adc1_init()`，按"时钟、序列、采样时间、上电、校准"顺序理解。

最后看 `adc1_read_channel1()`，把启动、等 EOC、读 DR 三步背后的硬件状态想清楚。

## 16. 扩展练习

1. 把阈值 2048 改成 1024 或 3072，观察 LED 触发位置变化
2. 把采样时间改短（如 1.5 cycles），观察读数是否更容易抖动
3. 在调试器中观察 `ADC1->SR`、`ADC1->DR` 和 `adc_value`
4. 连续读取 16 次求平均，比较稳定性
5. 思考：如果不想 CPU 一直等 EOC，应该怎样处理？

## 17. 下一课预告

上一课：[14_timer_advanced_tim1](../14_timer_advanced_tim1/README.md)

下一课：[16_adc_interrupt](../16_adc_interrupt/README.md)

下一课把 ADC 等待方式从轮询改为中断。ADC 完成转换后主动通知 CPU，CPU 不必卡在 `while(EOC==0)` 里。