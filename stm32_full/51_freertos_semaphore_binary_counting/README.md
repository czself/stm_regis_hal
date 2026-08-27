# 51_freertos_semaphore_binary_counting - FreeRTOS 二值信号量与计数信号量

## 1. 本课到底在学什么

本课表面现象是：PC13 大约每 1000ms 翻转一次，PA1 会按 worker 获取计数信号量后的节奏翻转。PC13 来自二值信号量的“给一次、取一次”，PA1 来自计数信号量的“拿一个资源、用 300ms、再还回去”。

真正要学的是 FreeRTOS 信号量的两种用途。二值信号量更像事件通知，关心“有没有事件”；计数信号量更像资源计数，关心“还有几个资源可用”。本课同时创建 `g_binary_sem` 和 `g_counting_sem`，让你把两种语义放在同一个工程里对比。

这节课接在队列之后。队列传递数据，信号量通常传递“许可”或“事件状态”。后面的互斥量、事件组、任务通知都会继续围绕“任务如何等待某个条件”展开。

## 2. 本课学习目标

学完本课你应该能做到：

- 能解释 PC13 为什么由 `giver` 每 1000ms 给出的二值信号量驱动。
- 能解释 PA1 为什么由 `worker` 获取 `g_counting_sem` 后翻转。
- 能说出 `xSemaphoreCreateBinary()` 创建出来初始是否已经可取。
- 能说出 `xSemaphoreCreateCounting(2, 2)` 的最大计数和初始计数。
- 能区分 `xSemaphoreGive()` 和 `xSemaphoreTake()` 的方向。
- 能说明 `portMAX_DELAY` 在信号量等待中的作用。
- 能解释计数信号量为什么适合表示有限资源。
- 能把 reg/hal 的 GPIO 翻转差异和 RTOS 信号量逻辑分开。
- 能根据 PC13 不动、PA1 不动、进入 hook、计数资源耗尽等现象排查。
- 能知道二值/计数信号量和互斥量不是同一个东西。

## 3. 本课目录结构

```text
51_freertos_semaphore_binary_counting/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两个版本都包含 `semphr.h`。信号量 API 属于 FreeRTOS，不属于 STM32 HAL。reg/hal 差别仍然在时钟、GPIO 初始化和 LED 翻转函数。

## 4. 实验硬件

- STM32F103C8T6 BluePill。
- ST-Link 下载器。
- HSE 8MHz，PLL x9，系统时钟 72MHz。
- PC13：由 taker 成功拿到二值信号量后翻转。
- PA1：由 worker 成功拿到计数信号量后翻转。
- PA2：初始化为输出，但当前源码未使用。
- PA0：初始化为上拉输入，但当前源码未读取。
- `configUSE_COUNTING_SEMAPHORES = 1`，所以计数信号量 API 可用。
- `configTOTAL_HEAP_SIZE = 12 * 1024`，信号量对象和任务栈从 heap 分配。

本课没有中断释放信号量，也没有按键参与。所有 Give/Take 都发生在任务上下文。

## 5. 先建立一个最基本的脑图

本课完整链路是：

```text
复位启动
  -> 配 72MHz 时钟和 GPIO
  -> 创建二值信号量 g_binary_sem
  -> 创建计数信号量 g_counting_sem，最大 2，初始 2
  -> 创建 giver、taker、worker 三个任务
  -> giver 每 1000ms 调用 xSemaphoreGive(g_binary_sem)
  -> taker 阻塞等待 g_binary_sem，取到后翻转 PC13
  -> worker 阻塞等待 g_counting_sem，取到后翻转 PA1
  -> worker 使用资源 300ms
  -> worker xSemaphoreGive(g_counting_sem) 归还资源
```

PC13 这条线体现“事件同步”：giver 发出一次许可，taker 消费一次许可。PA1 这条线体现“资源计数”：worker 先拿资源，工作一段时间，再归还资源。

要注意：本课只有一个 worker 任务，所以计数信号量的竞争效果不强，但源码已经展示了“最大计数 2、初始计数 2、take 后 give 回去”的资源模型。

## 6. 先认识本课里出现的核心名词

### 6.1 `Semaphore` 是什么

Semaphore 是信号量，属于 FreeRTOS 内核对象层。

它不直接传递业务数据，而是表达许可、事件或资源数量。本课二值信号量驱动 PC13，计数信号量驱动 PA1。

信号量创建失败时，对应任务无法同步，程序会停在错误分支或 hook。

### 6.2 `Binary Semaphore` 是什么

Binary Semaphore 是二值信号量，中文叫二值信号量，属于 RTOS 同步对象层。

它只有“可取”和“不可取”两种状态，适合事件通知。本课 `g_binary_sem` 由 giver 每 1000ms give 一次，taker take 成功后翻转 PC13。

如果没人 give，taker 会一直阻塞；如果 give 频率高于 take，二值信号量不会无限累计。

### 6.3 `Counting Semaphore` 是什么

Counting Semaphore 是计数信号量，属于 RTOS 资源计数层。

它内部有一个计数值，take 成功时计数减一，give 时计数加一，但不会超过最大值。本课 `xSemaphoreCreateCounting(2, 2)` 表示最多 2 个资源，初始已有 2 个可用。

如果资源被拿完，后续 take 会阻塞，直到有人 give。

### 6.4 `SemaphoreHandle_t` 是什么

`SemaphoreHandle_t` 是信号量句柄类型。

源码中 `static SemaphoreHandle_t g_binary_sem, g_counting_sem;` 分别保存二值和计数信号量对象。所有 give/take 都通过句柄找到对应对象。

句柄为 NULL 表示创建失败，继续使用会导致异常或断言。

### 6.5 `xSemaphoreCreateBinary()` 是什么

这是创建二值信号量的 API。

本课创建后没有先 give，所以 taker 一开始会阻塞，直到 giver 第一次执行 `xSemaphoreGive(g_binary_sem)`。这点和某些旧 API 创建后需要手动清空的历史行为要分清。

创建失败通常来自 heap 不足。

### 6.6 `xSemaphoreCreateCounting(2, 2)` 是什么

这是创建计数信号量的 API。

第一个参数 2 是最大计数，第二个参数 2 是初始计数。当前 worker 一开始就能 take 成功，因为初始资源数量不是 0。

如果初始计数写成 0，worker 会阻塞到有人 give。

### 6.7 `xSemaphoreGive()` 是什么

`xSemaphoreGive()` 是释放信号量或归还资源的 API。

giver 对 `g_binary_sem` give，表示事件发生；worker 对 `g_counting_sem` give，表示资源使用完归还。它们都是任务上下文 API。

如果对已经满的计数信号量 give，通常会失败，工程中应检查返回值。

### 6.8 `xSemaphoreTake()` 是什么

`xSemaphoreTake()` 是获取信号量或申请资源的 API。

taker 用它等待二值事件，worker 用它申请计数资源。第二个参数 `portMAX_DELAY` 表示一直等到可取。

如果等待有限时间，就必须处理超时返回。

### 6.9 `giver` 是什么

giver 是事件生产任务。

它每 1000ms give 一次 `g_binary_sem`，优先级 2。它决定 PC13 事件节奏。

若 giver 没创建成功，PC13 不会按二值信号量节奏翻转。

### 6.10 `taker` 是什么

taker 是二值信号量消费任务。

它阻塞等待 `g_binary_sem`，成功后翻转 PC13。它不自己延时，节奏来自 giver。

### 6.11 `worker` 是什么

worker 是计数资源使用任务。

它 take `g_counting_sem`，翻转 PA1，延时 300ms 模拟占用资源，再 give `g_counting_sem`。这条链路体现资源必须归还。

### 6.12 `portMAX_DELAY` 在信号量里是什么

它表示任务愿意一直等待信号量可用。

本课 taker 和 worker 都使用它，所以没有超时分支。任务等待期间不占 CPU。

### 6.13 `PC13` 在本课做什么

PC13 是二值信号量链路的现象层输出。

收到一次 `g_binary_sem`，taker 翻转一次 PC13。它证明事件通知被消费。

### 6.14 `PA1` 在本课做什么

PA1 是计数信号量链路的现象层输出。

worker 成功拿到 `g_counting_sem` 后翻转 PA1，再延时 300ms 并归还资源。PA1 证明资源申请路径在运行。

### 6.15 信号量和队列有什么区别

队列传递数据元素，信号量更多表达许可或计数。

本课二值信号量不携带 `uint8_t` 值；taker 只知道事件发生，不知道额外数据。若需要传命令内容，应使用队列。

### 6.16 信号量和互斥量有什么区别

互斥量也是基于信号量机制实现的一类对象，但语义不同。

互斥量用于保护共享资源，并带有优先级继承；二值/计数信号量主要用于同步或资源计数。下一课会专门讲互斥量和优先级反转。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件分工

寄存器版包含 CMSIS、FreeRTOS task 和 `semphr.h`。

`semphr.h` 提供 `SemaphoreHandle_t` 和信号量 API。GPIO 寄存器仍来自 `stm32f1xx.h`。

### 7.2 时钟初始化

代码配置 Flash、HSE、PLL x9 和总线分频到 72MHz。

信号量本身不依赖 GPIO 时钟，但任务延时 `1000ms` 和 `300ms` 依赖 FreeRTOS tick，tick 又依赖正确系统时钟。

### 7.3 GPIO 初始化

PC13、PA1、PA2 被配置为输出，PA0 被配置为上拉输入。

当前只使用 PC13 和 PA1。PA2/PA0 不是本课现象来源。

### 7.4 创建 `g_binary_sem`

`g_binary_sem = xSemaphoreCreateBinary();`

这一步创建二值信号量。创建失败时返回 NULL。源码在 main 末尾统一检查。

### 7.5 创建 `g_counting_sem`

`g_counting_sem = xSemaphoreCreateCounting(2, 2);`

最大计数 2，初始计数 2。worker 第一次运行时能立即 take，不需要等待 giver。

### 7.6 创建三个任务

main 创建 giver 优先级 2，taker 优先级 1，worker 优先级 1。

giver 周期性产生二值事件；taker 消费事件；worker 演示计数资源。

### 7.7 giver 的 give 和 delay

giver 循环里先 `xSemaphoreGive(g_binary_sem)`，再 `vTaskDelay(1000ms)`。

这表示每秒释放一次二值事件。若 give 失败，源码没有处理返回值，真实工程应记录。

### 7.8 taker 的 take

taker 阻塞在 `xSemaphoreTake(g_binary_sem, portMAX_DELAY)`。

give 到来后 take 返回 `pdTRUE`，taker 翻转 PC13。然后再次等待下一次 give。

### 7.9 worker 的资源申请

worker 调用 `xSemaphoreTake(g_counting_sem, portMAX_DELAY)`。

初始计数为 2，所以第一次 take 立即成功。成功后翻转 PA1，表示拿到资源。

### 7.10 worker 的资源占用和归还

worker 延时 300ms，模拟占用资源，然后 `xSemaphoreGive(g_counting_sem)` 归还。

如果忘记 give，资源计数会下降，最终 worker 或其他任务可能永远等不到资源。

### 7.11 hook 的排错价值

信号量创建和任务创建都从 heap 分配。

如果进入 malloc failed hook，说明对象或任务内存不足。若进入 stack overflow hook，说明任务栈不够或被破坏。

### 7.12 当前课程没有 ISR 版本

本课没有从中断 give 信号量。

若将来在 ISR 中释放信号量，应使用 `xSemaphoreGiveFromISR()`，并处理 `portYIELD_FROM_ISR()`。当前源码全部是任务上下文 API。

### 7.13 二值信号量为什么可能合并事件

二值信号量只有 0/1 状态。若 giver 在 taker 尚未 take 前连续 give，多余 give 不会像队列那样积累多条消息。

本课 giver 每 1000ms 才 give 一次，taker 处理很快，所以通常看不出合并问题。但把 giver 周期改得很短、或让 taker 处理很慢后，就能观察到二值信号量不适合统计每一次事件。

### 7.14 计数信号量如何体现资源池

`g_counting_sem` 最大计数为 2，初始计数为 2，相当于资源池里一开始有两个可用资源。

当前只有一个 worker，所以资源竞争不明显。若增加第二、第三个 worker，它们会共同从同一个计数信号量里申请资源；最多两个任务能同时拿到资源，超过数量的任务会阻塞等待。

## 8. HAL 版代码逐步讲解

### 8.1 HAL 初始化边界

HAL 版调用 `HAL_Init()`、HAL RCC 和 HAL GPIO API 初始化硬件。

信号量创建和任务同步仍由 FreeRTOS API 完成。

### 8.2 HAL RCC 配置

`RCC_OscInitTypeDef` 和 `RCC_ClkInitTypeDef` 配置 HSE、PLL x9、总线分频和 Flash latency。

这对应寄存器版时钟配置，影响任务延时准确性。

### 8.3 HAL GPIO 配置

PC13、PA1、PA2 输出，PA0 上拉输入。

PC13 用于二值信号量现象，PA1 用于计数信号量现象。

### 8.4 HAL 版二值信号量

HAL 版同样 `xSemaphoreCreateBinary()`，giver/taker 逻辑不变。

PC13 翻转从寄存器函数换成 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)`。

### 8.5 HAL 版计数信号量

HAL 版同样 `xSemaphoreCreateCounting(2, 2)`。

worker 成功 take 后用 `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_1)` 翻转 PA1。

### 8.6 HAL 版不使用 HAL_Delay

giver 和 worker 都使用 `vTaskDelay()`。

RTOS 任务等待应进入阻塞态，不能用裸机忙等思路替代。

### 8.7 HAL 版也要检查对象创建

`g_binary_sem`、`g_counting_sem` 和三个任务创建结果都要检查。

HAL 不会替 FreeRTOS heap 分配失败兜底。

### 8.8 HAL 版调试仍可看寄存器

HAL 翻转 GPIO 后，底层 ODR 位仍会变化。

可观察 GPIOC ODR13 和 GPIOA ODR1，分别验证二值和计数链路。

### 8.9 HAL 版也要区分同步和资源

HAL 版 PC13 不闪时，先查二值信号量链路；PA1 不闪时，先查计数信号量链路。

不要只查 `HAL_GPIO_TogglePin()`。GPIO 翻转只是最后一步，前面还有信号量创建、give、take、任务创建和延时节奏。

### 8.10 HAL 版不自动防止资源泄漏

如果 worker take 成功后因为后续代码提前返回或卡住，没有执行 `xSemaphoreGive(g_counting_sem)`，HAL 也不会自动归还资源。

真实工程里常用统一退出路径或清理代码，确保每次成功 take 后最终都会 give。

## 9. 两个版本真正应该怎么学

reg/hal 的差异在硬件表达，信号量语义完全相同。二值信号量看 PC13，计数信号量看 PA1。

学习时先不要纠结 HAL 封装，先把 `g_binary_sem` 和 `g_counting_sem` 的语义分清：一个表示事件到达，一个表示资源数量。再看任务如何阻塞、被唤醒、处理和再次等待。

## 10. 检验问题清单

### 10.1 二值信号量适合表达什么？

**答**：适合表达事件发生或一次许可，不携带业务数据。

### 10.2 计数信号量适合表达什么？

**答**：适合表达有限资源数量，例如最多允许几个任务同时使用某类资源。

### 10.3 `xSemaphoreCreateCounting(2, 2)` 两个 2 分别是什么？

**答**：第一个是最大计数，第二个是初始计数。

### 10.4 taker 为什么不自己延时？

**答**：它阻塞等待 `g_binary_sem`，节奏由 giver 的 give 决定。

### 10.5 worker 为什么要 give 回 `g_counting_sem`？

**答**：它拿到资源后必须归还，否则计数会减少，最终资源耗尽。

### 10.6 PC13 对应哪条链路？

**答**：对应二值信号量 `g_binary_sem` 的 giver/taker 链路。

### 10.7 PA1 对应哪条链路？

**答**：对应计数信号量 `g_counting_sem` 的资源申请/归还链路。

### 10.8 本课有没有 FromISR API？

**答**：没有。所有 give/take 都在任务上下文。

### 10.9 信号量会传递 `uint8_t` 数据吗？

**答**：本课信号量不传数据，只传许可。要传数据应使用队列。

### 10.10 互斥量和二值信号量能混用吗？

**答**：不能随便混用。互斥量有所有者和优先级继承语义，适合保护共享资源；二值信号量更适合事件同步。

## 11. 工程实现步骤

### 11.1 需求分析

同时演示二值信号量事件同步和计数信号量资源计数。

### 11.2 硬件核查

确认 PC13 和 PA1 可观察，HSE 正常，ST-Link 可下载。PA0 不参与本课。

### 11.3 寄存器路线

配置时钟和 GPIO，创建 `g_binary_sem`、`g_counting_sem` 和三个任务。PC13 用寄存器翻转，PA1 用寄存器翻转。

### 11.4 HAL 路线

用 HAL 配置硬件，RTOS 信号量 API 不变。GPIO 翻转改用 HAL API。

### 11.5 工程思维

事件同步用二值信号量，资源数量用计数信号量。要检查创建结果、take/give 返回值，并避免资源拿了不还。

### 11.6 常见工程陷阱

把信号量当队列传数据、忘记归还计数资源、二值信号量 give 太快导致事件合并、在 ISR 中误用任务 API、忽略创建失败，都是常见问题。

## 12. 运行现象

PC13 大约每 1000ms 翻转一次，因为 giver 每秒 give 一次二值信号量。PA1 由 worker 获取计数信号量后翻转，并在 300ms 后归还资源。

如果只看 PC13，会错过计数信号量链路。建议同时观察 PC13 和 PA1，或者在两个 take 成功后打断点。

## 13. 常见问题排查

### 13.1 PC13 不闪

查 `g_binary_sem` 是否创建成功、giver/taker 是否创建成功、giver 是否执行 give、taker 是否阻塞在正确句柄上。

### 13.2 PA1 不闪

查 `g_counting_sem` 是否创建成功，初始计数是否为 2，worker 是否创建成功并执行 take。

### 13.3 程序进入 malloc failed hook

说明信号量或任务创建分配失败。检查 heap、任务栈和新增对象数量。

### 13.4 程序进入 stack overflow hook

说明某个任务栈不足或被破坏。当前任务简单，若增加打印或大局部变量要增大栈。

### 13.5 资源拿了不还

如果删掉 `xSemaphoreGive(g_counting_sem)`，计数会逐步耗尽，后续 worker 会阻塞。真实工程中要确保每条错误路径也归还资源。

### 13.6 二值事件看起来丢失

二值信号量不是计数队列。多次 give 可能合并成一个可取状态。若每次事件都不能丢，应考虑队列或计数信号量。

### 13.7 HAL 版 GPIO 不动

先确认信号量 take 是否成功。若成功但 GPIO 不动，再查 HAL GPIO 初始化和具体 Pin。

### 13.8 等待永远不返回

检查对应 give 是否发生、句柄是否正确、任务是否创建成功。`portMAX_DELAY` 会一直等，不会自动超时。

### 13.9 PC13 正常但 PA1 异常

这说明二值信号量链路可能正常，但计数信号量链路有问题。优先查 `g_counting_sem`、worker、PA1，而不是反复检查 giver/taker。

### 13.10 PA1 正常但 PC13 异常

这说明计数信号量链路可能正常，但二值信号量链路有问题。优先查 `g_binary_sem`、giver、taker、PC13。

### 13.11 增加多个 worker 后节奏变化

多个 worker 共享 `g_counting_sem` 时，计数信号量才更像资源池。若资源数是 2，第三个 worker 应该阻塞。若三个都能同时进入资源区，说明没有正确使用同一个 `g_counting_sem`。

### 13.12 give/take 返回值被忽略

当前源码为了简洁，没有检查每次 `xSemaphoreGive()` 的返回值。真实工程里应该检查，尤其计数信号量已经满时 give 会失败。返回值能帮你区分“任务没运行”和“信号量状态不允许这次操作”。

### 13.13 初始计数和最大计数写反

`xSemaphoreCreateCounting(max, initial)` 的第一个参数是最大值，第二个是初始值。初始值不能大于最大值。若把参数理解反，worker 的启动行为会和预期不同，甚至创建失败或断言。

## 14. 本课最核心的结论

1. 二值信号量适合事件同步，计数信号量适合资源计数。
2. `xSemaphoreGive()` 释放许可，`xSemaphoreTake()` 获取许可。
3. `portMAX_DELAY` 让任务阻塞等待，不占 CPU。
4. `xSemaphoreCreateCounting(2, 2)` 表示最大 2、初始 2。
5. PC13 对应二值信号量链路，PA1 对应计数信号量链路。
6. 信号量不传业务数据，传数据应使用队列。
7. 互斥量有优先级继承语义，不应简单等同二值信号量。
8. reg/hal 差异只在 GPIO，RTOS 信号量语义相同。

## 15. 建议你现在怎么读这节课

先把 `g_binary_sem` 和 `g_counting_sem` 分开画两条线：giver -> g_binary_sem -> taker -> PC13，worker -> g_counting_sem -> PA1 -> 延时 -> give 回去。第二遍再看创建参数，特别是计数信号量的两个 2。最后对比 reg/hal 的 GPIO 翻转方式。

## 16. 扩展练习

1. 把 `g_counting_sem` 初始计数改成 0，观察 worker 是否阻塞。
2. 注释掉 worker 的 `xSemaphoreGive(g_counting_sem)`，观察资源耗尽。
3. 检查 `xSemaphoreGive()` 返回值，记录满计数时的失败。
4. 增加第二个 worker，观察计数信号量的资源共享效果。
5. 把 giver 周期改成 200ms，观察二值信号量事件合并风险。

## 17. 下一课预告

- 上一课：[50_freertos_queue](../50_freertos_queue/README.md)
- 下一课：[52_freertos_mutex_priority_inversion](../52_freertos_mutex_priority_inversion/README.md)
