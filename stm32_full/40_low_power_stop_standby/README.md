# 第 40 课：Stop 低功耗与 PA0 唤醒

## 1. 本课到底在学什么

本课表面现象是：程序进入 Stop 模式后，CPU 停止继续跑主循环；按下 PA0 后，EXTI0 中断把芯片唤醒，程序重新配置系统时钟，然后翻转 PC13 LED。

真正要学的是 Stop 模式的进入和恢复链路：

```text
PA0 配成 EXTI0 下降沿唤醒源
  -> PWR 时钟打开
  -> PWR->CR 选择 Stop 而不是 Standby
  -> SCB->SCR.SLEEPDEEP=1
  -> __WFI() 让 Cortex-M3 进入深睡眠
  -> PA0 下降沿触发 EXTI0
  -> CPU 唤醒并执行 EXTI0_IRQHandler()
  -> 退出 WFI 后重新配置 72MHz 系统时钟
  -> PC13 翻转作为唤醒反馈
```

上一课 Sleep 只是让 CPU 暂停，外设和时钟基本保持；本课 Stop 更深，唤醒后系统时钟不再保持在原来的 PLL 72MHz 状态，所以必须恢复时钟。这是 Stop 和 Sleep 最容易混淆的地方。

虽然目录名里带有 `standby`，但本课源码只演示 Stop。后面所有讲解都按当前代码展开，不把未实现的 Standby 流程塞进来。

## 2. 本课学习目标

学完本课，你应该能回答：

1. Stop 和上一课 Sleep 的差别是什么？
2. 为什么进入 Stop 前要配置 PA0/EXTI0/NVIC？
3. `SCB->SCR.SLEEPDEEP` 控制的是 Cortex-M 内核的哪一步？
4. `PWR->CR.PDDS=0` 为什么是本课进入 Stop 的必要条件？
5. `PWR->CR.CWUF` 为什么要在进入低功耗前写 1？
6. 为什么 Stop 唤醒后要重新调用 `system_clock_72mhz_init()`？
7. HAL 版 `HAL_SuspendTick()` 和 `HAL_ResumeTick()` 解决什么问题？
8. 如果按 PA0 后 LED 不翻转，应该按什么顺序排查？

## 3. 本课目录结构

```text
40_low_power_stop_standby/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接操作 `PWR->CR`、`SCB->SCR`、EXTI 和 NVIC。  
`hal/` 使用 `HAL_PWR_EnterSTOPMode()` 进入 Stop，并用 HAL GPIO/EXTI API 配置唤醒源。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- LED：PC13，常见 BluePill 为低电平点亮
- 按键：PA0 接按键，按下接 GND，内部上拉
- 时钟：HSE 8MHz，PLL 到 72MHz

PA0 在本课不是普通输入反馈，而是低功耗唤醒源。它必须先通过 EXTI0 和 NVIC 配好，否则 `WFI` 后没有外部中断能把 CPU 唤醒。

## 5. 先建立一个最基本的脑图

```text
初始化阶段
  -> 配 72MHz 系统时钟
  -> 配 PC13 输出
  -> 配 PA0 上拉输入
  -> AFIO 把 PA0 映射到 EXTI0
  -> EXTI0 下降沿中断
  -> NVIC 允许 EXTI0_IRQn

进入 Stop
  -> 打开 PWR 时钟
  -> PDDS=0 选择 Stop
  -> CWUF=1 清旧唤醒标志
  -> SLEEPDEEP=1
  -> __WFI()

唤醒后
  -> PA0 下降沿进入 EXTI0_IRQHandler()
  -> 清 EXTI->PR.PR0
  -> woken=1
  -> WFI 返回
  -> SLEEPDEEP 清 0
  -> 重新配置 HSE/PLL/总线分频
  -> PC13 翻转
```

这条链路里，EXTI/NVIC 负责“能不能醒”，PWR/SCB 负责“睡到哪一层”，RCC 负责“醒后系统时钟是否恢复到工程期望”。

## 6. 先认识本课里出现的核心名词

### 6.1 `Stop` 是什么

Stop 是 STM32F1 的深睡眠低功耗模式之一。它比 Sleep 更深：CPU 停止执行，系统时钟也会停止，SRAM 和寄存器内容保持。

本课使用 Stop 是为了展示“唤醒后还要恢复系统时钟”。如果忘记恢复时钟，程序能继续跑，但不再是原来的 72MHz，HAL 延时、串口波特率和后续外设时序都可能不对。

### 6.2 `PWR` 是什么

`PWR` 是 STM32 的电源控制外设，挂在 APB1 总线上。

本课进入 Stop 前必须打开 `RCC_APB1ENR_PWREN`，否则写 `PWR->CR` 不可靠。PWR 在这里负责选择 Stop 路线，并管理唤醒标志。

### 6.3 `PWR->CR.PDDS` 是什么

`PDDS` 是 Power Down Deepsleep 位。

`PDDS=0` 时，`SLEEPDEEP=1` 加 `WFI` 进入 Stop。本课在 `enter_stop_mode()` 里明确清 `PDDS`，保证代码走当前 Demo 的 Stop 路线。

### 6.4 `PWR->CR.CWUF` 是什么

`CWUF` 是 Clear Wakeup Flag，清唤醒标志位。

进入低功耗前清旧唤醒标志，可以避免上一次残留状态干扰这一次判断。它不是“允许唤醒”，只是清除历史标志。

### 6.5 `SCB->SCR.SLEEPDEEP` 是什么

`SLEEPDEEP` 属于 Cortex-M3 内核的系统控制寄存器 `SCR`，不是 STM32 普通外设位。

`SLEEPDEEP=0` 时 `WFI` 进入 Sleep；本课把它置 1，让 `WFI` 进入由 PWR 配置好的 Stop。唤醒后清 0，避免后续普通 `WFI` 继续走深睡眠路线。

### 6.6 `__WFI()` 是什么

`WFI` 是 Wait For Interrupt 指令。

它让 CPU 等待一个可响应中断。本课 PA0/EXTI0 就是这个中断来源。如果 EXTI0 或 NVIC 没配好，CPU 进入 Stop 后就没有本课预期的按键唤醒。

### 6.7 `EXTI0` 是什么

EXTI0 是外部中断线 0。本课把 PA0 映射到 EXTI0，并选择下降沿触发。

它在链路里的位置是唤醒源：按键从高电平变低，EXTI0 产生挂起请求，NVIC 响应该请求，CPU 才能从 WFI 返回。

### 6.8 `woken` 是什么

`woken` 是 `volatile` 全局标志，由 ISR 写、主循环读。

它不负责唤醒硬件，只负责让主循环知道这次 WFI 返回确实经过了 EXTI0 中断。没有 `volatile`，编译器可能缓存变量读取，主循环看不到 ISR 更新。

### 6.9 `HAL_PWR_EnterSTOPMode()` 是什么

HAL 版用它进入 Stop。

`PWR_LOWPOWERREGULATOR_ON` 选择 Stop 中电压调节器低功耗运行；`PWR_STOPENTRY_WFI` 表示用 WFI 进入。底层仍然是配置 PWR/SCB 并执行等待指令。

### 6.10 `HAL_SuspendTick()` 是什么

HAL 默认 SysTick 每 1ms 中断一次。如果进入 Stop 前不暂停 tick，SysTick 或相关节拍行为可能干扰低功耗观察。

本课 HAL 版进入 Stop 前暂停 tick，唤醒并恢复系统时钟后再恢复 tick，这是低功耗 HAL 工程里很常见的顺序。

### 6.11 `HAL_ResumeTick()` 是什么

`HAL_ResumeTick()` 是 HAL 恢复 SysTick 节拍的 API。

本课 HAL 版唤醒后先重新配置系统时钟，再恢复 tick。这样 `HAL_Delay(500)` 使用的是恢复后的时钟节拍。如果先恢复 tick，再处理时钟，调试时会更难判断延时异常到底来自 tick 还是 RCC。

### 6.12 `RCC` 在 Stop 唤醒后为什么又出现

`RCC` 是 Reset and Clock Control，复位和时钟控制模块。

Stop 模式会让 HSE/PLL 等高速时钟停止。唤醒后 CPU 能继续执行，但系统时钟配置不等于自动回到进入 Stop 前的 72MHz。本课主循环在 `enter_stop_mode()` 返回后再次调用 `system_clock_72mhz_init()`，就是把 RCC 状态恢复成课程统一的运行条件。

### 6.13 `FLASH->ACR` 为什么也要跟着时钟恢复讲

`FLASH->ACR` 控制 Flash 预取和等待周期。

72MHz 下执行 Flash 中的代码需要合适等待周期。`system_clock_72mhz_init()` 每次恢复 72MHz 前都设置 `FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2`，避免 CPU 高频取指不稳定。它不是低功耗寄存器，但属于“醒后恢复到 72MHz”这条链路的一部分。

### 6.14 `EXTI->IMR` 在唤醒里控制什么

`IMR` 是 EXTI 的 Interrupt Mask Register，中断屏蔽寄存器。

本课设置 `EXTI->IMR |= EXTI_IMR_MR0`，允许 EXTI0 把 PA0 下降沿事件送到 NVIC。若只配置了下降沿但没打开 IMR，PA0 电平变化不会成为可响应中断，Stop 中的 `WFI` 也就不会被它唤醒。

### 6.15 `EXTI->FTSR` 在唤醒里控制什么

`FTSR` 是 Falling Trigger Selection Register，下降沿触发选择寄存器。

PA0 使用内部上拉，松开为高，按下接 GND 为低，所以按下动作是下降沿。本课设置 `FTSR.TR0=1`。如果误设成上升沿，现象会变成松开按键时唤醒，容易误判按键无效。

### 6.16 `NVIC_EnableIRQ(EXTI0_IRQn)` 在唤醒里控制什么

EXTI 负责产生中断请求，NVIC 负责让 Cortex-M3 响应该请求。

本课 `NVIC_EnableIRQ(EXTI0_IRQn)` 打开 CPU 侧入口。缺这一步时，EXTI 的 PR 可能已经挂起，但 CPU 不会进入 ISR，`WFI` 也不会按本课预期返回。

## 7. 寄存器版代码逐步讲解

### 7.1 `system_clock_72mhz_init()`

代码先配置 Flash 等待周期、打开 HSE、配置 PLL x9、设置 APB1 二分频，最后切换 SYSCLK 到 PLL。

这一步在上电时需要做一次，在 Stop 唤醒后也要再做一次。因为 Stop 会停掉 HSE/PLL，醒来后系统通常回到内部时钟状态。

### 7.2 `pc13_led_init()`

打开 GPIOC 时钟，把 PC13 配成推挽输出，并先输出高电平让 LED 熄灭。

PC13 是现象层输出，用来观察唤醒后程序是否继续执行。

### 7.3 `pa0_exti_init()` 打开 GPIOA 和 AFIO

`RCC->APB2ENR` 同时打开 GPIOA 和 AFIO。GPIOA 负责 PA0 引脚输入，AFIO 负责把 PA0 连接到 EXTI0。

少开 AFIO 时钟，EXTI 映射可能不是你以为的来源；少开 GPIOA 时钟，PA0 模式配置不可靠。

### 7.4 PA0 上拉输入

`GPIOA->CRL` 清掉 PA0 的 MODE/CNF，再设置 `CNF0_1`，表示输入上拉/下拉模式；随后 `GPIOA->BSRR=GPIO_BSRR_BS0` 让 ODR0 为 1，选择内部上拉。

因此松开按键读高，按下接地读低，本课选择下降沿唤醒。

### 7.5 EXTI0 配置

`AFIO->EXTICR[0]` 清 EXTI0 字段，选择 PA0。`EXTI->IMR.MR0=1` 允许中断请求，`EXTI->FTSR.TR0=1` 选择下降沿。

这三步决定 PA0 电平变化能不能变成中断请求。

### 7.6 NVIC 使能

`NVIC_EnableIRQ(EXTI0_IRQn)` 打开 CPU 侧响应。EXTI 配好只是外设侧发请求，NVIC 使能后 Cortex-M3 才会进入 `EXTI0_IRQHandler()`。

### 7.7 `enter_stop_mode()` 打开 PWR 时钟

`RCC->APB1ENR |= RCC_APB1ENR_PWREN` 打开 PWR 外设时钟。

Stop 选择在 PWR 控制寄存器里，不开 PWR 时钟就谈不上可靠配置低功耗模式。

### 7.8 选择 Stop 并清唤醒标志

`PWR->CR &= ~PWR_CR_PDDS` 选择 Stop。`PWR->CR |= PWR_CR_CWUF` 清旧唤醒标志。

这一步把本次深睡眠路线限定为 Stop。

### 7.9 `SLEEPDEEP` 和 `__WFI()`

`SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk` 告诉 Cortex-M3 这次 WFI 要进入深睡眠。随后 `__WFI()` 停止 CPU，等待 EXTI0 中断。

唤醒后代码清掉 `SLEEPDEEP`，这是工程习惯，避免后续再执行 WFI 时误入深睡眠。

### 7.10 `EXTI0_IRQHandler()`

ISR 检查 `EXTI->PR.PR0`，写 1 清除挂起位，然后设置 `woken=1`。

如果不清 PR，退出中断后可能反复进入；如果不设 `woken`，主循环仍会继续，但无法判断是否为按键唤醒。

### 7.11 主循环恢复时钟

`enter_stop_mode()` 返回后立刻调用 `system_clock_72mhz_init()`。这是 Stop 课的核心：醒来不等于时钟自动恢复。

确认 `woken` 后翻转 PC13 并延时，作为一次唤醒完成的可见反馈。

### 7.12 为什么 `woken=0` 放在进入 Stop 前

主循环每次进入 Stop 前先把 `woken` 清零。

这样 `enter_stop_mode()` 返回后，`woken` 只代表这一次等待期间是否经过 EXTI0 ISR。如果不清零，上一轮的唤醒结果会残留，主循环可能误以为本轮也由 PA0 唤醒。

### 7.13 Stop 返回后先恢复时钟再延时

寄存器版在判断 LED 反馈前已经恢复了 72MHz。

`delay_cycles(3600000U)` 是按 CPU 执行速度粗略消耗时间。若不恢复 PLL，CPU 频率变低，延时会明显变长。这个现象正好能用来验证“Stop 唤醒后时钟需要恢复”。

### 7.14 为什么 ISR 里不做长延时

`EXTI0_IRQHandler()` 只清标志、置 `woken`。

低功耗唤醒中断应该短。真正的 LED 延时反馈放回主循环，这样中断只负责唤醒事件，不把 CPU 长时间困在 ISR 里。

### 7.15 `AFIO->EXTICR[0]` 为什么清零就是 PA0

STM32F1 的 EXTI 线需要通过 AFIO 选择来自哪个 GPIO 端口。

EXTI0 字段为 0 表示 PA0，字段为 1 表示 PB0，字段为 2 表示 PC0。源码清掉 `AFIO_EXTICR1_EXTI0`，就是选择 PA0。若这里映射到别的端口，PA0 按键不会触发 EXTI0。

### 7.16 为什么没有配置 EXTI 上升沿

源码只设置 `FTSR.TR0`，没有设置 `RTSR.TR0`。

这表示只在按下瞬间唤醒，不在松开瞬间唤醒。这样每次按键动作只产生一个主要唤醒事件，现象更容易观察。

### 7.17 Stop 前没有关 GPIO 时钟意味着什么

代码没有手动关闭 GPIOA/GPIOC 时钟，而是直接进入 Stop。

Stop 模式会由芯片低功耗机制处理时钟停止和恢复边界；GPIO 配置寄存器内容保持，所以唤醒后 PC13 和 PA0 配置仍在。需要恢复的是系统高速时钟，不是重新配置所有 GPIO。

### 7.18 如何用调试器验证寄存器版链路

可以在 `enter_stop_mode()` 前观察 `SCB->SCR`，确认 `SLEEPDEEP` 即将置位；在 `EXTI0_IRQHandler()` 里观察 `EXTI->PR`，确认 PR0 被置位并随后写 1 清除；在 `system_clock_72mhz_init()` 返回后观察 `RCC->CFGR.SWS` 是否回到 PLL。

这三个观察点分别对应“进入深睡眠”“唤醒源触发”“醒后时钟恢复”。

## 8. HAL 版代码逐步讲解

### 8.1 `HAL_Init()`

初始化 HAL 基础环境和 SysTick。低功耗前后要注意 tick，因为 HAL 延时依赖它。

### 8.2 HAL 时钟配置

`HAL_RCC_OscConfig()` 配 HSE/PLL，`HAL_RCC_ClockConfig()` 配 SYSCLK/HCLK/PCLK 和 Flash latency。它们对应寄存器版的 RCC 和 FLASH 配置。

Stop 唤醒后 HAL 版也重新调用 `system_clock_72mhz_init()`。

### 8.3 `GPIO_MODE_IT_FALLING`

PA0 使用下降沿外部中断模式。HAL 内部会配置 GPIO 输入、AFIO/EXTI 映射和下降沿触发。

这对应寄存器版的 PA0 CRL、EXTICR、IMR、FTSR。

### 8.4 `HAL_NVIC_SetPriority()` 和 `HAL_NVIC_EnableIRQ()`

这两个 API 设置并使能 EXTI0 的 CPU 侧响应。

没有 NVIC 使能，PA0 下降沿只能在 EXTI 里挂起，不能唤醒正在 WFI 的 CPU。

### 8.5 `HAL_SuspendTick()`

进入 Stop 前暂停 HAL tick，减少 SysTick 对低功耗过程的干扰。

暂停 tick 后不要再依赖 `HAL_Delay()` 计时，直到唤醒并 `HAL_ResumeTick()`。

### 8.6 `HAL_PWR_EnterSTOPMode()`

```c
HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
```

该 API 选择 Stop 参数并执行 WFI。它封装了 PWR/SCB/WFI 的组合，但不负责唤醒后自动恢复 72MHz 时钟。

### 8.7 HAL EXTI 入口

`EXTI0_IRQHandler()` 调用 `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)`。HAL 检查并清除 EXTI PR，然后调用用户回调。

本课回调里没有业务，只用中断本身唤醒 Stop。

### 8.8 `HAL_ResumeTick()`

唤醒后先恢复系统时钟，再恢复 HAL tick。顺序反了，HAL tick 可能基于错误的时钟状态运行。

随后翻转 PC13 并 `HAL_Delay(500)`，观察唤醒反馈。

### 8.9 `__HAL_RCC_PWR_CLK_ENABLE()`

HAL 版在 PA0 EXTI 初始化里打开 PWR 时钟。

虽然 `HAL_PWR_EnterSTOPMode()` 是进入 Stop 的 API，但 PWR 外设时钟仍要先打开。这个宏对应寄存器版 `RCC->APB1ENR |= RCC_APB1ENR_PWREN`。

### 8.10 HAL EXTI 回调为什么为空

本课 HAL 版的 `HAL_GPIO_EXTI_Callback()` 只接收参数并丢弃，没有写业务。

这是因为本课用 EXTI0 的中断到来作为 WFI 唤醒源，唤醒后的业务放在 `HAL_PWR_EnterSTOPMode()` 返回之后执行。回调为空不是漏写，而是刻意把“唤醒事件”和“主循环反馈”分开。

### 8.11 `PWR_LOWPOWERREGULATOR_ON`

这个参数表示 Stop 中使用低功耗调节器。

它影响 Stop 期间功耗和唤醒特性，但不改变本课最核心的链路：进入 Stop、由 EXTI0 唤醒、恢复系统时钟。若换成其他调节器选项，应重新按手册确认功耗和唤醒时间。

### 8.12 `PWR_STOPENTRY_WFI`

这个参数表示用 WFI 指令进入 Stop。

HAL 低功耗 API 还支持其他入口方式，但本课和寄存器版保持一致，都使用“等待中断”。因此唤醒条件必须是 NVIC 可响应的中断，PA0/EXTI0 配置就成了进入 Stop 前的必要准备。

### 8.13 HAL 版为什么不使用 `woken`

寄存器版用 `woken` 区分本次是否由 EXTI0 ISR 唤醒；HAL 版没有这个变量，`HAL_PWR_EnterSTOPMode()` 返回后直接翻转 LED。

这意味着 HAL 版更像“只要从 Stop 返回就反馈一次”。当前 Demo 只有 PA0/EXTI0 这一个预期唤醒源，所以这样写能成立；如果以后增加多个唤醒源，就应该像寄存器版一样记录来源。

### 8.14 HAL 回调参数为什么仍要判断

当前回调只写 `(void)GPIO_Pin;`，没有判断引脚。

这是因为源码只有 EXTI0 这一路 HAL EXTI 入口。真实项目里多个 EXTI 共用回调时，必须判断 `GPIO_Pin`，否则不同按键或唤醒源会混在一起。

### 8.15 `GPIO_PULLUP` 对应底层哪两步

HAL 版 PA0 设置 `gpio.Pull = GPIO_PULLUP`。

在 F103 底层，上拉输入并不只是一个字段：GPIO 模式要配置成输入上拉/下拉，ODR 对应位还要置 1 选择上拉。本课按键接 GND，所以松开必须被上拉成高电平，下降沿才有明确起点。

### 8.16 `GPIO_MODE_IT_FALLING` 封装了哪些动作

这个 HAL 模式不只是普通输入。

它表达“GPIO 输入 + EXTI 下降沿中断”。HAL 会处理 GPIO 输入配置、EXTI 线触发配置和相关映射。之后仍要手动设置 NVIC 优先级并使能中断，否则 CPU 侧不会响应。

### 8.17 HAL 版调试时看哪些变量和寄存器

可以看 `EXTI->PR` 是否出现 PR0，`SCB->SCR` 是否在进入 Stop 时设置深睡眠位，`RCC->CFGR` 是否在唤醒后回到 PLL，`uwTick` 是否在 `HAL_ResumeTick()` 后继续变化。

这些观察点比单看 LED 更可靠，因为 LED 只告诉你主循环最终执行过，不能告诉你卡在哪个层级。

## 9. 两个版本真正应该怎么学

寄存器版按“唤醒源 -> PWR/SCB -> WFI -> EXTI ISR -> 恢复时钟”读。HAL 版按“GPIO 中断配置 -> 暂停 tick -> 进入 Stop -> 恢复时钟 -> 恢复 tick”读。

两者做的是同一件事。寄存器版让你看见每个硬件开关，HAL 版展示工程里常用的封装顺序。

## 10. 检验问题清单

### 10.1 Stop 唤醒后为什么要重配时钟？

**答**：Stop 会停止 HSE/PLL 等高速时钟，唤醒后系统不自动回到原来的 72MHz 配置，所以要重新配置 RCC 和 Flash latency。

### 10.2 `SLEEPDEEP=1` 为什么还要配 PWR？

**答**：`SLEEPDEEP` 只告诉 Cortex-M 走深睡眠入口，STM32 侧还要通过 PWR 配置具体低功耗行为。本课清 `PDDS`，让入口对应 Stop。

### 10.3 PA0 为什么能唤醒 Stop？

**答**：PA0 被配置成 EXTI0 下降沿中断，并且 NVIC 使能了 EXTI0_IRQn。WFI 等待可响应中断，所以 PA0 中断能唤醒 CPU。

### 10.4 `CWUF` 是允许唤醒吗？

**答**：不是。`CWUF` 是清旧唤醒标志，避免历史标志干扰判断。

### 10.5 HAL 版为什么暂停 SysTick？

**答**：SysTick 是周期中断，会影响低功耗观察。进入 Stop 前暂停 tick，唤醒恢复时钟后再恢复 tick，是更清楚的工程顺序。

### 10.6 为什么要清 `EXTI->PR`？

**答**：PR 是 EXTI 挂起标志。触发后不清，退出 ISR 后可能再次进入中断。

### 10.7 `woken` 为什么要 `volatile`？

**答**：它由 ISR 写、主循环读。`volatile` 告诉编译器每次都从内存读取，避免主循环看不到中断更新。

### 10.8 本课是否需要讲 Standby 流程？

**答**：不需要。当前源码只调用 Stop 相关流程，文档只围绕 Stop 的进入、唤醒和时钟恢复展开。

### 10.9 EXTI 配好了为什么还要 NVIC？

**答**：EXTI 是外设侧请求，NVIC 是 CPU 侧响应。WFI 等待的是能被 CPU 响应的中断，所以两边都要配。

### 10.10 为什么 PA0 要上拉？

**答**：按键按下接 GND，松开时必须有确定高电平。上拉保证松开为 1，按下为 0，下降沿才稳定。

## 11. 工程实现步骤

### 11.1 需求分析

需要证明 CPU 能进入更深低功耗，并能由 PA0 唤醒后继续执行。PC13 只负责反馈结果。

### 11.2 硬件核查

确认 PA0 按下接 GND，内部上拉可用；确认 PC13 LED 正常。

### 11.3 寄存器路线

配置 PA0/EXTI0/NVIC，配置 PWR 和 `SLEEPDEEP`，执行 WFI，唤醒后清深睡眠位并恢复系统时钟。

### 11.4 HAL 路线

用 `GPIO_MODE_IT_FALLING` 配 PA0，使用 `HAL_PWR_EnterSTOPMode()` 进入 Stop，唤醒后重新配置时钟并恢复 HAL tick。

### 11.5 工程思维

低功耗不是只调用一个进入函数。进入前要准备唤醒源，进入时要选择模式，唤醒后要恢复时钟和软件节拍。

### 11.6 常见工程陷阱

忘记配置唤醒中断、Stop 后忘记恢复时钟、SysTick 干扰低功耗观察、没有清 EXTI 挂起位，都会让现象不符合预期。

进入 Stop 前还要确认没有未处理的挂起中断。否则 `__WFI()` 可能刚执行就立刻被已有中断唤醒，看起来像“根本没睡进去”。

Stop 唤醒后恢复时钟时，也要保证 Flash latency 和总线分频仍符合 72MHz 条件。只打开 PLL 而忘了等待 `PLLRDY` 或切换 `SWS`，后面的延时判断仍不可信。

## 12. 运行现象

程序初始化后进入 Stop。按下 PA0 后，CPU 被 EXTI0 唤醒，恢复系统时钟，PC13 翻转一次并延时，然后再次进入 Stop。

如果没有按键，主循环不会持续翻转 LED，因为 CPU 正在 Stop 中等待中断。

寄存器版可以在调试器里观察 `woken`：进入 Stop 前为 0，按 PA0 唤醒后 ISR 把它置 1。HAL 版没有 `woken`，可通过 PC13 翻转和断点位置确认 Stop 返回。

如果接了电流表，理论上 Stop 期间电流应低于普通运行。但调试器连接、板载电源 LED、外部上拉下拉和 ST-Link 供电都会影响实际读数。本课功能验收以唤醒和时钟恢复为主。

## 13. 常见问题排查

### 13.1 按 PA0 没反应

检查 PA0 接线、内部上拉、AFIO 映射、EXTI 下降沿、IMR 和 NVIC 使能。

### 13.2 只能唤醒一次或反复进中断

检查 `EXTI->PR.PR0` 是否在 ISR 中写 1 清除。

### 13.3 唤醒后延时或外设时序不对

检查 Stop 返回后是否重新配置 HSE/PLL 和总线分频。

### 13.4 HAL 版很快又醒来

检查是否在进入 Stop 前调用 `HAL_SuspendTick()`，以及是否存在其他已使能中断。当前 Demo 预期唤醒源是 PA0/EXTI0。

### 13.5 程序像没有恢复到原来速度

检查 Stop 返回后是否重新执行 `system_clock_72mhz_init()`。如果没有恢复 PLL 72MHz，延时和后续外设时序都会变。

### 13.6 一上电就像被唤醒过

检查进入 Stop 前是否清了唤醒标志，寄存器版对应 `PWR->CR |= PWR_CR_CWUF`。旧标志不清会让你误判当前唤醒来源。

### 13.7 调试时低功耗现象不稳定

调试器连接会影响低功耗观察，断点也会改变时间关系。先确认功能链路：PA0 能唤醒、PR 能清、时钟能恢复；再单独测电流。

### 13.8 按键松开才唤醒

检查是否误用了上升沿触发。当前按键是上拉输入、按下接地，本课应使用下降沿 `FTSR.TR0`。

### 13.9 `woken` 一直是 0

确认是否真的进入了 `EXTI0_IRQHandler()`。如果 PR0 置位但 ISR 不进，查 NVIC；如果 PR0 不置位，查 AFIO 映射、PA0 电平和 EXTI 触发边沿。

## 14. 本课最核心的结论

1. Stop 比 Sleep 更深，唤醒后必须关注系统时钟恢复。
2. `SLEEPDEEP` 属于 Cortex-M 内核，PWR 决定深睡眠具体模式。
3. PA0 唤醒依赖 EXTI 和 NVIC，两道链路都要配置。
4. `PDDS=0` 是本课 Stop 路线的必要配置。
5. HAL 低功耗 API 封装了底层动作，但不会替你自动恢复 72MHz 时钟。
6. 低功耗工程要同时考虑唤醒源、时钟恢复和 tick 恢复。

## 15. 建议你现在怎么读这节课

先把上一课 Sleep 的 `SLEEPDEEP=0` 和本课 Stop 的 `SLEEPDEEP=1` 对比清楚。再重点读 `enter_stop_mode()`，最后读 HAL 版进入 Stop 前后的 tick 和时钟恢复顺序。

## 16. 扩展练习

1. 去掉唤醒后的 `system_clock_72mhz_init()`，观察延时和后续外设行为变化。
2. 在 ISR 中翻转 LED，比较 ISR 反馈和主循环反馈的区别。
3. 暂时不调用 `HAL_SuspendTick()`，观察低功耗现象是否更难判断。
4. 把 PA0 的触发边沿改成上升沿，观察按下和松开哪个动作唤醒。

## 17. 下一课预告

- 上一课：[39_low_power_sleep](../39_low_power_sleep/README.md)
- 下一课：[41_debug_toolchain](../41_debug_toolchain/README.md)
