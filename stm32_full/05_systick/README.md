# 第 5 课：SysTick 毫秒节拍

## 1. 本课到底在学什么

这节课表面上是在做：

- 用 `SysTick` 每 1ms 产生一次中断
- 在中断里累加一个毫秒计数变量
- 用这个毫秒计数实现 `delay_ms()`
- 让 `PC13` LED 以 500ms 亮、500ms 灭的节奏闪烁

真正学习的是 Cortex-M3 内核提供的时间基准链路：

```text
HCLK = 72MHz
  -> SysTick 使用 HCLK 作为时钟源
  -> LOAD = 72000 - 1
  -> SysTick 每 1ms 递减到 0
  -> 触发 SysTick_Handler()
  -> g_ms_ticks 每 1ms 加 1
  -> delay_ms() 等待毫秒差值
  -> 主循环按时间控制 PC13 亮灭
```

前面课程里你已经见过粗糙空循环延时。空循环延时的问题是：它依赖编译优化、CPU 频率和循环指令开销，不适合作为稳定时间基准。本课开始，你要把“延时”理解成硬件计数器、内核中断、软件毫秒变量共同构成的一条链路。

这一课很关键。后面的 HAL_Delay、定时器周期、中断节奏、RTOS tick，都会继续围绕“稳定时间基准”这个概念展开。

## 2. 本课学习目标

学完本课，你应该能回答：

1. `SysTick` 为什么属于 Cortex-M3 内核，而不是普通 STM32 外设？
2. 为什么 72MHz 下 `SysTick->LOAD = 72000 - 1` 表示 1ms？
3. `SysTick->LOAD`、`SysTick->VAL`、`SysTick->CTRL` 分别控制哪一步？
4. `CLKSOURCE`、`TICKINT`、`ENABLE` 三个位分别少了会怎样？
5. `SysTick_Handler()` 为什么必须叫这个名字？
6. `g_ms_ticks` 为什么必须加 `volatile`？
7. `delay_ms()` 为什么用“当前 tick - 起始 tick”做差？
8. HAL 版为什么既要 `g_ms_ticks++`，又要 `HAL_IncTick()`？
9. `HAL_Delay()` 卡住时，应该优先查哪条链路？

## 3. 本课目录结构

```text
05_systick/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 是寄存器版。它直接配置 Cortex-M3 的 `SysTick` 寄存器，并自己维护 `g_ms_ticks`。

`hal/` 是 HAL 版。它用 `HAL_SYSTICK_Config()` 配置同一个 SysTick，同时维护自定义 tick 和 HAL 内部 tick，让你看到 `delay_ms()` 与 `HAL_Delay()` 背后的共同基础。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- 观察对象：板载 `PC13` LED
- 时钟假设：HSE 8MHz，PLL 后 `HCLK = 72MHz`

本课没有额外接线。`PC13` 仍然是可见运行反馈。常见 BluePill 板载 LED 为低电平点亮：

```text
PC13 = 0 -> LED 亮
PC13 = 1 -> LED 灭
```

如果 LED 不闪，不要立刻怀疑 SysTick。先确认程序是否已经成功下载、是否卡在时钟初始化、PC13 是否配置正确。

## 5. 先建立一个最基本的脑图

本课完整链路如下：

```text
system_clock_72mhz_init()
  -> HCLK 配到 72MHz
  -> pc13_led_init()
  -> GPIOC 时钟打开，PC13 配成推挽输出
  -> systick_1ms_init()
  -> SysTick->LOAD = 72000 - 1
  -> SysTick->VAL = 0
  -> SysTick->CTRL 打开 CLKSOURCE/TICKINT/ENABLE
  -> 每 1ms 进入 SysTick_Handler()
  -> g_ms_ticks++
  -> delay_ms(500) 等待 tick 差值达到 500
  -> 主循环拉低/拉高 PC13
  -> LED 以约 1s 完整周期闪烁
```

这条链路有三个关键点：

1. `LOAD` 的计算依赖 `HCLK`。如果 HCLK 不是 72MHz，`72000 - 1` 就不是 1ms。
2. `TICKINT` 必须打开。否则 SysTick 可以计数，但不会进入 `SysTick_Handler()`。
3. `g_ms_ticks` 必须真的持续增长。否则 `delay_ms()` 和 HAL 的 `HAL_Delay()` 都会等不到时间变化。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在前 4 课已完整讲解，本课只用不重复展开：

| 名词 | 一句话提醒 |
|------|-----------|
| `HCLK=72MHz` | 系统时钟，SysTick 用 HCLK 作为计数时钟源 |
| `system_clock_72mhz_init()` | 配置 HSE+PLL 产生 72MHz，此函数必须先于 SysTick 配置调用 |
| `RCC->APB2ENR` | APB2 外设时钟使能寄存器，PC13 的 GPIOC 时钟在这里打开 |
| `GPIOC->CRH` | GPIOC 高 8 位引脚配置寄存器，PC13 在 MODE13/CNF13 位段中配置 |
| `GPIOC->BRR/BSRR` | 位复位/位设置寄存器，BRR 写 BR13 拉低 PC13（LED 亮），BSRR 写 BS13 拉高（LED 灭） |
| `PC13 LED` | BluePill 板载 LED，低电平点亮（PC13=0→亮，PC13=1→灭） |

### 6.2 `SysTick` 是什么

`SysTick` 全称可以理解为：

- System Tick Timer

中文通常叫：

- 系统节拍定时器

它属于 Cortex-M3 内核，不属于 GPIO、TIM2、USART 这类 STM32 普通外设。也就是说，`SysTick` 是 ARM 内核自带的一个小定时器，所有 Cortex-M 系列芯片通常都会有类似机制。

它的作用是：

- 使用内核时钟或内核时钟的分频作为计数来源。
- 从重装载值向下递减。
- 递减到 0 时产生标志和可选中断。
- 给裸机延时、HAL Tick、RTOS Tick 提供基础节拍。

在本课里，`SysTick` 位于“72MHz HCLK -> 1ms 中断”这一段。没有 SysTick，就只能继续用空循环延时；有了 SysTick，软件就能用更稳定的毫秒计数来控制 LED。

如果 SysTick 没启动，现象通常是：程序卡在 `delay_ms()`，PC13 不再继续闪烁。

### 6.3 `SysTick->LOAD` 是什么

`LOAD` 全称可以理解为：

- Reload Value Register

中文通常叫：

- SysTick 重装载值寄存器

它属于 Cortex-M3 SysTick 寄存器组，控制的是 SysTick 每一轮计数的周期。

SysTick 是递减计数器。它从 `LOAD` 指定的值往下数，数到 0 后产生一次计数完成事件，然后重新装载 `LOAD` 继续数。

本课代码写：

```c
SysTick->LOAD = 72000U - 1U;
```

计算来源是：

```text
HCLK = 72,000,000 Hz
1ms = 1 / 1000 s
72,000,000 / 1000 = 72,000 个时钟周期
SysTick 计数个数 = LOAD + 1
所以 LOAD = 72,000 - 1
```

如果 `LOAD` 写太小，SysTick 中断来得太快，LED 闪烁变快。写太大，中断来得太慢，LED 闪烁变慢。若系统时钟不是 72MHz 但仍写 72000 - 1，毫秒节拍会整体不准。

### 6.4 `SysTick->VAL` 是什么

`VAL` 全称可以理解为：

- Current Value Register

中文通常叫：

- SysTick 当前计数值寄存器

它属于 SysTick 寄存器组，表示当前递减到哪个值。

本课初始化时写：

```c
SysTick->VAL = 0U;
```

对 SysTick 来说，向 `VAL` 写任意值都会清当前计数值。这样做不是为了把它固定成 0，而是为了让 SysTick 从一个干净状态开始计数，避免第一次中断周期带着旧值或未知值。

如果不清 `VAL`，第一次中断可能不是完整的 1ms。后续周期通常会正常，但第一次延时可能出现轻微异常。

### 6.5 `SysTick->CTRL` 是什么

`CTRL` 全称可以理解为：

- Control and Status Register

中文通常叫：

- SysTick 控制与状态寄存器

它属于 SysTick 寄存器组，控制 SysTick 是否启动、是否产生中断、使用什么时钟源，同时也包含计数到 0 的状态信息。

本课代码写：

```c
SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                SysTick_CTRL_TICKINT_Msk |
                SysTick_CTRL_ENABLE_Msk;
```

这一步是 SysTick 真正开始工作的开关组合。前面写 `LOAD` 和 `VAL` 只是准备周期和起点，写 `CTRL` 才让硬件开始递减并产生中断。

如果 `CTRL` 没写，`LOAD` 再正确也不会产生毫秒节拍。

### 6.6 `SysTick_CTRL_CLKSOURCE_Msk` 是什么

`CLKSOURCE` 可以理解为：

- Clock Source

中文通常叫：

- SysTick 时钟源选择位

它属于 `SysTick->CTRL` 里的一个控制位，决定 SysTick 计数使用哪路时钟。

本课设置 `SysTick_CTRL_CLKSOURCE_Msk`，表示使用处理器时钟 HCLK。因为本课已经把 HCLK 配成 72MHz，所以 `LOAD = 72000 - 1` 才能对应 1ms。

如果时钟源选择错，`LOAD` 的计算基准就错了，现象就是 LED 闪烁周期不符合 500ms 预期。

### 6.7 `SysTick_CTRL_TICKINT_Msk` 是什么

`TICKINT` 可以理解为：

- Tick Interrupt Enable

中文通常叫：

- SysTick 中断使能位

它属于 `SysTick->CTRL`，控制 SysTick 计数到 0 时是否发出中断请求。

本课必须打开它，因为 `g_ms_ticks++` 写在 `SysTick_Handler()` 里。只有中断进来，毫秒变量才会增长。

如果漏掉 `TICKINT`，SysTick 可能仍在计数，但不会进入 `SysTick_Handler()`。现象通常是程序卡在第一次 `delay_ms(500)`，LED 只停在某个状态。

### 6.8 `SysTick_CTRL_ENABLE_Msk` 是什么

`ENABLE` 中文通常叫：

- SysTick 计数器使能位

它属于 `SysTick->CTRL`，控制 SysTick 递减计数器是否真正启动。

本课必须设置它。没有 `ENABLE`，`LOAD`、`VAL`、`TICKINT` 都配置好了也没有用，因为计数器没有开始跑。

如果漏掉 `ENABLE`，现象和漏掉 `TICKINT` 很像：`g_ms_ticks` 不增长，`delay_ms()` 一直等。

### 6.9 `SysTick_Handler()` 是什么

`SysTick_Handler()` 是 SysTick 的中断服务函数。

它属于 Cortex-M 异常/中断入口层，不是普通随便命名的函数。启动文件里的中断向量表会把 SysTick 异常入口指向名为 `SysTick_Handler` 的函数。

本课代码写：

```c
void SysTick_Handler(void)
{
    g_ms_ticks++;
}
```

每次 SysTick 计数到 0 并触发中断，CPU 就会自动跳到这里执行。因为本课把周期配置为 1ms，所以这里每执行一次，就代表过去了约 1ms。

如果函数名写错，比如写成 `Systick_Handler`，启动文件找不到正确入口，`g_ms_ticks` 就不会增长。

### 6.10 `g_ms_ticks` 是什么

`g_ms_ticks` 是本课自己定义的软件毫秒计数变量：

```c
static volatile uint32_t g_ms_ticks = 0;
```

它属于软件状态层，不是硬件寄存器。硬件 SysTick 只能告诉 CPU“又过了一拍”，软件要用变量把这些事件累计起来。

它必须加 `volatile`，因为它在中断里被修改，在主循环或 `delay_ms()` 里被读取。没有 `volatile`，编译器可能认为循环里变量不会变，把读取优化掉，导致 `delay_ms()` 判断失效。

如果 `g_ms_ticks` 不增长，所有基于它的延时都会卡住。

### 6.11 `delay_ms()` 是什么

`delay_ms()` 是本课基于 `g_ms_ticks` 写出的毫秒阻塞延时函数。

它属于软件逻辑层，不是硬件外设。它的核心逻辑是：

```c
uint32_t start = g_ms_ticks;
while ((g_ms_ticks - start) < ms) {
}
```

它不是自己制造时间，而是等待 SysTick 中断不断更新 `g_ms_ticks`。硬件负责产生 1ms 节拍，中断负责累加变量，`delay_ms()` 负责等待差值达到目标。

这里用无符号减法，是为了自然处理 `uint32_t` 回绕。只要单次延时远小于 2^32 ms，`g_ms_ticks - start` 在回绕后仍然能得到正确经过时间。

### 6.12 `HAL_SYSTICK_Config()` 是什么

`HAL_SYSTICK_Config()` 是 HAL 提供的 SysTick 配置函数。

它属于 HAL 软件封装层，底层仍然配置 Cortex-M 的 SysTick 寄存器。

HAL 版代码写：

```c
HAL_SYSTICK_Config(hclk_hz / 1000U)
```

如果 `hclk_hz = 72000000`，传入值就是 72000。它表达的意思和寄存器版 `LOAD = 72000 - 1` 一致：每 72000 个 HCLK 周期触发一次 SysTick。

如果传入值算错，HAL Tick、自定义 tick、`HAL_Delay()` 的时间都会不准。

### 6.13 `HAL_IncTick()` 是什么

`HAL_IncTick()` 是 HAL 内部毫秒 tick 的递增函数。

它属于 HAL 软件状态层。HAL 库内部维护了一个 tick 变量，`HAL_Delay()` 和很多带超时的 HAL API 都依赖这个变量判断时间是否过去。

本课 HAL 版的 `SysTick_Handler()` 同时做两件事：

```c
g_ms_ticks++;
HAL_IncTick();
```

`g_ms_ticks++` 服务我们自己的 `delay_ms()`，`HAL_IncTick()` 服务 HAL 的 `HAL_Delay()`。少了前者，自定义延时卡住；少了后者，`HAL_Delay()` 卡住。

### 6.14 `HAL_Delay()` 是什么

`HAL_Delay()` 是 HAL 提供的毫秒级阻塞延时函数。

它属于 HAL 软件封装层。它并不是自己配置一个新定时器，而是读取 HAL 内部 tick，然后等待 tick 差值达到目标。

本课 HAL 版故意同时使用：

```c
delay_ms(500U);
HAL_Delay(500U);
```

这样你能看到：自定义 `delay_ms()` 和 HAL 的 `HAL_Delay()` 背后都依赖 SysTick 中断，只是维护的 tick 变量不同。

如果 `HAL_Delay()` 卡住，优先查 `SysTick_Handler()` 里有没有调用 `HAL_IncTick()`。

## 7. 寄存器版代码讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 已学步骤（快速过）

以下步骤在前 4 课已完整讲解，本课用列表方式快速过：

- `system_clock_72mhz_init()`：FLASH 等 2 周期 → 开 HSE → 等 HSE 稳定 → 配 PLL 9 倍频 → 等 PLL 稳定 → 切 SYSCLK 到 PLL。结果为 72MHz。
- `led_pc13_init()`：开 GPIOC 时钟 → 清 CRH 的 MODE13/CNF13 → 设 MODE13=01（2MHz 推挽输出）→ BSRR 写 BS13 拉高，LED 熄灭。

顺序约束：`system_clock_72mhz_init()` 必须最先调用，因为后续 SysTick 的 `LOAD` 值计算（72000-1）直接依赖 HCLK=72MHz。如果把 `systick_1ms_init()` 放在时钟配置前面，`LOAD` 可能基于错误的时钟频率。

### 7.2 本课新增步骤概览

本课新增的是 SysTick 1ms 节拍的产生和使用：

```text
systick_1ms_init()
  → SysTick->LOAD = 72000 - 1    // 重装值，决定 1ms 周期
  → SysTick->VAL = 0             // 清零当前值，避免首次周期异常
  → SysTick->CTRL = CLKSOURCE | TICKINT | ENABLE  // 启动：选时钟、开中断、开计数
  → 每 1ms 进入 SysTick_Handler()
  → g_ms_ticks++
  → delay_ms() 用 tick 差值判断时间
```

### 7.3 `static volatile uint32_t g_ms_ticks = 0;` 为什么这样写

这句定义了软件毫秒计数：

```c
static volatile uint32_t g_ms_ticks = 0;
```

`uint32_t` 表示 32 位无符号整数，能表示很长的毫秒计数范围。

`static` 把变量限制在当前 C 文件内部使用，避免其他文件随便访问。

`volatile` 是最关键的。`g_ms_ticks` 在 `SysTick_Handler()` 中被中断异步修改，在 `delay_ms()` 中被主流程读取。编译器不能假设它在 `while` 循环里不变。

如果去掉 `volatile`，优化级别较高时，`delay_ms()` 可能读一次旧值后一直不再重新读取，表现为卡死。

### 7.3 `system_clock_72mhz_init()` 和本课 SysTick 有什么关系

本课 SysTick 周期计算基于：

```text
HCLK = 72MHz
```

`system_clock_72mhz_init()` 仍然沿用前面时钟树课程的逻辑：

- 配置 `FLASH->ACR`
- 打开并等待 HSE
- 配置 PLL 输入 HSE、倍频 x9
- 配置 AHB/APB 分频
- 打开并等待 PLL
- 切换 SYSCLK 到 PLL

如果这里没成功，后面的 `SysTick->LOAD = 72000 - 1` 就没有意义。比如实际 HCLK 是 8MHz，那么 72000 个周期不是 1ms，而是 9ms，LED 会明显变慢。

### 7.4 `led_pc13_init()` 为什么仍然需要

本课主角是 SysTick，但现象仍然靠 PC13 LED 显示。

`led_pc13_init()` 做三件事：

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
GPIOC->CRH |= GPIO_CRH_MODE13_1;
GPIOC->BSRR = GPIO_BSRR_BS13;
```

第一句打开 GPIOC 时钟。第二句清 PC13 模式字段。第三句把 PC13 配成 2MHz 通用推挽输出。第四句让 PC13 初始输出高电平，LED 先灭。

如果 PC13 没配好，SysTick 可能已经正确计时，但你看不到 LED 闪烁。排错时要区分“时间基准坏了”和“显示现象坏了”。

### 7.5 `systick_1ms_init()` 第一步：写 `LOAD`

代码是：

```c
SysTick->LOAD = 72000U - 1U;
```

这一步决定 SysTick 多久产生一次计数完成事件。

为什么是 `72000 - 1`，不是 `72000`？

因为 SysTick 计数个数是 `LOAD + 1`。写入 71999 后，计数覆盖 72000 个时钟周期。

本课选择 HCLK = 72MHz：

```text
72MHz = 72,000,000 次/秒
1ms = 1/1000 秒
1ms 需要 72,000 次
LOAD = 72,000 - 1
```

这一步如果写错，LED 仍然可能闪，但节奏不对。

### 7.6 `systick_1ms_init()` 第二步：写 `VAL`

代码是：

```c
SysTick->VAL = 0U;
```

`VAL` 是当前计数值寄存器。向它写任意值都会清当前计数，让下一轮从干净状态开始。

这一步依赖 `LOAD` 已经设置好。先设周期，再清当前计数，最后启动，是比较稳定的初始化顺序。

如果不清 `VAL`，第一次中断可能早到或晚到。虽然后续周期通常会稳定，但第一次 `delay_ms()` 可能不够规整。

###  7.7 `systick_1ms_init()` 第三步：写 `CTRL`

代码是：

```c
SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                SysTick_CTRL_TICKINT_Msk |
                SysTick_CTRL_ENABLE_Msk;
```

这一步同时设置三个关键控制位：

- `CLKSOURCE = 1`：选择 HCLK 作为 SysTick 时钟源。
- `TICKINT = 1`：计数到 0 时允许产生 SysTick 中断。
- `ENABLE = 1`：启动 SysTick 计数器。

这三个位缺一不可：

- 少 `CLKSOURCE`：计数时钟源和计算不匹配，周期不对。
- 少 `TICKINT`：计数到了也不进 `SysTick_Handler()`。
- 少 `ENABLE`：计数器根本不跑。

### 7.8 `SysTick_Handler()` 为什么只做 `g_ms_ticks++`

代码是：

```c
void SysTick_Handler(void)
{
    g_ms_ticks++;
}
```

`SysTick_Handler()` 由中断向量表调用，不是主循环主动调用。每 1ms 进入一次，就把软件毫秒计数加 1。

中断函数要短。这里不直接翻转 LED，是为了让中断只负责维护时间，业务动作仍然放在主循环里。这样以后扩展代码时，中断不会变得又长又难排查。

如果在中断里做太多事，可能影响其他中断响应，甚至让系统时间基准本身变得不稳定。

### 7.9 `delay_ms()` 为什么用差值等待

代码是：

```c
uint32_t start = g_ms_ticks;
while ((g_ms_ticks - start) < ms) {
}
```

进入函数时记录起始时间，之后反复查看当前时间与起始时间的差值。

为什么不写成等待 `g_ms_ticks == start + ms`？

因为主循环可能错过某个精确相等瞬间，而且 `g_ms_ticks` 迟早会回绕。用无符号差值，只要等待时间不超过计数范围的一半，就能自然处理回绕。

这一步依赖 SysTick 中断持续更新 `g_ms_ticks`。如果中断不进，`while` 条件永远不满足。

### 7.10 主循环如何控制 PC13

主循环里：

```c
GPIOC->BRR = GPIO_BRR_BR13;
delay_ms(500U);

GPIOC->BSRR = GPIO_BSRR_BS13;
delay_ms(500U);
```

`BRR` 写 1 把 PC13 拉低。BluePill 常见 LED 低电平点亮，所以这一步是亮 500ms。

`BSRR` 写 1 把 PC13 拉高，所以这一步是灭 500ms。

完整亮灭周期约 1s。如果你看到的是 9s 左右，优先怀疑 HCLK 实际不是 72MHz。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 HAL 版和寄存器版的本质差异

HAL 版主流程是：

```c
HAL_Init();
system_clock_72mhz_init();
led_pc13_init();
systick_1ms_init(HAL_RCC_GetHCLKFreq());
```

寄存器版直接写 `SysTick->LOAD/VAL/CTRL`。HAL 版用 `HAL_SYSTICK_Config()`、`HAL_SYSTICK_CLKSourceConfig()`、`HAL_NVIC_SetPriority()` 表达同样意图。

本课 HAL 版还故意同时使用自定义 `delay_ms()` 和 `HAL_Delay()`，让你对比两套 tick 变量。

### 8.2 `HAL_Init()` 在本课里做什么

`HAL_Init()` 初始化 HAL 基础状态，并准备 HAL 默认 Tick。

本课后面会重新调用 `systick_1ms_init(HAL_RCC_GetHCLKFreq())` 配置 SysTick。即便如此，`HAL_Init()` 仍然应该放在 HAL 工程开头，因为 HAL 的状态、优先级分组、Tick 机制都依赖它建立基本环境。

如果省略 `HAL_Init()`，后面的 HAL API 和 `HAL_Delay()` 可能表现异常。

### 8.3 `system_clock_72mhz_init()` 的 HAL 映射

HAL 版使用两个结构体：

- `RCC_OscInitTypeDef osc`
- `RCC_ClkInitTypeDef clk`

`osc` 负责 HSE 和 PLL：

- `HSEState = RCC_HSE_ON` 对应打开 HSE。
- `PLLSource = RCC_PLLSOURCE_HSE` 对应 PLL 输入选择 HSE。
- `PLLMUL = RCC_PLL_MUL9` 对应 PLL x9。

`clk` 负责 SYSCLK 和总线分频：

- `SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK` 对应系统时钟选择 PLL。
- `APB1CLKDivider = RCC_HCLK_DIV2` 对应 APB1 二分频。
- `APB2CLKDivider = RCC_HCLK_DIV1` 对应 APB2 不分频。

这部分和 03 时钟树课程是一脉相承的。本课重点是：SysTick 的 `hclk_hz / 1000` 依赖这里最终得到的 HCLK。

### 8.4 `HAL_RCC_GetHCLKFreq()` 为什么出现在这里

HAL 版调用：

```c
systick_1ms_init(HAL_RCC_GetHCLKFreq());
```

`HAL_RCC_GetHCLKFreq()` 返回 HAL 当前记录的 HCLK 频率。本课在时钟配置后调用它，理论上得到 72000000。

这样写比直接写死 72000000 更像工程做法：如果以后 HCLK 配置变了，SysTick 初始化可以跟着当前 HCLK 计算。

如果时钟配置失败或 HAL 内部频率没有更新，传入的 `hclk_hz` 就不可靠，SysTick 周期也会不准。

### 8.5 `HAL_SYSTICK_Config(hclk_hz / 1000U)` 做了什么

代码是：

```c
HAL_SYSTICK_Config(hclk_hz / 1000U)
```

如果 `hclk_hz = 72000000`，传入值是 72000。

这个 API 底层会配置 SysTick 重装载值，并启动 SysTick。它对应寄存器版：

```c
SysTick->LOAD = 72000U - 1U;
SysTick->VAL = 0U;
```

以及部分 `CTRL` 启动配置。

如果传入值为 0，或者大于 SysTick 24 位计数器能容纳的范围，配置会失败，本课会进入 `error_handler()`。

### 8.6 `HAL_SYSTICK_CLKSourceConfig()` 对应哪一位

代码是：

```c
HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);
```

它对应寄存器版 `SysTick_CTRL_CLKSOURCE_Msk`，表示 SysTick 使用 HCLK 作为计数时钟源。

这一步必须和 `hclk_hz / 1000U` 的计算基准一致。用 HCLK 算重装值，就应该选择 HCLK 作为 SysTick 时钟源。

### 8.7 `HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0)` 是什么

`SysTick_IRQn` 是 SysTick 在 Cortex-M 异常/中断系统中的编号。

代码是：

```c
HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
```

它设置 SysTick 中断优先级。本课没有复杂中断竞争，设置为较高优先级即可。

这里要注意：SysTick 是内核异常，不是 STM32 普通外设中断。但 HAL 仍然通过类似 NVIC 的接口设置它的优先级。

### 8.8 HAL 版 `SysTick_Handler()` 为什么有两句

HAL 版中断函数是：

```c
void SysTick_Handler(void)
{
    g_ms_ticks++;
    HAL_IncTick();
}
```

第一句服务本课自定义的 `delay_ms()`。

第二句服务 HAL 内部 tick。`HAL_Delay()` 读取 HAL 内部 tick，如果 `HAL_IncTick()` 不执行，HAL 会认为时间没有前进。

这就是 HAL 版最容易漏的点：只维护自己的 tick 不够，使用 `HAL_Delay()` 时还必须维护 HAL tick。

### 8.9 HAL 版主循环为什么故意混用两个延时

HAL 版主循环是：

```c
HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
delay_ms(500U);

HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
HAL_Delay(500U);
```

前半段用自定义 `delay_ms()`，依赖 `g_ms_ticks`。

后半段用 `HAL_Delay()`，依赖 HAL 内部 tick。

这样设计是为了证明：两者都依赖 SysTick 中断，只是维护和读取的变量不同。

### 8.10 `error_handler()` 为什么关闭中断

HAL 版如果 `HAL_SYSTICK_Config()` 或时钟配置失败，会调用：

```c
error_handler();
```

里面执行：

```c
__disable_irq();
while (1) {
}
```

关闭中断后停在死循环，便于调试器接管。否则错误状态下中断继续进来，可能干扰你观察调用栈和寄存器状态。

## 9. 两个版本真正应该怎么学

### 9.1 为什么先学寄存器版

寄存器版能让你看见 SysTick 的硬件本体：`LOAD` 决定周期，`VAL` 清当前值，`CTRL` 选择时钟源、打开中断、启动计数器。

如果先只学 HAL，你可能会以为 `HAL_Delay()` 自带某种神秘时间能力。看过寄存器版后就知道，它最终依赖的是 SysTick 中断和毫秒计数。

### 9.2 为什么再看 HAL 版

HAL 版更接近实际工程。很多 HAL 驱动函数都有超时等待，`HAL_Delay()` 也经常使用。理解 HAL Tick 后，你就能排查“为什么 HAL_Delay 卡住”“为什么 HAL 超时不准”这类问题。

### 9.3 正确心智模型

本课要建立的映射是：

- `SysTick->LOAD` -> `HAL_SYSTICK_Config()` 的 tick 参数
- `SysTick->CTRL.CLKSOURCE` -> `HAL_SYSTICK_CLKSourceConfig()`
- `SysTick_Handler()` -> HAL 和自定义 tick 的共同入口
- `g_ms_ticks++` -> 自定义 `delay_ms()`
- `HAL_IncTick()` -> `HAL_Delay()`

## 10. 检验问题清单

### 10.1 为什么 `LOAD = 72000 - 1` 对应 1ms？

答：本课 HCLK 是 72MHz，1ms 需要 72000 个时钟周期。SysTick 实际计数个数是 `LOAD + 1`，所以写 `72000 - 1`。

### 10.2 如果 HCLK 实际是 8MHz，但仍写 `72000 - 1`，LED 会怎样？

答：72000 个 8MHz 周期约为 9ms，不是 1ms。`delay_ms(500)` 实际会接近 4.5s，LED 变得很慢。

### 10.3 `TICKINT` 漏掉会发生什么？

答：SysTick 可能仍然计数，但计数到 0 不会进入 `SysTick_Handler()`，`g_ms_ticks` 不增长，`delay_ms()` 会卡住。

### 10.4 `ENABLE` 漏掉和 `TICKINT` 漏掉有什么相似现象？

答：两者都会导致 `g_ms_ticks` 不增长。区别是 `ENABLE` 漏掉时计数器根本不跑，`TICKINT` 漏掉时可能计数但不触发中断。

### 10.5 为什么 `g_ms_ticks` 要加 `volatile`？

答：它在中断里修改，在主流程里读取。`volatile` 告诉编译器每次都要重新读，不要把循环里的读取优化成旧值。

### 10.6 为什么 `delay_ms()` 用差值，而不是等于某个目标 tick？

答：差值判断不怕错过精确相等瞬间，也能自然处理 `uint32_t` 回绕。等待相等值更容易因为时机错过而卡住。

### 10.7 HAL 版为什么 `SysTick_Handler()` 里要同时写 `g_ms_ticks++` 和 `HAL_IncTick()`？

答：`g_ms_ticks++` 服务自定义 `delay_ms()`，`HAL_IncTick()` 服务 HAL 内部 tick 和 `HAL_Delay()`。少任何一个，对应的延时都会出问题。

### 10.8 `HAL_Delay()` 卡住时，应该按什么顺序排查？

答：先查 `HAL_Init()` 是否调用，再查 SysTick 是否配置成功，再查 `SysTick_Handler()` 是否存在并调用 `HAL_IncTick()`，最后查中断是否被关闭或优先级异常。

## 11. 工程实现步骤

### 11.1 需求分析

本课需求是建立一个可靠的毫秒时间基准：

- 1ms 产生一次 SysTick 中断
- 软件能累计毫秒数
- 主循环能用毫秒数延时
- LED 闪烁周期能验证时间基准大致正确

需要的硬件资源很少：只用 Cortex-M3 内核 SysTick、PC13 LED 和 72MHz HCLK。

### 11.2 硬件核查

先确认：

- 板子 HSE 可用，系统能配置到 72MHz。
- PC13 板载 LED 正常，且低电平点亮。
- 程序没有卡在 HSE/PLL 等待循环。

SysTick 是内核定时器，不需要额外开启 RCC 外设时钟。这个点和 TIM2/TIM3 不同。

### 11.3 寄存器实现路线

按这个顺序：

1. 配置系统时钟到 72MHz。`LOAD` 的计算依赖 HCLK。
2. 初始化 PC13。否则无法观察 LED 现象。
3. 设置 `SysTick->LOAD = 72000 - 1`。决定 1ms 周期。
4. 写 `SysTick->VAL = 0`。清掉当前计数，避免第一次节拍异常。
5. 写 `SysTick->CTRL`。选择 HCLK、打开中断、启动计数。
6. 在 `SysTick_Handler()` 中递增 `g_ms_ticks`。
7. 在 `delay_ms()` 中等待 tick 差值。
8. 主循环按 500ms 控制 PC13。

顺序错了会直接影响现象。例如先调用 `delay_ms()` 再启动 SysTick，程序会卡住。

### 11.4 HAL 实现路线

按这个顺序：

1. `HAL_Init()`：准备 HAL 基础状态。
2. `system_clock_72mhz_init()`：配置 HCLK。
3. `HAL_RCC_GetHCLKFreq()`：读取当前 HCLK。
4. `HAL_SYSTICK_Config(hclk_hz / 1000)`：配置 SysTick 周期。
5. `HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK)`：选择 HCLK 作为 SysTick 时钟源。
6. `HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0)`：设置 SysTick 优先级。
7. `SysTick_Handler()` 中同时维护自定义 tick 和 HAL tick。
8. 主循环分别使用 `delay_ms()` 和 `HAL_Delay()`。

HAL 版的关键不是少写几行，而是让你看到 HAL Tick 也必须由 SysTick 中断持续维护。

### 11.5 工程思维

学习阶段先写寄存器版，是为了把时间基准拆清楚：时钟源、重装值、当前值、中断、软件计数。

工程阶段常用 HAL，是因为 HAL 的很多驱动都依赖统一 tick。你不一定每次手写 `SysTick->LOAD`，但必须知道 `HAL_Delay()` 卡住时该回头查 SysTick。

长期维护时，要特别注意：系统时钟一旦改变，SysTick 配置也要跟着改变，否则所有基于毫秒的逻辑都会偏。

### 11.6 常见工程陷阱

- HCLK 改了但 `LOAD` 没改：所有延时整体变快或变慢。
- 忘记 `TICKINT`：SysTick 不进中断，`delay_ms()` 卡住。
- 忘记 `HAL_IncTick()`：自定义延时正常，但 `HAL_Delay()` 卡住。
- 中断里做太多事：SysTick 抢占过频繁，影响系统响应。
- 把 SysTick 当普通 TIM 外设：误以为要开 RCC 时钟，其实它属于 Cortex-M 内核。

## 12. 运行现象

寄存器版中，PC13 低电平约 500ms，高电平约 500ms，完整亮灭周期约 1s。

HAL 版中，亮的 500ms 来自自定义 `delay_ms()`，灭的 500ms 来自 `HAL_Delay()`。两者都正常，说明自定义 tick 和 HAL tick 都在增长。

如果 LED 周期明显不对，优先检查 HCLK 和 SysTick 重装值。

## 13. 常见问题排查

### 13.1 LED 完全不闪

现象是下载成功后，PC13 一直保持亮或灭。

排查顺序：

1. 程序是否卡在 `system_clock_72mhz_init()` 等待 HSE/PLL。
2. GPIOC 时钟是否打开。
3. PC13 是否配置为推挽输出。
4. `systick_1ms_init()` 是否被调用。
5. `SysTick->CTRL` 是否设置了 `ENABLE` 和 `TICKINT`。

最容易忽略的是：SysTick 正常也需要 PC13 输出链路正常才能看到现象。

### 13.2 LED 闪烁周期明显不对

现象是 LED 在闪，但不是约 500ms 亮、500ms 灭。

优先检查：

1. HCLK 是否真的是 72MHz。
2. `SysTick->LOAD` 是否按 HCLK/1000 计算。
3. `CLKSOURCE` 是否选择 HCLK。
4. HAL 版 `hclk_hz / 1000U` 中的 `hclk_hz` 是否正确。

如果实际 HCLK 比预期低，LED 会变慢；实际 HCLK 比预期高，LED 会变快。

### 13.3 程序卡在 `delay_ms()`

现象是 LED 可能只亮一次或只灭一次，然后停住。

排查顺序：

1. `g_ms_ticks` 是否在调试器里持续增长。
2. `SysTick_Handler()` 函数名是否正确。
3. `TICKINT` 是否打开。
4. 全局中断是否被关闭。
5. `g_ms_ticks` 是否加了 `volatile`。

### 13.4 HAL 版卡在 `HAL_Delay()`

现象是自定义 `delay_ms()` 可能工作，但执行到 `HAL_Delay()` 后卡住。

优先检查 `SysTick_Handler()`：

```c
void SysTick_Handler(void)
{
    g_ms_ticks++;
    HAL_IncTick();
}
```

如果没有 `HAL_IncTick()`，HAL 内部 tick 不增长，`HAL_Delay()` 就无法结束。

### 13.5 第一次延时不规整

现象是第一次亮或灭的时间明显短一些或长一些，后面正常。

优先检查启动前是否写过：

```c
SysTick->VAL = 0U;
```

不清当前计数值时，第一次计数可能从旧值开始，不一定是完整周期。

## 14. 本课最核心的结论

1. SysTick 是 Cortex-M3 内核自带定时器，不是 STM32 普通外设。
2. `LOAD + 1` 决定 SysTick 一轮计数需要多少个时钟周期。
3. `CLKSOURCE` 决定计数基准，`TICKINT` 决定是否进中断，`ENABLE` 决定是否启动计数。
4. `SysTick_Handler()` 是毫秒变量增长的入口，函数名必须和启动文件向量表匹配。
5. `volatile g_ms_ticks` 把硬件中断事件变成主循环可见的软件时间。
6. HAL 的 `HAL_Delay()` 也依赖 SysTick，只是它使用 HAL 内部 tick。

## 15. 建议你现在怎么读这节课

建议顺序：

1. 先读第 5 章，把 `HCLK -> LOAD -> 中断 -> g_ms_ticks -> delay_ms` 这条链路背下来。
2. 再看寄存器版 `systick_1ms_init()`，确认三个寄存器各自解决什么问题。
3. 然后看 `SysTick_Handler()` 和 `delay_ms()`，理解硬件节拍如何变成软件延时。
4. 最后看 HAL 版，重点比较 `g_ms_ticks++` 和 `HAL_IncTick()`。

能自己解释“为什么 HAL_Delay 卡住时要查 SysTick_Handler”，这节课就基本学透了。

## 16. 扩展练习

1. 把 `delay_ms(500)` 改成 100、1000，观察 LED 节奏。
2. 把 `LOAD` 改成 `36000 - 1`，观察延时是否变成原来一半。
3. 故意去掉 `TICKINT`，确认程序卡在 `delay_ms()`。
4. HAL 版中注释掉 `HAL_IncTick()`，观察 `HAL_Delay()` 的故障。
5. 在调试器里观察 `g_ms_ticks`、`SysTick->CTRL`、`SysTick->LOAD`。

## 17. 下一课预告

下一课进入 [06_timer_base](../06_timer_base/README.md)。

本课使用 Cortex-M3 内核自带的 SysTick 建立毫秒节拍。下一课会进入 STM32 普通定时器 `TIM2`：你会看到另一套更通用的定时链路，也就是外设时钟、`PSC`、`ARR`、更新事件、`UIF` 标志、NVIC 中断和 `TIM2_IRQHandler()`。
