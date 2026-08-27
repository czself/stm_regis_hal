# 11_input_capture - 输入捕获

## 1. 本课到底在学什么

本课表面现象是：STM32 自己用 TIM2_CH1 在 PA0 输出一个 1kHz PWM，再用一根杜邦线把 PA0 接到 PA6，让 TIM3_CH1 捕获这个信`号的上升沿。测得周期接近 1000 个计数时，PC13 LED 点亮。

真正要学的是输入捕获。输入捕获不是“CPU 看到引脚变化后马上读时间”，而是定时器硬件在边沿到来的瞬间，把当前 `CNT` 自动锁存到 `CCR1`。软件随后读取 `CCR1`，用本次捕获值减去上次捕获值，就能得到两个边沿之间的计数差。

这节课接在 EXTI 之后。EXTI 解决的是“边沿来了以后通知 CPU”；输入捕获更进一步，解决“边沿到底发生在定时器时间轴的哪个位置”。后续 PWM 输入、频率测量、脉宽测量、编码器等内容都会建立在这个能力上。

## 2. 本课学习目标

学完本课，你应该能做到：

- 解释为什么本课要用 PA0 接 PA6，而不是只配置一个引脚。
- 说明 TIM2_CH1 是信号源，TIM3_CH1 是测量端。
- 解释 `CCR1` 在输出 PWM 和输入捕获两种模式下含义为什么不同。
- 根据 `PSC = 72 - 1` 算出 TIM3 计数频率是 1MHz。
- 说明捕获差值 1000 为什么表示 1000us，也就是 1kHz 周期。
- 看懂 `CCMR1.CC1S`、`IC1PSC`、`IC1F`、`CCER.CC1E`、`CC1P/CC1NP` 的作用。
- 解释 `SR.CC1IF` 和 `DIER.CC1IE` 如何形成输入捕获中断。
- 说明为什么 `volatile` 变量用于中断和主循环共享数据。
- 看懂 HAL 版 `HAL_TIM_IC_Start_IT()`、`HAL_TIM_IRQHandler()`、`HAL_TIM_IC_CaptureCallback()`、`HAL_TIM_ReadCapturedValue()` 的关系。

## 3. 本课目录结构

```text
11_input_capture/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接配置 TIM2 PWM 输出和 TIM3 输入捕获。

`hal/` 使用 `TIM_HandleTypeDef`、`TIM_OC_InitTypeDef`、`TIM_IC_InitTypeDef` 和 HAL 回调完成同样链路。

两份工程都使用 `genericSTM32F103C8`、`stm32cube`、`stlink`，并通过 `HSE_VALUE=8000000U` 说明外部晶振按 8MHz 计算。

## 4. 实验硬件

本课使用：

- STM32F103C8T6 BluePill
- ST-Link 下载器
- 杜邦线一根
- PA0 引脚
- PA6 引脚
- PC13 板载 LED

必须接线：

```text
PA0 ---- 杜邦线 ---- PA6
```

PA0 是 TIM2_CH1 输出端，负责产生 1kHz PWM。PA6 是 TIM3_CH1 输入端，负责捕获 PA0 送来的上升沿。

没有这根线，TIM3_CH1 捕获不到 TIM2_CH1 的信号。PC13 是否点亮，取决于软件测得的 `g_period_ticks` 是否落在 990 到 1010 之间。

## 5. 先建立一个最基本的脑图

本课分六层看。

现象层：PA0 输出 1kHz 方波，PA6 接收到这个方波。调试器里 `g_period_ticks` 应接近 1000，PC13 点亮。如果断开 PA0 到 PA6，捕获值不再更新。

物理/硬件层：PA0 和 PA6 是两个真实引脚。本课用一根线把输出端接到输入端，让开发板自己产生被测信号、自己测量信号，不依赖外部信号发生器。

芯片模块层：TIM2 负责输出 PWM，GPIOA 配置 PA0 复用输出和 PA6 输入，TIM3 负责输入捕获，NVIC 负责 TIM3 中断，GPIOC 负责 PC13 LED。

寄存器/bit 层：TIM2 的 `PSC/ARR/CCR1/OC1M/CC1E` 生成 1kHz PWM；TIM3 的 `PSC/ARR` 生成 1us 计数时间轴；`CCMR1.CC1S` 把 CH1 设置为输入；`CCER` 选择上升沿并使能捕获；`DIER.CC1IE` 打开捕获中断；`SR.CC1IF` 表示有新捕获；`CCR1` 保存捕获时间戳。

C/CMSIS 层：寄存器版在 `TIM3_IRQHandler()` 中读取 `TIM3->CCR1`，用 `uint16_t` 差值计算周期，通过 `volatile` 全局变量把结果交给主循环。

HAL/工程层：HAL 版用 `HAL_TIM_PWM_Start()` 启动 TIM2 输出，用 `HAL_TIM_IC_Start_IT()` 启动 TIM3 捕获中断，用 `HAL_TIM_IC_CaptureCallback()` 接收捕获事件，用 `HAL_TIM_ReadCapturedValue()` 读取时间戳。

完整链路是：

1. 系统时钟配置到 72MHz。
2. PC13 配成输出，初始熄灭。
3. TIM2_CH1 在 PA0 输出 1kHz、50% 占空比 PWM。
4. 杜邦线把 PA0 波形送到 PA6。
5. PA6 配成输入，作为 TIM3_CH1 的输入脚。
6. TIM3 设置 `PSC = 72 - 1`，计数频率为 1MHz。
7. TIM3 设置 `ARR = 0xFFFF`，扩大可测周期范围。
8. TIM3_CH1 设置为输入捕获，直接连接 TI1。
9. 捕获极性选择上升沿。
10. 打开 CH1 捕获和捕获中断。
11. PA6 每来一个上升沿，TIM3 硬件把 `CNT` 锁存到 `CCR1`。
12. TIM3 中断读取 `CCR1`，计算本次和上次捕获差值。
13. 主循环判断差值是否接近 1000。
14. 差值正确时点亮 PC13，否则熄灭。

## 6. 核心名词解释

### 6.1 输入捕获是什么

输入捕获是定时器通道的一种输入模式。

它属于 TIM 外设通道功能层。外部边沿到来时，定时器不是只置一个标志，而是把当前计数器 `CNT` 的值自动保存到捕获/比较寄存器 `CCR` 中。

本课用输入捕获测量 PA0 输出信号的周期。每次上升沿到来，TIM3_CH1 把 `CNT` 锁存到 `CCR1`。两次 `CCR1` 的差值就是周期。

如果没有输入捕获，软件只能在中断里临时读计数器，读到的时间会受中断响应延迟影响。

### 6.2 `TIM2_CH1` 是什么

`TIM2_CH1` 是 TIM2 的通道 1。

它属于信号源层。本课用它在 PA0 输出 1kHz PWM，给 TIM3 输入捕获提供被测信号。

代码中 TIM2_CH1 对应 `TIM2->CCR1`、`TIM2->CCMR1.OC1M`、`TIM2->CCER.CC1E`，HAL 版对应 `HAL_TIM_PWM_ConfigChannel(&htim2, ..., TIM_CHANNEL_1)`。

如果 TIM2 没输出，TIM3 捕获端就没有信号来源。

### 6.3 `PA0` 是什么

PA0 是 GPIOA 的 0 号引脚。

它属于物理引脚层和 GPIO 复用输出层。本课中 PA0 是 TIM2_CH1 的输出脚，需要配置成复用推挽输出。

寄存器版设置 `GPIOA->CRL` 的 `MODE0=10`、`CNF0=10`。HAL 版用 `GPIO_MODE_AF_PP`。

如果 PA0 没配成复用推挽，TIM2 内部可能有 PWM，但波形到不了引脚，也就送不到 PA6。

### 6.4 `PA6` 是什么

PA6 是 GPIOA 的 6 号引脚。

它属于物理引脚层和定时器输入层。本课中 PA6 是 TIM3_CH1 的输入脚，接收 PA0 送来的方波。

寄存器版把 PA6 配成浮空输入，HAL 版用 `GPIO_MODE_INPUT` 和 `GPIO_NOPULL`。

如果 PA6 悬空且没有接 PA0，捕获结果不会稳定，甚至可能没有捕获。

### 6.5 `TIM3_CH1` 是什么

`TIM3_CH1` 是 TIM3 的通道 1。

它属于测量端定时器通道层。本课让 TIM3_CH1 连接 PA6，并在 PA6 上升沿到来时捕获 TIM3 的 `CNT`。

代码中通过 `TIM3->CCMR1` 的 `CC1S` 字段把通道 1 设置为输入，通过 `TIM3->CCER` 选择上升沿和使能捕获。

如果 CH1 没设置为输入捕获，`CCR1` 不会保存边沿时间戳。

### 6.6 `CNT` 是什么

`CNT` 是 Counter，中文叫计数器当前值。

它属于定时器时间轴层。TIM3 的 `CNT` 以 1MHz 频率递增，所以 1 个计数就是 1us。

本课并不直接在代码里读 `TIM3->CNT`，因为输入捕获硬件会在边沿瞬间把 `CNT` 锁存进 `CCR1`。

如果 TIM3 没启动，`CNT` 不走，捕获值就没有时间意义。

### 6.7 `CCR1` 在输入捕获里是什么

`CCR1` 是 Capture/Compare Register 1，中文叫捕获/比较寄存器 1。

它属于 TIM3_CH1 捕获寄存器层。在输出 PWM 时，`CCR1` 是比较值；在输入捕获时，`CCR1` 是硬件锁存的边沿时间戳。

本课最容易混淆的是：`TIM2->CCR1 = 500` 是 PWM 占空比；`TIM3->CCR1` 是 PA6 上升沿发生时的 `CNT` 值。两个寄存器名字一样，但属于不同定时器、不同模式、不同含义。

如果把 TIM3 的 `CCR1` 当作要写入的比较值，就会完全误解输入捕获。

### 6.8 `CCMR1.CC1S` 是什么

`CC1S` 是 Capture/Compare 1 Selection，中文叫通道 1 选择字段。

它属于 TIM3 捕获/比较模式寄存器字段。它决定通道 1 是输出比较还是输入捕获，以及输入来自 TI1、TI2 还是 TRC。

本课设置：

```c
TIM3->CCMR1 |= TIM_CCMR1_CC1S_0;
```

也就是 `CC1S = 01`，表示通道 1 作为输入，映射到 TI1。对 TIM3_CH1 来说，TI1 对应 PA6。

### 6.9 `IC1PSC` 是什么

`IC1PSC` 是 Input Capture 1 Prescaler，中文叫输入捕获 1 预分频。

它属于输入捕获边沿分频层。本课清零，让 `IC1PSC = 00`，表示每个有效边沿都捕获。

如果设置成分频，比如每 2 个边沿捕获一次，测得的周期就会变成两个周期的间隔，不再是单周期。

### 6.10 `IC1F` 是什么

`IC1F` 是 Input Capture 1 Filter，中文叫输入捕获 1 滤波器。

它属于输入信号滤波层。滤波器可以要求输入电平稳定若干采样后才认为边沿有效，用来抑制噪声。

本课信号来自 PA0 到 PA6 的短线，质量稳定，所以设置 `IC1F = 0000`，不滤波。

如果外部信号噪声大，可以适当增加滤波，但滤波会引入边沿确认延迟。

### 6.11 `CCER.CC1E` 是什么

`CC1E` 是 Capture/Compare 1 Enable，中文叫通道 1 捕获/比较使能。

它属于 TIM3 通道使能层。在输入捕获模式下，`CC1E = 1` 表示允许通道 1 捕获有效边沿。

本课代码：

```c
TIM3->CCER |= TIM_CCER_CC1E;
```

如果不打开它，PA6 有上升沿也不会更新 `CCR1`。

### 6.12 `CC1P/CC1NP` 是什么

`CC1P` 和 `CC1NP` 是通道 1 极性选择位。

它们属于捕获边沿选择层。本课清掉这两个位：

```c
TIM3->CCER &= ~(TIM_CCER_CC1P | TIM_CCER_CC1NP);
```

表示上升沿捕获。

如果改成下降沿，仍然能测周期，但捕获的是每个周期的下降沿时间戳。若改成双边沿，差值会变成高电平宽度、低电平宽度交替出现。

### 6.13 `SR.CC1IF` 是什么

`CC1IF` 是 Capture/Compare 1 Interrupt Flag，中文叫通道 1 捕获/比较中断标志。

它属于 TIM3 状态标志层。每次 CH1 捕获到新值后，硬件置位 `SR.CC1IF`，表示 `CCR1` 有新捕获值。

寄存器版 ISR 里先判断它，再读取 `CCR1`，最后清掉它。

如果不清 `CC1IF`，可能反复进入 TIM3 中断。

### 6.14 `DIER.CC1IE` 是什么

`CC1IE` 是 Capture/Compare 1 Interrupt Enable，中文叫通道 1 捕获/比较中断使能。

它属于 TIM3 中断使能层。`CC1IF` 是“有事发生”，`CC1IE` 是“允许这件事发中断”。

本课代码：

```c
TIM3->DIER |= TIM_DIER_CC1IE;
```

如果 `CC1IF` 会置位但 `CC1IE` 没开，软件必须轮询 `SR`，CPU 不会自动进 TIM3 中断。

### 6.15 `uint16_t` 回绕差值是什么

这是 C 语言无符号整数运算在输入捕获中的工程用法。

它属于 C 语言/CMSIS 数据处理层。TIM3 是 16 位计数器，`ARR = 0xFFFF`，`CNT` 从 65535 回到 0。用 `uint16_t` 保存捕获值时，本次减上次会按 65536 取模。

例如上次 65500，本次 200，`200 - 65500` 按 16 位无符号结果是 236，正好等于真实经过计数。

前提是两次捕获之间最多回绕一次。本课 1kHz 周期是 1000us，远小于 65536us。

### 6.16 `volatile` 是什么

`volatile` 是 C 语言关键字，中文常说易变变量。

它属于 C 代码和编译器优化层。本课 `g_last_capture`、`g_period_ticks`、`g_capture_ready` 在中断里写，在主循环里读，所以必须让编译器每次都从内存重新读取。

如果没有 `volatile`，编译器可能把主循环里的读取优化成缓存值，导致主循环看不到中断更新。

### 6.17 `HAL_TIM_IC_Start_IT()` 是什么

`HAL_TIM_IC_Start_IT()` 是 HAL 的输入捕获中断启动接口。

它属于 HAL 运行控制层。它会启用指定通道的捕获、中断和计数器运行。对应寄存器版的 `CC1IE`、清标志、`CEN` 等启动动作。

如果只调用 `HAL_TIM_IC_Start()`，不带 `_IT`，捕获可以发生，但不会进入 HAL 捕获回调。

### 6.18 `HAL_TIM_IC_CaptureCallback()` 是什么

这是 HAL 的输入捕获回调函数。

它属于 HAL 用户业务层。`TIM3_IRQHandler()` 调用 `HAL_TIM_IRQHandler(&htim3)` 后，HAL 检测到 TIM3_CH1 捕获事件，就调用这个回调。

本课在回调里读取捕获值、计算周期、设置 `g_capture_ready`。

### 6.19 `HAL_TIM_ReadCapturedValue()` 是什么

`HAL_TIM_ReadCapturedValue()` 是 HAL 读取捕获寄存器的接口。

它属于 HAL 寄存器访问封装层。本课：

```c
HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1)
```

本质上读取的是 `TIM3->CCR1`。HAL 返回 `uint32_t`，但本课 TIM3 是 16 位计数器，实际有效范围仍是 0 到 65535。

## 7. 寄存器版代码逐步讲解

### 7.1 全局变量为什么这样定义

代码：

```c
static volatile uint16_t g_last_capture = 0U;
static volatile uint16_t g_period_ticks = 0U;
static volatile uint8_t  g_capture_ready = 0U;
```

`g_last_capture` 保存上一次 `CCR1` 时间戳。`g_period_ticks` 保存本次和上次的差值。`g_capture_ready` 是中断通知主循环的新数据标志。

这些变量在 `TIM3_IRQHandler()` 中写，在 `main()` 的 `while` 中读，所以使用 `volatile`。

### 7.2 系统时钟和 TIM 时钟基础

`system_clock_72mhz_init()` 把 HSE 8MHz 经 PLL x9 配成 72MHz。

TIM2 和 TIM3 都挂在 APB1。APB1 分频为 2 时，PCLK1 是 36MHz，但定时器时钟会变成 2 倍 PCLK1，也就是 72MHz。本课所有 `PSC = 72 - 1` 都建立在这个前提上。

### 7.3 PC13 LED 初始化

PC13 配成推挽输出：

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
GPIOC->CRH |= GPIO_CRH_MODE13_1;
GPIOC->BSRR = GPIO_BSRR_BS13;
```

PC13 在 `CRH`，初始写高电平让 LED 熄灭。主循环后面根据测量结果写 `BRR` 点亮或写 `BSRR` 熄灭。

### 7.4 TIM2 在 PA0 输出 1kHz PWM

`tim2_pwm_output_init()` 先打开 GPIOA、AFIO、TIM2 时钟，再把 PA0 配成复用推挽输出。

关键参数：

```c
TIM2->PSC = 72U - 1U;
TIM2->ARR = 1000U - 1U;
TIM2->CCR1 = 500U;
```

TIM2 计数频率是 1MHz，`ARR=999` 得到 1kHz PWM，`CCR1=500` 得到 50% 占空比。

### 7.5 TIM2 通道设置为 PWM mode 1

代码：

```c
TIM2->CCMR1 &= ~(TIM_CCMR1_CC1S | TIM_CCMR1_OC1M);
TIM2->CCMR1 |= TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2;
TIM2->CCMR1 |= TIM_CCMR1_OC1PE;
TIM2->CCER |= TIM_CCER_CC1E;
TIM2->EGR |= TIM_EGR_UG;
TIM2->CR1 |= TIM_CR1_CEN;
```

这部分和 PWM 基础课一致：通道 1 作为输出，`OC1M=110`，打开预装载，打开通道输出，产生更新事件，启动计数器。

这不是本课测量端，但它提供稳定被测信号。

### 7.6 PA6 配成浮空输入

代码：

```c
GPIOA->CRL &= ~(GPIO_CRL_MODE6 | GPIO_CRL_CNF6);
GPIOA->CRL |= GPIO_CRL_CNF6_0;
```

`MODE6=00` 表示输入模式，`CNF6=01` 表示浮空输入。

PA6 被 PA0 的推挽输出直接驱动，不需要内部上拉或下拉。加上拉/下拉反而会干扰外部信号边沿。

### 7.7 TIM3 设置 1us 计数时间轴

代码：

```c
RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
TIM3->PSC = 72U - 1U;
TIM3->ARR = 0xFFFFU;
```

`PSC=71` 让 TIM3 以 1MHz 计数，1 个 tick 是 1us。`ARR=0xFFFF` 让 16 位计数器跑满，最大一轮是 65536us。

本课 1kHz 信号周期约 1000us，远小于这个范围。

### 7.8 TIM3_CH1 设为输入捕获

代码：

```c
TIM3->CCMR1 &= ~(TIM_CCMR1_CC1S | TIM_CCMR1_IC1PSC | TIM_CCMR1_IC1F);
TIM3->CCMR1 |= TIM_CCMR1_CC1S_0;
```

清掉旧字段后，`CC1S=01` 表示通道 1 作为输入，映射到 TI1，也就是 TIM3_CH1 的输入路径。

`IC1PSC=00` 表示每个边沿捕获，`IC1F=0000` 表示不滤波。

### 7.9 选择上升沿并打开捕获

代码：

```c
TIM3->CCER &= ~(TIM_CCER_CC1P | TIM_CCER_CC1NP);
TIM3->CCER |= TIM_CCER_CC1E;
```

清 `CC1P/CC1NP` 选择上升沿捕获。置 `CC1E` 使能通道 1 捕获。

PA6 上每个上升沿到来时，TIM3 把当前 `CNT` 保存到 `CCR1`。

### 7.10 打开捕获中断并启动 TIM3

代码：

```c
TIM3->DIER |= TIM_DIER_CC1IE;
TIM3->SR &= ~TIM_SR_CC1IF;
NVIC_SetPriority(TIM3_IRQn, 1U);
NVIC_EnableIRQ(TIM3_IRQn);
TIM3->CR1 |= TIM_CR1_CEN;
```

`CC1IE` 让捕获事件可触发中断。启动前清一次 `CC1IF`，避免残留标志。NVIC 放行 `TIM3_IRQn`。`CEN` 启动计数器。

TIM3 中断能进 CPU，必须同时满足 TIM3 内部中断使能和 NVIC 使能。

### 7.11 `TIM3_IRQHandler()` 判断捕获标志

代码：

```c
if ((TIM3->SR & TIM_SR_CC1IF) != 0U) {
```

TIM3 可能有多个中断源。进入 TIM3 全局中断后，先判断是不是通道 1 捕获事件。

如果不判断来源，未来加入更新中断或其他通道时会误处理。

### 7.12 读取 `CCR1` 并计算差值

代码：

```c
uint16_t current_capture = (uint16_t)TIM3->CCR1;
g_period_ticks = (uint16_t)(current_capture - g_last_capture);
g_last_capture = current_capture;
```

`current_capture` 是本次上升沿时间戳。上次捕获值保存在 `g_last_capture`。两者相减得到周期 tick。

因为 TIM3 计数频率是 1MHz，`g_period_ticks = 1000` 就表示周期 1000us。

### 7.13 设置 ready 标志并清 `CC1IF`

代码：

```c
g_capture_ready = 1U;
TIM3->SR &= ~TIM_SR_CC1IF;
```

中断只负责快速保存数据并通知主循环。清 `CC1IF` 让下一次捕获中断能正常到来。

注意这里的 TIM3 `SR.CC1IF` 是通过清 0 方式清除，不是 EXTI 那种写 1 清 pending。

### 7.14 主循环判断周期范围

代码：

```c
if ((g_period_ticks >= 990U) && (g_period_ticks <= 1010U)) {
    GPIOC->BRR = GPIO_BRR_BR13;
} else {
    GPIOC->BSRR = GPIO_BSRR_BS13;
}
```

理论周期是 1000us。代码允许 990 到 1010 的容差。测得正确时 PC13 输出低电平，LED 点亮；否则输出高电平，LED 熄灭。

### 7.15 为什么主循环不清 `g_capture_ready`

代码注释说明这里选择不清标志，让主循环持续判断最新有效数据。

如果每次处理后清零，在两次捕获之间 LED 可能出现短暂不稳定显示。本课更关注测量结果是否稳定，所以保留 ready 状态。

## 8. HAL 版代码逐步讲解

### 8.1 `HAL_Init()` 和句柄

HAL 版先调用 `HAL_Init()`，再使用两个定时器句柄：

```c
static TIM_HandleTypeDef htim2;
static TIM_HandleTypeDef htim3;
```

`htim2` 表示 TIM2 PWM 输出，`htim3` 表示 TIM3 输入捕获。句柄里包含 `Instance` 和 `Init` 配置。

### 8.2 HAL 版时钟配置

`RCC_OscInitTypeDef` 配置 HSE 和 PLL，`RCC_ClkInitTypeDef` 配置 SYSCLK、AHB、APB1、APB2。

目标和寄存器版相同：SYSCLK 72MHz，APB1 分频 /2，定时器输入按 72MHz 用于 `PSC = 72 - 1`。

### 8.3 HAL 配置 PC13

`led_pc13_init()` 用 `GPIO_InitTypeDef` 配置 PC13 为 `GPIO_MODE_OUTPUT_PP`，再用 `HAL_GPIO_WritePin()` 写高电平让 LED 初始熄灭。

这对应寄存器版 GPIOC 时钟、`CRH` 模式位和 `BSRR`。

### 8.4 HAL 配置 PA0 PWM 输出脚

代码：

```c
gpio.Pin = GPIO_PIN_0;
gpio.Mode = GPIO_MODE_AF_PP;
gpio.Pull = GPIO_NOPULL;
gpio.Speed = GPIO_SPEED_FREQ_LOW;
HAL_GPIO_Init(GPIOA, &gpio);
```

`GPIO_MODE_AF_PP` 对应 PA0 复用推挽输出，让 TIM2_CH1 可以驱动 PA0。

### 8.5 HAL 配置 PA6 输入脚

代码：

```c
gpio.Pin = GPIO_PIN_6;
gpio.Mode = GPIO_MODE_INPUT;
gpio.Pull = GPIO_NOPULL;
HAL_GPIO_Init(GPIOA, &gpio);
```

PA6 作为 TIM3_CH1 输入捕获脚。HAL 中没有“复用输入”模式，本课把它配置成普通输入且不上拉下拉。

### 8.6 `HAL_TIM_PWM_Init()` 配 TIM2 时基

TIM2 句柄配置：

```c
htim2.Instance = TIM2;
htim2.Init.Prescaler = 72U - 1U;
htim2.Init.Period = 1000U - 1U;
```

`Prescaler` 对应 `TIM2->PSC`，`Period` 对应 `TIM2->ARR`。这让 TIM2 输出 1kHz PWM 的时间轴。

### 8.7 `HAL_TIM_PWM_ConfigChannel()` 配 TIM2_CH1

`TIM_OC_InitTypeDef` 字段：

```c
sConfigOC.OCMode = TIM_OCMODE_PWM1;
sConfigOC.Pulse = 500U;
sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
```

`OCMode` 对应 `OC1M=110`，`Pulse` 对应 `TIM2->CCR1=500`，`OCPolarity` 对应输出有效极性。

`HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)` 在 `main()` 中启动 PWM 输出。

### 8.8 `HAL_TIM_IC_Init()` 配 TIM3 时基

TIM3 句柄配置：

```c
htim3.Instance = TIM3;
htim3.Init.Prescaler = 72U - 1U;
htim3.Init.Period = 0xFFFFU;
```

这对应寄存器版 `TIM3->PSC = 71` 和 `TIM3->ARR = 0xFFFF`。TIM3 计数频率是 1MHz，捕获值单位是 1us。

### 8.9 `TIM_IC_InitTypeDef` 配捕获通道

代码：

```c
sConfigIC.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
sConfigIC.ICSelection = TIM_ICSELECTION_DIRECTTI;
sConfigIC.ICPrescaler = TIM_ICPSC_DIV1;
sConfigIC.ICFilter = 0U;
```

`ICPolarity` 对应上升沿捕获。`ICSelection = DIRECTTI` 对应 CH1 直接连接 TI1。`ICPrescaler = DIV1` 对应每个边沿都捕获。`ICFilter = 0` 对应不滤波。

### 8.10 `HAL_TIM_IC_ConfigChannel()` 落地通道配置

代码：

```c
HAL_TIM_IC_ConfigChannel(&htim3, &sConfigIC, TIM_CHANNEL_1)
```

它把捕获极性、输入选择、捕获分频、滤波等字段写入 TIM3_CH1 相关寄存器。

它对应寄存器版设置 `CCMR1.CC1S`、`IC1PSC`、`IC1F`、`CCER.CC1P/CC1NP`、`CC1E`。

### 8.11 `HAL_TIM_IC_Start_IT()` 启动捕获中断

主函数中：

```c
HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_1)
```

它启动 TIM3_CH1 输入捕获中断。对应寄存器版的 `DIER.CC1IE`、清标志、`CR1.CEN` 等动作。

如果调用普通 `HAL_TIM_IC_Start()`，不会进入捕获回调。

### 8.12 `TIM3_IRQHandler()` 交给 HAL 分发

HAL 版中断入口：

```c
void TIM3_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim3);
}
```

`HAL_TIM_IRQHandler()` 会检查 TIM3 的标志和中断使能，判断是哪个事件触发，再调用对应回调。

### 8.13 `HAL_TIM_IC_CaptureCallback()` 处理捕获

回调里先判断：

```c
if (htim->Instance == TIM3 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
```

这能确认来源是 TIM3 的通道 1。然后读取捕获值、计算差值、设置 ready 标志。

### 8.14 `HAL_TIM_ReadCapturedValue()` 读取 `CCR1`

代码：

```c
uint32_t current_capture = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
```

本质上读取 `TIM3->CCR1`。HAL 返回 `uint32_t`，代码用差值后转成 `uint16_t`，保持和 16 位计数器回绕逻辑一致。

### 8.15 `SysTick_Handler()` 和 `error_handler()`

`SysTick_Handler()` 调用 `HAL_IncTick()`，维护 HAL tick。当前主循环不使用 `HAL_Delay()`，但 HAL 工程保留它是标准做法。

`error_handler()` 关闭中断并死循环。如果 HAL 初始化或启动返回错误，程序停在这里，方便调试器查看调用栈。

## 9. 两个版本真正应该怎么学

寄存器版要把 TIM2 和 TIM3 分开看：

```text
TIM2：输出端，CCR1 是 PWM 比较值
TIM3：输入端，CCR1 是边沿时间戳
```

然后看 TIM3 捕获链：

```text
PA6 -> TIM3_CH1/TI1 -> 上升沿 -> CNT 锁存到 CCR1 -> CC1IF -> CC1IE -> TIM3_IRQn -> ISR
```

HAL 版要看启动和回调链：

```text
HAL_TIM_IC_Start_IT -> TIM3_IRQHandler -> HAL_TIM_IRQHandler -> HAL_TIM_IC_CaptureCallback -> HAL_TIM_ReadCapturedValue
```

只要能把 `TIM_IC_InitTypeDef` 的字段翻译回 `CCMR1` 和 `CCER`，HAL 版就不再是黑箱。

## 10. 检验问题清单

### 10.1 为什么本课要用 PA0 接 PA6？

答：PA0 是 TIM2_CH1 输出端，PA6 是 TIM3_CH1 输入端。用一根线连接后，TIM3 才能测量 TIM2 输出的信号。

### 10.2 捕获差值 1000 表示什么？

答：TIM3 计数频率是 1MHz，1 个 tick 是 1us。差值 1000 表示两个上升沿间隔 1000us，也就是 1ms，对应 1kHz。

### 10.3 `TIM2->CCR1` 和 `TIM3->CCR1` 含义一样吗？

答：不一样。TIM2 的 `CCR1` 在 PWM 输出中是比较值，决定占空比；TIM3 的 `CCR1` 在输入捕获中是边沿时间戳。

### 10.4 `CC1IF` 置位但 `CC1IE` 没开会怎样？

答：捕获值可能已经写入 `CCR1`，`CC1IF` 也会置位，但不会自动进入 TIM3 中断。软件需要轮询标志。

### 10.5 为什么 `uint16_t` 差值能处理回绕？

答：16 位无符号减法按 65536 取模。只要两次捕获间隔小于 65536 个 tick，本次减上次就能得到真实经过计数。

### 10.6 为什么中断里只算周期和置标志？

答：ISR 应尽量短。捕获中断负责保存关键数据，主循环负责判断周期和控制 LED，这样不会长时间占用中断。

### 10.7 HAL 版为什么必须用 `HAL_TIM_IC_Start_IT()`？

答：带 `_IT` 的启动会打开捕获中断并让 HAL 回调链工作。普通 Start 不会进入 `HAL_TIM_IC_CaptureCallback()`。

### 10.8 HAL 回调里为什么判断 `htim->Instance` 和 `htim->Channel`？

答：多个定时器或多个通道都可能触发同一个回调。判断来源能保证只处理 TIM3_CH1 的捕获事件。

## 11. 工程实现步骤

### 11.1 需求分析

需求是：生成一个已知 1kHz 信号，再用输入捕获测出它的周期。

这要求输出端 TIM2 正常生成 PWM，物理线把 PA0 接到 PA6，输入端 TIM3 以 1us 分辨率运行，CH1 捕获上升沿，中断把捕获差值交给主循环判断。

### 11.2 硬件核查

必须确认 PA0 和 PA6 已用杜邦线连接。

不要只下载程序就期待捕获成功。TIM2 的输出和 TIM3 的输入在芯片内部不会自动相连，本课依赖外部连线。

如果用示波器，可以先测 PA0 是否有 1kHz PWM，再测 PA6 是否收到同样波形。

### 11.3 寄存器路线

寄存器版按这个顺序实现：

1. 配置系统时钟到 72MHz。
2. 配置 PC13 输出。
3. 配置 PA0 为 TIM2_CH1 复用推挽输出。
4. 配置 TIM2 为 1kHz PWM。
5. 配置 PA6 为浮空输入。
6. 配置 TIM3 `PSC=72-1`、`ARR=0xFFFF`。
7. 设置 TIM3_CH1 为输入捕获，直接连接 TI1。
8. 选择上升沿捕获。
9. 打开 CH1 捕获和捕获中断。
10. 清 `CC1IF`。
11. NVIC 使能 `TIM3_IRQn`。
12. 启动 TIM3。
13. 在 `TIM3_IRQHandler()` 中读取 `CCR1` 并计算差值。

### 11.4 HAL 路线

HAL 版按这个顺序实现：

1. `HAL_Init()`。
2. 配置系统时钟。
3. 配置 PC13 输出。
4. 配置 PA0 为 `GPIO_MODE_AF_PP`。
5. 配置 PA6 为 `GPIO_MODE_INPUT`、`GPIO_NOPULL`。
6. `HAL_TIM_PWM_Init()` 配 TIM2 时基。
7. `HAL_TIM_PWM_ConfigChannel()` 配 TIM2_CH1 PWM。
8. `HAL_TIM_IC_Init()` 配 TIM3 时基。
9. `HAL_TIM_IC_ConfigChannel()` 配 TIM3_CH1 捕获。
10. `HAL_NVIC_EnableIRQ(TIM3_IRQn)`。
11. `HAL_TIM_PWM_Start()` 启动 TIM2 输出。
12. `HAL_TIM_IC_Start_IT()` 启动 TIM3 捕获中断。
13. 在 `HAL_TIM_IC_CaptureCallback()` 中读取捕获值。

### 11.5 工程思维

输入捕获的价值是把边沿时刻交给硬件记录。CPU 响应中断有延迟，但捕获时间戳已经在边沿瞬间保存好了。

测周期时，软件不应该用延时估计，也不应该靠中断进入时间估计，而应该读两次硬件锁存值相减。

本课自发自测是很好的调试方法：先用 MCU 自己生成一个已知信号，确认输入捕获链路正确，再去测外部未知信号。

### 11.6 常见工程陷阱

第一个陷阱是忘记 PA0 到 PA6 的物理连线。

第二个陷阱是把 PA6 配成复用输出。输入捕获需要输入脚，本课 PA6 配成浮空输入。

第三个陷阱是混淆两个 `CCR1`：TIM2 的用于输出 PWM，TIM3 的用于输入捕获。

第四个陷阱是只启动输入捕获不启动中断，导致回调不进。

第五个陷阱是忘记清 `CC1IF`，导致中断重复进入。

## 12. 运行现象

PA0 接 PA6 后，TIM3 会持续捕获 PA0 的上升沿。

`g_period_ticks` 应该稳定在 1000 附近。主循环判断它在 990 到 1010 之间时，PC13 输出低电平，板载 LED 点亮。

如果断开 PA0 到 PA6，捕获数据不再更新，LED 可能保持旧状态或不再反映新的测量结果。

## 13. 常见问题排查

### 13.1 PC13 不亮

先查 PA0 到 PA6 是否连好。再查 PA0 是否真的有 1kHz PWM 输出。

如果 PA0 有波形但 PA6 没波形，问题是接线或引脚测错。如果 PA6 有波形但 `g_period_ticks` 不更新，问题在 TIM3 捕获配置或中断链路。

### 13.2 `g_period_ticks` 不是 1000 附近

重新计算 TIM2 和 TIM3 的时钟。两者都按 72MHz 输入、`PSC=72-1` 时才是 1MHz 计数。

还要确认 TIM2 的 `ARR=999`。如果把 PWM 改成 500Hz，捕获周期应接近 2000。

### 13.3 只捕获一次

检查 `CC1IF` 是否清除，`CC1IE` 是否打开，NVIC 是否使能 `TIM3_IRQn`。

HAL 版检查是否调用 `HAL_TIM_IC_Start_IT()`，不是普通 `HAL_TIM_IC_Start()`。

### 13.4 HAL 版不进回调

检查 `TIM3_IRQHandler()` 是否调用 `HAL_TIM_IRQHandler(&htim3)`。

再检查回调里的判断是否正确：`htim->Instance == TIM3`，`htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1`。

### 13.5 捕获值抖动很大

本课 PA0 到 PA6 短线通常不需要滤波。如果外部信号来自长线或噪声环境，可以增大 `ICFilter`。

也要确认 PA6 没有悬空，GND 参考一致，信号边沿足够干净。

## 14. 本课最核心的结论

- 输入捕获的本质是边沿到来时硬件锁存 `CNT` 到 `CCR1`。
- TIM2_CH1 是本课的信号源，TIM3_CH1 是测量端。
- PA0 到 PA6 的物理连线是实验成立的关键。
- TIM3 以 1MHz 计数时，捕获差值的单位就是微秒。
- `CC1S` 决定通道进入输入捕获，`CC1P/CC1NP` 决定捕获边沿，`CC1E` 决定是否捕获。
- `CC1IF` 是有新捕获值，`CC1IE` 是允许它触发中断。
- HAL 的输入捕获回调链最终仍然是在读 `TIM3->CCR1`。

## 15. 建议你现在怎么读这节课

先画出物理线：`TIM2 -> PA0 -> 杜邦线 -> PA6 -> TIM3`。

再把两个定时器分开：TIM2 只负责造信号，TIM3 只负责测信号。

最后盯住 `TIM3_IRQHandler()` 或 `HAL_TIM_IC_CaptureCallback()` 里的三步：读本次捕获值、减上次捕获值、保存本次捕获值。

## 16. 扩展练习

1. 把 TIM2 PWM 改成 500Hz，观察 `g_period_ticks` 是否接近 2000。
2. 把 TIM2 PWM 改成 2kHz，观察 `g_period_ticks` 是否接近 500。
3. 改成下降沿捕获，观察周期测量是否仍然正确。
4. 尝试双边沿捕获，观察差值为什么会变成高/低电平宽度交替。
5. 增大 `ICFilter`，理解输入滤波对噪声信号的影响。
6. 在调试器里观察 `TIM3->CCR1`、`g_last_capture`、`g_period_ticks`。

## 17. 下一课预告

上一课：[10_exti](../10_exti/README.md)

下一课：[12_timer_pwm_input](../12_timer_pwm_input/README.md)

下一课会学习 PWM 输入模式。它会用定时器硬件同时捕获周期和高电平时间，比本课手动用两次捕获值相减更进一步。
