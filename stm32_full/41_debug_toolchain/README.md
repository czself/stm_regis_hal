# 第 41 课：调试工具链与断点观察

## 1. 本课到底在学什么

本课表面现象是：PC13 LED 周期翻转，同时你可以在调试器里观察 `g_breakpoint_counter`、`g_last_odr`、`g_watch_counter`、`g_cycle_counter` 和 `DWT->CYCCNT` 的变化。

真正要学的是“代码运行现象如何被调试器看见”：

```text
PlatformIO 工程
  -> ST-Link 连接 SWD
  -> GDB 下载并暂停 Cortex-M3
  -> 断点让 CPU 停在某一行
  -> Watch 读取 RAM 变量和外设寄存器
  -> CoreDebug 打开 trace
  -> DWT->CYCCNT 记录 CPU 周期数
```

前面课程主要靠 LED、串口、波形判断代码是否正确。本课开始把调试器作为学习工具：不仅看“亮不亮”，还要看变量、寄存器和 CPU 周期计数怎么变化。

## 2. 本课学习目标

学完本课，你应该能回答：

1. `platformio.ini` 里的 `upload_protocol = stlink` 和调试有什么关系？
2. 断点停住 CPU 后，为什么 LED 也会停住？
3. `volatile` 为什么会让调试观察更可靠？
4. `GPIOC->ODR` 在 Watch 里能告诉你什么？
5. `CoreDebug->DEMCR.TRCENA` 为什么要先打开？
6. `DWT->CYCCNT` 为什么可以用来观察运行周期？
7. HAL 版 `HAL_Delay(500)` 和寄存器版忙等待，在调试观察上有什么差别？
8. 如果 Watch 看不到变量或变量不变化，应该先查什么？

## 3. 本课目录结构

```text
41_debug_toolchain/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 用寄存器配置 PC13，并把 `GPIOC->ODR` 保存到 `g_last_odr`。  
`hal/` 用 HAL 配置 PC13，并把 `DWT->CYCCNT` 保存到 `g_cycle_counter`。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器/调试器：ST-Link，使用 SWD 连接
- LED：PC13，常见 BluePill 为低电平点亮
- 时钟：HSE 8MHz，PLL 到 72MHz

本课不需要额外外设。观察重点是调试器里的变量和寄存器，而不是新增硬件模块。

## 5. 先建立一个最基本的脑图

```text
上板运行
  -> system_clock_72mhz_init() 配 72MHz
  -> pc13_led_init() 配 PC13 输出
  -> debug_counter_init() 打开 DWT 周期计数器
  -> while(1) 中更新调试变量
  -> 翻转 PC13
  -> 延时

调试观察
  -> 在 while(1) 内设置断点
  -> Watch g_breakpoint_counter/g_watch_counter
  -> Watch GPIOC->ODR 或 g_last_odr
  -> Watch DWT->CYCCNT 或 g_cycle_counter
  -> 单步观察每条语句带来的变量/寄存器变化
```

这节课的核心不是新增一个复杂外设，而是把“CPU 正在执行哪一行、变量在哪里变、寄存器什么时候变”看清楚。

## 6. 先认识本课里出现的核心名词

### 6.1 `ST-Link` 是什么

ST-Link 是下载器和调试器。它通过 SWD 接口连接 STM32 的调试端口，可以下载程序、设置断点、单步执行、读取寄存器和 RAM。

本课需要它，是因为普通运行只能看到 LED；调试运行能看到 `GPIOC->ODR`、全局变量和 DWT 周期计数器。ST-Link 没连好时，下载和调试都会失败。

### 6.2 `SWD` 是什么

SWD 是 Serial Wire Debug，Cortex-M 常用两线调试接口。

它属于芯片调试物理/协议层，不是 GPIO 普通输入输出。接线错误时，程序可能已经能跑，但调试器无法连接，也无法看 Watch。

### 6.3 `断点` 是什么

断点是调试器让 CPU 在指定指令处暂停的机制。

暂停后，外设寄存器和 RAM 保持当前状态，Watch 可以读取它们。本课适合把断点放在 `g_breakpoint_counter++`、`g_last_odr = GPIOC->ODR` 或 `HAL_GPIO_TogglePin()` 附近。

### 6.4 `Watch` 是什么

Watch 是调试器观察表达式的窗口，可以显示变量、指针地址、外设寄存器表达式。

本课可以观察 `g_breakpoint_counter` 是否递增、`g_last_odr` 是否跟随 LED 翻转、`g_cycle_counter` 是否保存了周期数。表达式写错或变量被优化时，Watch 可能显示不可用。

### 6.5 `volatile` 是什么

`volatile` 是 C 语言限定符，告诉编译器这个变量可能被外部因素观察或改变，读写不能随意优化掉。

本课的 `g_breakpoint_counter`、`g_last_odr`、`g_watch_counter`、`g_cycle_counter` 都是为了调试观察而存在。若没有 `volatile`，优化器可能把变量放进寄存器或删除看似无用的写入，Watch 观察就不直观。

### 6.6 `GPIOC->ODR` 是什么

`ODR` 是 GPIO 输出数据寄存器。PC13 输出高低电平时，`ODR13` 会反映输出锁存状态。

寄存器版把 `GPIOC->ODR` 读到 `g_last_odr`，让你既能看外设寄存器，也能看普通 RAM 变量。若 PC13 没配置成输出，ODR 变化不一定对应 LED 现象。

### 6.7 `CoreDebug->DEMCR` 是什么

`DEMCR` 是 Cortex-M 调试异常和监控控制寄存器。`TRCENA` 位用于打开 trace 功能。

本课要用 DWT 周期计数器，所以先执行：

```c
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
```

如果不打开 trace，后面的 `DWT->CYCCNT` 可能不计数。

### 6.8 `DWT` 是什么

DWT 是 Data Watchpoint and Trace，数据观察与跟踪单元，属于 Cortex-M 内核调试组件。

本课只用它的周期计数功能，不涉及复杂 trace。它不是 STM32 普通外设，不需要 RCC 时钟。

### 6.9 `DWT->CYCCNT` 是什么

`CYCCNT` 是 CPU 周期计数寄存器。使能后，CPU 每运行一个周期它通常递增一次。

寄存器版初始化时清零并使能，HAL 版循环里把它读到 `g_cycle_counter`。如果停在断点处，你可以看到两次循环之间大致经过了多少 CPU 周期。

### 6.10 `step over` 是什么

`step over` 是调试器的单步命令之一。它执行当前行，但遇到函数调用时不进入函数内部。

本课用它观察主流程很合适：你可以跳过 `delay_cycles()` 或 `HAL_Delay()` 的内部细节，重点看计数变量和 LED 输出状态变化。

### 6.11 `platformio.ini` 是什么

`platformio.ini` 是 PlatformIO 工程配置文件。

本课里面的 `board = genericSTM32F103C8` 决定目标芯片和链接参数，`framework = stm32cube` 决定使用 STM32Cube 框架，`upload_protocol = stlink` 决定下载和调试工具链走 ST-Link。配置错时，常见现象是编译能过但下载失败，或者调试器连接到错误目标。

### 6.12 `HSE_VALUE=8000000U` 是什么

`HSE_VALUE` 告诉库和时钟计算代码：外部高速晶振是 8MHz。

本课系统时钟按 8MHz HSE 乘 PLL 9 得到 72MHz。如果这个宏和板子实际晶振不一致，HAL 时钟配置、延时估算、DWT 周期换算都会跟着错。

### 6.13 `优化等级` 和调试有什么关系

编译器优化会改变代码形态：变量可能被放进寄存器，某些看似无用的写入可能被删除，语句顺序也可能被重排。

本课把观察变量声明成 `volatile`，就是为了让调试器更容易看到每一轮循环的变化。若你以后发现断点行号跳动或 Watch 值“不符合直觉”，要想到优化可能参与了。

### 6.14 `GDB` 是什么

GDB 是调试器后端，PlatformIO 调试时会通过它控制目标芯片。

你在 IDE 里点击断点、继续、单步、查看变量，底层通常会转成 GDB 和调试探针之间的操作。本课不要求背 GDB 命令，但要知道调试不是 printf，而是通过调试接口直接控制 CPU。

### 6.15 `寄存器视图` 是什么

寄存器视图是调试器显示外设寄存器或内核寄存器的窗口。

本课可以观察 `GPIOC->ODR`、`CoreDebug->DEMCR`、`DWT->CTRL`、`DWT->CYCCNT`。寄存器视图能帮助你确认“代码写了寄存器”是否真的改变了硬件可见状态。

### 6.16 `局部变量` 和 `全局变量` 调试差别是什么

全局变量有固定符号，程序停在任何位置通常都比较容易观察。

局部变量只在函数作用域内有效，优化后还可能消失或放在寄存器里。本课选择全局 `volatile` 变量作为观察对象，是为了降低初学调试时的干扰。

## 7. 寄存器版代码逐步讲解

### 7.1 系统时钟配置

`system_clock_72mhz_init()` 先设置 `FLASH->ACR` 的预取和等待周期，再打开 HSE、等待 `HSERDY`，配置 PLL x9，等待 `PLLRDY`，最后切换 SYSCLK。

调试周期计数依赖真实 CPU 时钟。时钟没配对，`CYCCNT` 仍会数，但你把周期换算成时间时会错。

### 7.2 PC13 输出配置

代码打开 `RCC_APB2ENR_IOPCEN`，清 `GPIOC->CRH` 的 `MODE13/CNF13`，再设置 `MODE13_1`。

PC13 属于高 8 位引脚，所以配置 `CRH`。如果误配 `CRL`，Watch 里可能看到 ODR 改了，但 LED 引脚模式没对，现象不可靠。

### 7.3 `pc13_toggle()`

函数读取 `GPIOC->ODR.ODR13` 判断当前输出锁存状态。若为 1，就写 `BRR` 拉低；否则写 `BSRR` 拉高。

这正好适合单步观察：执行前看 ODR，执行后再看 ODR，就能确认是哪条写寄存器语句改变了 LED 状态。

### 7.4 `delay_cycles()`

`delay_cycles()` 通过循环和 `__NOP()` 消耗时间。

调试时如果单步进入这个函数，会在循环里耗很久；所以更适合用 step over 跳过，或者把断点放在延时前后。

### 7.5 `g_breakpoint_counter`

这个变量每轮循环自增一次。它用于确认主循环确实在反复执行。

若断点每次停住时它不变，说明程序没有走到这行，或者你观察的不是当前工程下载后的变量。

### 7.6 `g_last_odr`

`g_last_odr = GPIOC->ODR;` 把外设寄存器快照保存到 RAM 变量。

这样做的好处是：Watch 既能直接看 `GPIOC->ODR`，也能看 `g_last_odr`。如果两者不一致，要确认断点停在赋值前还是赋值后。

### 7.7 `debug_counter_init()` 打开 trace

`CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk` 打开 trace，`DWT->CYCCNT = 0` 清周期计数，`DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk` 使能计数。

顺序很重要：先开 trace，再清零，再使能。否则 Watch 里的 `CYCCNT` 可能不动。

### 7.8 主循环观察顺序

主循环先更新计数变量，再读 ODR，再翻转 LED，最后延时。

所以你停在不同语句，看到的值含义不同：停在 `pc13_toggle()` 前，`g_last_odr` 是翻转前快照；停在延时后，ODR 已经被翻转。

### 7.9 为什么先读 ODR 再翻转

寄存器版主循环中：

```c
g_last_odr = GPIOC->ODR;
pc13_toggle();
```

这个顺序让 `g_last_odr` 保存“翻转前”的输出状态。若你想保存翻转后的状态，就要把赋值放到 `pc13_toggle()` 后面。调试时必须把断点位置和赋值顺序对应起来。

### 7.10 `BRR` 和 `BSRR` 在调试里的意义

`BRR` 用来把 PC13 输出清 0，`BSRR` 的 BS 位用来把 PC13 输出置 1。

BluePill 上 PC13 LED 常见为低电平点亮，所以输出清 0 不一定是“关灯”，而是常常会点亮。Watch `ODR13` 时要同时记住硬件连接极性。

### 7.11 DWT 不是普通外设

`CoreDebug` 和 `DWT` 都属于 Cortex-M 内核调试组件，不挂在 APB1/APB2，也不需要 RCC 使能位。

这和 GPIOC 不同：GPIOC 必须开 `IOPCEN`，DWT 只需要打开 trace 和计数使能。把这两类寄存器层级分清，是调试课的重点。

### 7.12 断点会改变时间

断点停住 CPU 后，`DWT->CYCCNT` 是否继续计数取决于调试行为和内核状态，LED 也不会继续由主循环翻转。

所以本课用 DWT 看“代码运行期间的周期”，不要用断点停住的墙钟时间判断程序周期。

### 7.13 断点应该先打在哪里

第一次调试建议把断点打在 `g_breakpoint_counter++` 这一行。

这个位置进入主循环后很快会命中，而且每次继续运行后都会再次命中。它能证明主循环在跑，也能让你观察每轮循环的变量变化。

### 7.14 第二个断点应该放在哪里

第二个断点可以放在 `pc13_toggle()` 后面。

这样你能比较执行前后的 `GPIOC->ODR`。如果 ODR 变化但 LED 不亮，要查硬件极性或板载 LED；如果 ODR 不变，要查 `pc13_toggle()` 是否执行和 GPIO 配置是否正确。

### 7.15 为什么 `g_breakpoint_counter++` 比 LED 更适合确认循环

LED 翻转受延时、硬件极性、肉眼观察影响。

计数变量每进一次主循环就增加，调试器里变化更直接。它不能替代硬件验证，但能帮你先确认软件流程没有卡死。

### 7.16 `__NOP()` 在调试里有什么意义

`__NOP()` 是空操作指令，用来消耗一个很小的 CPU 执行步骤。

在 `delay_cycles()` 中，它避免循环体完全为空，也让延时循环更直观。调试时通常不要单步进入大量 NOP 循环，否则会浪费时间。

## 8. HAL 版代码逐步讲解

### 8.1 `HAL_Init()`

HAL 初始化基础运行环境，并配置 SysTick 作为 HAL tick 来源。HAL 版后面的 `HAL_Delay(500)` 依赖这个 tick。

若没有 `SysTick_Handler()` 调用 `HAL_IncTick()`，`HAL_Delay()` 可能卡住。

### 8.2 `RCC_OscInitTypeDef`

`osc.OscillatorType = RCC_OSCILLATORTYPE_HSE` 选择 HSE，`PLLSource = HSE`，`PLLMUL = RCC_PLL_MUL9` 表示 8MHz x9。

这对应寄存器版的 `HSEON/HSERDY/PLLSRC/PLLMULL/PLLON`。

### 8.3 `RCC_ClkInitTypeDef`

`SYSCLKSource = PLLCLK`，`AHBCLKDivider = DIV1`，`APB1CLKDivider = DIV2`，`APB2CLKDivider = DIV1`。

这对应寄存器版 `RCC->CFGR` 的系统时钟选择和总线分频。APB1 二分频是因为 F103 的 APB1 最高 36MHz。

### 8.4 `GPIO_InitTypeDef`

`gpio.Pin = GPIO_PIN_13` 选择 PC13；`Mode = GPIO_MODE_OUTPUT_PP` 对应推挽输出；`Speed = GPIO_SPEED_FREQ_LOW` 对应低速输出配置。

底层仍然写 GPIOC 的配置寄存器，不是 HAL 魔法。

### 8.5 `HAL_GPIO_TogglePin()`

这个 API 读取当前输出状态并写入相反状态，效果对应寄存器版 `pc13_toggle()`。

如果 PC13 时钟没开或模式没配置好，API 调用本身能执行，但 LED 不会按预期变化。

### 8.6 `HAL_Delay(500)`

`HAL_Delay()` 使用 HAL tick 计时。正常情况下 PC13 每约 500ms 翻转一次。

调试时断点会暂停 CPU，时间感会被打断；所以不要用断点停住期间的真实墙钟时间判断 `HAL_Delay()` 是否准确。

### 8.7 HAL 版 DWT 初始化

HAL 版同样用独立的 `debug_counter_init()` 写 `CoreDebug->DEMCR`、`DWT->CYCCNT` 和 `DWT->CTRL`，与寄存器版命名一致。

这和寄存器版是同一组 Cortex-M 调试寄存器，和 HAL GPIO/RCC 没有直接关系。

### 8.8 `g_watch_counter` 和 `g_cycle_counter`

`g_watch_counter` 每轮循环递增，证明主流程在跑；`g_cycle_counter = DWT->CYCCNT` 保存当前周期计数快照。

Watch 这两个变量，比直接盯着 LED 更适合判断程序是否卡在 `HAL_Delay()`、是否反复进入主循环。

### 8.9 `SysTick_Handler()`

HAL 版实现了：

```c
void SysTick_Handler(void)
{
    HAL_IncTick();
}
```

这让 HAL 的毫秒 tick 持续递增。`HAL_Delay(500)` 内部依赖这个 tick 判断是否等待够 500ms。若删掉这个中断函数，调试器可能显示程序停在 `HAL_Delay()`。

### 8.10 HAL API 返回值为什么本课没展开

本课 HAL 时钟配置调用了 `HAL_RCC_OscConfig()` 和 `HAL_RCC_ClockConfig()`，源码没有检查返回值。

教学上你仍要知道：真实工程应检查返回值。若 HSE 不起振或 PLL 配置失败，后续 GPIO 和 DWT 代码可能仍执行，但系统时钟不是你以为的 72MHz。

### 8.11 HAL 版也能直接访问内核寄存器

HAL 版仍直接写 `CoreDebug->DEMCR`、`DWT->CYCCNT`、`DWT->CTRL`。

这说明 HAL 只封装 STM32 外设常用初始化，不会把所有 Cortex-M 内核调试能力都变成 HAL API。需要时仍要读 CMSIS 名字。

### 8.12 `HAL_GPIO_TogglePin()` 和 Watch 的关系

HAL 版没有把 `GPIOC->ODR` 保存到普通变量，但你仍然可以直接 Watch `GPIOC->ODR`。

执行 `HAL_GPIO_TogglePin()` 前后观察 ODR13，可以把 HAL API 翻译回底层 GPIO 输出锁存状态。这样读 HAL 版时不会停留在“函数调用成功了”这种表层。

### 8.13 `uwTick` 可以观察什么

HAL 内部有毫秒 tick 计数，常见变量名是 `uwTick`。

本课 HAL 版如果怀疑 `HAL_Delay(500)` 没工作，可以观察 tick 是否递增。tick 不动，延时就不会结束；tick 正常递增，问题就更可能在 GPIO 或断点位置。

### 8.14 调试 HAL 返回值

源码没有保存 `HAL_RCC_OscConfig()` 的返回值，但你可以在这行之后单步查看函数返回。

如果返回不是 `HAL_OK`，后续现象都不能按 72MHz 正常运行来解释。调试时不要只看最后 LED，要先确认初始化 API 是否成功。

### 8.15 HAL 版第一个断点放在哪里

建议先放在 `g_watch_counter++`。

它和寄存器版的 `g_breakpoint_counter++` 作用相同，证明主循环不断返回到这一行。若它只命中一次，说明后面可能卡在 `HAL_Delay()` 或异常位置。

### 8.16 HAL 版第二个断点放在哪里

第二个断点放在 `HAL_GPIO_TogglePin()` 后面。

执行前后观察 `GPIOC->ODR`，能把 HAL API 和底层 LED 输出对应起来。这样你不会把 HAL 函数当成不可见黑箱。

### 8.17 `g_cycle_counter` 为什么是快照

`DWT->CYCCNT` 一直在变，而 `g_cycle_counter` 只在赋值语句执行时更新。

所以 Watch `DWT->CYCCNT` 和 Watch `g_cycle_counter` 看到的现象不同：前者是硬件计数器当前值，后者是上一次主循环保存的值。

## 9. 两个版本真正应该怎么学

寄存器版重点看“外设寄存器和调试寄存器如何被直接写”。HAL 版重点看“外设初始化被 HAL 封装后，调试寄存器仍然是 Cortex-M 层直接访问”。

两个版本都在训练同一件事：不要只看最终 LED，要学会在代码运行中间停下来，观察变量、外设寄存器和内核调试寄存器。

## 10. 检验问题清单

### 10.1 为什么本课变量要用 `volatile`？

**答**：这些变量主要用于调试观察。`volatile` 能避免优化器删除或缓存读写，让 Watch 更稳定地看到变化。

### 10.2 `GPIOC->ODR` 和 PC13 LED 有什么关系？

**答**：`ODR13` 是 PC13 输出锁存位。它变化说明软件输出状态变化，但 LED 是否亮还要结合 BluePill 低电平点亮的硬件连接。

### 10.3 为什么要先写 `CoreDebug->DEMCR.TRCENA`？

**答**：DWT 周期计数属于 trace 功能，必须先打开 trace，否则 `CYCCNT` 可能不计数。

### 10.4 断点停住后为什么 LED 不继续闪？

**答**：断点暂停的是 CPU 执行。主循环不再运行，自然不会继续翻转 PC13。

### 10.5 HAL 版 `HAL_Delay()` 卡住先查什么？

**答**：先查 `SysTick_Handler()` 是否调用 `HAL_IncTick()`，再查是否停在断点或中断被关闭。

### 10.6 Watch 里变量不变化说明什么？

**答**：可能程序没跑到那一行、断点位置不对、下载的不是当前工程、变量被优化，或者变量更新在断点之后。

### 10.7 `CYCCNT` 能直接等于毫秒吗？

**答**：不能。它是 CPU 周期数。若系统时钟 72MHz，约 72000 个周期才是 1ms。

### 10.8 step over 适合用在哪里？

**答**：适合跳过 `delay_cycles()`、`HAL_Delay()` 这类内部细节很多但本课不关心的函数，保持观察主流程。

## 11. 工程实现步骤

### 11.1 需求分析

本课要证明调试器能观察程序运行中的变量、GPIO 寄存器和 Cortex-M 调试寄存器。

### 11.2 硬件核查

确认 ST-Link 的 SWD 接线可靠，PC13 LED 可用，工程使用 `upload_protocol = stlink`。

### 11.3 寄存器路线

配置 72MHz、PC13 输出，打开 DWT 周期计数，在主循环中更新 `g_breakpoint_counter` 和 `g_last_odr`。

### 11.4 HAL 路线

用 HAL 配置时钟和 PC13，打开 DWT 周期计数，在主循环中更新 `g_watch_counter` 和 `g_cycle_counter`。

### 11.5 工程思维

调试不是猜。先用断点确认程序走到哪里，再用 Watch 看变量和寄存器，最后再解释 LED 或延时现象。

### 11.6 常见工程陷阱

断点打错工程、Watch 表达式写错、变量没加 `volatile`、优化等级影响观察、SysTick 没进导致 HAL_Delay 卡住，都会让调试结论失真。

另一个常见陷阱是忘记“当前停在哪一行”。同一个变量在赋值前、赋值后、函数调用前、函数调用后代表的含义不同。调试不是只看值，而是看“这行代码执行到哪一步时的值”。

还要注意 reg/hal 两个工程会生成各自的固件。你修改了 HAL 版，却下载了 reg 版，就会出现 Watch 变量名对不上或现象和代码不一致。

## 12. 运行现象

直接运行时，PC13 周期翻转。寄存器版中 `g_breakpoint_counter` 递增，`g_last_odr` 保存最近一次读取的 `GPIOC->ODR`；HAL 版中 `g_watch_counter` 递增，`g_cycle_counter` 保存 DWT 周期计数快照。

调试运行时，在主循环设置断点，程序会停住，Watch 中的变量保持在当前值。继续运行后，变量继续变化。

寄存器版如果断在 `g_last_odr = GPIOC->ODR` 后面，你应该看到 `g_last_odr` 记录翻转前的 ODR。再 step over `pc13_toggle()`，`GPIOC->ODR` 改变，但 `g_last_odr` 仍保持旧快照。

HAL 版如果断在 `g_cycle_counter = DWT->CYCCNT` 后面，你应该看到 `g_cycle_counter` 每轮变大。若直接 Watch `DWT->CYCCNT`，它会持续变化；若 Watch 快照变量，它只在赋值语句执行后更新。

## 13. 常见问题排查

### 13.1 调试器连不上

检查 ST-Link、SWDIO/SWCLK/GND 接线，确认 PlatformIO 工程使用 ST-Link。

### 13.2 LED 不闪

先确认 PC13 低电平点亮，再检查 GPIOC 时钟、PC13 模式和程序是否停在断点。

### 13.3 Watch 看不到变量

确认变量是全局或当前作用域可见，确认下载的是当前工程，必要时降低优化或保留 `volatile`。

### 13.4 `DWT->CYCCNT` 不递增

检查是否设置了 `CoreDebug->DEMCR.TRCENA`，是否清零后使能了 `DWT_CTRL_CYCCNTENA_Msk`。

### 13.5 HAL 版停在 `HAL_Delay()`

检查 `SysTick_Handler()` 是否存在并调用 `HAL_IncTick()`，以及调试时是否暂停在中断关闭区域。

### 13.6 断点位置和变量值对不上

确认变量是在断点前更新还是断点后更新。例如 `g_last_odr` 在 `pc13_toggle()` 前保存，看到的是翻转前状态。

### 13.7 周期数换算不对

确认系统时钟是否真为 72MHz，`HSE_VALUE` 是否为 8MHz。`CYCCNT` 是周期，不是毫秒。

### 13.8 单步进入延时后出不来

不要在 `delay_cycles()` 的循环内部连续 step。使用 step over 跳过延时函数，或者临时调小延时参数。

### 13.9 Watch 表达式写寄存器时报错

确认表达式写法符合当前调试器支持。有些环境能直接识别 `GPIOC->ODR`，有些需要展开外设寄存器视图或使用地址表达式。

### 13.10 运行和调试现象不同

调试器会暂停 CPU，断点也会改变时间关系。先用无断点运行确认 LED，再用断点观察变量，不要把调试暂停造成的“时间停止”当成程序错误。

## 14. 本课最核心的结论

1. 调试器能把“正在运行的代码”停在具体语句上观察。
2. Watch 可以同时观察 RAM 变量和外设寄存器。
3. `volatile` 能让为了调试而存在的变量更可靠地保留下来。
4. DWT 周期计数器属于 Cortex-M 调试组件，不需要 RCC 外设时钟。
5. HAL 封装 GPIO/RCC，但 CoreDebug 和 DWT 仍是内核调试寄存器。
6. 调试结论必须结合断点位置判断，不能只看某个变量瞬间值。

## 15. 建议你现在怎么读这节课

先运行程序确认 PC13 会闪，再用断点停在主循环。第一轮只看计数变量，第二轮看 `GPIOC->ODR`，第三轮看 `DWT->CYCCNT`。每次只验证一个问题，调试会清楚很多。

## 16. 扩展练习

1. 把断点放在 `pc13_toggle()` 前后，比较 `GPIOC->ODR` 的变化。
2. 用 `CYCCNT` 估算一次主循环大约消耗多少周期。
3. 去掉某个变量的 `volatile`，观察 Watch 结果是否变化。
4. 在 HAL 版中观察 `uwTick`，理解 `HAL_Delay()` 为什么能计时。

## 17. 下一课预告

- 上一课：[40_low_power_stop_standby](../40_low_power_stop_standby/README.md)
- 下一课：[42_fsmc_sram](../42_fsmc_sram/README.md)
