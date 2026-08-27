# 10_exti - 外部中断 EXTI

## 1. 本课到底在学什么

本课表面现象：PA0 接按键（另一端接 GND），每按一次，PC13 板载 LED 翻转一次。

真正要学的是外部中断链路。一个引脚上的电平变化，不是自动就能让 CPU 执行函数；它要先进入 GPIO 输入电路，再通过 AFIO 选择连接到哪条 EXTI 线，再由 EXTI 判断边沿、置 pending 标志、向 NVIC 发请求，最后 CPU 才跳进 `EXTI0_IRQHandler()`。

这节课接在 GPIO 按键和定时器课程之后。前面的按键读取是"主循环反复问 PA0 现在是不是低电平"；本课换成"PA0 出现下降沿时硬件主动打断 CPU"。这是后续学习中断、输入捕获、串口中断、FreeRTOS 中断交互的基础。

## 2. 本课学习目标

学完本课，你应该能做到：

- 解释 PA0 按键接 GND 时为什么要配置内部上拉
- 说明按下按键为什么是下降沿
- 说清楚 GPIO 输入、AFIO 映射、EXTI 线、NVIC、ISR 各自负责哪一步
- 解释为什么 EXTI0 同一时间只能来自 PA0/PB0/PC0 等同编号引脚中的一个
- 看懂 `EXTI->IMR`、`FTSR`、`RTSR`、`PR` 的不同作用
- 解释 `EXTI->PR = EXTI_PR_PR0` 为什么是写 1 清 pending
- 说明 HAL 版 `GPIO_MODE_IT_FALLING` 底层封装了哪些寄存器配置
- 解释 `EXTI0_IRQHandler()` → `HAL_GPIO_EXTI_IRQHandler()` → `HAL_GPIO_EXTI_Callback()` 三者调用关系

## 3. 本课目录结构

```text
10_exti/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

## 4. 实验硬件

- STM32F103C8T6 BluePill
- PA0 外接按键，按键另一端接 GND
- PC13 板载 LED

PA0 使用内部上拉。未按下时 PA0 保持高电平；按下时 PA0 被接到 GND，变成低电平。因此"按下"对应下降沿。

PC13 板载 LED 常见接法是低电平点亮，代码初始化时先写高电平让 LED 熄灭。

## 5. 先建立一个最基本的脑图

完整链路：

1. 系统时钟配置为 72MHz
2. GPIOC 时钟打开，PC13 配成推挽输出，初始写高电平
3. GPIOA 时钟打开，PA0 配成输入上拉
4. AFIO 时钟打开，`AFIO->EXTICR[0]` 把 EXTI0 来源选为 PA0
5. `EXTI->IMR` 允许 EXTI0 产生中断请求
6. `EXTI->FTSR` 打开下降沿触发，`EXTI->RTSR` 关闭上升沿
7. `EXTI->PR` 先清一次残留 pending
8. NVIC 设置 `EXTI0_IRQn` 优先级并使能
9. 按键按下 → PA0 下降沿 → EXTI0 置 pending → NVIC 发中断请求
10. CPU 进入 `EXTI0_IRQHandler()` → 清 `PR0` → 翻转 PC13

## 6. 核心名词速查

### 6.1 EXTI 中断链路

```text
按键按下 → PA0 下降沿 → AFIO 映射到 EXTI0 → EXTI 边沿检测 → 置 PR0 pending
→ NVIC 请求中断 → CPU 跳入 EXTI0_IRQHandler() → 清 PR0 → 翻转 PC13
```

### 6.2 EXTI0 引脚映射规则

| EXTI 线 | 可选引脚 | 选择寄存器 |
|---|---|---|
| EXTI0 | PA0 / PB0 / PC0 / ... | `AFIO->EXTICR[0]` |
| EXTI1 | PA1 / PB1 / PC1 / ... | `AFIO->EXTICR[0]` |
| EXTI5 | PA5 / PB5 / PC5 / ... | `AFIO->EXTICR[1]` |

同编号引脚同一时间只能选一个。EXTI0 选了 PA0，PB0 的信号就进不了 EXTI0。

### 6.3 本课核心寄存器

| 寄存器/位 | 作用 | 本课值 | 关键规则 |
|---|---|---|---|
| `AFIO->EXTICR[0]` | EXTI0 来源选择 | PA0 (0000) | 不开 AFIO 时钟则写入无效 |
| `EXTI->IMR` bit0 | 放行 EXTI0 中断 | 开 | 不开则边沿检测后不向 NVIC 请求 |
| `EXTI->FTSR` bit0 | 下降沿触发 | 开 | 按键按下时 PA0 高→低 |
| `EXTI->RTSR` bit0 | 上升沿触发 | 关 | 不需要检测松开 |
| `EXTI->PR` bit0 | pending 标志 | 写 1 清除 | 读到 1 表示有挂起中断，写 1 清除 |
| `NVIC` | 中断使能和优先级 | EXTI0_IRQn | 不使能则 CPU 不响应 |

### 6.4 HAL 对应关系

| HAL | 寄存器 |
|---|---|
| `GPIO_MODE_IT_FALLING` | 输入上拉 + AFIO 映射 + IMR + FTSR |
| `GPIO_PULLUP` | `GPIOA->BSRR` 让 ODR0=1 |
| `HAL_GPIO_Init()` | 一次性配好 GPIO + AFIO + EXTI |
| `HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0)` | `NVIC_SetPriority()` |
| `HAL_NVIC_EnableIRQ(EXTI0_IRQn)` | `NVIC_EnableIRQ()` |
| `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)` | 读 PR0 → 清 PR0 → 调 Callback |
| `HAL_GPIO_EXTI_Callback(GPIO_Pin)` | 用户写业务逻辑的位置 |

### 6.5 HAL 中断调用链

```text
EXTI0_IRQHandler()          ← CPU 中断入口
  → HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)   ← HAL 中断处理
    → 清除 PR0
    → HAL_GPIO_EXTI_Callback(GPIO_Pin)     ← 用户回调
```

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 完整逻辑

```c
int main(void)
{
    system_clock_72mhz_init();
    led_init();
    exti_init();

    while (1) {
        // 主循环空闲，LED 翻转由中断完成
    }
}

void EXTI0_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR0) {
        EXTI->PR = EXTI_PR_PR0;    // 写 1 清 pending
        GPIOC->ODR ^= GPIO_ODR_ODR13;  // 翻转 PC13
    }
}
```

### 7.2 `exti_init()` 关键步骤

| 步骤 | 代码 | 作用 |
|---|---|---|
| 1 | `RCC->APB2ENR \|= IOPAEN \| IOPCEN \| AFIOEN` | 开 GPIOA + GPIOC + AFIO 时钟 |
| 2 | `GPIOA->CRL` 配 MODE0=00, CNF0=10 | PA0 输入上拉 |
| 3 | `GPIOA->BSRR = 1<<0` | ODR0=1，选择上拉 |
| 4 | `GPIOC->CRH` 配 MODE13=10, CNF13=00 | PC13 推挽输出 |
| 5 | `GPIOC->BSRR = 1<<13` | PC13 初始高电平（LED 灭） |
| 6 | `AFIO->EXTICR[0] = 0x0000` | EXTI0 来源选 PA0 |
| 7 | `EXTI->IMR \|= 1<<0` | 放行 EXTI0 中断 |
| 8 | `EXTI->FTSR \|= 1<<0` | 下降沿触发 |
| 9 | `EXTI->PR = 1<<0` | 清残留 pending |
| 10 | `NVIC_SetPriority(EXTI0_IRQn, 0)` | 优先级 |
| 11 | `NVIC_EnableIRQ(EXTI0_IRQn)` | 使能中断 |

### 7.3 ISR 关键操作

```c
EXTI->PR = EXTI_PR_PR0;    // 写 1 清 pending（不是写 0）
GPIOC->ODR ^= GPIO_ODR_ODR13;  // 翻转 LED
```

`PR` 是写 1 清除寄存器。写 0 无效，写 1 清除对应位的 pending 标志。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 完整逻辑

```c
HAL_Init();
system_clock_72mhz_init();
led_init();
exti_init();

while (1) {
    // 主循环空闲
}

void EXTI0_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0) {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    }
}
```

### 8.2 HAL 版关键步骤

| 步骤 | HAL 代码 | 对应寄存器 |
|---|---|---|
| 配 PA0 | `GPIO_MODE_IT_FALLING, GPIO_PULLUP` | 输入上拉 + AFIO + IMR + FTSR |
| 配 PC13 | `GPIO_MODE_OUTPUT_PP` | 推挽输出 |
| 中断优先级 | `HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0)` | NVIC 优先级 |
| 使能中断 | `HAL_NVIC_EnableIRQ(EXTI0_IRQn)` | NVIC 使能 |
| 中断入口 | `EXTI0_IRQHandler()` | CPU 中断向量 |
| HAL 处理 | `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)` | 读/清 PR0 |
| 用户回调 | `HAL_GPIO_EXTI_Callback()` | 业务逻辑 |

## 9. 两个版本的学习策略

寄存器版抓链路：PA0 下降沿 → AFIO 映射 → EXTI 边沿检测 → PR0 pending → NVIC → ISR → 清 PR → 翻转 LED。

HAL 版抓封装：`GPIO_MODE_IT_FALLING` 一行代码做了"输入 + 上拉 + AFIO 映射 + IMR + FTSR"五件事。`HAL_GPIO_EXTI_IRQHandler()` 做了"读 PR → 清 PR → 调 Callback"三件事。

## 10. 检验问题清单

### 10.1 PA0 按键接 GND，为什么要配内部上拉？

没有上拉时 PA0 悬空，电平不确定。上拉让未按下时 PA0 保持高电平，按下时才确定变低。

### 10.2 按下按键为什么是下降沿？

未按下时 PA0 被上拉为高，按下时 PA0 被接到 GND 变低。高→低就是下降沿。

### 10.3 EXTI0 为什么只能来自 PA0/PB0/PC0 等？

EXTI 线按引脚编号分配。EXTI0 对应所有 GPIO 的 0 号引脚，同一时间只能选一个。

### 10.4 `EXTI->PR = EXTI_PR_PR0` 为什么是写 1 清除？

这是硬件设计。PR 是写 1 清零（w1c）寄存器，写 0 无效，写 1 清除对应位。

### 10.5 不开 AFIO 时钟会怎样？

`AFIO->EXTICR[]` 写入无效，EXTI0 来源保持默认（PA0），但如果想选 PB0/PC0 就做不到。

### 10.6 HAL 版 `GPIO_MODE_IT_FALLING` 封装了什么？

输入模式 + 上拉 + AFIO 映射 + EXTI IMR 放行 + FTSR 下降沿触发。

### 10.7 `HAL_GPIO_EXTI_Callback()` 和 `EXTI0_IRQHandler()` 的关系？

`EXTI0_IRQHandler()` 是 CPU 中断入口，它调用 `HAL_GPIO_EXTI_IRQHandler()` 清除标志，后者再调用 `HAL_GPIO_EXTI_Callback()` 执行用户逻辑。

### 10.8 一次按键按下为什么可能触发多次中断？

机械按键有抖动，按下瞬间触点反复弹跳，产生多个下降沿，每次都触发一次中断。

## 11. 工程实现步骤

### 11.1 寄存器路线

1. 配置系统时钟到 72MHz
2. 打开 GPIOA、GPIOC、AFIO 时钟
3. PA0 配成输入上拉，PC13 配成推挽输出（初始高电平）
4. `AFIO->EXTICR[0]` 选择 EXTI0 来源为 PA0
5. `EXTI->IMR` 放行 EXTI0，`EXTI->FTSR` 下降沿触发
6. `EXTI->PR` 清残留 pending
7. NVIC 使能 `EXTI0_IRQn`
8. 在 `EXTI0_IRQHandler()` 中清 PR0 并翻转 PC13

### 11.2 HAL 路线

1. `HAL_Init()`
2. 配置系统时钟
3. PA0 配成 `GPIO_MODE_IT_FALLING` + `GPIO_PULLUP`
4. PC13 配成 `GPIO_MODE_OUTPUT_PP`
5. `HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0)`
6. `HAL_NVIC_EnableIRQ(EXTI0_IRQn)`
7. 在 `EXTI0_IRQHandler()` 中调用 `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)`
8. 在 `HAL_GPIO_EXTI_Callback()` 中翻转 PC13

### 11.3 常见工程陷阱

- 不开 AFIO 时钟：EXTICR 写入无效，EXTI0 来源选不对
- 不清 PR：中断处理完不清 pending，会一直重复进中断
- PR 写 0 清除：PR 是 w1c 寄存器，写 0 无效
- 按键抖动：一次按下触发多次中断，需要硬件消抖或软件防抖
- HAL 版忘记写 `HAL_GPIO_EXTI_Callback()`：中断进去了但没业务逻辑
- HAL 版 `EXTI0_IRQHandler()` 里不调 `HAL_GPIO_EXTI_IRQHandler()`：Callback 永远不会被调用

## 12. 运行现象

每按一次 PA0 按键，PC13 LED 翻转一次。由于机械抖动，一次物理按下可能触发多次翻转。

## 13. 常见问题排查

### 13.1 按键无反应

1. 硬件接线：PA0 接按键接 GND
2. AFIO 时钟是否打开
3. EXTI IMR 和 FTSR 是否配置
4. NVIC 是否使能 EXTI0_IRQn
5. ISR 是否正确定义（函数名必须精确匹配）

### 13.2 一直进中断

PR 没有清除，或清除方式错误（写 0 而非写 1）。

### 13.3 一次按下触发多次

机械按键抖动。解决方案：硬件加 RC 滤波，或软件在 ISR 里延时/定时器消抖。

### 13.4 HAL 版 Callback 不被调用

检查 `EXTI0_IRQHandler()` 是否调用了 `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)`。

### 13.5 PA0 选了但 EXTI0 来源不对

检查 `AFIO->EXTICR[0]` 的值，确认 AFIO 时钟已打开。

## 14. 本课最核心的结论

EXTI 的本质：把引脚电平边沿变成 CPU 中断请求。

- AFIO 映射：选择哪个 GPIO 引脚连接到哪条 EXTI 线
- EXTI 边沿检测：IMR 放行 + FTSR/RTSR 选择触发边沿
- PR pending：边沿匹配后置位，写 1 清除
- NVIC：使能和优先级管理
- ISR：清 PR + 执行业务逻辑

## 15. 建议你现在怎么读这节课

先跟链路：按键按下 → PA0 下降沿 → AFIO → EXTI → PR → NVIC → ISR → 清 PR → 翻转 LED。

再跟寄存器：每个环节对应哪个寄存器的哪个位。

最后看 HAL 版：`GPIO_MODE_IT_FALLING` 做了什么，`HAL_GPIO_EXTI_IRQHandler()` 做了什么，Callback 在哪里写业务。

## 16. 扩展练习

1. 把触发方式改成上升沿，观察松开按键时才翻转
2. 同时使能上升沿和下降沿，观察按下和松开各翻转一次
3. 在 ISR 里加简单防抖：清 PR 后延时 10ms 再翻转 LED
4. 用 TIM2 做定时器消抖：EXTI ISR 只置标志，TIM2 中断里检查标志并消抖
5. 把按键改到 PB0，修改 AFIO 映射

## 17. 下一课预告

下一课将基于 EXTI 基础，进一步学习中断优先级分组、嵌套中断等 NVIC 进阶内容。