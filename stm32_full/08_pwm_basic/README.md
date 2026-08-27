# 08_pwm_basic - PWM 基础

## 1. 本课到底在学什么

本课表面现象：PA0 输出 1kHz PWM，外接 LED 亮度从暗到亮、再从亮到暗逐级变化。

真正要学的是 PWM 的硬件生成链路。PWM 不是主循环用延时反复拉高拉低 PA0，而是 TIM2_CH1 在每个计数周期里自动比较 `CNT` 和 `CCR1`，按 PWM mode 1 的规则决定输出高电平还是低电平。

本课和上一课是连续的。上一课是 `CNT == CCR1` 时翻转一次输出；本课是 `CNT < CCR1` 时保持有效电平，`CNT >= CCR1` 后变为无效电平。输出比较从"匹配点触发一个动作"变成了"比较值决定整个周期内高低电平比例"。

注意：本课不是固定 25% 占空比演示。初始化时 `CCR1 = 250`，但主循环会不断把 `CCR1` 改成 `duty`，所以你看到的是 LED 亮度阶梯式变化。

## 2. 本课学习目标

学完本课，你应该能做到：

- 根据 `PSC = 72 - 1` 和 `ARR = 1000 - 1` 算出 PWM 频率是 1kHz
- 根据 `CCR1 / (ARR + 1)` 估算 PWM 占空比
- 解释 PWM mode 1 中 `CNT < CCR1` 和 `CNT >= CCR1` 时输出状态为什么不同
- 解释 `OC1PE` 为什么适合运行中修改 `CCR1`
- 区分 PWM 频率、占空比、亮度变化速度三件事

## 3. 本课目录结构

```text
08_pwm_basic/
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
- PA0 引脚 + 220Ω 限流电阻 + LED
- 推荐接法：PA0 → 电阻 → LED 正极 → LED 负极 → GND
- 可选：示波器或逻辑分析仪

这种接法下占空比越大越亮。如果 LED 接 VCC 由 PA0 下拉，亮度直觉会反相。

## 5. 先建立一个最基本的脑图

完整链路：

1. 系统时钟配置到 72MHz
2. 打开 GPIOA、AFIO、TIM2 时钟
3. PA0 配置成复用推挽输出
4. `PSC = 72 - 1`，TIM2 计数频率变为 1MHz
5. `ARR = 1000 - 1`，每 1000 个计数形成一个 PWM 周期 = 1kHz
6. `CCR1 = 250`，初始占空比约 25%
7. `OC1M = 110`，选择 PWM mode 1
8. `OC1PE = 1`，运行中修改 `CCR1` 时使用预装载
9. `CC1E = 1`，TIM2_CH1 输出打开
10. `CEN = 1`，TIM2 开始计数
11. 主循环不断写 `CCR1`，占空比随 `duty` 从 0 到 1000 再回到 0

## 6. 核心名词速查

### 6.1 PWM 核心公式

```text
PWM 频率 = TIM2 计数频率 / (ARR + 1)
占空比 ≈ CCR1 / (ARR + 1)    （PWM mode 1，有效高）
```

本课：`72MHz / 72 / 1000 = 1kHz`，`CCR1=250` 时占空比约 25%。

### 6.2 PWM mode 1 vs 上一课 toggle

| 模式 | OC1M | 行为 |
|---|---|---|
| toggle（上一课） | 011 | CNT==CCR1 时翻转一次 |
| PWM mode 1（本课） | 110 | CNT < CCR1 输出有效，CNT >= CCR1 输出无效 |

PWM mode 1 让 CCR1 从"比较点"变成"占空比控制值"。

### 6.3 本课新增/变化寄存器

| 寄存器/位 | 作用 | 本课值 | 关键规则 |
|---|---|---|---|
| `PSC` | 预分频 | 71 | 72MHz/72 = 1MHz（上一课是 7199→10kHz） |
| `ARR` | 自动重装载 | 999 | 1MHz/1000 = 1kHz PWM |
| `CCR1` | 比较值 | 初始 250，运行中改 | 改 CCR1 = 改占空比 |
| `OC1M` | 输出比较模式 | 110 (PWM1) | 上一课 011 (toggle) |
| `OC1PE` | 预装载使能 | 开 | 运行中改 CCR1 时波形更稳定 |
| `CC1E` | 通道输出使能 | 开 | 不开则引脚无 PWM |

### 6.4 HAL 对应关系

| HAL | 寄存器 |
|---|---|
| `HAL_TIM_PWM_Init()` | 写入 PSC/ARR/CR1 等 |
| `sConfigOC.OCMode = TIM_OCMODE_PWM1` | `OC1M = 110` |
| `sConfigOC.Pulse = 250` | `TIM2->CCR1 = 250` |
| `HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1)` | 写 CCR1/OC1M/极性到 CH1 |
| `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)` | 开 CC1E + CEN |
| `__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, duty)` | `TIM2->CCR1 = duty` |

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 完整逻辑

```c
int main(void)
{
    system_clock_72mhz_init();
    pwm_init();

    while (1) {
        TIM2->CCR1 = duty;
        delay(180000U);
        // duty 和 step 更新逻辑...
    }
}
```

`delay()` 只控制亮度变化速度，不生成 PWM。PWM 由 TIM2 硬件以 1kHz 输出。

### 7.2 `pwm_init()` 关键步骤

| 步骤 | 代码 | 作用 |
|---|---|---|
| 1 | `RCC->APB2ENR \|= IOPAEN \| AFIOEN` | 开 GPIOA + AFIO 时钟 |
| 2 | `RCC->APB1ENR \|= TIM2EN` | 开 TIM2 时钟 |
| 3 | `GPIOA->CRL` 配 MODE0=10, CNF0=10 | PA0 复用推挽 |
| 4 | `TIM2->PSC = 72-1` | 72MHz/72 = 1MHz |
| 5 | `TIM2->ARR = 1000-1` | 1MHz/1000 = 1kHz PWM |
| 6 | `TIM2->CCR1 = 250` | 初始占空比约 25% |
| 7 | `CCMR1 &= ~(CC1S \| OC1M)` | 清旧模式 |
| 8 | `CCMR1 \|= OC1M_1 \| OC1M_2` | OC1M=110 PWM mode 1 |
| 9 | `CCMR1 \|= OC1PE` | 打开预装载 |
| 10 | `CCER \|= CC1E` | 打开通道 1 输出 |
| 11 | `EGR \|= UG` | 同步预装载 |
| 12 | `CR1 \|= CEN` | 启动计数器 |

步骤 9 的 `OC1PE`：主循环不断改 `CCR1`，打开预装载让新比较值在更新事件后生效，避免周期中间改值产生不规则边沿。

### 7.3 主循环占空比更新

```c
TIM2->CCR1 = duty;
```

`duty` 从 0 到 1000，步进 50。到边界后 `step` 取反，方向反转。这只是软件策略，不改变 PWM 频率。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 完整逻辑

```c
HAL_Init();
system_clock_72mhz_init();
pwm_init();
HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
while (1) {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, duty);
    HAL_Delay(40);
    // duty 和 step 更新逻辑...
}
```

### 8.2 HAL 版关键步骤

| 步骤 | HAL 代码 | 对应寄存器 |
|---|---|---|
| 开时钟 | `__HAL_RCC_GPIOA_CLK_ENABLE()` + `__HAL_RCC_TIM2_CLK_ENABLE()` | IOPAEN + TIM2EN |
| 配 PA0 | `GPIO_MODE_AF_PP` | CNF0=10 复用推挽 |
| 配时基 | `Prescaler=71, Period=999` | PSC + ARR |
| 初始化 | `HAL_TIM_PWM_Init(&htim2)` | 写入时基寄存器 |
| 配通道 | `OCMode=PWM1, Pulse=250` | OC1M + CCR1 |
| 落地通道 | `HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1)` | 写 CCR1/OC1M/极性 |
| 启动 | `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)` | CC1E + CEN |
| 更新占空比 | `__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, duty)` | `TIM2->CCR1 = duty` |

HAL 版需要 `SysTick_Handler()` 调用 `HAL_IncTick()`，否则 `HAL_Delay()` 会卡住。

## 9. 两个版本的学习策略

寄存器版先抓公式：

```text
TIM2 计数频率 = TIM2 输入时钟 / (PSC + 1)
PWM 频率 = TIM2 计数频率 / (ARR + 1)
占空比 ≈ CCR1 / (ARR + 1)
```

再抓通道输出链路：`PA0 复用推挽 → TIM2_CH1 → PWM mode 1 → CC1E → CEN`

HAL 版翻译：`Prescaler`→PSC，`Period`→ARR，`Pulse`→CCR1，`TIM_OCMODE_PWM1`→OC1M=110，`__HAL_TIM_SET_COMPARE`→运行中写 CCR1。

## 10. 检验问题清单

### 10.1 `ARR = 999` 时，为什么周期是 1000 个计数？

向上计数从 0 开始，包含 0 到 999，一共 1000 个值。

### 10.2 本课 PWM 频率怎么算？

72MHz / 72 / 1000 = 1kHz。

### 10.3 `CCR1 = 500` 时占空比约是多少？

500 / 1000 = 50%。

### 10.4 主循环改 `CCR1` 为什么能改 LED 亮度？

CCR1 决定每个 PWM 周期内有效电平持续多久，改 CCR1 就是改占空比。

### 10.5 改 `delay()` 会改变 PWM 频率吗？

不会。delay 只改变占空比更新速度，PWM 频率由 PSC 和 ARR 决定。

### 10.6 PA0 配成普通推挽输出会怎样？

TIM2_CH1 的 PWM 无法输出到引脚。

### 10.7 HAL 里的 `Pulse` 对应什么？

对应 `CCR1`。

### 10.8 `__HAL_TIM_SET_COMPARE()` 对应寄存器版哪句？

对应 `TIM2->CCR1 = duty;`。

## 11. 工程实现步骤

### 11.1 寄存器路线

1. 配置系统时钟到 72MHz
2. 打开 GPIOA、AFIO、TIM2 时钟
3. 配置 PA0 为复用推挽输出
4. 写 `PSC = 72 - 1`，`ARR = 1000 - 1`，`CCR1 = 250`
5. 清 `CC1S` 和 `OC1M`，设置 `OC1M = 110` PWM mode 1
6. 设置 `OC1PE`、`CC1E`
7. 触发 `EGR.UG`，设置 `CR1.CEN`
8. 主循环写 `TIM2->CCR1 = duty`

### 11.2 HAL 路线

1. `HAL_Init()`
2. 配置系统时钟
3. PA0 配成 `GPIO_MODE_AF_PP`
4. `htim2.Instance = TIM2`，设置 `Prescaler=71`/`Period=999`
5. `HAL_TIM_PWM_Init(&htim2)`
6. `TIM_OC_InitTypeDef` 设置 `OCMode=PWM1`/`Pulse=250`/`OCPolarity=HIGH`
7. `HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1)`
8. `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)`
9. 主循环 `__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, duty)`

### 11.3 常见工程陷阱

- 把亮度变化速度误认为 PWM 频率：delay 只改占空比更新速度
- 忘记 PA0 复用推挽：TIM2_CH1 内部有 PWM 但引脚不输出
- `CCR1` 超过 `ARR+1` 太多：占空比计算失去直观意义
- HAL 只配置通道不调用 `HAL_TIM_PWM_Start()`
- LED 接法反相：占空比越大越暗

## 12. 运行现象

PA0 输出约 1kHz PWM，LED 亮度按台阶逐渐变亮再变暗，循环往复。示波器下周期约 1ms，占空比随时间变化。

## 13. 常见问题排查

### 13.1 LED 完全不亮

1. 硬件接线：PA0、限流电阻、LED 极性、GND
2. 软件链路：GPIOA 时钟、PA0 复用推挽、TIM2 时钟、CC1E、CEN

### 13.2 LED 亮但亮度不变化

检查主循环是否持续更新比较值。寄存器版看 `TIM2->CCR1 = duty`，HAL 版看 `__HAL_TIM_SET_COMPARE()`。

### 13.3 PWM 频率不对

重新计算：`72MHz / (PSC+1) / (ARR+1)`，确认系统时钟确实是 72MHz。

### 13.4 占空比和亮度关系反了

先看 LED 接法（接 GND 还是 VCC），再看 `OCPolarity` 是否为 `TIM_OCPOLARITY_HIGH`。

### 13.5 HAL 版程序卡住

检查 `SysTick_Handler()` 是否调用 `HAL_IncTick()`，检查 HSE 是否存在。

## 14. 本课最核心的结论

PWM 的本质：定时器在固定周期内比较 `CNT` 和 `CCR1`，用比较结果决定输出电平。

- `PSC` 把 TIM2 计数变成 1MHz
- `ARR` 把 PWM 周期变成 1ms = 1kHz
- `CCR1` 决定每个周期内有效电平持续多久
- `OC1M = 110` 选择 PWM mode 1
- `OC1PE` 让运行中更新比较值更规整
- `CC1E` 和 PA0 复用推挽让 PWM 真正出现在引脚

## 15. 建议你现在怎么读这节课

先把三个数字算清楚：`72MHz / 72 = 1MHz`，`1MHz / 1000 = 1kHz`，`CCR1 / 1000 = 占空比`。

再回到代码找三条线：PA0 怎么变成 TIM2_CH1 输出脚 → TIM2 怎么生成 1kHz 周期 → 主循环怎么通过改 CCR1 改亮度。

最后看 HAL 版，把 `Prescaler`/`Period`/`Pulse`/`__HAL_TIM_SET_COMPARE()` 翻译回寄存器。

## 16. 扩展练习

1. 把 `duty` 固定为 100、500、900，观察 LED 亮度差异
2. 把步进 50 改成 10，观察亮度变化是否更细
3. 把 PWM 改成 500Hz，重新计算 `PSC` 和 `ARR`
4. 用示波器验证周期是否约 1ms
5. 把 PWM mode 1 改成 PWM mode 2，观察亮度变化方向

## 17. 下一课预告

下一课：[09_pwm_advanced](../09_pwm_advanced/README.md)

在 PWM 基础上做更像"呼吸灯"的占空比变化。硬件仍然是 TIM2_CH1 和 PA0，但软件更新 `CCR1` 的策略会更有节奏感。