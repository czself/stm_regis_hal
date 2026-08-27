# 07_timer_output_compare - TIM 输出比较

## 1. 本课到底在学什么

本课表面现象：PA0 引脚不靠主循环写 GPIO，而是由 TIM2_CH1 自动输出翻转电平；PC13 仍然在主循环里闪烁，只用来证明程序还在运行。

真正要学的是 STM32 定时器的输出比较功能。上一课 TIM2 溢出后产生中断，CPU 进入中断函数再翻转 LED。本课换成另一条链路：`CNT` 自己往上数，当 `CNT == CCR1` 时，定时器内部产生比较匹配事件，按 `OC1M` 配置直接改变 TIM2_CH1 的输出状态。

这就把"定时"和"输出动作"都放进了定时器硬件内部。CPU 只负责初始化；初始化完成后，PA0 的边沿由 TIM2 外设自动产生。

核心一句话：

```text
CNT 提供时间轴，CCR1 提供比较点，OC1M 决定匹配时怎么改输出，CC1E 决定这个输出能不能送到 PA0。
```

## 2. 本课学习目标

学完本课，你应该能做到：

- 解释为什么 `CNT == CCR1` 时 PA0 会发生电平翻转
- 说清楚 `PSC`、`ARR`、`CNT`、`CCR1` 在时间轴里的分工
- 解释为什么 PA0 必须配置为复用推挽输出，而不是普通 GPIO 输出
- 看懂 `CCMR1.OC1M = 011` 为什么表示输出比较 toggle 模式
- 看懂 `CCER.CC1E` 为什么是"通道输出开关"
- 区分定时器更新中断和定时器输出比较：前者让 CPU 做事，后者让定时器通道自己改输出
- 把 HAL 版的 `TIM_OC_InitTypeDef` 字段反推回寄存器版

## 3. 本课目录结构

```text
07_timer_output_compare/
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
- ST-Link 下载器
- PA0 引脚（TIM2_CH1 默认复用引脚，本课没有重映射）
- PC13 板载 LED（心跳灯）
- 可选：VOFA+ 观察 PA0 波形

PC13 闪只能说明主循环在跑；PA0 有没有波形，要看 TIM2、PA0 复用、通道输出是否配置正确。

### 4.1 用 VOFA+ 观察 PA0 波形

没有示波器时，用 VOFA+ 通过串口观察 PA0 电平变化。方法：

1. 用杜邦线把 PA0 连到另一个 GPIO 输入引脚（如 PA1）
2. PA1 配成输入模式，主循环里读 `GPIOA->IDR & 0x0002` 获取 PA1 状态
3. 通过 UART 把状态值发给 VOFA+，选 JustFloat 或 FireWater 协议
4. VOFA+ 上能看到 0/1 跳变的方波

本课 PA0 翻转很慢（约 1 秒一次），所以串口波特率 115200 完全够用，采样间隔 50~100ms 就能看到清晰的跳变沿。

## 5. 先建立一个最基本的脑图

完整执行链路：

1. 系统时钟配置到 72MHz
2. RCC 打开 GPIOA、AFIO、TIM2 时钟
3. PA0 配成复用推挽输出，让 TIM2_CH1 能驱动引脚
4. TIM2 设置 `PSC = 7200 - 1`，得到 10kHz 计数频率
5. TIM2 设置 `ARR = 10000 - 1`，计数器每 1 秒从 0 走到 9999 再更新
6. TIM2 设置 `CCR1 = 5000`，通道 1 在计数到 5000 时产生比较匹配
7. TIM2 设置 `OC1M = 011`，比较匹配时翻转通道输出
8. TIM2 设置 `CC1E = 1`，允许通道 1 的输出送到外部引脚
9. TIM2 设置 `CEN = 1`，计数器开始运行
10. 之后每当 `CNT == CCR1`，TIM2 硬件自动翻转 PA0

注意：本课没有在匹配后修改 `CCR1`，所以每个 `ARR` 周期里只在 `CNT = 5000` 这一点翻转一次。`ARR` 周期是 1 秒，PA0 每 1 秒翻转一次电平，完整高低电平周期约 2 秒。

## 6. 核心名词速查

### 6.1 输出比较核心概念

输出比较 = 定时器通道把 `CNT` 和 `CCR` 比较，在匹配时按预设规则改变通道输出或产生事件。

上一课的更新中断是"时间到了通知 CPU"。输出比较是"时间到了通道自己做输出动作"。本课选择的输出动作是 toggle（翻转）。

```text
CNT < CCR1  → 正常计数
CNT == CCR1 → 比较匹配，按 OC1M 执行动作
CNT == ARR  → 更新事件，CNT 回 0
```

### 6.2 本课新增寄存器

| 寄存器/位 | 作用 | 本课值 | 关键规则 |
|---|---|---|---|
| `CCR1` | 通道 1 比较值 | 5000 | CNT==CCR1 时产生比较匹配 |
| `CCMR1.OC1M` | 输出比较模式 | 011 (toggle) | 决定匹配时做什么动作 |
| `CCER.CC1E` | 通道 1 输出使能 | 开 | 不开则内部匹配但引脚无输出 |
| `EGR.UG` | 软件产生更新事件 | 触发一次 | 让 PSC/ARR 预装载同步生效 |

上一课的 PSC/ARR/CNT/CEN 本课继续使用，不再重复解释。

### 6.3 PA0 复用推挽

PA0 不是普通输出口。本课 PA0 要输出 TIM2_CH1 信号，必须配成复用推挽输出：

```text
MODE0 = 10 (2MHz 输出) + CNF0 = 10 (复用推挽) → PA0 输出来源切换到 TIM2_CH1
```

如果配成普通推挽，PA0 听 `GPIOA->ODR`，TIM2_CH1 信号到不了引脚。

### 6.4 HAL 对应关系

| HAL | 寄存器 |
|---|---|
| `GPIO_MODE_AF_PP` | `CNF0=10` 复用推挽 |
| `HAL_TIM_OC_Init()` | 写入 PSC/ARR/CR1 等 |
| `oc.OCMode = TIM_OCMODE_TOGGLE` | `OC1M = 011` |
| `oc.Pulse = 5000` | `TIM2->CCR1 = 5000` |
| `oc.OCPolarity = TIM_OCPOLARITY_HIGH` | 输出极性 |
| `HAL_TIM_OC_ConfigChannel(&htim2, &oc, TIM_CHANNEL_1)` | 写 CCR1/OC1M/极性到 CH1 |
| `HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_1)` | 开 CC1E + CEN |

通道号必须匹配：PA0 默认接 TIM2_CH1，HAL 里用 `TIM_CHANNEL_1`。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 完整逻辑

```c
int main(void)
{
    system_clock_72mhz_init();
    pc13_led_init();
    tim2_oc_init();

    while (1) {
        pc13_toggle();
        delay_cycles(3600000U);
    }
}
```

主循环只翻转 PC13 心跳灯，不参与 PA0 边沿。

### 7.2 `tim2_oc_init()` 关键步骤

| 步骤 | 代码 | 作用 |
|---|---|---|
| 1 | `RCC->APB2ENR \|= IOPAEN \| AFIOEN` | 开 GPIOA + AFIO 时钟 |
| 2 | `RCC->APB1ENR \|= TIM2EN` | 开 TIM2 时钟 |
| 3 | `GPIOA->CRL` 配 MODE0=10, CNF0=10 | PA0 复用推挽 |
| 4 | `TIM2->PSC = 7200-1` | 72MHz/7200 = 10kHz |
| 5 | `TIM2->ARR = 10000-1` | 10kHz 下 10000 tick = 1s |
| 6 | `TIM2->CCR1 = 5000` | 比较点在 0.5s 位置 |
| 7 | `CCMR1 &= ~OC1M` | 清旧模式 |
| 8 | `CCMR1 \|= OC1M_0 \| OC1M_1` | OC1M=011 toggle |
| 9 | `CCER \|= CC1E` | 打开通道 1 输出 |
| 10 | `EGR = UG` | 同步预装载 |
| 11 | `CR1 \|= CEN` | 启动计数器 |

步骤 7 是"先清再设"的典型寄存器写法：多 bit 字段不能只靠 `|=` 叠加，旧值可能残留导致模式编码错误。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 完整逻辑

```c
HAL_Init();
system_clock_72mhz_init();
tim2_oc_init();
HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_1);
while (1) {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    HAL_Delay(500);
}
```

和寄存器版的区别：`tim2_oc_init()` 只做配置，启动由 `HAL_TIM_OC_Start()` 完成。

### 8.2 HAL 版关键步骤

| 步骤 | HAL 代码 | 对应寄存器 |
|---|---|---|
| 开时钟 | `__HAL_RCC_GPIOA_CLK_ENABLE()` + `__HAL_RCC_TIM2_CLK_ENABLE()` | IOPAEN + TIM2EN |
| 配 PA0 | `GPIO_MODE_AF_PP` | CNF0=10 复用推挽 |
| 配时基 | `Prescaler=7199, Period=9999` | PSC + ARR |
| 初始化 | `HAL_TIM_OC_Init(&htim2)` | 写入时基寄存器 |
| 配通道 | `OCMode=TOGGLE, Pulse=5000` | OC1M + CCR1 |
| 落地通道 | `HAL_TIM_OC_ConfigChannel(&htim2, &oc, TIM_CHANNEL_1)` | 写 CCR1/OC1M/极性 |
| 启动 | `HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_1)` | CC1E + CEN |

HAL 版最常见问题：只配置通道不调用 Start，PA0 不出波形。

## 9. 两个版本的学习策略

寄存器版盯住四个层次：

1. **时钟**：GPIOA、AFIO、TIM2 时钟有没有打开
2. **引脚**：PA0 是否复用推挽输出
3. **时基**：PSC 和 ARR 决定 CNT 怎么跑
4. **通道**：CCR1 决定匹配点，OC1M 决定匹配动作，CC1E 决定输出是否打开

HAL 版把字段翻译回硬件：`Prescaler`→PSC，`Period`→ARR，`Pulse`→CCR1，`OCMode=TOGGLE`→OC1M=011，`Start`→CC1E+CEN。

## 10. 检验问题清单

### 10.1 PA0 为什么必须配置成复用推挽输出？

PA0 要输出 TIM2_CH1 信号，不是普通 GPIO 输出值。复用功能让引脚输出来源切换到片上外设。

### 10.2 `CCR1 = 5000` 表示每 0.5 秒翻转一次吗？

不准确。它表示在每个计数周期中 CNT 走到 5000 时翻转一次。本课 ARR=9999，一轮 1 秒，每轮只翻转一次，完整高低周期约 2 秒。

### 10.3 只设置 `OC1M`，不设置 `CC1E`，PA0 会有波形吗？

不会。OC1M 只决定匹配动作，CC1E 才打开通道 1 输出。内部事件和外部引脚输出不是同一个开关。

### 10.4 `PSC` 和 `ARR` 谁决定比较点？

PSC 和 ARR 决定时间轴和计数周期，CCR1 决定比较点。

### 10.5 HAL 里的 `Pulse` 对应哪个寄存器？

`oc.Pulse = 5000` 对应 `TIM2->CCR1 = 5000U`。

### 10.6 HAL 里的 `TIM_OCMODE_TOGGLE` 对应哪个寄存器字段？

对应 `CCMR1` 里的 `OC1M` 字段。

### 10.7 为什么本课不需要 TIM2 中断？

PA0 的翻转由 TIM2_CH1 输出比较硬件完成，CPU 不需要在每次匹配时进入中断函数。

### 10.8 PC13 正常闪烁，能证明 PA0 一定正常吗？

不能。PC13 只证明主循环和 GPIOC 正常。PA0 还依赖 GPIOA 复用模式、TIM2 时钟、输出比较模式、通道输出使能等。

## 11. 工程实现步骤

### 11.1 寄存器路线

1. 配置系统时钟到 72MHz
2. 打开 GPIOA、AFIO、TIM2 时钟
3. 配置 PA0 为复用推挽输出
4. 写 `TIM2->PSC = 7200 - 1`
5. 写 `TIM2->ARR = 10000 - 1`
6. 写 `TIM2->CCR1 = 5000`
7. 清 `CCMR1.OC1M` 旧值
8. 设置 `OC1M = 011` toggle
9. 设置 `CCER.CC1E`
10. 写 `EGR.UG`
11. 设置 `CR1.CEN`

### 11.2 HAL 路线

1. `HAL_Init()`
2. 配置系统时钟
3. `__HAL_RCC_GPIOA_CLK_ENABLE()` 和 `__HAL_RCC_TIM2_CLK_ENABLE()`
4. PA0 配成 `GPIO_MODE_AF_PP`
5. `htim2.Instance = TIM2`，设置 `Prescaler`/`Period`/`CounterMode`
6. `HAL_TIM_OC_Init(&htim2)`
7. `TIM_OC_InitTypeDef` 设置 `OCMode`/`Pulse`/`OCPolarity`
8. `HAL_TIM_OC_ConfigChannel(&htim2, &oc, TIM_CHANNEL_1)`
9. `HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_1)`

### 11.3 常见工程陷阱

- 把 PA0 当普通 GPIO：还在想用 `GPIOA->BSRR` 写 PA0，就没抓住输出比较的核心
- 忘记 `CC1E`：模式配置和输出使能是两道门，少一道都不行
- 把 `CCR1` 理解成周期：CCR1 是比较点，周期由 ARR 和计数频率决定
- HAL 配置了通道但没调用 `HAL_TIM_OC_Start()`
- 通道和引脚不匹配：PA0 默认是 TIM2_CH1，不是任意通道

## 12. 运行现象

PA0 大约每 1 秒翻转一次电平，完整高低周期约 2 秒。PC13 由主循环延时控制闪烁。

## 13. 常见问题排查

### 13.1 PA0 完全没有波形

1. GPIOA 时钟是否打开
2. PA0 是否配置为复用推挽输出
3. TIM2_CH1 是否通过 `CC1E` 打开输出

### 13.2 PA0 一直高或一直低

1. `OC1M` 是否真的是 toggle（011）
2. `CCR1` 是否在 `ARR` 有效范围内

### 13.3 波形频率不符合预期

重新计算：`TIM2 计数频率 = TIM2 时钟 / (PSC+1)`，`计数周期 = (ARR+1) / 计数频率`。

### 13.4 HAL 版没有输出

1. 是否调用 `__HAL_RCC_TIM2_CLK_ENABLE()`
2. PA0 是否 `GPIO_MODE_AF_PP`
3. 是否调用 `HAL_TIM_OC_Start()`

### 13.5 PC13 闪烁正常但 PA0 不动

主循环和 GPIOC 没问题，但 TIM2 输出比较链路有问题。优先查 PA0 复用推挽、TIM2 时钟、OC1M、CC1E、CEN。

## 14. 本课最核心的结论

输出比较不是 CPU 延时翻转 GPIO，而是定时器硬件在 `CNT == CCR1` 时按通道模式改变输出。

- `PSC` 决定 CNT 计数速度
- `ARR` 决定一轮计数周期
- `CCR1` 决定通道 1 的比较点
- `OC1M` 决定匹配时执行 toggle
- `CC1E` 决定 TIM2_CH1 输出是否打开
- PA0 的复用推挽模式决定通道信号能否到达引脚

## 15. 建议你现在怎么读这节课

先抓住硬件句子：

```text
TIM2 的 CNT 在跑，跑到 CCR1 时，CH1 根据 OC1M 翻转输出，CC1E 允许输出到 PA0。
```

然后回寄存器版代码按顺序找：谁让 TIM2 有时钟 → 谁让 PA0 接到 TIM2_CH1 → 谁让 CNT 以 10kHz 跑 → 谁规定一轮是 1 秒 → 谁规定 0.5 秒位置比较 → 谁规定比较时翻转 → 谁打开通道输出。

最后看 HAL 版，把 `Prescaler`/`Period`/`Pulse`/`OCMode`/`Start` 翻译回对应寄存器。

## 16. 扩展练习

1. 把 `CCR1` 改成 2500，观察 PA0 在每轮更早的位置翻转
2. 把 `ARR` 改成 `5000 - 1`，重新计算 PA0 翻转间隔
3. 注释掉 `TIM2->CCER |= TIM_CCER_CC1E`，观察内部配置完整但外部无输出
4. HAL 版注释掉 `HAL_TIM_OC_Start()`，观察只配置不启动的现象
5. 把 `TIM_OCMODE_TOGGLE` 改成其他输出比较模式，对比 PA0 波形变化

## 17. 下一课预告

下一课：[08_pwm_basic](../08_pwm_basic/README.md)

输出模式从"比较匹配翻转"变成 PWM。`ARR` 会决定 PWM 周期，`CCR1` 会决定占空比，PA0 上看到的就不再只是每次匹配翻转，而是周期性高低电平脉冲。