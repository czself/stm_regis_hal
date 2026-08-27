# 13_timer_encoder - TIM 编码器接口

## 1. 本课到底在学什么

本课表面现象是：旋转增量式编码器时，TIM3 的 `CNT` 会自动增加或减少；主循环发现 `CNT` 变化后，翻转一次 PC13 LED。

真正要学的是 TIM 编码器接口。编码器 A/B 两相信号接入 TIM3_CH1 和 TIM3_CH2 后，TIM3 硬件根据两相信号的先后关系判断方向，并让计数器 `CNT` 自动加减。软件不需要在 GPIO 中断里手动判断 A/B 状态。

这节课接在 PWM 输入模式之后。前面几课用定时器“测边沿时间”；本课让外部边沿直接成为计数器的驱动来源。普通定时模式下 `CNT` 由内部时钟推动，编码器模式下 `CNT` 由外部 A/B 相推动。

## 2. 本课学习目标

学完本课，你应该能做到：

- 解释增量式编码器为什么需要 A/B 两相，而不是一根线。
- 说明 PA6/PA7 为什么分别接 TIM3_CH1/TIM3_CH2。
- 解释输入上拉对机械编码器或开漏输出模块的意义。
- 看懂 `CCMR1.CC1S/CC2S` 如何把两个通道设成输入。
- 说明 `SMCR.SMS = encoder mode 3` 为什么会让外部 A/B 相驱动 `CNT`。
- 区分 `CNT` 的数值变化和方向判断。
- 解释 A/B 相接反时为什么方向反过来。
- 看懂 HAL 版 `TIM_Encoder_InitTypeDef` 的 `EncoderMode`、`ICxSelection`、`ICxPolarity`、`ICxFilter`。
- 说明 `HAL_TIM_Encoder_Start()` 和 `__HAL_TIM_GET_COUNTER()` 分别对应底层哪一步。

## 3. 本课目录结构

```text
13_timer_encoder/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接配置 GPIOA、TIM3 的通道输入和编码器模式。

`hal/` 使用 `TIM_Encoder_InitTypeDef` 配置编码器接口，并用 `__HAL_TIM_GET_COUNTER()` 读取计数器。

两份工程都使用 `genericSTM32F103C8`、`stm32cube`、`stlink`，并定义 `HSE_VALUE=8000000U`。

## 4. 实验硬件

本课使用：

- STM32F103C8T6 BluePill
- ST-Link 下载器
- 增量式旋转编码器
- PA6 接编码器 A 相clk
- PA7 接编码器 B 相dt
- PC13 板载 LED

常见接法：

```text
编码器 A 相 ---- PA6
编码器 B 相 ---- PA7
编码器公共端 ---- GND
```

本课代码给 PA6、PA7 开内部上拉，适合常见机械编码器或开漏输出编码器模块。如果你的编码器模块已经带外部上拉，内部上拉通常也不会妨碍基础实验。

## 5. 先建立一个最基本的脑图

本课按六层拆开看。

现象层：旋转编码器时，调试器里 `TIM3->CNT` 或 HAL 版读取值会变化；主循环检测到变化后 PC13 翻转。

物理/硬件层：编码器输出 A/B 两相方波，两者相差约 90 度。谁领先谁决定旋转方向。PA6 接 A 相，PA7 接 B 相；交换两根线会反转方向。

芯片模块层：GPIOA 配置 PA6/PA7 输入上拉，TIM3_CH1/TIM3_CH2 接收 A/B 相，TIM3 编码器接口负责解码和计数，GPIOC 负责 PC13 指示。

寄存器/bit 层：`CCMR1.CC1S/CC2S` 把通道 1/2 设为输入，`CCER` 选择极性，`SMCR.SMS=011` 进入 encoder mode 3，`CNT` 保存当前位置计数，`CR1.CEN` 启动编码器计数。

C/CMSIS 层：寄存器版直接读 `TIM3->CNT`，用 `last` 和 `now` 比较判断是否变化，再调用 `pc13_toggle()`。

HAL/工程层：HAL 版用 `TIM_Encoder_InitTypeDef` 描述编码器模式、两路输入选择、极性和滤波；用 `HAL_TIM_Encoder_Start()` 启动；用 `__HAL_TIM_GET_COUNTER()` 读取计数值。

完整链路是：

1. 系统时钟配置到 72MHz。
2. PC13 配成输出，初始熄灭。
3. GPIOA 时钟打开，PA6/PA7 配成输入上拉。
4. TIM3 时钟打开。
5. TIM3 设置 `PSC = 0`，`ARR = 0xFFFF`。
6. CH1 配成输入，接 TI1，也就是 PA6/A 相。
7. CH2 配成输入，接 TI2，也就是 PA7/B 相。
8. `SMCR.SMS` 设置 encoder mode 3。
9. `CNT` 清零。
10. `CEN` 启动 TIM3。
11. 编码器旋转，A/B 相边沿进入 TIM3。
12. TIM3 硬件判断方向并自动修改 `CNT`。
13. 主循环读取 `CNT`，发现变化就翻转 PC13。

## 6. 核心名词解释

### 6.1 增量式编码器是什么

增量式编码器是一种输出相对位移脉冲的传感器。

它属于外部传感器层。它通常输出 A/B 两相信号，旋转时产生脉冲；上电后不知道绝对位置，只能从当前值开始累计变化。

本课用它作为 TIM3 编码器接口的输入源。旋转一次产生多少脉冲，取决于编码器规格和是否按倍频计数。

如果需要上电就知道绝对角度，那不是增量式编码器的能力，需要绝对值编码器或回零过程。

### 6.2 A/B 相是什么

A/B 相是编码器输出的两路相位错开的方波。

它属于物理信号层。两路信号相差约 90 度，TIM3 可以根据哪一路先变化判断方向。

本课 A 相接 PA6，B 相接 PA7。如果接反，计数方向会反过来。

如果只接一相，硬件只能知道有脉冲，不能可靠知道方向。

### 6.3 PA6/PA7 是什么

PA6 和 PA7 是 GPIOA 的 6、7 号引脚。

它们属于物理引脚层和 TIM3 输入层。PA6 对应 TIM3_CH1，PA7 对应 TIM3_CH2。

寄存器版在 `GPIOA->CRL` 中配置它们，HAL 版用 `GPIO_PIN_6 | GPIO_PIN_7`。

如果接到其他引脚而不改定时器通道映射，TIM3 不会收到编码器信号。

### 6.4 输入上拉是什么

输入上拉表示引脚在没有被外部拉低时默认保持高电平。

它属于 GPIO 电气层。机械编码器常见接法是触点闭合时接地，松开时靠上拉为高。开漏输出模块也需要上拉才能产生高电平。

寄存器版 `CNF6/CNF7=10` 加 `ODR6/ODR7=1`，HAL 版 `GPIO_PULLUP`。

如果没有上拉，输入悬空会抖动或随机跳变。

### 6.5 TIM3 编码器接口是什么

TIM3 编码器接口是定时器的一种特殊从模式。

它属于 TIM 外设解码层。它把 TI1/TI2 的边沿作为计数器输入，并根据另一路电平决定加计数还是减计数。

本课通过 `SMCR.SMS=011` 启用 encoder mode 3，让 TI1 和 TI2 都参与计数。

如果没启用编码器模式，`CNT` 仍然由内部时钟计数，不会表示编码器位置。

### 6.6 `TIM3_CH1/CH2` 是什么

`TIM3_CH1` 和 `TIM3_CH2` 是 TIM3 的两个通道。

它们属于定时器通道层。本课 CH1 接 PA6/TI1，CH2 接 PA7/TI2。两个通道都配置为输入。

寄存器版用 `CCMR1` 设置，HAL 版用 `TIM_Encoder_InitTypeDef` 的 `IC1Selection`、`IC2Selection`。

### 6.7 `CCMR1.CC1S/CC2S` 是什么

`CC1S` 和 `CC2S` 是通道选择字段。

它们属于 TIM3 捕获/比较模式寄存器层。本课代码：

```c
TIM3->CCMR1 = TIM_CCMR1_CC1S_0 | TIM_CCMR1_CC2S_0;
```

`CC1S=01` 表示 CH1 输入接 TI1。`CC2S=01` 表示 CH2 输入接 TI2。

如果通道仍是输出模式，编码器信号不会进入解码路径。

### 6.8 `SMCR.SMS` 是什么

`SMS` 是 Slave Mode Selection，中文叫从模式选择。

它属于 TIM3 从模式控制层。本课设置：

```c
TIM3->SMCR = TIM_SMCR_SMS_0 | TIM_SMCR_SMS_1;
```

也就是 `SMS=011`，encoder mode 3。该模式下 TI1 和 TI2 的边沿都能驱动计数器，硬件根据相位关系决定方向。

如果只选择普通定时器模式，`CNT` 不会由编码器 A/B 相控制。

### 6.9 encoder mode 3 是什么

encoder mode 3 是 TIM 编码器接口的一种模式。

它属于定时器编码器解码层。相较只用 TI1 或只用 TI2 的编码器模式，mode 3 使用 TI1 和 TI2 两路边沿计数，分辨率更高。

HAL 版用 `TIM_ENCODERMODE_TI12` 表示这个模式。

如果编码器抖动严重，mode 3 可能把抖动也计进去，需要滤波或消抖。

### 6.10 `CNT` 是什么

`CNT` 是 Counter，中文叫计数器当前值。

它属于 TIM3 位置计数层。在编码器模式下，`CNT` 代表相对位置计数，而不是内部时钟走过的时间。

本课主循环读 `TIM3->CNT` 或 `__HAL_TIM_GET_COUNTER(&htim3)`。

`CNT` 是 16 位，计到 65535 后会回到 0，反向也可能从 0 回到 65535，这是正常回绕。

### 6.11 `DIR` 是什么

`DIR` 是 Direction，中文叫计数方向位。

它属于 TIM3 控制状态层。编码器模式下，TIM3 硬件根据 A/B 相相位决定当前方向，并体现在计数增减上。

本课代码没有直接读取 `DIR`，而是通过 `CNT` 增减间接观察方向。

如果 A/B 相接反，`DIR` 判断会反，`CNT` 增减方向也会反。

### 6.12 `PSC=0` 是什么含义

`PSC` 是预分频器。

它属于 TIM3 时基配置层。但编码器模式下，计数脉冲主要来自外部 A/B 相，不是内部定时器时钟，所以本课把 `PSC` 设为 0，不对外部计数再分频。

如果把 `PSC` 设置成非 0，可能会让计数行为不符合你对编码器脉冲的直觉。

### 6.13 `ARR=0xFFFF` 是什么含义

`ARR` 是自动重装载寄存器。

它属于计数范围层。本课设置 `ARR=0xFFFF`，让 16 位计数器完整使用 0 到 65535 的范围。

编码器持续旋转时，超过范围会回绕。工程里如果需要长期位置，可以在溢出/下溢时扩展成 32 位软件位置。

### 6.14 `ICFilter` 是什么

`ICFilter` 是输入捕获滤波器。

它属于输入信号抗抖层。HAL 版设置 `IC1Filter=4`、`IC2Filter=4`，用来减轻机械编码器触点抖动。

寄存器版没有显式设置滤波，适合先观察最直接的硬件行为。

滤波值太小可能抖动，太大可能漏掉高速编码器脉冲。

### 6.15 `TIM_Encoder_InitTypeDef` 是什么

`TIM_Encoder_InitTypeDef` 是 HAL 的编码器配置结构体。

它属于 HAL 软件封装层。它把编码器模式、两路输入极性、输入选择、预分频、滤波放在一个结构体里。

本课 `EncoderMode = TIM_ENCODERMODE_TI12`，`IC1Selection/IC2Selection = DIRECTTI`，`IC1Filter/IC2Filter = 4`。

### 6.16 `HAL_TIM_Encoder_Start()` 是什么

`HAL_TIM_Encoder_Start()` 是 HAL 启动编码器接口的函数。

它属于 HAL 运行控制层。本课：

```c
HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL);
```

它启动 CH1、CH2，并启动 TIM3 计数器。只初始化不 Start，`CNT` 不会随编码器变化。

### 6.17 `__HAL_TIM_GET_COUNTER()` 是什么

`__HAL_TIM_GET_COUNTER()` 是 HAL 读取定时器 `CNT` 的宏。

它属于 HAL 寄存器访问封装层。对 `htim3` 来说，它本质上就是读取 `TIM3->CNT`。

本课主循环用它和 `last` 比较，检测编码器是否发生位置变化。

## 7. 寄存器版代码逐步讲解

### 7.1 系统时钟初始化

`system_clock_72mhz_init()` 配置 HSE、PLL x9、SYSCLK 72MHz。

编码器计数不靠内部时钟产生脉冲，但系统时钟仍然影响 CPU、GPIO、TIM 寄存器访问和整体工程一致性。

### 7.2 PC13 LED 初始化

代码打开 GPIOC 时钟，配置 PC13 为通用推挽输出，初始写高电平熄灭 LED。

PC13 只是运行指示。真正的位置变化看 `TIM3->CNT`。

### 7.3 打开 GPIOA 和 TIM3 时钟

代码：

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
```

PA6/PA7 属于 GPIOA，TIM3 挂在 APB1。配置它们前必须打开对应时钟。

### 7.4 PA6/PA7 配成输入上拉

代码：

```c
GPIOA->CRL &= ~(GPIO_CRL_MODE6 | GPIO_CRL_CNF6 | GPIO_CRL_MODE7 | GPIO_CRL_CNF7);
GPIOA->CRL |= GPIO_CRL_CNF6_1 | GPIO_CRL_CNF7_1;
GPIOA->BSRR = GPIO_BSRR_BS6 | GPIO_BSRR_BS7;
```

`MODE6/7=00` 表示输入，`CNF6/7=10` 表示上拉/下拉输入。`BSRR` 把 `ODR6/7` 置 1，选择上拉。

### 7.5 TIM3 基础范围配置

代码：

```c
TIM3->PSC = 0;
TIM3->ARR = 0xFFFFU;
```

编码器模式下，外部 A/B 相驱动 `CNT`，所以 `PSC=0`。`ARR=0xFFFF` 让计数器使用完整 16 位范围。

### 7.6 CH1/CH2 设置为输入

代码：

```c
TIM3->CCMR1 = TIM_CCMR1_CC1S_0 | TIM_CCMR1_CC2S_0;
```

CH1 输入接 TI1，CH2 输入接 TI2。这样 PA6/PA7 的 A/B 相进入定时器输入捕获路径。

### 7.7 `CCER = 0` 的含义

代码：

```c
TIM3->CCER = 0;
```

这清掉输入极性相关设置，保持默认极性。默认情况下按上升沿/常规极性进入编码器解码。

如果方向与预期相反，可以交换 A/B 相，或调整输入极性。

### 7.8 启用 encoder mode 3

代码：

```c
TIM3->SMCR = TIM_SMCR_SMS_0 | TIM_SMCR_SMS_1;
```

`SMS=011`，也就是 encoder mode 3。硬件开始根据 TI1/TI2 的相位关系更新 `CNT`。

这是本课最核心的一句。

### 7.9 清零并启动计数器

代码：

```c
TIM3->CNT = 0;
TIM3->CR1 = TIM_CR1_CEN;
```

先把相对位置清零，再启动 TIM3。之后旋转编码器，`CNT` 会由外部脉冲驱动变化。

### 7.10 主循环读取 `CNT`

代码：

```c
uint16_t now = (uint16_t)TIM3->CNT;
if (now != last) {
    last = now;
    pc13_toggle();
}
```

主循环不处理中断，只轮询计数值。只要编码器导致 `CNT` 改变，PC13 就翻转。

## 8. HAL 版代码逐步讲解

### 8.1 `HAL_Init()` 和系统时钟

HAL 版先调用 `HAL_Init()`，再用 HAL RCC 结构体配置 HSE、PLL 和总线分频。

这和寄存器版目标相同，都是让系统工作在 72MHz。

### 8.2 HAL 配置 PC13

`GPIO_InitTypeDef` 把 PC13 配成 `GPIO_MODE_OUTPUT_PP`，初始 `HAL_GPIO_WritePin(..., GPIO_PIN_SET)` 熄灭 LED。

这对应寄存器版 GPIOC `CRH` 和 `BSRR`。

### 8.3 HAL 配置 PA6/PA7 输入上拉

代码：

```c
gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
gpio.Mode = GPIO_MODE_INPUT;
gpio.Pull = GPIO_PULLUP;
HAL_GPIO_Init(GPIOA, &gpio);
```

它对应寄存器版 PA6/PA7 输入上拉配置。两个输入脚共同接收编码器 A/B 相。

### 8.4 `htim3.Init` 基础配置

代码：

```c
htim3.Instance = TIM3;
htim3.Init.Prescaler = 0;
htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
htim3.Init.Period = 0xFFFF;
htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
```

`Instance=TIM3` 选择外设。`Prescaler=0` 不分频。`Period=0xFFFF` 使用完整 16 位范围。

### 8.5 `EncoderMode = TIM_ENCODERMODE_TI12`

代码：

```c
enc.EncoderMode = TIM_ENCODERMODE_TI12;
```

它对应寄存器版 `SMCR.SMS=011`，也就是 encoder mode 3。TI1 和 TI2 都参与计数。

### 8.6 IC1/IC2 输入选择和极性

代码：

```c
enc.IC1Polarity = TIM_ICPOLARITY_RISING;
enc.IC1Selection = TIM_ICSELECTION_DIRECTTI;
enc.IC2Polarity = TIM_ICPOLARITY_RISING;
enc.IC2Selection = TIM_ICSELECTION_DIRECTTI;
```

CH1 direct 接 TI1，CH2 direct 接 TI2。两路极性都用 rising。极性设置会影响方向和计数边沿解释。

### 8.7 输入滤波和预分频

代码：

```c
enc.IC1Prescaler = TIM_ICPSC_DIV1;
enc.IC1Filter = 4;
enc.IC2Prescaler = TIM_ICPSC_DIV1;
enc.IC2Filter = 4;
```

`DIV1` 表示不对输入边沿分频。`Filter=4` 给两路输入加轻度滤波，适合机械编码器抖动较多的场景。

### 8.8 `HAL_TIM_Encoder_Init()`

代码：

```c
HAL_TIM_Encoder_Init(&htim3, &enc);
```

它把 `htim3.Init` 和 `enc` 写入 TIM3 的 `CR1`、`ARR`、`CCMR1`、`CCER`、`SMCR` 等寄存器。

### 8.9 `HAL_TIM_Encoder_Start()`

代码：

```c
HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL);
```

它启动编码器接口的两个通道和计数器。`TIM_CHANNEL_ALL` 表示 CH1/CH2 都参与。

### 8.10 主循环读取计数器

代码：

```c
uint16_t now = (uint16_t)__HAL_TIM_GET_COUNTER(&htim3);
```

它本质是读取 `TIM3->CNT`。发现 `now != last` 后，HAL 版用 `HAL_GPIO_TogglePin()` 翻转 PC13。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
GPIOA 输入上拉 -> CCMR1 两通道输入 -> SMCR encoder mode 3 -> CNT 自动增减
```

HAL 版重点看：

```text
TIM_Encoder_InitTypeDef 把编码器模式、输入选择、极性、滤波打包
HAL_TIM_Encoder_Start 真正启动
__HAL_TIM_GET_COUNTER 读取 CNT
```

不要把编码器接口理解成普通中断计数。本课没有 EXTI 中断，也没有 TIM3 中断，计数由 TIM3 硬件直接完成。

## 10. 检验问题清单

### 10.1 为什么编码器需要 A/B 两根信号线？

答：一根线只能知道有脉冲，不能知道方向。A/B 两相有先后关系，硬件根据谁领先谁判断顺时针或逆时针。

### 10.2 A/B 相接反会怎样？

答：计数仍然可能变化，但方向会反过来。原来增加的方向会变成减少。

### 10.3 编码器模式下 `PSC` 为什么设为 0？

答：计数脉冲来自外部 A/B 相，不是内部时钟。设为 0 表示不对外部计数再分频。

### 10.4 `SMCR.SMS=011` 表示什么？

答：表示 encoder mode 3，TI1 和 TI2 都参与计数，TIM3 根据两相状态自动加减 `CNT`。

### 10.5 HAL 版 `TIM_CHANNEL_ALL` 启动了什么？

答：启动编码器接口需要的所有通道，本课就是 CH1 和 CH2。只启动单通道会让编码器模式不完整。

### 10.6 机械编码器为什么需要滤波？

答：机械触点会抖动，一个真实边沿可能变成多个短脉冲。滤波能减少误计数，但过大滤波可能漏掉高速脉冲。

### 10.7 `CNT` 从 65535 跳到 0 是错误吗？

答：不是。TIM3 是 16 位计数器，超过 `ARR=0xFFFF` 后会回绕。反向也可能从 0 回到 65535。

### 10.8 为什么主循环只读 `CNT` 就够了？

答：方向判断和边沿计数都由 TIM3 编码器硬件完成。软件只需要读取结果。

## 11. 工程实现步骤

### 11.1 需求分析

需求是读取增量式编码器的相对位置变化。

这要求 A/B 两相信号接入正确通道，输入电平稳定，TIM3 进入编码器模式，`CNT` 能随旋转增减，主循环能读取变化。

### 11.2 硬件核查

确认 A 相接 PA6，B 相接 PA7，公共端按模块说明接 GND 或电源。

如果模块是开漏输出，要有上拉。本课内部上拉已开启。若模块有 VCC/GND，请确认供电和 STM32 共地。

### 11.3 寄存器路线

寄存器版按这个顺序实现：

1. 配置系统时钟。
2. 配置 PC13 输出。
3. 打开 GPIOA 和 TIM3 时钟。
4. PA6/PA7 配成输入上拉。
5. 设置 TIM3 `PSC=0`、`ARR=0xFFFF`。
6. `CC1S=01`，CH1 输入接 TI1。
7. `CC2S=01`，CH2 输入接 TI2。
8. 清 `CCER`，保持默认极性。
9. `SMCR.SMS=011`，进入 encoder mode 3。
10. 清 `CNT=0`。
11. 设置 `CEN=1`。
12. 主循环读取 `CNT`。

### 11.4 HAL 路线

HAL 版按这个顺序实现：

1. `HAL_Init()`。
2. 配置系统时钟。
3. 配置 PC13 输出。
4. PA6/PA7 配成 `GPIO_MODE_INPUT` 和 `GPIO_PULLUP`。
5. 填写 `htim3.Init`。
6. 填写 `TIM_Encoder_InitTypeDef`。
7. 调用 `HAL_TIM_Encoder_Init()`。
8. 调用 `HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL)`。
9. 主循环用 `__HAL_TIM_GET_COUNTER()` 读取 `CNT`。

### 11.5 工程思维

编码器接口的价值是把高速边沿解码交给硬件。软件轮询两个 GPIO 或用两个 EXTI 很容易漏边沿，也容易被抖动和中断延迟影响。

工程里通常会定期读取 `CNT`，计算位置增量、速度，必要时处理 16 位回绕。

### 11.6 常见工程陷阱

第一个陷阱是 A/B 相接反，方向与预期相反。

第二个陷阱是输入没有上拉，机械编码器悬空乱跳。

第三个陷阱是只配置 GPIO，没有启动 TIM 编码器模式。

第四个陷阱是 HAL 只 Init 不 Start。

第五个陷阱是机械抖动导致多计数，需要滤波或硬件消抖。

## 12. 运行现象

旋转编码器时，`TIM3->CNT` 或 HAL 版 `__HAL_TIM_GET_COUNTER()` 读数会变化。

顺时针和逆时针应表现为相反方向的计数变化。具体哪个方向增加，取决于 A/B 相接线。

PC13 会在主循环发现计数变化时翻转。它只是“有变化”的指示，不表示具体位置。

## 13. 常见问题排查

### 13.1 旋转编码器，`CNT` 不变

检查 PA6/PA7 是否接到 A/B 相，公共端和供电是否正确。

再查 GPIOA 时钟、输入上拉、TIM3 时钟、`SMCR.SMS`、`CEN` 是否配置。

### 13.2 方向反了

交换 A/B 相，或调整输入极性。

方向反通常不是程序坏了，而是编码器相位定义和接线方向不同。

### 13.3 数值跳动严重

机械编码器触点抖动会造成多个伪边沿。HAL 版已经设置 `ICFilter=4`，寄存器版可尝试配置输入滤波，或增加硬件消抖。

### 13.4 HAL 版读数不动

确认调用了：

```c
HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL);
```

只调用 `HAL_TIM_Encoder_Init()` 不会启动计数。

### 13.5 计数回绕

`ARR=0xFFFF` 时，计数超过 65535 会回到 0，反向低于 0 会回到 65535。

工程中要用差值算法或扩展计数处理长期位置。

## 14. 本课最核心的结论

- 编码器接口让 TIM3 硬件解码 A/B 两相信号。
- PA6/PA7 分别对应 TIM3_CH1/TIM3_CH2。
- A/B 两相的先后关系决定 `CNT` 增加还是减少。
- `SMCR.SMS=encoder mode 3` 是让外部脉冲驱动计数器的关键。
- 软件不需要在中断里判断 A/B 状态，只需要读取 `CNT`。
- HAL 的 `TIM_Encoder_InitTypeDef` 本质上配置的是输入通道、极性、滤波和编码器模式。

## 15. 建议你现在怎么读这节课

先理解 A/B 相：两路方波都有边沿，谁领先谁决定方向。

再看寄存器版的 `CCMR1` 和 `SMCR`。`CCMR1` 把两路信号接进来，`SMCR` 让 TIM3 进入编码器解码模式。

最后看 HAL 版，把 `TIM_ENCODERMODE_TI12` 对应回 `SMCR.SMS=011`，把 `__HAL_TIM_GET_COUNTER()` 对应回 `TIM3->CNT`。

## 16. 扩展练习

1. 交换 A/B 相，观察计数方向反转。
2. 修改 HAL 输入滤波值，比较机械抖动情况。
3. 用调试器观察 `TIM3->CNT` 和方向变化。
4. 根据一段时间内 `CNT` 差值估算旋转速度。
5. 处理 16 位回绕，把位置扩展成 32 位软件计数。

## 17. 下一课预告

上一课：[12_timer_pwm_input](../12_timer_pwm_input/README.md)

下一课：[14_timer_advanced_tim1](../14_timer_advanced_tim1/README.md)

下一课会进入高级定时器 TIM1，重点会变成高级定时器相对通用定时器多出来的主输出使能、互补输出和刹车等能力。
