# 60_freertos_adc_dma - FreeRTOS ADC DMA 采样

## 1. 本课到底在学什么

本课表面现象是：PA0 模拟电压被 ADC1 连续采样，DMA 自动把 16 个采样值搬到 `g_adc_buf`，DMA 半满或全满中断通知 `adc_task`，任务计算平均值，大于 2048 时点亮 PC13，否则熄灭。

真正要学的是 ADC、DMA、任务通知三层协作。CPU 不再轮询每一次 ADC 转换，而是让 DMA 搬运数据，让中断只通知事件，让任务处理平均值和 LED 输出。

本课继续按六层拆解：现象层看板子和串口输出，硬件层看引脚和连接，芯片模块层看 ADC/DMA/UART/GPIO/NVIC，寄存器层看控制位和状态位，C/CMSIS 层看源码语句，HAL/工程层看结构体字段、回调和返回值；FreeRTOS 部分还要解释任务、队列、通知、阻塞和 ISR 边界。

## 2. 本课学习目标

- 能说出现象来自哪条执行链路。
- 能解释每个新对象属于硬件层、寄存器层还是 RTOS 层。
- 能把寄存器版语句和 HAL 字段对应起来。
- 能根据具体现象排查时钟、GPIO、外设、DMA、中断和任务。
- 能说明 ISR 里为什么只做短动作。
- 能说明任务为什么阻塞等待而不是轮询。
- 能看懂源码里的缓冲区、句柄和返回值风险。
- 能把本课和前后课程的边界分清楚。

## 3. 本课目录结构

```text
60_freertos_adc_dma/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两个版本都要读。寄存器版让你看到 RCC、GPIO、ADC、DMA 等寄存器怎样配合；HAL 版让你看到同一件事如何变成 handle、Init 结构体、回调和封装 API。

## 4. 实验硬件与工程前提

- PA0：ADC1_IN0 模拟输入。
- PC13：平均值阈值指示 LED。
- ADC 时钟：72MHz / 6 = 12MHz。
- DMA1_Channel1：ADC1 固定映射通道。
- 缓冲区：`g_adc_buf[16]`，半满 8 点，全满 16 点。
- DMA IRQ 优先级 6，可调用 FromISR API。

## 5. 先建立一个最基本的脑图

```text
PA0 电压 -> ADC1 连续转换 -> ADC1->DR
  -> DMA1_Channel1 搬到 g_adc_buf[16]
  -> HTIF1/TCIF1 中断
  -> vTaskNotifyGiveFromISR(adc_task)
  -> adc_task 求平均值
  -> 平均值和 2048 比较
  -> PC13 输出状态变化
```

先用这张图记住数据从哪里来、经过哪个中断或任务、最后落到哪个输出。后面所有寄存器和 API 都应该能放回这张图里。

## 6. 先认识本课里出现的核心名词

### 6.1 `ADC1` 是什么

ADC1 是模数转换外设，把 PA0 电压转换成 0-4095 的 12 位数字。
本课只用规则序列第 1 个通道 ADC_CHANNEL_0。

### 6.2 `DMA1_Channel1` 是什么

它是 ADC1 在 STM32F1 上固定使用的 DMA 通道。
通道选错时 ADC 转换了也不会搬到缓冲区。

### 6.3 `g_adc_buf[16]` 是什么

它是 DMA 写入的采样缓冲区。
16 个 halfword 对应 16 次 ADC 转换结果。

### 6.4 `HTIF1` 是什么

半传输标志，表示缓冲区前半已经填好。
ISR 清标志后通知任务。

### 6.5 `TCIF1` 是什么

全传输标志，表示整个缓冲区填好。
循环模式下 DMA 随后回到开头继续搬。

### 6.6 `CIRC` 是什么

DMA 循环模式。
没有循环模式时搬完 16 个数据就停。

### 6.7 `MINC` 是什么

内存地址递增。
它让 DMA 依次写入数组不同元素。

### 6.8 `PSIZE/MSIZE` 是什么

外设和内存数据宽度。
ADC DR 是 16 位有效承载 12 位结果，所以本课用 halfword。

### 6.9 `ADC_CR2_DMA` 是什么

它允许 ADC 转换结果触发 DMA 请求。
不打开时 DMA 不会跟着 ADC 工作。

### 6.10 `ADC_CR2_CONT` 是什么

连续转换模式。
它让 ADC 不断采样 PA0。

### 6.11 `ADCPRE_DIV6` 是什么

ADC 预分频为 6，72MHz APB2 分到 12MHz。
F1 ADC 时钟不能超过规格上限。

### 6.12 `CAL` 是什么

ADC 校准位。
上电后校准能减少偏差。

### 6.13 `vTaskNotifyGiveFromISR` 是什么

ISR 中给任务发送通知的 API。
DMA 中断不能直接做长时间求平均。

### 6.14 `ulTaskNotifyTake` 是什么

adc_task 阻塞等待 DMA 事件。
收到通知后才处理缓冲区。

### 6.15 `2048` 阈值是什么

12 位 ADC 中间值约为 4096 的一半。
输入电压高于约 Vref/2 时 LED 状态改变。

## 7. 寄存器版代码逐步讲解

### 7.1 系统时钟和 ADC 分频

寄存器版在 RCC_CFGR 里同时配置 PLL 和 ADCPRE_DIV6。
这保证 ADC_CLK 为 12MHz。

### 7.2 PA0 模拟输入

清 MODE0/CNF0 让 PA0 成为模拟输入。
模拟输入关闭数字输入缓冲，适合 ADC。

### 7.3 DMA CPAR/CMAR

CPAR 指向 `&ADC1->DR`，CMAR 指向 `g_adc_buf`。
方向由外设到内存。

### 7.4 DMA CNDTR

CNDTR 写 16，表示一轮搬运 16 个 halfword。
循环模式会自动重装。

### 7.5 DMA CCR

MINC、CIRC、PSIZE、MSIZE、HTIE、TCIE 共同决定搬运和中断。
先配置再使能。

### 7.6 ADC 序列

SQR1=0 表示 1 个转换，SQR3=0 表示通道 0。
SMPR2 设置采样时间。

### 7.7 ADC 校准

先 ADON 唤醒，再 CAL 等待清零。
没有校准会增加偏差。

### 7.8 DMA 中断

ISR 检查 HTIF1/TCIF1，写 IFCR 清标志。
清标志后通知任务。

### 7.9 任务求平均

adc_task 对 16 个样本求和除以长度。
平均值减少单点抖动。

### 7.10 LED 输出

平均值大于 2048 写 BRR 点亮 PC13，否则写 BSRR 熄灭。
这是采样结果的可见证据。

## 8. HAL 版代码逐步讲解

### 8.1 HAL ADC handle

`hadc1.Instance=ADC1` 绑定 ADC1。
Init 字段配置连续转换、软件触发和右对齐。

### 8.2 HAL DMA handle

`hdma_adc1.Instance=DMA1_Channel1`。
Direction、MemInc、Alignment、Mode 对应寄存器版 DMA CCR/地址配置。

### 8.3 __HAL_LINKDMA

把 ADC handle 和 DMA handle 关联。
没有关联时 HAL_ADC_Start_DMA 不知道用哪个 DMA。

### 8.4 HAL_ADC_ConfigChannel

配置 ADC_CHANNEL_0、Rank 1、采样时间 239.5 cycles。
对应 SQR3 和 SMPR2。

### 8.5 HAL_ADC_Start_DMA

启动 ADC 和 DMA。
任务开头调用一次，后续 DMA 循环搬运。

### 8.6 HAL_ADC_ConvHalfCpltCallback

HAL 半满回调。
内部通知 adc_task。

### 8.7 HAL_ADC_ConvCpltCallback

HAL 全满回调。
同样通知 adc_task。

### 8.8 DMA IRQHandler

DMA1_Channel1_IRQHandler 调 HAL_DMA_IRQHandler。
HAL 再分发到半满/全满回调。

## 9. 两个版本真正应该怎么学

寄存器版和 HAL 版做的是同一个系统。寄存器版适合看清时钟、引脚模式、状态标志、DMA 通道映射和中断清标志；HAL 版适合看清工程结构、handle 关联、回调入口和错误处理。

不要只背 API 名称。每看到一个 API，都要问它最终配置了哪个外设、影响了哪个数据流、失败后现象是什么。这样读，后面遇到综合项目才不会散。

## 10. 检验问题清单

### 10.1 为什么用 DMA？

**答**：避免 CPU 等待每次 ADC 转换，让采样搬运自动进行。

### 10.2 为什么半满也通知？

**答**：半满说明前 8 个样本已可处理，能降低延迟。

### 10.3 为什么平均 16 个样本？

**答**：平均能减小抖动，比单点阈值稳定。

### 10.4 ADC 时钟为什么分频？

**答**：F1 ADC 有最大时钟限制，72MHz 不能直接喂给 ADC。

### 10.5 为什么 ISR 不求平均？

**答**：求平均是较长逻辑，应放任务里，ISR 只通知。

### 10.6 HAL 版为什么要 LINKDMA？

**答**：HAL 需要通过 handle 找到关联 DMA。

### 10.7 阈值 2048 代表什么？

**答**：12 位 ADC 中间值，约等于参考电压的一半。

### 10.8 DMA 通道能换吗？

**答**：ADC1 在 F1 上固定映射到 DMA1_Channel1。

### 10.9 PC13 不动先查什么？

**答**：查 PA0 电压、ADC/DMA 是否启动、DMA 中断是否触发、任务是否收到通知。

### 10.10 缓冲区为什么 volatile？

**答**：寄存器版 DMA 和 CPU 都访问它，volatile 避免编译器错误假设。

## 11. 工程实现步骤

### 11.1 需求分析

输入是 PA0 模拟电压；处理是 ADC1 连续采样、DMA1_Channel1 循环搬运 16 个 halfword、任务通知后求平均；输出是 PC13 LED 根据平均值是否大于 2048 改变状态。

### 11.2 环境配置

- 目标板：STM32F103C8T6 或同系列。
- PlatformIO：`board = bluepill_f103c8`，寄存器版用 `framework = cmsis`，HAL 版用 `framework = stm32cube`。
- FreeRTOS：通过 `lib_deps` 或 `lib/` 引入源码。
- 时钟：HSE 8MHz，PLL ×9 到 72MHz，ADC 预分频 6 得到 12MHz。

### 11.3 寄存器版实现要点

1. 配置系统时钟和 ADCPRE_DIV6。
2. 初始化 PC13 为推挽输出并置位（熄灭）。
3. 配置 PA0 为模拟输入，打开 GPIOA、ADC1、DMA1 时钟。
4. 配置 DMA1_Channel1：CPAR = `&ADC1->DR`，CMAR = `g_adc_buf`，CNDTR = 16，CCR 使能 MINC、CIRC、halfword 宽度、HTIE、TCIE。
5. NVIC 优先级 6，使能 DMA1_Channel1_IRQn。
6. 配置 ADC1：CR2 = DMA | CONT，SQR1/SQR3 设 1 个规则通道（通道 0），设置采样时间。
7. 上电延迟、校准、使能 DMA 和 ADC。
8. 创建 `adc_task` 并保存句柄，启动调度器。
9. 在 `DMA1_Channel1_IRQHandler()` 中清 HTIF1/TCIF1，调用 `vTaskNotifyGiveFromISR()`。

### 11.4 HAL 版实现要点

1. 用 `HAL_RCC_OscConfig` / `HAL_RCC_ClockConfig` / `HAL_RCCEx_PeriphCLKConfig` 配同样时钟。
2. 初始化 PC13 和 PA0；配置 `hdma_adc1` 为 DMA1_Channel1、外设到内存、MemInc、halfword、Circular、Low priority，调用 `HAL_DMA_Init()`。
3. `__HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1)`。
4. NVIC 优先级 6，使能 DMA1_Channel1_IRQn。
5. 配置 `hadc1`：Instance = ADC1，连续转换、软件触发、右对齐、1 个转换；配置通道 0，Rank 1，采样时间 239.5 cycles。
6. 创建 `adc_task` 并保存句柄，启动调度器。
7. 在任务中调用 `HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_adc_buf, ADC_BUF_LEN)`。
8. 在 `HAL_ADC_ConvHalfCpltCallback()` 和 `HAL_ADC_ConvCpltCallback()` 中调用 `vTaskNotifyGiveFromISR()`。
9. `DMA1_Channel1_IRQHandler()` 中调用 `HAL_DMA_IRQHandler(&hdma_adc1)`。

### 11.5 编译下载与验证

- 寄存器版：`pio run -e reg -t upload`
- HAL 版：`pio run -e hal -t upload`
- 用可调电源或电位器给 PA0 0V ~ 3.3V，观察 PC13 随阈值 2048 翻转。

验证时若 PC13 不动，按链路检查：PA0 电压 → ADC/DMA 是否启动 → DMA 中断是否触发 → 任务是否收到通知 → LED 控制逻辑是否正确。