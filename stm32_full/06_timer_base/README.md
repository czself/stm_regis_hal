# 第 6 课：定时器基础

## 1. 本课到底在学什么

这节课表面上是在做：

- 配置 `TIM2` 每 1 秒产生一次更新中断
- 在 `TIM2_IRQHandler()` 或 HAL 回调里翻转 `PC13`
- 让板载 LED 每 1 秒改变一次亮灭状态

真正学习的是 STM32 通用定时器的基础定时链路：

```text
TIM2 输入时钟 = 72MHz
  -> PSC = 7200 - 1，把计数频率分到 10kHz
  -> CNT 从 0 向上计数
  -> ARR = 10000 - 1，计满 10000 个 tick
  -> 产生更新事件
  -> SR.UIF 置位
  -> DIER.UIE 允许更新事件发中断
  -> NVIC 放行 TIM2_IRQn
  -> 进入 TIM2_IRQHandler()
  -> 清 UIF 并翻转 PC13
```

上一课 `SysTick` 是 Cortex-M3 内核自带的系统节拍定时器。本课开始进入 STM32 外设定时器。后面的输出比较、PWM、输入捕获、编码器接口，本质都建立在 TIM 能稳定计数、产生事件、驱动通道或中断的基础上。

## 2. 本课学习目标

学完本课，你应该能回答：

1. `TIM2` 为什么是 STM32 外设，而不是 Cortex-M 内核组件？
2. 为什么 `TIM2` 挂在 APB1 上，但本课定时器输入时钟仍然是 72MHz？
3. `PSC = 7200 - 1` 和 `ARR = 10000 - 1` 为什么刚好得到 1 秒？
4. `CNT`、`PSC`、`ARR`、`SR.UIF`、`DIER.UIE`、`CR1.CEN` 分别控制哪一层行为？
5. 为什么只产生 `UIF` 还不够，必须打开 `UIE` 和 NVIC？
6. 为什么 `TIM2_IRQHandler()` 里必须先判断并清除 `UIF`？
7. HAL 版的 `Prescaler`、`Period`、`HAL_TIM_Base_Start_IT()` 分别对应哪些寄存器动作？
8. `HAL_TIM_PeriodElapsedCallback()` 是谁调用的，为什么业务代码写在这里？

## 3. 本课目录结构

```text
06_timer_base/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- 观察对象：板载 `PC13` LED
- 时钟假设：HSE 8MHz，PLL 后 `SYSCLK/HCLK = 72MHz`
- 总线配置：APB1 = 36MHz，APB2 = 72MHz

本课没有额外接线。

## 5. 先建立一个最基本的脑图

本课完整链路如下：

```text
system_clock_72mhz_init()
  -> SYSCLK/HCLK = 72MHz
  -> APB1 = 36MHz，但 APB1 分频不为 1
  -> TIM2 定时器时钟 = 2 x PCLK1 = 72MHz
  -> led_pc13_init()
  -> GPIOC 时钟打开，PC13 配成输出
  -> tim2_base_init()
  -> RCC->APB1ENR 打开 TIM2 时钟
  -> TIM2->PSC = 7200 - 1，计数频率变成 10kHz
  -> TIM2->ARR = 10000 - 1，10000 个计数产生一次更新
  -> 清 TIM2->SR.UIF，避免旧标志误触发
  -> TIM2->DIER.UIE = 1，允许更新中断
  -> NVIC_EnableIRQ(TIM2_IRQn)，CPU 放行 TIM2 中断
  -> TIM2->CR1.CEN = 1，计数器开始跑
  -> CNT 溢出产生更新事件，UIF 置位
  -> TIM2_IRQHandler() 清 UIF 并翻转 PC13
```

这条链路里最关键的是三层开关：

1. `RCC->APB1ENR.TIM2EN`：TIM2 外设有没有时钟。
2. `TIM2->DIER.UIE`：TIM2 更新事件是否允许发中断请求。
3. `NVIC_EnableIRQ(TIM2_IRQn)`：CPU 是否接收 TIM2 这个中断。

少任何一层，LED 都不会按 1 秒中断节奏翻转。

## 6. 核心名词速查

### 6.1 TIM2 与 APB1 时钟

TIM2 是 STM32 通用定时器，挂在 APB1 总线，不是 Cortex-M 内核组件（区别于 SysTick）。

本课时钟链路：

```text
HCLK = 72MHz → APB1 分频 /2 → PCLK1 = 36MHz
但 APB1 分频≠1 时，定时器时钟 = 2 × PCLK1 = 72MHz
```

按 36MHz 算 PSC/ARR，周期会错一倍。

### 6.2 核心寄存器

| 寄存器 | 作用 | 本课值 | 关键规则 |
|---|---|---|---|
| `RCC_APB1ENR.TIM2EN` | TIM2 时钟开关 | 开 | 不开则 TIM2 不工作 |
| `PSC` | 预分频 | 7199 | 计数频率 = TIM2CLK / (PSC+1) |
| `ARR` | 自动重装载 | 9999 | CNT 数到 ARR 产生更新事件 |
| `CNT` | 当前计数值 | 硬件自增 | 调试器可观察，不动则查时钟和 CEN |
| `SR.UIF` | 更新中断标志 | 中断里清 | 不清会反复进中断 |
| `DIER.UIE` | 更新中断使能 | 开 | 不开则 UIF 置位但不进中断 |
| `CR1.CEN` | 计数器使能 | 开 | 不开则 CNT 不跑 |

### 6.3 三层开关

TIM2 中断要进入 CPU，必须同时满足：

```text
1. RCC_APB1ENR.TIM2EN = 1  → TIM2 有时钟
2. DIER.UIE = 1             → 更新事件允许申请中断
3. NVIC_EnableIRQ(TIM2_IRQn) → CPU 放行 TIM2 中断
```

少任何一层，都不进 `TIM2_IRQHandler()`。

更新事件 = CNT 数到 ARR 并溢出回 0。`UIF` 是"事件发生了"，`UIE` 是"允许申请中断"，两者缺一不可。

### 6.4 HAL 对应关系

| HAL | 寄存器 |
|---|---|
| `__HAL_RCC_TIM2_CLK_ENABLE()` | `RCC->APB1ENR \|= TIM2EN` |
| `htim2.Init.Prescaler` | `TIM2->PSC` |
| `htim2.Init.Period` | `TIM2->ARR` |
| `HAL_TIM_Base_Init()` | 写入 PSC/ARR/CR1 等 |
| `HAL_TIM_Base_Start_IT()` | 开 CEN + UIE |
| `HAL_TIM_Base_Start()` | 只开 CEN，不开中断 |
| `HAL_TIM_PeriodElapsedCallback()` | ISR 中判断 UIF 后的回调 |
| `HAL_TIM_IRQHandler()` | ISR 里判断标志+清标志+调回调 |

回调里要判断 `htim->Instance == TIM2`，因为多个定时器共用同一个回调。`TIM2_IRQHandler()` 里做三件事：判断 UIF、清 UIF、翻转 PC13。函数名必须和启动文件向量表匹配。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 完整逻辑

```c
int main(void)
{
    system_clock_72mhz_init();
    led_pc13_init();
    tim2_base_init();

    while (1) {
    }
}
```

主循环为空，LED 翻转完全由 TIM2 更新中断驱动。顺序不能乱：先配时钟（PSC/ARR 依赖 TIM2CLK），再配 PC13（中断里要翻转 LED），最后配 TIM2。

### 7.2 `tim2_base_init()` 七步

| 步骤 | 代码 | 作用 |
|---|---|---|
| 1 | `RCC->APB1ENR \|= RCC_APB1ENR_TIM2EN` | 打开 TIM2 时钟 |
| 2 | `TIM2->PSC = 7200U - 1U` | 72MHz / 7200 = 10kHz |
| 3 | `TIM2->ARR = 10000U - 1U` | 10kHz 下 10000 个 tick = 1s |
| 4 | `TIM2->SR &= ~TIM_SR_UIF` | 清旧标志，避免一上电误触发 |
| 5 | `TIM2->DIER \|= TIM_DIER_UIE` | 允许更新事件发中断 |
| 6 | `NVIC_EnableIRQ(TIM2_IRQn)` | CPU 放行 TIM2 中断 |
| 7 | `TIM2->CR1 \|= TIM_CR1_CEN` | 启动计数器 |

注意 `+1` 规则：PSC=7199 意味着分频 7200，ARR=9999 意味着计数 10000。写成 7200 则实际分频 7201，有误差。

### 7.3 `TIM2_IRQHandler()` 为什么要先判断 UIF

TIM2 只有一个中断入口，但内部有多种中断源（更新、捕获比较、触发等）。先判断 `UIF` 确认是更新事件，后续增加其他中断源时逻辑才不会乱。

先清 `UIF` 再翻转 LED，尽快告诉硬件"已处理"。不清 `UIF` 的典型现象：CPU 刚退出 ISR 又立刻进去，主循环几乎无法运行。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 完整逻辑

```c
HAL_Init();
system_clock_72mhz_init();
led_pc13_init();
tim2_base_init();
HAL_TIM_Base_Start_IT(&htim2);
while (1) {
}
```

和寄存器版的区别：`tim2_base_init()` 只做配置，启动由 `HAL_TIM_Base_Start_IT()` 完成。

### 8.2 HAL 版关键步骤

| 步骤 | HAL 代码 | 对应寄存器 |
|---|---|---|
| 开时钟 | `__HAL_RCC_TIM2_CLK_ENABLE()` | `RCC->APB1ENR \|= TIM2EN` |
| 绑定外设 | `htim2.Instance = TIM2` | — |
| 配 PSC | `htim2.Init.Prescaler = 7199` | `TIM2->PSC` |
| 配 ARR | `htim2.Init.Period = 9999` | `TIM2->ARR` |
| 写入寄存器 | `HAL_TIM_Base_Init(&htim2)` | 写 PSC/ARR/CR1 等 |
| 配 NVIC | `HAL_NVIC_EnableIRQ(TIM2_IRQn)` | NVIC 使能 |
| 启动+开中断 | `HAL_TIM_Base_Start_IT(&htim2)` | CEN + UIE |

其他字段：`CounterMode=UP`（向上计数）、`ClockDivision=DIV1`（不额外分频）、`AutoReloadPreload=DISABLE`（不预装载 ARR）。本课不动态改 ARR，这些用默认值即可。

### 8.3 HAL 中断流程

```text
TIM2 更新事件
  → TIM2_IRQHandler()
    → HAL_TIM_IRQHandler(&htim2)    // HAL 检查标志+清标志
      → HAL_TIM_PeriodElapsedCallback(htim)  // 用户回调
        → if (htim->Instance == TIM2) 翻转 PC13
```

回调里判断 `htim->Instance == TIM2`，因为多个定时器共用同一个回调。

## 9. 两个版本的学习策略

寄存器版让你看到完整硬件链路：RCC 开时钟、PSC/ARR 定周期、UIF 表示事件、UIE 允许中断、NVIC 放行、ISR 清标志。如果只看 HAL，很容易把 `HAL_TIM_Base_Start_IT()` 当成魔法。

HAL 版更接近实际项目，把配置放进句柄、中断分发到回调，适合多人维护。前提是你知道每个字段对应寄存器版哪一步。

核心映射：

```text
TIM2->PSC                    → htim2.Init.Prescaler
TIM2->ARR                    → htim2.Init.Period
DIER.UIE + CR1.CEN           → HAL_TIM_Base_Start_IT()
TIM2_IRQHandler() 手动处理    → HAL_TIM_IRQHandler() 分发
手动翻转 LED                  → HAL_TIM_PeriodElapsedCallback()
```

## 10. 检验问题清单

### 10.1 为什么 TIM2 输入时钟是 72MHz，而不是 PCLK1 的 36MHz？

答：本课 APB1 分频是 /2，不为 1。STM32F1 中当 APB 预分频不为 1 时，挂在该 APB 上的定时器时钟会变成 `2 x PCLK`，所以 TIM2CLK = 72MHz。

### 10.2 `PSC = 7200 - 1` 为什么得到 10kHz？

答：定时器分频系数是 `PSC + 1`。TIM2CLK 是 72MHz，除以 7200 后得到 10000Hz，也就是 10kHz。

### 10.3 `ARR = 10000 - 1` 为什么得到 1 秒更新？

答：PSC 后计数频率是 10kHz，每秒 10000 个 tick。计数器从 0 到 9999 一共 10000 个计数，所以 1 秒产生一次更新。

### 10.4 只产生 `UIF`，但不开 `UIE`，CPU 会进入中断吗？

答：不会。`UIF` 只是事件标志，`UIE` 才允许更新事件向 NVIC 发出中断请求。

### 10.5 开了 `UIE`，但没 `NVIC_EnableIRQ(TIM2_IRQn)`，会怎样？

答：TIM2 外设内部会发中断请求，但 CPU 的 NVIC 没放行，不会跳进 `TIM2_IRQHandler()`。

### 10.6 `TIM2_IRQHandler()` 里不清 `UIF` 会怎样？

答：中断标志一直保持置位，CPU 退出 ISR 后可能立刻再次进入，表现为程序被中断反复占住。

### 10.7 HAL 版的 `HAL_TIM_Base_Start()` 和 `HAL_TIM_Base_Start_IT()` 有什么区别？

答：普通 Start 只启动基础定时计数，不打开更新中断；Start_IT 会启动计数并打开更新中断。本课必须使用 Start_IT。

### 10.8 HAL 回调里为什么要判断 `htim->Instance == TIM2`？

答：多个定时器可能共用同一个周期回调。判断来源可以保证只有 TIM2 的更新事件才翻转 PC13。

## 11. 工程实现步骤

### 11.1 寄存器实现路线

1. 配系统时钟。确定 TIM2CLK 计算基础。
2. 初始化 PC13。准备观察现象。
3. 打开 TIM2 时钟。让 TIM2 外设开始工作。
4. 配 `PSC`。把 72MHz 分成 10kHz。
5. 配 `ARR`。让 10000 个 tick 形成 1 秒。
6. 清 `UIF`。避免旧更新标志误触发。
7. 开 `UIE`。允许更新事件发中断。
8. 配 NVIC。允许 CPU 接收 TIM2 中断。
9. 置 `CEN`。启动计数器。
10. 在 ISR 中判断并清 `UIF`，再翻转 PC13。

### 11.2 HAL 实现路线

1. `HAL_Init()`：准备 HAL 基础环境。
2. 配时钟和 PC13：和前面课程一致。
3. `__HAL_RCC_TIM2_CLK_ENABLE()`：打开 TIM2 时钟。
4. 填 `htim2.Instance = TIM2`：绑定外设。
5. 填 `Prescaler/Period/CounterMode`：表达 PSC/ARR/计数模式。
6. 调 `HAL_TIM_Base_Init()`：把配置写入 TIM2。
7. 配 `HAL_NVIC_SetPriority()` 和 `HAL_NVIC_EnableIRQ()`。
8. 调 `HAL_TIM_Base_Start_IT()`：启动计数并打开更新中断。
9. 在 `TIM2_IRQHandler()` 中调用 `HAL_TIM_IRQHandler()`。
10. 在 `HAL_TIM_PeriodElapsedCallback()` 中判断 TIM2 并翻转 LED。

### 11.3 常见工程陷阱

- 把 PCLK1 当 TIM2CLK：周期错一倍。
- 忘记开 `RCC_APB1ENR_TIM2EN`：TIM2 不计数。
- 忘记 `UIE`：`UIF` 可能置位，但不进中断。
- 忘记 NVIC：外设请求发出，但 CPU 不响应。
- ISR 不清 `UIF`：反复进入同一个中断。
- HAL 版用了 `HAL_TIM_Base_Start()`：计数启动了，但不会进周期回调。

## 12. 运行现象

正常情况下，PC13 每 1 秒翻转一次。因为翻转一次只是从亮到灭或从灭到亮，所以完整亮灭周期约 2 秒。

## 13. 常见问题排查

### 13.1 LED 完全不闪

1. 程序是否卡在系统时钟初始化。
2. PC13 是否初始化成输出。
3. TIM2 时钟是否打开。
4. `TIM2->CR1.CEN` 是否置位。
5. `TIM2_IRQHandler()` 是否正确命名。

如果 `TIM2->CNT` 在调试器里不动，重点查 TIM2 时钟和 CEN。

### 13.2 LED 闪烁周期不对

1. TIM2CLK 是否按 72MHz 计算。
2. `PSC` 是否是 `7200 - 1`。
3. `ARR` 是否是 `10000 - 1`。
4. APB1 分频是否为 /2。

按 36MHz 算但硬件按 72MHz 跑，周期会快一倍。

### 13.3 程序像卡在中断里

1. `TIM2_IRQHandler()` 是否清 `TIM_SR_UIF`。
2. 是否有其他 TIM2 标志也在触发中断。

最常见原因是不清 `UIF`。

### 13.4 HAL 版不进 `HAL_TIM_PeriodElapsedCallback()`

1. 是否调用 `HAL_TIM_Base_Start_IT()`。
2. 是否配置 `HAL_NVIC_EnableIRQ(TIM2_IRQn)`。
3. `TIM2_IRQHandler()` 是否调用 `HAL_TIM_IRQHandler(&htim2)`。

### 13.5 一上电立刻翻转一次

启动前没清旧 `UIF` 标志。检查是否执行 `TIM2->SR &= ~TIM_SR_UIF`。

## 14. 本课最核心的结论

1. TIM2 是 STM32 通用定时器外设，使用前必须先开 APB1 时钟。
2. APB1 分频不为 1 时，TIM2 输入时钟会变成 `2 x PCLK1`，本课就是 72MHz。
3. `PSC` 决定 CNT 计数频率，`ARR` 决定多久产生更新事件。
4. `UIF` 是事件标志，`UIE` 是事件中断使能，NVIC 是 CPU 接收中断的入口。
5. `CEN` 才是真正启动 TIM2 计数器的开关。
6. HAL 的 `Prescaler/Period/Start_IT/PeriodElapsedCallback` 都能对应回寄存器版链路。

## 15. 建议你现在怎么读这节课

1. 先把第 5 章链路画一遍，尤其标出 `UIF`、`UIE`、NVIC 的区别。
2. 再手算 `72MHz / 7200 / 10000 = 1Hz`。
3. 然后读寄存器版 `tim2_base_init()`，逐句对应计算和中断链路。
4. 最后读 HAL 版，把 `Prescaler`、`Period`、`Start_IT` 翻译回 `PSC`、`ARR`、`UIE/CEN`。

能自己解释"为什么 CNT 在跑但不进中断"，这节课就算学到核心了。

## 16. 扩展练习

1. 把 `ARR` 改成 `5000 - 1`，观察翻转间隔变成 0.5 秒。
2. 把 `PSC` 改成 `72000 - 1`，再重新计算 ARR 应该怎么写。
3. 注释掉 `TIM_DIER_UIE`，观察是否还会进中断。
4. 注释掉清 `UIF`，观察故障现象。
5. 在调试器里观察 `TIM2->CNT`、`TIM2->SR`、`TIM2->DIER`、`TIM2->CR1`。

## 17. 下一课预告

下一课进入 [07_timer_output_compare](../07_timer_output_compare/README.md)。

本课只使用 TIM2 的"到时间产生更新中断"。下一课会使用定时器通道：当 `CNT` 计到 `CCR1` 时，不再只产生更新中断，而是让输出比较通道自动改变 PA0 的电平。那会把本课的 `CNT` 时间轴继续用起来。