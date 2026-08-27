# 53_freertos_queue_set - FreeRTOS 队列集

## 1. 本课到底在学什么

本课表面现象是：PC13 大约按 700ms 相关节奏翻转，PA1 大约按 1100ms 相关节奏翻转。它们不是两个 consumer 分别等两个队列，而是一个 `select_task` 同时等待两个队列所属的队列集。

真正要学的是 Queue Set，中文可叫队列集。它让一个任务阻塞等待多个队列或信号量中的任意一个变为可读。本课有两个队列：`g_a` 接收 producer A 的值 1，`g_b` 接收 producer B 的值 2；两个队列都加入 `g_set`，selector 任务先用 `xQueueSelectFromSet()` 判断哪个成员有数据，再到对应队列里 `xQueueReceive()`。

这节课接在队列、信号量、互斥量之后。普通队列是一对或多对任务的数据通道；队列集解决的是“一个任务要同时等多个通道”的问题。后续如果一个任务同时等串口命令、按键事件、传感器事件，就会遇到类似结构。

不过要提醒一点：队列集并不是 FreeRTOS 里唯一、也不是最常用的“多事件等待”方案。下一课的事件组适合等“多个条件 bit 到齐”，55 课的任务通知适合轻量一对一通知。队列集最适合的场景是“一个任务要同时等待多个不同队列/信号量，并且这些成员来源不同、语义不同”。学完本课后，你要能把这三种方案放在心里对比，而不是把所有多事件等待都写成队列集。

## 2. 本课学习目标

学完本课你应该能做到：

- 能解释为什么本课有两个普通队列 `g_a` 和 `g_b`。
- 能解释 `xQueueCreateSet(8)` 的容量为什么要覆盖成员队列可能积压的事件数。
- 能说明 `xQueueAddToSet(g_a, g_set)` 和 `xQueueAddToSet(g_b, g_set)` 的作用。
- 能说明 `xQueueSelectFromSet(g_set, portMAX_DELAY)` 返回的不是消息值，而是哪个成员可读。
- 能解释 selector 为什么还要再调用 `xQueueReceive()`。
- 能把 PC13 对应到 `g_a`，PA1 对应到 `g_b`。
- 能知道 producer A 周期 700ms，producer B 周期 1100ms。
- 能区分队列集和事件组：队列集选“哪个对象可读”，事件组等“哪些 bit 到齐”。
- 能根据 PC13/PA1 不动、选择后收不到、队列集创建失败等现象排查。

## 3. 本课目录结构

```text
53_freertos_queue_set/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两套工程都使用 `queue.h`。队列集 API 和队列 API 同属 FreeRTOS queue 模块，和 STM32 HAL 没有从属关系。reg/hal 差异只在 GPIO 翻转方式。

## 4. 实验硬件

- STM32F103C8T6 BluePill。
- ST-Link 下载器。
- PC13：selector 收到 `g_a` 事件后翻转。
- PA1：selector 收到 `g_b` 事件后翻转。
- PA2：初始化为输出，当前未使用。
- PA0：初始化为上拉输入，当前未读取。
- 系统时钟：HSE 8MHz，PLL x9 到 72MHz。
- FreeRTOS 配置：`configUSE_QUEUE_SETS = 1`。

本课没有中断，也没有按键。两个事件来源都来自普通任务的周期发送。

## 5. 先建立一个最基本的脑图

```text
复位启动
  -> 配时钟和 GPIO
  -> 创建队列 g_a，长度 4，元素 uint8_t
  -> 创建队列 g_b，长度 4，元素 uint8_t
  -> 创建队列集 g_set，容量 8
  -> 把 g_a 加入 g_set
  -> 把 g_b 加入 g_set
  -> prod_a 每 700ms 向 g_a 发送 1
  -> prod_b 每 1100ms 向 g_b 发送 2
  -> select_task 阻塞等待 g_set
  -> 队列集返回可读成员 active
  -> active == g_a 时读 g_a 并翻转 PC13
  -> active == g_b 时读 g_b 并翻转 PA1
```

这条链路里最关键的一点是：队列集不替你取出消息。它只告诉你“哪个成员现在有东西可取”。真正的数据仍在原队列里，所以 selector 必须对 `g_a` 或 `g_b` 再调用 `xQueueReceive()`。

## 6. 先认识本课里出现的核心名词

### 6.1 `Queue Set` 是什么

Queue Set 是 FreeRTOS 队列集，属于 RTOS 内核对象层。

它把多个队列或信号量放进一个集合，让一个任务可以阻塞等待其中任意成员变为可读。本课用 `g_set` 同时等待 `g_a` 和 `g_b`。

### 6.2 `QueueSetHandle_t` 是什么

`QueueSetHandle_t` 是队列集句柄类型。

`static QueueSetHandle_t g_set;` 保存队列集对象。后续创建、添加成员、选择成员都通过它。

### 6.3 `xQueueCreateSet(8)` 是什么

这是创建队列集的 API。

参数 8 是队列集容量。因为 `g_a` 长度 4，`g_b` 长度 4，容量 8 能覆盖两个队列都满时的成员通知需求。

### 6.4 `xQueueAddToSet()` 是什么

它把队列或信号量加入队列集。

源码把 `g_a` 和 `g_b` 加入 `g_set`。如果忘记添加，队列里即使有数据，selector 也等不到该队列的可读通知。

### 6.5 `xQueueSelectFromSet()` 是什么

这是等待队列集中任意成员可读的 API。

本课 `active = xQueueSelectFromSet(g_set, portMAX_DELAY)`。返回值是成员句柄，不是队列里存放的 `uint8_t` 数据。

### 6.6 `QueueSetMemberHandle_t` 是什么

它是队列集成员句柄类型。

`active` 用它保存本次被选中的成员。代码用 `active == g_a` 或 `active == g_b` 判断下一步读哪个队列。

### 6.7 `g_a` 是什么

`g_a` 是普通 FreeRTOS 队列。

`prod_a` 每 700ms 发送 `uint8_t value = 1` 到它。selector 发现 `active == g_a` 后读取它并翻转 PC13。

### 6.8 `g_b` 是什么

`g_b` 是第二个普通队列。

`prod_b` 每 1100ms 发送 `uint8_t value = 2`。selector 发现 `active == g_b` 后读取它并翻转 PA1。

### 6.9 `prod_a` 是什么

producer A 任务。

它只负责周期性向 `g_a` 发送值 1，不直接翻转 PC13。这体现生产和选择处理分离。

### 6.10 `prod_b` 是什么

producer B 任务。

它向 `g_b` 发送值 2，周期 1100ms。两个 producer 周期不同，所以 PC13 和 PA1 的翻转节奏不同。

### 6.11 `select_task` 是什么

selector 任务是本课核心任务。

它优先级 2，高于两个 producer 的 1。它等待队列集，选中成员后读取对应队列并翻转对应 GPIO。

### 6.12 `portMAX_DELAY` 在队列集里是什么

表示 selector 愿意一直等到任意成员可读。

等待期间 selector 阻塞，不占 CPU。producer 发送后，队列集唤醒 selector。

### 6.13 队列集容量为什么重要

队列集容量必须能容纳所有成员可能产生的待处理事件通知。

本课两个队列长度都是 4，所以队列集容量设为 8。容量不足会造成队列集行为不符合预期。

### 6.14 队列集和事件组有什么区别

队列集返回“哪个队列/信号量可读”，事件组返回 bit 状态。

本课要从两个队列里取不同来源的数据，所以用队列集；下一课要等 BIT_A 和 BIT_B 同时到齐，所以用事件组。

### 6.15 `PC13` 和 `PA1` 在本课做什么

PC13 是 `g_a` 被处理的现象证据，PA1 是 `g_b` 被处理的现象证据。

它们由同一个 selector 翻转，不是由两个 producer 直接翻转。

### 6.16 为什么 selector 优先级更高

selector 优先级为 2，两个 producer 优先级为 1。

这表示队列集中有成员可读时，selector 能更快运行并清空对应队列，降低队列积压风险。若 selector 优先级太低，两个 producer 可能持续投递，队列更容易满。

### 6.17 `value` 为什么仍要接收

selector 虽然当前只用来源决定翻转哪个 LED，但仍声明 `uint8_t value` 接收数据。

这是为了体现队列集只选对象，真正消息仍在队列中。将来可以根据 `value` 的值进一步区分命令。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件分工

寄存器版包含 `queue.h`，因为队列和队列集 API 都在这个模块。

### 7.2 时钟和 GPIO 初始化

系统配置到 72MHz。GPIOC/GPIOA 打开时钟，PC13/PA1/PA2 输出，PA0 上拉输入。

本课使用 PC13 和 PA1。

### 7.3 创建两个队列

`g_a = xQueueCreate(4, sizeof(uint8_t));`，`g_b = xQueueCreate(4, sizeof(uint8_t));`。

两个队列元素大小相同，但语义不同：A 表示来源 A，B 表示来源 B。

### 7.4 创建队列集

`g_set = xQueueCreateSet(8);`

容量 8 对应两个长度 4 队列的总容量。创建失败时句柄为 NULL。

### 7.5 添加成员

`xQueueAddToSet(g_a, g_set);` 和 `xQueueAddToSet(g_b, g_set);`。

源码没有检查返回值，真实工程应检查添加是否成功，尤其成员已经被加入其他集合时。

### 7.6 producer A

`prod_a` 向 `g_a` 发送 1，然后延时 700ms。

发送等待时间为 0，队列满时会失败，源码未检查返回值。

### 7.7 producer B

`prod_b` 向 `g_b` 发送 2，然后延时 1100ms。

不同周期让 selector 收到两个来源的节奏不同。

### 7.8 selector 等待队列集

`active = xQueueSelectFromSet(g_set, portMAX_DELAY);`

它阻塞直到某个成员可读。返回后要判断 active。

### 7.9 active 是 `g_a`

代码从 `g_a` 接收数据，再翻转 PC13。

接收等待时间为 0，因为队列集已经告诉我们 `g_a` 可读。

### 7.10 active 是 `g_b`

代码从 `g_b` 接收数据，再翻转 PA1。

同样使用 0 等待，避免 selector 在已经选择后又长时间阻塞。

### 7.11 创建三个任务

prod_a/prod_b 优先级 1，select_task 优先级 2。

selector 优先级高，消息到来后能更快处理。

### 7.12 当前没有 ISR

本课所有发送都来自任务，不使用 FromISR API。

若从中断向队列集成员队列发送，应使用对应 FromISR API。

### 7.13 队列集容量不足的后果

如果 `xQueueCreateSet()` 的容量小于成员可能同时积压的总数，队列集可能无法记录所有成员就绪状态。

本课 `g_a` 和 `g_b` 各能积压 4 条，因此设置为 8 是保守且正确的教学写法。真实工程要按成员队列长度和信号量最大计数计算，而不是随便写一个小数。

### 7.14 为什么选择后接收等待时间为 0

队列集已经告诉 selector 某个成员可读，所以随后对该成员队列 `xQueueReceive(..., 0)`。

如果这里再用长等待，代码语义会变得混乱：你已经知道它可读，却还允许任务在接收处长期阻塞。正确做法是立即取出并处理。

### 7.15 如果 active 不是任何已知成员

当前源码只处理 `g_a` 和 `g_b` 两种成员。

真实工程里若队列集成员更多，应该为每个成员写明确分支，或添加默认错误处理。否则新增成员后 selector 可能被唤醒却没有处理对应消息。

### 7.16 为什么 producer 不直接翻转 LED

`prod_a` 和 `prod_b` 只发送队列，不直接翻转 PC13/PA1。

这是为了突出队列集的作用：两个来源都交给同一个 selector 统一处理。如果 producer 自己翻转 LED，就看不出 selector 是如何在多个来源之间选择的。

### 7.17 两个队列元素大小相同但来源不同

`g_a` 和 `g_b` 都保存 `uint8_t`，但来源语义不同。

`v=1` 表示 A 来源，`v=2` 表示 B 来源。selector 当前主要靠 active 判断来源，而不是靠 v 判断。这样做更可靠，因为 active 直接来自队列集选择结果。

### 7.18 queue set 不是轮询

selector 没有循环试探 `g_a` 再试探 `g_b`。

它阻塞在 `xQueueSelectFromSet()`，由内核在任意成员可读时唤醒。这比手动轮询两个队列更省 CPU，也更容易扩展到更多来源。

## 8. HAL 版代码逐步讲解

### 8.1 HAL 初始化

HAL 版用 HAL 初始化时钟和 GPIO，队列集逻辑不变。

### 8.2 HAL GPIO 映射

PC13、PA1 输出对应 selector 处理两个来源。HAL 字段最终仍写 GPIO 寄存器。

### 8.3 HAL 版队列和队列集

`xQueueCreate()`、`xQueueCreateSet()`、`xQueueAddToSet()` 与寄存器版完全相同。

这些不是 HAL API。

### 8.4 HAL 版 producer

两个 producer 发送数据和延时的逻辑不变。

### 8.5 HAL 版 selector

selector 选中 `g_a` 后用 HAL 翻转 PC13；选中 `g_b` 后用 HAL 翻转 PA1。

### 8.6 HAL 版也要检查创建结果

队列、队列集和任务都可能创建失败。

HAL 初始化成功不等于 FreeRTOS 对象成功。

### 8.7 HAL 版观察底层寄存器

仍可观察 GPIOC ODR13 和 GPIOA ODR1 来确认输出变化。

### 8.8 HAL 版不使用 HAL_Delay

producer 使用 `vTaskDelay()`，selector 使用队列集阻塞。等待由 RTOS 管理。

### 8.9 HAL 版添加成员也要看返回值

当前源码没有检查 `xQueueAddToSet()` 返回值。

工程代码应检查它是否为 `pdPASS`。成员已经属于其他队列集、参数错误或对象无效时，添加可能失败。添加失败后，producer 发送再多消息，selector 也不会被该成员唤醒。

### 8.10 HAL 版如何确认来源

可以在 selector 中观察 `active`，也可以观察 `v`。

`active == g_a` 且 `v == 1` 时翻转 PC13，`active == g_b` 且 `v == 2` 时翻转 PA1。两个证据一起看，比只看 LED 更可靠。

### 8.11 HAL 版和寄存器版的阻塞点一致

两个版本真正的阻塞点都是 `xQueueSelectFromSet(g_set, portMAX_DELAY)` 和 producer 的 `vTaskDelay()`。

HAL GPIO 函数只是被唤醒后的输出动作。若 selector 没被唤醒，换 HAL 或寄存器翻转方式都不会让 LED 动起来。

### 8.12 HAL 版不要用多个 consumer 替代本课目标

如果给 `g_a` 和 `g_b` 各写一个 consumer，当然也能处理两个队列，但那就不是队列集教学目标。

队列集的价值在于一个任务统一等待多个来源，方便集中处理优先级、状态机和资源访问。

## 9. 两个版本真正应该怎么学

两个版本的队列集逻辑完全相同。reg/hal 只改变 GPIO 翻转方式。

学习重点是三步：先等集合，得到成员句柄，再到成员队列取数据。漏掉任意一步都会误解队列集。

## 10. 检验问题清单

### 10.1 队列集会直接返回数据吗？

**答**：不会。它返回可读成员句柄，数据还在原队列里。

### 10.2 为什么 `xQueueCreateSet(8)` 是 8？

**答**：两个成员队列长度各 4，总容量是 8。

### 10.3 为什么选择后还要 `xQueueReceive()`？

**答**：因为选择只告诉你哪个队列可读，不会替你取出消息。

### 10.4 PC13 对应哪个来源？

**答**：对应 `g_a`，也就是 `prod_a` 发送的事件。

### 10.5 PA1 对应哪个来源？

**答**：对应 `g_b`，也就是 `prod_b` 发送的事件。

### 10.6 producer 发送失败会怎样？

**答**：当前源码未检查返回值，队列满时事件可能丢失。

### 10.7 队列不加入 set 会怎样？

**答**：该队列有数据时 selector 不会被队列集唤醒。

### 10.8 队列集和事件组一样吗？

**答**：不一样。队列集选可读对象，事件组等待 bit 条件。

### 10.9 本课有没有中断？

**答**：没有，所有发送来自任务。

### 10.10 HAL 版队列集 API 是否不同？

**答**：不同点不在 API。HAL 版仍用 FreeRTOS 队列集 API。

## 11. 工程实现步骤

### 11.1 需求分析

用一个 selector 任务同时等待两个队列来源。

### 11.2 硬件核查

确认 PC13 和 PA1 可观察，PA0 不参与。

### 11.3 寄存器路线

配置 GPIO，创建两个队列和一个队列集，添加成员，创建两个 producer 和 selector。

### 11.4 HAL 路线

用 HAL 初始化硬件，队列集逻辑保持不变。

### 11.5 工程思维

当一个任务要等多个队列或信号量来源时，队列集比轮询多个对象更清晰。

### 11.6 常见工程陷阱

忘记添加成员、队列集容量太小、把返回成员当数据、选择后不接收、发送失败不检查，都是常见问题。

## 12. 运行现象

PC13 按 `prod_a` 的 700ms 来源翻转，PA1 按 `prod_b` 的 1100ms 来源翻转。两个节奏会不断错开。

可在 `xQueueSelectFromSet()` 返回后观察 `active` 是 `g_a` 还是 `g_b`。

## 13. 常见问题排查

### 13.1 PC13 不翻

查 `g_a` 是否创建成功、是否加入 set、`prod_a` 是否发送、selector 是否判断 `active == g_a`。

### 13.2 PA1 不翻

查 `g_b`、`prod_b`、`active == g_b` 分支和 PA1 GPIO。

### 13.3 selector 一直阻塞

说明没有成员通知到队列集。查 producer 是否运行，队列是否添加到 set。

### 13.4 选择后接收失败

检查是否从 active 对应的队列接收，是否误从另一个队列取数据。

### 13.5 创建失败

队列、队列集或任务创建都可能因 heap 不足失败。查看 NULL 句柄和 hook。

### 13.6 事件丢失

producer 发送等待为 0，队列满时会失败。应检查 `xQueueSend()` 返回值。

### 13.7 容量设置过小

队列集容量不足会导致成员通知不可靠。容量应覆盖所有成员最大积压总数。

### 13.8 HAL 版 GPIO 不动

先确认 selector 是否处理了对应成员，再查 HAL GPIO 初始化。

### 13.9 active 正确但 v 不对

检查 producer 是否发送了预期值，队列元素大小是否为 `sizeof(uint8_t)`，是否从正确队列接收。active 只能证明来源，不保证消息内容符合业务预期。

### 13.10 两个来源同时到达

如果 `g_a` 和 `g_b` 同时都有数据，selector 每次 select 返回其中一个可读成员，处理完后下一轮会继续返回另一个。不要期待一次 select 同时返回两个成员。

### 13.11 队列集里成员一直可读

如果 selector 选择后没有真正 `xQueueReceive()`，该成员会保持有数据状态，selector 可能反复被同一个成员唤醒。队列集使用规则要求选中后要从对应成员取走数据。

### 13.12 PC13 和 PA1 节奏互相影响吗

两个来源周期不同，但由同一个 selector 处理。若 selector 处理很快，它们基本各按各的 producer 周期表现；若 selector 里加入耗时操作，就可能让另一个来源积压。真实工程要避免 selector 里做长时间阻塞。

### 13.13 selector 优先级太低

如果把 selector 优先级降到 producer 以下，producer 可能更容易连续发送并填满队列。队列集仍能工作，但处理延迟和丢事件风险会增加。

### 13.14 添加信号量到队列集时要重新算容量

队列集也可以加入信号量，但容量计算要包含信号量可能产生的可读次数。计数信号量最大计数越大，占用的集合容量预算也越大。

## 14. 本课最核心的结论

1. 队列集让一个任务等待多个队列或信号量来源。
2. `xQueueSelectFromSet()` 返回成员句柄，不返回消息数据。
3. 选择后仍要到原队列 `xQueueReceive()`。
4. 队列集容量要覆盖成员最大积压。
5. PC13 对应 `g_a`，PA1 对应 `g_b`。
6. 队列集和事件组语义不同：队列集选“哪个对象可读”，事件组等“哪些 bit 到齐”。
7. 任务通知（55 课）适合更轻量的一对一通知；队列集适合“多个不同队列/信号量来源，需要知道具体来源”的场景。
8. reg/hal 差异在 GPIO，不在队列集机制。
9. 发送、添加成员和创建对象都应该检查返回值。

## 15. 建议你现在怎么读这节课

先画三个对象：`g_a`、`g_b`、`g_set`。再记住 selector 的三步：等 set、判断 active、读对应队列。最后用 PC13/PA1 对应两个来源。

## 16. 扩展练习

1. 把队列集容量改小，观察异常风险。
2. 检查 `xQueueAddToSet()` 返回值。
3. 增加第三个队列和第三个输出引脚。
4. 把 `prod_a` 周期改成 300ms，观察 PC13 更快。
5. 在 selector 中记录 `v` 的值，确认来源数据。

## 17. 下一课预告

- 上一课：[52_freertos_mutex_priority_inversion](../52_freertos_mutex_priority_inversion/README.md)
- 下一课：[54_freertos_event_group](../54_freertos_event_group/README.md)
