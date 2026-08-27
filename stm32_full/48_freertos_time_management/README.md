# 48_freertos_time_management - FreeRTOS 时间管理

## 1. 本课到底在学什么

本课表面现象是：PC13 按相对稳定的 500ms 节奏翻转，PA1 按约 700ms 节奏翻转。两个 LED 节奏不同，说明两个任务都在用 FreeRTOS tick 做时间等待，而不是靠裸机空循环拖时间。

真正要学的是 FreeRTOS 里两种常见延时模型：`vTaskDelay()` 表示“从现在开始再等一段时间”，`vTaskDelayUntil()` 表示“按上一次唤醒时间推算下一次固定周期”。前者适合普通等待，后者适合周期任务，尤其适合减少任务执行耗时带来的周期漂移。

这节课接在调度器与时间片之后。上一课讲“谁能运行”，本课讲“任务什么时候重新变成就绪态”。后面的软件定时器、低功耗 tickless、周期采样、通信超时，都会用到本课的 tick、相对延时和绝对周期思想。

## 2. 本课学习目标

学完本课你应该能做到：

- 能解释 `precise_task` 为什么用 `vTaskDelayUntil()`，而不是普通 `vTaskDelay()`。
- 能解释 `relative_task` 为什么用 `vTaskDelay(700ms)`。
- 能说明 `TickType_t last = xTaskGetTickCount()` 保存了什么。
- 能说出 `pdMS_TO_TICKS(500)` 和 `pdMS_TO_TICKS(700)` 与 `configTICK_RATE_HZ` 的关系。
- 能区分相对延时、绝对周期、任务阻塞和任务就绪。
- 能把 PC13 对应到 500ms 周期任务，把 PA1 对应到 700ms 相对延时任务。
- 能解释为什么任务延时期间 CPU 可以运行其他任务或空闲任务。
- 能知道 HAL 版也应使用 `vTaskDelay()` / `vTaskDelayUntil()`，不是在任务里依赖 `HAL_Delay()`。
- 能根据周期不准、LED 不闪、任务进 hook 等现象定位到时钟、tick、GPIO、栈或 heap。

## 3. 本课目录结构

```text
48_freertos_time_management/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两个版本都使用同一个 FreeRTOS 内核和同一个 `FreeRTOSConfig.h`。`platformio.ini` 中 `-I../../freertos` 指向配置文件，`lib_extra_dirs = ../../lib` 指向本仓库的 FreeRTOS-Kernel。reg/hal 的时间 API 完全一样，差别只是 GPIO 初始化与翻转方式。

## 4. 实验硬件

- STM32F103C8T6 BluePill。
- ST-Link 下载器。
- HSE 8MHz，PLL x9，系统时钟 72MHz。
- PC13：由 `precise_task` 翻转，观察 500ms 周期。
- PA1：由 `relative_task` 翻转，观察 700ms 相对延时。
- PA2：初始化为输出，但当前任务没有使用。
- PA0：初始化为上拉输入，当前任务没有读取。
- FreeRTOS tick：`configTICK_RATE_HZ = 1000`，即 1ms 一个 tick。
- 关键配置：`INCLUDE_vTaskDelay = 1`，`INCLUDE_vTaskDelayUntil = 1`。

若要精确观察周期，建议用逻辑分析仪测 PC13/PA1，而不是只靠肉眼。

## 5. 先建立一个最基本的脑图

本课完整链路是：

```text
复位启动
  -> 配 72MHz 时钟
  -> 配 PC13、PA1、PA2 输出，PA0 上拉输入
  -> 创建 precise_task，优先级 2
  -> 创建 relative_task，优先级 1
  -> 启动调度器
  -> precise_task 读取当前 tick 到 last
  -> precise_task 翻转 PC13
  -> precise_task 调用 vTaskDelayUntil(&last, 500ms)
  -> relative_task 翻转 PA1
  -> relative_task 调用 vTaskDelay(700ms)
  -> tick 中断推进系统时间
  -> 到期任务回到就绪态，调度器按优先级运行
```

这条链路有两个时间模型。`relative_task` 每次从“执行到 delay 这一刻”开始再等 700ms；`precise_task` 每次根据 `last` 推算下一次固定唤醒点，并且 API 会更新 `last`。所以后者更适合稳定周期任务。

硬件层看到的是两个引脚翻转，RTOS 层真正发生的是任务进入阻塞态、tick 到期、任务回到就绪态、调度器再次选择任务运行。

## 6. 先认识本课里出现的核心名词

### 6.1 `Tick` 是什么

Tick 是 FreeRTOS 系统节拍，属于 RTOS 时间基准层。

本仓库 `configTICK_RATE_HZ = 1000`，表示每秒 1000 个 tick。SysTick 中断周期性进入 FreeRTOS tick handler，内核用它维护延时列表和任务超时。

tick 不准，所有 `vTaskDelay()`、`vTaskDelayUntil()`、超时等待都会不准。

### 6.2 `TickType_t` 是什么

`TickType_t` 是保存 tick 计数的类型，属于 FreeRTOS C 类型层。

源码里 `TickType_t last = xTaskGetTickCount();` 保存 precise 任务第一次运行时的 tick。后续 `vTaskDelayUntil(&last, period)` 会读写这个变量。

如果用普通 `uint16_t` 或错误类型保存 tick，可能在不同配置下溢出或类型不匹配。

### 6.3 `xTaskGetTickCount()` 是什么

`xTaskGetTickCount()` 返回当前 tick 计数，属于 FreeRTOS 时间查询 API。

本课 precise 任务用它初始化 `last`。这个值不是毫秒字符串，也不是硬件定时器 CNT，而是 FreeRTOS 内核维护的系统节拍计数。

如果调度器没启动或 SysTick 映射错误，tick 不增长，延时任务就不会按预期醒来。

### 6.4 `vTaskDelay()` 是什么

`vTaskDelay()` 是相对延时 API，属于 FreeRTOS 时间管理层。

`relative_task` 每次翻转 PA1 后调用 `vTaskDelay(pdMS_TO_TICKS(700))`，意思是从此刻开始阻塞约 700ms。任务执行本身花费的时间会加到下次翻转间隔里。

它适合普通等待、退避、低频轮询，不适合要求严格周期的控制任务。

### 6.5 `vTaskDelayUntil()` 是什么

`vTaskDelayUntil()` 是绝对周期延时 API，属于 FreeRTOS 周期任务层。

`precise_task` 使用 `vTaskDelayUntil(&last, pdMS_TO_TICKS(500))`。API 以 `last` 为基准计算下一次唤醒时间，并更新 `last`，让任务尽量保持固定周期。

如果 `last` 没有正确初始化，第一次周期可能异常；如果任务执行时间超过周期，任务会出现赶不上节拍的现象。

### 6.6 `pdMS_TO_TICKS()` 是什么

`pdMS_TO_TICKS()` 把毫秒换算成 tick，属于 FreeRTOS 时间宏层。

本课 500ms 和 700ms 都通过它表达。这样即使 tick 频率改变，源码仍能保持以毫秒为语义的可读性。

手写 500 或 700 看似也能跑，但会把代码和 tick 频率绑死。

### 6.7 `precise_task` 是什么

`precise_task` 是本课的周期任务，属于应用任务层。

它优先级 2，翻转 PC13，并使用 `vTaskDelayUntil()` 保持 500ms 周期。它演示的是“按计划时刻醒来”，不是“执行完再等一段时间”。

PC13 周期明显漂移时，应优先查这个任务和 tick 基准。

### 6.8 `relative_task` 是什么

`relative_task` 是本课的普通延时任务，属于应用任务层。

它优先级 1，翻转 PA1，并使用 `vTaskDelay(700ms)`。它演示相对延时，适合和 precise 任务对比。

PA1 不动时，要查任务创建、GPIOA 配置和是否进入 hook。

### 6.9 `阻塞态` 在本课是什么

两个任务调用延时 API 后都会进入阻塞态，属于 RTOS 状态层。

阻塞态不是死循环等待，任务不占 CPU。tick 到期后，内核把任务放回就绪列表。期间 CPU 可以运行其他任务或空闲任务。

### 6.10 `SysTick` 和本课有什么关系

SysTick 是 Cortex-M 内核定时异常，属于硬件/内核桥接层。

FreeRTOS 把 `xPortSysTickHandler` 映射到 `SysTick_Handler`。它周期性推进 tick，驱动延时任务恢复。SysTick 配错，两个 LED 周期都会异常。

### 6.11 `PC13` 在本课做什么

PC13 是 precise 任务的输出证据，属于 GPIO 硬件层。

寄存器版用 `led_toggle_pc13()` 翻转，HAL 版用 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)`。它显示 500ms 周期任务是否在按节奏运行。

### 6.12 `PA1` 在本课做什么

PA1 是 relative 任务的输出证据。

寄存器版用 `led_toggle_pa1()`，HAL 版用 HAL GPIO 翻转。它显示相对延时任务是否在运行。

### 6.13 `优先级 2 和 1` 有什么影响

precise 任务优先级 2，高于 relative 的 1。

当两个任务同时就绪时，precise 会先运行。这能让 500ms 周期任务更及时，但它每次很快进入阻塞，所以不会长期饿死 relative。

### 6.14 `HAL_Delay()` 为什么不是本课重点

HAL 版任务没有使用 `HAL_Delay()`，而是使用 FreeRTOS 延时 API。

在 RTOS 任务里，时间等待应进入阻塞态，让调度器安排其他任务。使用 HAL 延时容易带来 tick 归属和调度协作问题。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件分工

`stm32f1xx.h` 提供寄存器定义，`FreeRTOS.h` 和 `task.h` 提供任务与时间 API。

本课没有队列、信号量和中断业务，重点都在 task 模块。

### 7.2 系统时钟对时间 API 的影响

`system_clock_72mhz_init()` 把系统配到 72MHz，这必须和 `configCPU_CLOCK_HZ` 一致。

如果时钟实际不是 72MHz，FreeRTOS tick 频率会偏，500ms 和 700ms 都不准。

### 7.3 GPIO 初始化

代码打开 GPIOC、GPIOA、AFIO 时钟，配置 PC13/PA1/PA2 输出，PA0 上拉输入。

PC13 反馈 precise，PA1 反馈 relative，PA2 当前不用。初始化 PA2 不代表本课有第三个时间任务。

### 7.4 `precise_task()` 初始化 `last`

任务入口里先执行 `TickType_t last = xTaskGetTickCount();`。

这个变量记录周期任务的参考时间点。它放在循环外，表示后续每轮都基于同一个持续更新的时间基准，而不是每次重新取当前时间。

### 7.5 precise 任务翻转 PC13

循环里先调用 `led_toggle_pc13()`。

这一步产生肉眼或仪器可观察的周期信号。翻转发生后才调用延时，所以周期测量要按相邻翻转边沿理解。

### 7.6 `vTaskDelayUntil(&last, 500ms)` 的后果

该 API 根据 `last + 500ms` 计算下一次唤醒点，并更新 `last`。

任务进入阻塞态，直到目标 tick 到达。若任务执行时间较短，周期稳定；若执行时间超过 500ms，任务可能来不及按计划阻塞。

### 7.7 `relative_task()` 的相对延时

relative 任务翻转 PA1 后调用 `vTaskDelay(700ms)`。

这表示从调用延时这一刻开始等 700ms。若循环体里增加耗时代码，实际相邻翻转间隔会是“执行耗时 + 700ms”。

### 7.8 main 创建两个任务

main 创建 precise，栈 128，优先级 2；创建 relative，栈 128，优先级 1。

创建失败会关中断停住。两个任务都成功后才启动调度器。

### 7.9 为什么没有 `taskYIELD()`

本课任务都通过延时进入阻塞态，不需要主动 yield。

这和上一课不同。上一课 PA1/PA2 任务没有延时，所以用 yield 展示同级轮转；本课重点是时间阻塞。

### 7.10 hook 的排错意义

malloc failed hook 表示任务创建或内核对象分配失败，stack overflow hook 表示任务栈问题。

LED 周期异常时不一定是时间 API 错，先确认没有进入 hook。

### 7.11 `vTaskStartScheduler()` 后时间由内核推进

调度器启动后，SysTick 周期性进入 FreeRTOS tick handler。

任务延时到期、阻塞列表更新、优先级选择都由内核处理。main 后面的死循环正常不执行。

### 7.12 `vTaskDelayUntil()` 如何减少漂移

假设 precise 任务目标周期是 500ms，任务本身执行 GPIO 翻转花了很短时间。

`vTaskDelayUntil()` 不是简单从“当前执行完这一刻”再等 500ms，而是根据上一次计划唤醒点计算下一次计划唤醒点。这样每轮少量执行耗时不会不断累加到周期里。对采样、控制、通信保活这类周期任务，这个差别很重要。

### 7.13 任务执行超过周期会怎样

如果 precise 任务内部加入很耗时的代码，超过 500ms 周期，`vTaskDelayUntil()` 可能发现下一次计划时间已经过去。

这种情况下任务无法倒回时间，只能尽快继续下一轮。工程上这说明任务负载超过了周期预算，应减少任务耗时、降低频率、拆分工作或提高系统能力，而不是指望延时 API 修复超时。

### 7.14 tick 溢出为什么通常不用自己处理

FreeRTOS 的时间 API 用 `TickType_t` 和内核内部比较逻辑处理 tick 回绕。

应用层使用 `vTaskDelay()`、`vTaskDelayUntil()`、`pdMS_TO_TICKS()` 时，通常不需要自己写“tick 到最大值后归零怎么办”的代码。自己用普通加减比较 tick 时反而更容易写错。

### 7.15 为什么 precise 优先级更高

源码把 precise 任务优先级设为 2，把 relative 任务优先级设为 1。

这表示当两个任务同时因为 tick 到期回到就绪态时，调度器会先运行 precise。周期任务通常更关心及时性，所以给更高优先级是合理的教学选择。但它每轮很快调用 `vTaskDelayUntil()` 阻塞，不会长期压住 relative。

### 7.16 如何用断点判断相对延时

在 relative 任务的 `led_toggle_pa1()` 后打断点，再继续运行到下一次命中。

两次命中之间包含任务执行时间和 700ms 阻塞时间。若你在任务里增加耗时代码，间隔会随之增加。这能直观看出相对延时不是固定“边沿到边沿绝对周期”。

## 8. HAL 版代码逐步讲解

### 8.1 HAL 初始化

HAL 版先 `HAL_Init()`，再配置 RCC 和 GPIO。

这不改变 FreeRTOS 时间 API，只是让 HAL 外设函数具备基础运行环境。

### 8.2 HAL RCC 配置

`RCC_OscInitTypeDef` 和 `RCC_ClkInitTypeDef` 设置 HSE、PLL x9、总线分频和 Flash latency。

这些字段对应寄存器版时钟配置，影响 tick 准确性。

### 8.3 HAL GPIO 配置

PC13、PA1、PA2 都配置为推挽输出，PA0 为上拉输入。

`GPIO_InitTypeDef` 字段底层对应 F1 的 CRH/CRL/ODR 配置。

### 8.4 HAL 版 precise 任务

HAL 版 precise 同样使用 `TickType_t last`、`xTaskGetTickCount()` 和 `vTaskDelayUntil()`。

唯一差别是翻转 PC13 使用 `HAL_GPIO_TogglePin()`。

### 8.5 HAL 版 relative 任务

HAL 版 relative 翻转 PA1 后调用 `vTaskDelay(700ms)`。

相对延时语义和寄存器版完全相同。

### 8.6 HAL 版为什么不用 `HAL_Delay()`

任务里用 `vTaskDelay()` 才会把任务交给调度器阻塞。

`HAL_Delay()` 是裸机 HAL 常用等待方式，在 RTOS 任务里不是本课要训练的时间模型。

### 8.7 HAL 版创建结果

两个任务创建结果分别存入 `precise_ok` 和 `relative_ok`，任一失败就停住。

HAL 不能掩盖 FreeRTOS heap 或栈不足，排错仍要看 hook 和返回值。

### 8.8 HAL 版观察寄存器仍然有效

虽然用 HAL 翻转 GPIO，但底层仍会改变 GPIO 输出寄存器。

你可以观察 GPIOC/ GPIOA 的 ODR 位验证 PC13 和 PA1 输出。

### 8.9 HAL 版周期排查不要只看 HAL tick

本课任务延时由 FreeRTOS tick 控制。HAL 初始化阶段的 tick 和调度器启动后的 FreeRTOS tick 不是你排查的同一个重点。

如果 HAL 版周期不准，先查系统时钟、FreeRTOSConfig、SysTick handler 映射和任务延时 API，而不是只盯着 HAL_Delay 的经验。

### 8.10 HAL 版如果加入外设耗时操作

如果在 precise 任务里加入 I2C、SPI、串口打印或复杂计算，执行耗时会影响周期任务能否按时完成。

`vTaskDelayUntil()` 能减少普通执行耗时带来的长期漂移，但不能让超预算任务准时。HAL 外设调用如果阻塞很久，同样会破坏周期。

### 8.11 HAL 版任务优先级和寄存器版完全一致

HAL 版创建 precise 的优先级仍是 2，relative 仍是 1。

所以如果两个版本周期表现差异很大，不应先怀疑 FreeRTOS 调度策略不同，而应检查 HAL 初始化、GPIO 翻转耗时、系统时钟或是否误用了 HAL 延时。

### 8.12 HAL 版没有软件定时器

本课没有使用 FreeRTOS software timer，也没有 `TimerHandle_t`。

它讲的是任务自己通过 delay API 管理时间。软件定时器是后续课程的另一个内核对象，不能把本课的 `vTaskDelayUntil()` 误认为软件定时器回调。

## 9. 两个版本真正应该怎么学

reg/hal 的差异是硬件初始化和翻转函数，不是时间管理。两个版本都用同样的 FreeRTOS tick、同样的 `vTaskDelayUntil()`、同样的 `vTaskDelay()`。

学习时先把 PC13 和 PA1 当作两个时间模型的输出。PC13 是绝对周期，PA1 是相对延时。再回到源码看 `last` 为什么要放在 precise 循环外，为什么 relative 不需要保存上次唤醒点。

## 10. 检验问题清单

### 10.1 `vTaskDelay()` 和 `vTaskDelayUntil()` 最大区别是什么？

**答**：`vTaskDelay()` 从当前调用时刻开始等待；`vTaskDelayUntil()` 根据上次唤醒时间计算下一次固定周期唤醒。

### 10.2 `last` 为什么不能放进 while 循环里每次重新初始化？

**答**：那样每轮都会把基准重置为当前时间，失去固定周期意义，效果更像相对延时。

### 10.3 PC13 对应哪个任务？

**答**：对应 `precise_task`，它使用 `vTaskDelayUntil()`，周期约 500ms。

### 10.4 PA1 对应哪个任务？

**答**：对应 `relative_task`，它使用 `vTaskDelay()`，每次翻转后相对等待约 700ms。

### 10.5 tick 不准会影响什么？

**答**：会影响所有 FreeRTOS 时间等待，PC13 和 PA1 的周期都会偏。

### 10.6 为什么任务延时时 CPU 没有浪费？

**答**：任务进入阻塞态，不占 CPU。调度器可以运行其他就绪任务或空闲任务。

### 10.7 本课需要时间片吗？

**答**：时间片不是重点。两个任务优先级不同且都会阻塞，本课重点是 tick 延时。

### 10.8 HAL 版时间 API 是否不同？

**答**：不同点不在时间 API。HAL 版仍使用 FreeRTOS 的 `vTaskDelay()` 和 `vTaskDelayUntil()`。

### 10.9 如果 PC13 周期漂移，先查什么？

**答**：查系统时钟、`configCPU_CLOCK_HZ`、`configTICK_RATE_HZ`、`last` 的初始化位置和 `vTaskDelayUntil()` 参数。

### 10.10 PA2 为什么没有时间现象？

**答**：PA2 只是初始化为输出，当前没有任务翻转它。

## 11. 工程实现步骤

### 11.1 需求分析

用两个任务对比相对延时和绝对周期延时，并用两个 GPIO 输出观察结果。

### 11.2 硬件核查

确认 PC13 和 PA1 可观察，HSE 晶振正常，ST-Link 可下载。精确测周期建议用逻辑分析仪。

### 11.3 寄存器路线

配置时钟和 GPIO，创建 precise 与 relative。PC13 使用 `vTaskDelayUntil()`，PA1 使用 `vTaskDelay()`。

### 11.4 HAL 路线

用 HAL 配置同样硬件，时间管理仍使用 FreeRTOS API。

### 11.5 工程思维

周期任务优先考虑 `vTaskDelayUntil()`，普通等待用 `vTaskDelay()`。不要在 RTOS 任务里用忙等代替阻塞。

### 11.6 常见工程陷阱

tick 配置和系统时钟不一致、把 `last` 放错位置、误用 `HAL_Delay()`、忘记检查任务创建、用肉眼判断精确周期，都是本课常见问题。

## 12. 运行现象

PC13 大约每 500ms 翻转一次，PA1 大约每 700ms 翻转一次。两个节奏会不断错开，这是正常现象，因为周期不同。

调试器可观察 tick 计数增长，也可在两个任务的延时 API 前打断点确认任务确实周期运行。

## 13. 常见问题排查

### 13.1 PC13 不闪

查 precise 任务创建结果、PC13 GPIO 配置、是否进入 hook，以及 `vTaskDelayUntil()` 是否被执行。

### 13.2 PA1 不闪

查 relative 任务创建结果、PA1 GPIO 配置和 `vTaskDelay(700ms)` 是否正常返回。

### 13.3 周期整体不准

优先查 HSE/PLL 配置、`configCPU_CLOCK_HZ`、`configTICK_RATE_HZ` 和 SysTick 映射。

### 13.4 PC13 周期逐渐漂移

检查 `last` 是否在循环外初始化，是否误改成了 `vTaskDelay()`，任务执行时间是否超过周期。

### 13.5 进入 malloc failed hook

说明任务或内核对象分配失败。查 heap 大小、任务栈和新增对象数量。

### 13.6 进入 stack overflow hook

说明任务栈不够或被破坏。当前任务简单，若新增复杂代码要增加栈并测栈水位。

### 13.7 HAL 版卡在延时

确认任务里使用的是 FreeRTOS 延时 API，SysTick handler 映射正确，调度器已经启动。

### 13.8 PA2 没反应

这是当前源码的正常现象。PA2 被初始化，但没有任务翻转它。

### 13.9 两个周期偶尔同时翻转

500ms 和 700ms 的节奏不同，但它们会在某些时刻接近同时到期。调度器会先运行高优先级 precise，再运行 relative。看到两个引脚边沿接近，不代表时间 API 混乱，而是两个周期的数学关系自然重合。

### 13.10 加了打印后周期变差

串口打印可能阻塞较久，尤其在没有 DMA 或缓冲设计时。若把打印放进 precise 任务，500ms 周期可能出现抖动甚至超期。周期任务里应减少阻塞式 I/O，把耗时输出交给低优先级任务或队列。

### 13.11 precise 和 relative 同时就绪时谁先跑

按当前优先级，precise 先跑。若你看到 PC13 和 PA1 接近同时翻转，调试器单步时也应先进入优先级 2 的 precise，再轮到优先级 1 的 relative。若顺序不符合，检查任务优先级是否被改动。

### 13.12 改 tick 频率后延时变化

如果修改 `configTICK_RATE_HZ`，必须继续使用 `pdMS_TO_TICKS()`。若代码里混入手写 tick 数，改频率后实际延时会变。排查时搜索是否有人直接写了 `500` 当 tick，而不是毫秒换算宏。

## 14. 本课最核心的结论

1. FreeRTOS 时间管理建立在 tick 基准上。
2. `vTaskDelay()` 是相对延时，从调用时刻开始等待。
3. `vTaskDelayUntil()` 是绝对周期延时，适合稳定周期任务。
4. `TickType_t last` 必须保存跨循环的上次唤醒时间。
5. `pdMS_TO_TICKS()` 让代码用毫秒表达时间。
6. 延时任务进入阻塞态，不占用 CPU。
7. reg/hal 差异在 GPIO，不在 FreeRTOS 时间模型。
8. 时钟配置错误会让所有 RTOS 延时一起失准。

## 15. 建议你现在怎么读这节课

先只看两个任务：一个 PC13、一个 PA1；一个 500ms、一个 700ms；一个 `vTaskDelayUntil()`、一个 `vTaskDelay()`。再看 `last` 的位置，理解为什么它在 while 外初始化。

最后用调试器或逻辑分析仪观察周期。肉眼能看出节奏不同，但很难判断漂移。

## 16. 扩展练习

1. 把 precise 周期改成 1000ms，观察 PC13 变慢。
2. 把 relative 延时改成 500ms，对比两个引脚是否更接近。
3. 故意把 `last` 放进 while 内，观察周期模型变化。
4. 在 precise 任务里增加耗时代码，观察 `vTaskDelayUntil()` 的表现。
5. 用逻辑分析仪测 PC13 和 PA1 周期。

## 17. 下一课预告

- 上一课：[47_freertos_scheduler_time_slice](../47_freertos_scheduler_time_slice/README.md)
- 下一课：[49_freertos_interrupt_critical](../49_freertos_interrupt_critical/README.md)
