# 09_pwm_advanced - PWM 进阶

## 1. 本课到底在学什么

本课表面现象：PA0 外接 LED 呈现呼吸灯效果，从暗到亮，再从亮到暗，循环变化。

真正要学的是"硬件 PWM"和"软件亮度策略"的分工。TIM2_CH1 继续负责输出稳定的 1kHz PWM；主循环不直接制造 PWM 波形，只是周期性修改 `CCR1`。亮度如何变化，由 `next_duty()` 这个软件函数决定。

和 08_pwm_basic 相比，PWM 频率、PA0 复用输出、PWM mode 1 这些硬件基础基本不变。进阶点在于：上一课用固定步进 50 线性改变占空比，本课用分段步进，让暗部、中间亮度、高亮区的变化速度不同，视觉上更像呼吸。

本课还包含 `pwm_to_din_test/` 小实验：把普通 1kHz PWM 接到数字灯条 DIN，观察灯条通常不会正常工作。它用来说明普通 PWM 和 WS2812/SK6812 单线数字灯条协议不是一回事。

## 2. 本课学习目标

学完本课，你应该能做到：

- 说明呼吸灯为什么本质上是稳定 PWM 加周期性修改 `CCR1`
- 区分 PWM 频率、占空比、占空比更新节奏、视觉亮度曲线
- 解释 `next_duty()` 为什么属于软件策略，而不是定时器硬件模式
- 看懂分段步进对观感的影响
- 解释为什么普通 PWM 接数字灯条 DIN 不能等价于发送灯条协议

## 3. 本课目录结构

```text
09_pwm_advanced/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
├── hal/
│   ├── platformio.ini
│   └── src/main.c
└── pwm_to_din_test/
    ├── platformio.ini
    └── src/main.c
```

## 4. 实验硬件

- STM32F103C8T6 BluePill
- PA0 引脚 + 220Ω 限流电阻 + LED
- 推荐接法：PA0 → 电阻 → LED 正极 → LED 负极 → GND
- 可选：示波器或逻辑分析仪

`pwm_to_din_test/` 需要额外：外部 5V 电源供电灯条，STM32 GND 与电源共地，PA0 → 220Ω~470Ω → 灯条 DIN。

## 5. 先建立一个最基本的脑图

完整主线链路：

1. 系统时钟配置到 72MHz
2. PA0 配成复用推挽输出
3. TIM2 配成 1kHz PWM：`PSC = 72 - 1`，`ARR = 1000 - 1`
4. CH1 使用 PWM mode 1，初始 `CCR1 = 0`
5. 打开 `OC1PE`，触发 `EGR.UG`，打开 `CC1E` 并启动 `CEN`
6. 主循环把当前 `duty` 写入 `CCR1`，延时一小段时间
7. 到达 0 或 1000 时改变 `direction`
8. `next_duty()` 根据当前区间算出下一档 duty

## 6. 核心名词速查

### 6.1 呼吸灯 = 硬件 PWM + 软件占空比策略

```text
硬件层：TIM2_CH1 输出 1kHz PWM（和上一课一样）
软件层：next_duty() 决定占空比如何变化
```

PWM 没有稳定输出，呼吸效果没有基础。`CCR1` 不变化，LED 只会停在某个固定亮度。

### 6.2 分段步进

| duty 范围 | step | 理由 |
|---|---|---|
| 0~119 | 5 | 暗部变化更细，避免刚亮起来时突兀 |
| 120~399 | 15 | 中间偏暗区，让变化不拖 |
| 400~749 | 25 | 中间偏亮区，变化更快 |
| 750~1000 | 12 | 高亮区微调，避免一下冲到顶 |

如果所有区间都用固定 50，亮度台阶更明显，呼吸效果更硬。

### 6.3 本课新增软件概念

| 概念 | 作用 | 关键规则 |
|---|---|---|
| `duty` | 准备写入 CCR1 的占空比数值 | 只有写入 CCR1 后才影响硬件 |
| `direction` | 亮度变化方向（1=变亮，-1=变暗） | 到达 0 或 1000 时反转 |
| `next_duty()` | 根据当前 duty 和 direction 算出下一档值 | 纯软件策略，不是硬件功能 |
| `delay()`/`HAL_Delay()` | 控制呼吸变化速度 | 不生成 PWM，只决定多久更新一次 duty |

### 6.4 HAL 对应关系

| HAL | 寄存器 |
|---|---|
| `__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, duty)` | `TIM2->CCR1 = duty` |
| `HAL_Delay(25)` | `delay(120000U)` |
| `__HAL_AFIO_REMAP_SWJ_NOJTAG()` | 关闭 JTAG 保留 SWD，释放 GPIO |

PWM 硬件配置（PSC/ARR/OC1M/OC1PE/CC1E/CEN）和上一课完全相同，不再重复。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 完整逻辑

```c
int main(void)
{
    system_clock_72mhz_init();
    pwm_init();

    uint16_t duty = 0U;
    int8_t direction = 1;

    while (1) {
        TIM2->CCR1 = duty;
        delay(120000U);

        if (duty >= 1000U) direction = -1;
        else if (duty == 0U) direction = 1;

        duty = next_duty(duty, direction);
    }
}
```

### 7.2 PWM 初始化

和上一课相同：PSC=71, ARR=999, CCR1=0, OC1M=110(PWM1), OC1PE=1, CC1E=1, EGR.UG, CEN=1。唯一区别是初始 `CCR1 = 0`（从暗开始）。

### 7.3 `next_duty()` 分段步进逻辑

```c
static uint16_t next_duty(uint16_t duty, int8_t direction)
{
    uint16_t step;
    if (duty < 120U) step = 5U;
    else if (duty < 400U) step = 15U;
    else if (duty < 750U) step = 25U;
    else step = 12U;

    int32_t next = (int32_t)duty + direction * (int32_t)step;
    if (next > 1000) next = 1000;
    if (next < 0) next = 0;
    return (uint16_t)next;
}
```

这是纯软件算法，不属于 STM32 硬件。它让暗部步进更细，中间区域变化更快，高亮区再调整步进。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 完整逻辑

```c
HAL_Init();
system_clock_72mhz_init();
pwm_init();
HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);

uint32_t duty = 0U;
int8_t direction = 1;

while (1) {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, duty);
    HAL_Delay(25);

    if (duty >= 1000U) direction = -1;
    else if (duty == 0U) direction = 1;

    duty = next_duty(duty, direction);
}
```

### 8.2 HAL 版和寄存器版的差异

| 差异点 | 寄存器版 | HAL 版 |
|---|---|---|
| 更新 CCR1 | `TIM2->CCR1 = duty` | `__HAL_TIM_SET_COMPARE()` |
| 延时 | `delay(120000U)` 空循环 | `HAL_Delay(25)` 依赖 SysTick |
| next_duty 参数类型 | `uint16_t` | `uint32_t` |
| JTAG 处理 | 无 | `__HAL_AFIO_REMAP_SWJ_NOJTAG()` |

PWM 硬件配置完全相同。

## 9. 两个版本的学习策略

本课硬件和上一课基本一样，学习重点在软件策略：

1. `duty` 和 `direction` 是纯软件变量，只有写入 CCR1 后才影响硬件
2. `next_duty()` 的分段步进是视觉效果策略，不是定时器功能
3. `delay()`/`HAL_Delay()` 只控制呼吸变化速度，不改变 PWM 频率

## 10. 检验问题清单

### 10.1 为什么本课不需要改变 `ARR` 就能改变亮度？

亮度由 `CCR1` 决定（占空比），`ARR` 决定 PWM 周期。改占空比不需要改周期。

### 10.2 `next_duty()` 是硬件功能吗？

不是，它是纯软件算法，根据当前 duty 和 direction 计算下一档值。

### 10.3 为什么低亮度区步进用 5？

暗部人眼对变化更敏感，小步进让亮度过渡更平滑，避免刚亮起来时突兀。

### 10.4 `direction` 为什么要在 0 和 1000 处改变？

0 是最暗，1000 是最亮。到达边界后必须反转方向，否则呼吸循环不完整。

### 10.5 `OC1PE` 打开后有什么好处？

运行中写入的 CCR1 新值在更新事件后生效，避免周期中间改值产生不规则边沿。

### 10.6 HAL 版 `__HAL_TIM_SET_COMPARE()` 对应寄存器版哪句？

`TIM2->CCR1 = duty;`

### 10.7 `HAL_Delay(25)` 会改变 PWM 频率吗？

不会，只改变占空比更新速度。

### 10.8 普通 PWM 接灯条 DIN 为什么不能正常控制颜色？

数字灯条需要按协议发送一串精确高低电平编码，普通 PWM 只有周期和占空比，不包含数据编码。

## 11. 工程实现步骤

### 11.1 寄存器路线

1. 配置系统时钟到 72MHz
2. 打开 GPIOA、AFIO、TIM2 时钟
3. 配置 PA0 为复用推挽输出
4. 写 `PSC = 72 - 1`，`ARR = 1000 - 1`，`CCR1 = 0`
5. 设置 `OC1M = 110` PWM mode 1，`OC1PE`，`CC1E`
6. 触发 `EGR.UG`，设置 `CR1.CEN`
7. 主循环：写 `TIM2->CCR1 = duty`，延时，更新 duty

### 11.2 HAL 路线

1. `HAL_Init()`
2. 配置系统时钟
3. PA0 配成 `GPIO_MODE_AF_PP`
4. `htim2.Instance = TIM2`，设置 `Prescaler=71`/`Period=999`
5. `HAL_TIM_PWM_Init(&htim2)`
6. `TIM_OC_InitTypeDef` 设置 `OCMode=PWM1`/`Pulse=0`/`OCPolarity=HIGH`
7. `HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1)`
8. `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)`
9. 主循环：`__HAL_TIM_SET_COMPARE()`，`HAL_Delay(25)`，更新 duty

### 11.3 常见工程陷阱

- 把呼吸变化速度误认为 PWM 频率
- duty 计算对了但没写入 CCR1，LED 亮度不变
- 所有区间用固定步进，呼吸效果生硬
- HAL 版缺少 `SysTick_Handler()` 导致 `HAL_Delay()` 卡住
- 数字灯条 DIN 接普通 PWM，期望灯条按颜色亮

## 12. 运行现象

PA0 外接 LED 呈现呼吸灯效果：从暗到亮，再从亮到暗，循环变化。示波器下 PA0 仍是约 1kHz PWM，占空比在慢慢变化。

## 13. 常见问题排查

### 13.1 LED 完全不亮

1. 硬件接线
2. GPIOA 时钟、PA0 复用推挽、TIM2 时钟、CC1E、CEN

### 13.2 LED 有亮度但不呼吸

1. 主循环是否持续更新 CCR1
2. `next_duty()` 是否被调用
3. `direction` 是否在边界处反转

### 13.3 呼吸到最亮或最暗后卡住

`direction` 没有在边界处反转，或 `next_duty()` 返回值没有正确钳位。

### 13.4 亮度突然跳变

分段步进的区间边界或 step 值设置不合理，导致相邻区间步进差距太大。

### 13.5 波形频率不对

重新计算：`72MHz / (PSC+1) / (ARR+1)`。

### 13.6 HAL 版卡住或不更新

检查 `SysTick_Handler()` 是否调用 `HAL_IncTick()`。

## 14. 本课最核心的结论

呼吸灯 = 硬件 PWM + 软件占空比策略。

- TIM2_CH1 输出稳定的 1kHz PWM（硬件层不变）
- `next_duty()` 用分段步进让亮度变化更自然（软件策略层）
- `duty` 和 `direction` 是纯软件变量，只有写入 CCR1 后才影响硬件
- `delay()`/`HAL_Delay()` 只控制呼吸变化速度，不改变 PWM 频率

## 15. 建议你现在怎么读这节课

先确认硬件层和上一课一样：PSC=71, ARR=999, OC1M=110, OC1PE=1, CC1E=1, CEN=1。

然后把注意力放在软件策略：`next_duty()` 怎么分段、`direction` 怎么反转、延时怎么控制节奏。

最后看 HAL 版，确认 `__HAL_TIM_SET_COMPARE()` 就是写 CCR1。

## 16. 扩展练习

1. 把所有区间 step 改成固定 50，对比呼吸效果
2. 调整分段区间边界，观察视觉变化
3. 把 `HAL_Delay(25)` 改成 `HAL_Delay(10)` 或 `HAL_Delay(50)`，观察呼吸节奏
4. 用示波器观察占空比变化过程
5. 尝试 `pwm_to_din_test/`，观察数字灯条对普通 PWM 的反应

## 17. 下一课预告

下一课：[10_exti](../10_exti/README.md)

从定时器转到外部中断。PA0 从 PWM 输出变成按键输入，通过 EXTI 检测下降沿触发中断，在 ISR 中翻转 PC13。