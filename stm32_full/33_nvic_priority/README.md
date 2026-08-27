# 第 33 课：NVIC 优先级

## 1. 本课到底在学什么

本课表面现象是：TIM2 每 2 秒触发一次低优先级中断，让 PC13 LED 长亮一小段时间；如果在这个长亮窗口里按下 PA0，EXTI0 高优先级中断会立刻插入，让 LED 出现一次反向短脉冲。

真正要学的是 Cortex-M3 的中断仲裁机制：

```text
外设产生事件
  -> 外设侧中断允许
  -> NVIC 侧中断允许
  -> NVIC 根据优先级决定 CPU 先响应谁
  -> 高优先级中断可以抢占低优先级中断
  -> ISR 中必须正确清除标志
```

本课很重要的一点是：`NVIC` 属于 Cortex-M3 内核，不是 GPIO、TIM、EXTI 这种 STM32 外设。外设负责“发请求”，NVIC 负责“排队和仲裁”。

## 2. 本课学习目标

学完本课，你应该能回答：

1. NVIC 是什么，属于哪个层级？
2. 为什么中断要同时打开外设侧和 NVIC 侧两道门？
3. 优先级数字越小为什么优先级越高？
4. EXTI0 为什么能抢占 TIM2？
5. `EXTI->PR` 为什么是写 1 清除，而 `TIM2->SR` 是写 0 清除？
6. `NVIC_SetPriority()` 和 `NVIC_EnableIRQ()` 分别写了什么？
7. HAL 版 IRQHandler 和 Callback 的关系是什么？
8. 为什么真实工程不应在中断里长时间忙等待？

## 3. 本课目录结构

```text
33_nvic_priority/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接配置 EXTI、TIM2 和 NVIC。  
`hal/` 使用 HAL GPIO/TIM/NVIC API，并通过 Callback 写业务逻辑。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- LED：PC13，常见 BluePill 为低电平点亮
- 按键：PA0 接按键，按下接 GND，内部上拉

本课不需要额外通信模块。观察重点是 LED 在 TIM2 中断窗口里是否能被 PA0 按键中断“插入”改变。

## 5. 先建立一个最基本的脑图

```text
PA0 按键
  -> GPIOA 输入上拉
  -> AFIO 把 PA0 映射到 EXTI0
  -> EXTI0 下降沿触发
  -> EXTI->IMR 允许请求
  -> NVIC 使能 EXTI0_IRQn，优先级 0

TIM2
  -> PSC/ARR 配成约 2 秒更新一次
  -> TIM2->DIER.UIE 允许更新中断
  -> NVIC 使能 TIM2_IRQn，优先级 2

运行现象
  -> TIM2 ISR 低优先级，LED 长亮
  -> 长亮期间按 PA0
  -> EXTI0 优先级更高，抢占 TIM2
  -> LED 出现短脉冲
  -> EXTI0 返回后 TIM2 继续执行
```

这节课故意在中断里忙等待，是为了制造肉眼可见的抢占窗口。真实工程中不要这样写。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在前面课程已完整讲解，本课只用不重复展开：

| 名词 | 一句话提醒 |
|------|-----------|
| `system_clock_72mhz_init()` | 配置 HSE+PLL 产生 72MHz，TIM2 定时器时钟为 72MHz（APB1 分频≠1 时定时器加倍） |
| `PC13 LED` | BluePill 板载 LED，低电平点亮。本课用 LED 的长亮/短脉冲直观显示中断抢占 |
| `GPIOC->CRH/BSRR/BRR` | PC13 的引脚配置和电平控制 |
| `GPIOA->CRL` | PA0 的引脚配置寄存器，配成上拉输入 |
| `AFIO->EXTICR` | AFIO 外部中断映射寄存器，选择 PA0 作为 EXTI0 的来源 |
| `EXTI->IMR/FTSR/RTSR/PR` | EXTI 中断屏蔽/触发选择/挂起标志，控制 PA0 下降沿触发中断 |
| `TIM2->PSC/ARR` | 预分频和自动重装载，决定 TIM2 更新周期。本课 PSC=7200-1, ARR=20000-1 → 2s |
| `TIM2->DIER.UIE` | TIM2 更新中断使能，外设侧第一道门 |
| `TIM2->SR.UIF` | TIM2 更新中断标志，进入 ISR 后需写 0 清除 |
| `TIM2->CR1.CEN` | TIM2 计数器使能，启动后计数器开始递增 |
| `RCC->APB1ENR/APB2ENR` | 外设时钟使能寄存器，TIM2 在 APB1，GPIOA/GPIOC/AFIO 在 APB2 |

### 6.2 `NVIC` 是什么

`NVIC` 是 Nested Vectored Interrupt Controller，嵌套向量中断控制器。

它属于 Cortex-M3 内核层，不属于 STM32 的普通外设。它负责接收外设中断请求、比较优先级、决定 CPU 跳到哪个中断服务函数。

没有 NVIC，使能外设中断也不等于 CPU 会响应。

### 6.3 `IRQn` 是什么

`IRQn` 是中断号，用来标识某个中断源。

本课使用：

- `EXTI0_IRQn`：外部中断线 0
- `TIM2_IRQn`：TIM2 更新等中断

它属于 CMSIS/C 代码层，是软件访问 NVIC 寄存器时使用的编号。

### 6.4 `优先级数字` 是什么

在 Cortex-M 中，中断优先级数字越小，优先级越高。

本课设置：

- EXTI0 优先级 0
- TIM2 优先级 2

所以 EXTI0 可以抢占 TIM2。若把两者设成相同优先级，EXTI0 就不能在 TIM2 ISR 正在执行时插入。

### 6.5 `抢占` 是什么

抢占表示高优先级中断打断正在执行的低优先级中断。

本课的可见证据是：TIM2 中断让 LED 长亮时，按 PA0 能立即让 LED 短暂反向变化，然后 TIM2 继续执行。

同优先级中断不会互相抢占。若两个中断抢占优先级相同，后来的中断只能等当前 ISR 退出后再响应；子优先级只影响多个挂起中断同时等待时谁先执行，不会让它打断正在执行的同抢占级 ISR。

所以本课要观察长亮期间插入短脉冲，必须保证 EXTI0 的抢占优先级高于 TIM2。只把子优先级设得更高，无法产生本课现象。

### 6.6 `EXTI0` 是什么

EXTI0 是外部中断线 0。

它属于 STM32 外设/中断触发层。PA0、PB0、PC0 等都可以映射到 EXTI0，本课通过 AFIO 选择 PA0。

EXTI 负责检测边沿并向 NVIC 发请求，NVIC 决定 CPU 是否响应。

### 6.7 `AFIO->EXTICR` 是什么

`EXTICR` 是 AFIO 里的外部中断映射寄存器。

它决定 EXTI0 的输入来自哪个 GPIO 端口。本课选择 PA0，所以 `EXTICR[0]` 中 EXTI0 字段为 0。

如果映射错到 PB0，按 PA0 不会触发 EXTI0。

### 6.8 `EXTI->IMR` 是什么

`IMR` 是 Interrupt Mask Register，中断屏蔽寄存器。

`IMR.MR0=1` 表示允许 EXTI0 把中断请求送到 NVIC。这是外设侧第一道门。

如果 IMR 没开，即使 PA0 有下降沿，NVIC 也收不到请求。

### 6.9 `EXTI->FTSR / RTSR` 是什么

`FTSR` 选择下降沿触发，`RTSR` 选择上升沿触发。

本课 PA0 内部上拉，按下接地，所以按下时是高到低，使用下降沿：`FTSR.TR0=1`，`RTSR.TR0=0`。

### 6.10 `EXTI->PR` 是什么

`PR` 是 Pending Register，挂起标志寄存器。

EXTI0 触发后 `PR0=1`。清除方式是写 1 到 `PR0`。这和很多寄存器“写 0 清除”不同。

不清 PR，退出 ISR 后可能立刻再次进入。

### 6.11 `TIM2->DIER.UIE` 是什么

`DIER` 是 DMA/Interrupt Enable Register，`UIE` 是更新中断使能。

`UIE=1` 表示 TIM2 更新事件发生时允许发出中断请求。这是 TIM2 外设侧第一道门。

### 6.12 `TIM2->SR.UIF` 是什么

`UIF` 是 Update Interrupt Flag，更新事件标志。

TIM2 溢出时 `UIF=1`。本课 ISR 中检查它，然后通过写 0 清除。

如果不清 `UIF`，TIM2 中断会反复进入。

### 6.13 `NVIC_SetPriority()` 是什么

这是 CMSIS 提供的设置优先级函数。

它最终写 NVIC 的优先级寄存器 `IPR`。STM32F103 实际实现 4 位优先级，有效范围通常为 0 到 15。

### 6.14 `NVIC_EnableIRQ()` 是什么

这是 CMSIS 提供的使能中断函数。

它最终写 NVIC 的 `ISER` 寄存器，打开 NVIC 侧第二道门。外设侧和 NVIC 侧都打开，CPU 才会进入 ISR。

### 6.15 `HAL Callback` 是什么

HAL 回调是 HAL 在中断处理流程中调用的用户函数。

本课中：

- `EXTI0_IRQHandler()` 调 `HAL_GPIO_EXTI_IRQHandler()`
- HAL 检查并清 PR 后调 `HAL_GPIO_EXTI_Callback()`
- `TIM2_IRQHandler()` 调 `HAL_TIM_IRQHandler()`
- HAL 检查并清 UIF 后调 `HAL_TIM_PeriodElapsedCallback()`

它属于 HAL 工程层，不是中断向量表入口。真正写在启动文件向量表里的仍然是 `EXTI0_IRQHandler` 和 `TIM2_IRQHandler`。如果 IRQHandler 没有调用 HAL 对应 Handler，Callback 永远不会执行。

Callback 的好处是把“硬件清标志”和“用户业务”分开；排查时也要按层次走：先确认硬件是否进 IRQHandler，再确认 HAL Handler 是否清标志，最后确认 Callback 是否执行。

### 6.16 `优先级分组` 是什么

优先级分组决定 4 位优先级里多少位用于抢占优先级，多少位用于子优先级。抢占优先级决定能不能打断正在执行的中断，子优先级只决定同一抢占级别下谁先响应。

本课 HAL 默认使用 `NVIC_PRIORITYGROUP_4`，可以理解为 4 位都用于抢占优先级，子优先级不起作用。因此 `HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0)` 和 `HAL_NVIC_SetPriority(TIM2_IRQn, 2, 0)` 的关键差异就是抢占优先级。

## 7. 寄存器版代码讲解

### 7.1 已学步骤（快速过）

以下步骤在前 10 课已逐条拆解，本课用列表方式快速过：

- `system_clock_72mhz_init()`：72MHz 系统时钟。TIM2 挂在 APB1，因 APB1 分频≠1，定时器实际时钟 = 72MHz（STM32F1 特殊机制）。
- `led_pc13_init()`：PC13 推挽输出，BRR 拉低亮，BSRR 拉高灭。
- PA0 上拉输入：CRL 配 CNF0=10（上拉/下拉）+ ODR0=1（上拉），按键按下接 GND，未按下为高。
- AFIO 映射：`AFIO->EXTICR[0]` 选择 PA0 为 EXTI0 来源。EXTI 输入必须先过 AFIO 才能到达 EXTI 模块，这是 F1 的重要细节。
- EXTI 配置：`IMR.MR0=1` 开中断屏蔽，`FTSR.TR0=1` 下降沿触发，`RTSR.TR0=0` 上升沿关闭。
- 清挂起：`EXTI->PR = EXTI_PR_PR0` 写 1 清除，避免使能 NVIC 后立刻进入一次"幽灵中断"。
- TIM2 定时参数：PSC=7200-1, ARR=20000-1 → 计数频率 10kHz → 约 2 秒溢出一次。`TIM2->DIER.UIE=1` 允许更新事件发中断请求，`TIM2->SR.UIF` 写 0 清除，`TIM2->CR1.CEN=1` 启动计数器。

### 7.2 本课新增步骤概览

本课新增的是 NVIC 优先级配置和中断抢占观察：

```text
EXTI0:
  → NVIC_SetPriority(EXTI0_IRQn, 0)  // 高优先级
  → NVIC_EnableIRQ(EXTI0_IRQn)
  → EXTI0_IRQHandler: 清 PR → 翻转 LED → 忙等待 → 翻转 LED

TIM2:
  → NVIC_SetPriority(TIM2_IRQn, 2)   // 低优先级
  → NVIC_EnableIRQ(TIM2_IRQn)
  → TIM2_IRQHandler: 清 UIF → LED 亮 → 忙等待 → LED 灭

抢占现象：
  TIM2 长亮期间按 PA0 → EXTI0 优先级更高 → 抢占执行 → 短脉冲 → 返回 TIM2
```

### 7.3 设置 EXTI0 NVIC 优先级

`NVIC_SetPriority(EXTI0_IRQn, 0U)` 设置高优先级，`NVIC_EnableIRQ(EXTI0_IRQn)` 打开 NVIC 侧响应。

### 7.4 TIM2 定时参数

`PSC=7200-1`，`ARR=20000-1`。TIM2 计数频率为 10kHz，20000 次溢出一次，所以约 2 秒触发一次更新中断。

### 7.5 TIM2 外设侧中断使能

`TIM2->DIER |= TIM_DIER_UIE` 允许更新事件发中断请求。随后 NVIC 设置 TIM2 优先级为 2 并使能。

### 7.6 启动 TIM2

设置 `TIM2->CR1.CEN` 后计数器开始工作。没有 CEN，PSC/ARR 配好也不会产生更新事件。

### 7.7 `EXTI0_IRQHandler()`

ISR 中检查 `EXTI->PR.PR0`，写 1 清除，然后翻转 LED、忙等待、再翻转回来。这个短脉冲用于观察抢占。

### 7.8 `TIM2_IRQHandler()`

ISR 中检查 `TIM2->SR.UIF`，清除后让 LED 长亮，忙等待，再熄灭。这个长亮窗口用于给 EXTI0 抢占机会。

### 7.9 抢占现象如何发生

TIM2 ISR 正在忙等待时，如果 PA0 下降沿触发，EXTI0 请求进入 NVIC。因为优先级 0 高于 2，CPU 保存 TIM2 上下文，先执行 EXTI0 ISR，结束后再回到 TIM2 ISR。

## 8. HAL 版代码讲解

### 8.1 已学步骤（快速过）

- `HAL_Init()`：初始化 HAL 基础环境（SysTick tick、优先级分组默认 NVIC_PRIORITYGROUP_4）。
- `system_clock_72mhz_init()`（HAL 版）：`RCC_OscInitTypeDef` + `RCC_ClkInitTypeDef`，结果同寄存器版。
- `gpio_init()` 中的 PC13 和 PA0：`GPIO_InitTypeDef` 配输出 + `GPIO_MODE_IT_FALLING`。`GPIO_MODE_IT_FALLING` 封装了 AFIO 映射、EXTI IMR/FTSR、PR 清除等近 10 行寄存器操作。
- `TIM_HandleTypeDef` 和 `HAL_TIM_Base_Init()`：PSC/ARR/CounterMode 等字段写入 TIM2 寄存器。
- `HAL_TIM_Base_Start_IT()`：同时使能 `DIER.UIE` 和 `CR1.CEN`。

### 8.2 `HAL_Init()` 与优先级分组

`HAL_Init()` 初始化 HAL Tick，并设置默认优先级分组。本课使用抢占优先级比较，子优先级不参与观察。

### 8.3 `GPIO_MODE_IT_FALLING`

HAL 配 PA0 为下降沿外部中断模式。它封装 GPIO 输入、AFIO 映射、EXTI IMR、FTSR 和 PR 清除等操作。

### 8.4 `HAL_NVIC_SetPriority()`

HAL 版用它设置 EXTI0 优先级 0、TIM2 优先级 2。底层仍然写 NVIC 优先级寄存器。

### 8.5 `HAL_NVIC_EnableIRQ()`

打开 NVIC 侧中断响应，对应 CMSIS 的 NVIC 使能动作。

### 8.6 `TIM_HandleTypeDef htim2`

HAL 用这个句柄描述 TIM2。`Prescaler` 对应 PSC，`Period` 对应 ARR，`Instance=TIM2` 绑定硬件外设。

### 8.7 `HAL_TIM_Base_Init()`

根据 `htim2.Init` 写 TIM2 的 PSC、ARR、计数模式等寄存器。

### 8.8 `HAL_TIM_Base_Start_IT()`

它同时使能 TIM2 更新中断并启动计数器，对应 `DIER.UIE=1` 和 `CR1.CEN=1`。

这一步只打开 TIM2 这一路外设事件，不会替 EXTI0 做任何事。每个中断源都有自己的外设侧使能和 NVIC 侧使能，不能因为 TIM2 能进中断，就推断 EXTI0 配置也一定正确。

如果忘记调用 `HAL_TIM_Base_Start_IT()`，即使 `HAL_TIM_Base_Init()` 和 NVIC 都配置了，TIM2 也不会周期进入回调。初始化结构体只是写 PSC/ARR 等参数，启动中断和启动计数器是另一步。

### 8.9 HAL EXTI 回调链

`EXTI0_IRQHandler()` 是硬件入口，调用 `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)`。HAL 检查并清除 PR，然后调用 `HAL_GPIO_EXTI_Callback()`。

### 8.10 HAL TIM 回调链

`TIM2_IRQHandler()` 调用 `HAL_TIM_IRQHandler(&htim2)`。HAL 检查并清除 UIF，然后调用 `HAL_TIM_PeriodElapsedCallback()`。

### 8.11 Callback 里为什么不清标志

HAL 的 IRQHandler 已经清了对应标志。Callback 只写业务逻辑。重复清标志通常没必要，也容易混淆层次。

寄存器版需要你在 ISR 里亲自清 `EXTI->PR` 或 `TIM2->SR`；HAL 版把这件事放进 `HAL_GPIO_EXTI_IRQHandler()` 和 `HAL_TIM_IRQHandler()`。这就是同一个硬件动作在不同抽象层的位置变化。

如果 HAL Callback 反复执行，优先检查 IRQHandler 是否调用了正确的 HAL Handler、触发源是否持续有效，而不是先在 Callback 里乱写清标志代码。

### 8.12 SysTick 优先级

HAL_Init() 会配置 SysTick 作为 HAL tick，通常优先级较低。本课观察的是 EXTI0 和 TIM2 的抢占关系，SysTick 不应参与现象判断。

如果真实项目里某个高优先级 ISR 执行太久，SysTick 可能延迟，HAL_Delay 和超时判断也会受影响。本课故意用忙等待制造窗口，是教学演示，不是实时系统写法。

## 9. 两个版本真正应该怎么学

寄存器版按中断链路读：

```text
外设事件 -> 外设允许 -> 标志位 -> NVIC 优先级/使能 -> ISR -> 清标志
```

HAL 版按封装链路读：

```text
HAL_GPIO_Init/HAL_TIM_Init -> HAL_NVIC -> IRQHandler -> HAL Handler -> Callback
```

理解寄存器版后，HAL 回调就不再像“突然被调用的魔法函数”。

## 10. 检验问题清单

### 10.1 NVIC 属于 STM32 外设吗？

**答**：不属于普通 STM32 外设。NVIC 是 Cortex-M3 内核的一部分，负责中断优先级和响应。

### 10.2 为什么外设侧和 NVIC 侧都要使能？

**答**：外设侧允许事件发出中断请求，NVIC 侧允许 CPU 响应该 IRQ。两道门少任意一道都进不了 ISR。

### 10.3 优先级 0 和 2 谁更高？

**答**：优先级 0 更高。Cortex-M 中数字越小，优先级越高。

### 10.4 本课抢占的证据是什么？

**答**：TIM2 中断让 LED 长亮期间，按 PA0 能立刻插入短脉冲，说明 EXTI0 抢占了 TIM2。

### 10.5 `EXTI->PR` 怎么清？

**答**：写 1 清除对应位。本课写 `EXTI_PR_PR0` 清 EXTI0 挂起标志。

### 10.6 `TIM2->SR.UIF` 怎么清？

**答**：对 UIF 写 0 清除。不同外设清标志方式不同，不能混用。

### 10.7 HAL Callback 是硬件直接调用的吗？

**答**：不是。硬件进入 IRQHandler，IRQHandler 调 HAL Handler，HAL Handler 清标志后再调用用户 Callback。

### 10.8 为什么真实项目不该在 ISR 忙等待？

**答**：长时间占用 ISR 会阻塞低优先级中断和主循环，增加系统延迟。本课只是为了制造可观察窗口。

## 11. 工程实现步骤

### 11.1 需求分析

本课要观察中断抢占，所以需要一个低优先级长窗口和一个高优先级可手动触发事件。TIM2 提供长窗口，PA0/EXTI0 提供手动触发。

### 11.2 硬件核查

确认 PC13 LED 正常，PA0 按键按下接 GND，未按下由内部上拉保持高电平。

### 11.3 寄存器路线

配置 PA0 输入、AFIO 映射、EXTI 下降沿、TIM2 更新中断、NVIC 优先级和使能，然后分别在两个 ISR 中清标志并控制 LED。

### 11.4 HAL 路线

用 HAL GPIO 配外部中断，用 HAL TIM 配定时器，用 HAL NVIC 配优先级，业务逻辑写在 Callback 中。

### 11.5 工程思维

中断设计要短、快、可控。优先级只解决“谁先响应”，不解决 ISR 里做太多事导致的整体延迟。

优先级是最后的仲裁工具，不是业务设计的替代品。真正稳的系统会尽量缩短 ISR，把复杂处理推迟到主循环、状态机或事件队列中。优先级只用来保证少数真正紧急的事件能及时进入。

### 11.6 常见工程陷阱

忘开 AFIO、边沿选错、只开外设侧没开 NVIC、优先级数字理解反、忘清标志、在 ISR 里做耗时业务，都会导致现象不对。

还有一个陷阱是只设置优先级，不确认优先级分组。抢占是否发生看的是抢占优先级字段，不是两个参数肉眼不同就一定能抢占。HAL 项目里尤其要确认 `HAL_Init()` 或系统初始化有没有改分组。

本课在 ISR/Callback 里忙等待，是故意制造可观察窗口。真实工程应该在 ISR 里置标志、记录时间戳或投递事件，耗时 LED 动作放到主循环或任务里处理。

## 12. 运行现象

正常运行时，TIM2 大约每 2 秒让 LED 长亮一段时间。若在长亮期间按下 PA0，LED 会立刻出现反向短脉冲，然后回到 TIM2 的长亮流程。

如果按键只能在 TIM2 结束后才响应，说明抢占没有按预期发生，通常是优先级或 NVIC 设置问题。

## 13. 常见问题排查

### 13.1 按 PA0 没反应

检查 PA0 接线、内部上拉、AFIO EXTICR 映射、EXTI FTSR、IMR 和 NVIC EXTI0 使能。

### 13.2 TIM2 没有周期现象

检查 TIM2 时钟、PSC/ARR、DIER.UIE、CR1.CEN 和 NVIC TIM2 使能。

### 13.3 中断反复进入

检查 ISR 是否正确清标志。EXTI 是写 1 清 PR，TIM2 是清 SR.UIF。

### 13.4 PA0 不能抢占 TIM2

检查优先级是否 EXTI0 数字小于 TIM2。若两者相同或 EXTI0 更大，就不会抢占。

还要检查优先级分组。如果分组把有效位分给子优先级，而两个中断的抢占优先级实际相同，那么数字看起来不同也不会形成抢占。本课默认分组下 0 和 2 是抢占优先级差异，所以能观察到插入短脉冲。

确认现象时要在 TIM2 的长亮窗口内按键。如果 TIM2 ISR 已经结束，PA0 当然不会表现为“抢占”，只会表现为普通中断响应。

### 13.5 HAL 版 Callback 不执行

检查 IRQHandler 是否正确调用 HAL Handler，例如 `EXTI0_IRQHandler()` 是否调用 `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)`。

## 14. 本课最核心的结论

1. NVIC 是 Cortex-M 内核中断控制器，负责优先级和抢占。
2. 中断响应需要外设侧和 NVIC 侧两道门都打开。
3. Cortex-M 优先级数字越小，优先级越高。
4. 高优先级中断可以抢占正在执行的低优先级中断。
5. 不同外设清中断标志方式不同，必须按手册来。
6. HAL Callback 是 HAL Handler 调用的用户业务入口，不是硬件直接入口。
7. ISR 中长时间忙等待只适合教学演示，不适合真实工程。

## 15. 建议你现在怎么读这节课

先把“两道门模型”画出来，再对照 EXTI0 和 TIM2 各自的外设标志、使能位、NVIC 使能位。最后读 HAL 版调用链，把 IRQHandler 和 Callback 分清楚。

## 16. 扩展练习

1. 把 EXTI0 优先级改成 3，观察是否还能抢占 TIM2。
2. 把 TIM2 周期改成 1 秒，重新计算 PSC/ARR。
3. 去掉 EXTI 清标志，观察中断反复进入。
4. 把 ISR 中忙等待改成只置标志位，在主循环处理 LED。

## 17. 下一课预告

- 上一课：[32_can_basic](../32_can_basic/README.md)
- 下一课：[34_watchdog_iwdg](../34_watchdog_iwdg/README.md)
