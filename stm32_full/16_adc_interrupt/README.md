# 16_adc_interrupt - ADC 中断采样

## 1. 本课到底在学什么

本课表面现象：PA1 接电位器，旋转时 PC13 LED 在电压超过约 1.65V 时亮、低于时灭。主循环为空，什么都不做。

真正要学的是 ADC 转换完成中断链路。上一课轮询方式是 CPU 启动转换后一直等 `EOC`；本课改成 `EOCIE` 使能后，ADC 转换完成主动通知 CPU，CPU 进入 `ADC1_2_IRQHandler()` 读取结果并再次启动下一次转换。

**核心变化只有一处：等待方式从"CPU 傻等 EOC"变成"ADC 完成后通知 CPU"。** 硬件接线、ADC 基础配置（通道、采样时间、校准）和上一课完全一样。

## 2. 本课学习目标

1. 中断采样比轮询采样改变了哪一步？
2. 只开 `EOCIE` 不开 NVIC 会怎样？反过来呢？
3. 为什么 ISR 里不需要再轮询 `EOC`？
4. 单次转换模式下为什么必须在 ISR 里再次启动？
5. `volatile` 在这里的作用是什么？
6. HAL 的 Start_IT / IRQHandler / Callback 分别对应寄存器版哪一步？

## 3. 本课目录结构

```text
16_adc_interrupt/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

硬件接线和上一课完全相同：电位器中间脚 → PA1。

## 4. 实验硬件

与上一课完全相同：

- STM32F103C8T6 BluePill
- ST-Link 下载器
- 电位器（推荐 10kΩ），接法：
  - 一端 → 3.3V
  - 另一端 → GND
  - 中间脚 → PA1
- PC13 板载 LED

**PA1 输入不能超过 3.3V。外部模拟源必须共地。**

## 5. 先建立一个最基本的脑图

```text
1. 系统时钟 72MHz
2. PC13 推挽输出
3. PA1 模拟输入
4. ADC1 时钟 PCLK2/6 = 12MHz，规则组通道 1，采样 239.5 cycles
5. ADON 上电 → RSTCAL → CAL 校准
6. ★ CR1.EOCIE = 1（允许转换完成中断）
7. ★ NVIC 使能 ADC1_2_IRQn
8. main 启动第一次转换
9. ADC 转换完成 → EOC 置位 → EOCIE 触发中断请求 → NVIC 放行
10. CPU 进 ADC1_2_IRQHandler() → 读 DR → 控制 LED → 再次启动转换
11. 后续形成"转换→中断→再启动"持续链
```

第 6~7 步是本课新增。第 9~11 步是中断方式的核心流程。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在第 15 课已详细讲过，本课不再重复：

| 名词 | 一句话提醒 |
|------|-----------|
| `ADC1_IN1 / PA1` | PA1 第二功能是 ADC1 通道 1，配模拟输入 |
| `ADCPRE = PCLK2/6` | 12MHz，不能超 14MHz |
| `SQR1.L=0, SQR3.SQ1=1` | 规则组只采 1 个通道，就是通道 1 |
| `SMPR2.SMP1=111` | 239.5 cycles 采样时间 |
| `ADON / RSTCAL / CAL` | 上电 → 复位校准 → 执行校准 |
| `EXTTRIG / SWSTART` | 允许触发 + 软件启动一次转换 |
| `EOC` | 转换完成标志，SR 寄存器 |
| `DR` | 数据寄存器，12 位结果，读 DR 清 EOC |
| `GPIO_MODE_ANALOG` | HAL 模拟输入模式 |

本课新增重点在下面。

### 6.2 `EOCIE` 是什么

End Of Conversion Interrupt Enable，位于 `ADC1->CR1` bit 5。

上一课 `EOCIE=0`，`EOC` 置位但不会发中断请求，CPU 靠轮询检测。本课 `EOCIE=1`，每次 `EOC` 置位时 ADC 外设向 NVIC 发中断请求。

如果忘了设 `EOCIE`，即使 NVIC 配了也没用——ADC 根本不发出中断请求。

### 6.3 `ADC1_2_IRQn` / NVIC 是什么

STM32F103 中 ADC1 和 ADC2 共用一个 NVIC 中断号 `ADC1_2_IRQn`。无论哪个 ADC 触发中断，CPU 都跳转到同一个入口函数 `ADC1_2_IRQHandler()`。

NVIC 是 Cortex-M 内核的中断控制器。ADC 内部 `EOCIE` 是"分闸"，NVIC 是"总闸"，两道门都开才能进 ISR。

如果只开了 `EOCIE` 但没配置 NVIC，或者反过来，中断都不会执行。

### 6.4 `ADC1_2_IRQHandler()` 是什么

ADC1/ADC2 共用的中断服务函数。函数名必须和启动文件向量表一致。

进入 ISR 就说明 `EOC` 已经置位了，所以不需要再 while 等待。这是中断和轮询最根本的区别。

因为 ADC1/ADC2 共用入口，寄存器版需要判断 `(ADC1->SR & ADC_SR_EOC)` 确认是 ADC1 触发的。HAL 版由 `HAL_ADC_IRQHandler()` 统一处理。

### 6.5 `volatile g_adc_value` 是什么

全局变量保存最新 ADC 结果。ISR 中写，主循环可能读，编译器不能把它优化到寄存器里缓存。

去掉 `volatile` 后编译器可能认为主循环里值没变，直接跳过读取或用旧值。

### 6.6 单次转换 + 再启动是什么

本课 `ContinuousConvMode = DISABLE`（单次转换）。每完成一次就停住，必须再次调用 `SWSTART` 或 `Start_IT` 才会转下一轮。

所以 ISR/回调里必须再次启动，否则只采一次就不动了。这形成了"转换→中断→再启动→转换"的持续循环。

### 6.7 `HAL_ADC_Start_IT()` 是什么

HAL 启动 ADC 并使能中断的接口。内部做了：清状态标志 + 设 `CR1.EOCIE=1` + 设 `SWSTART`。对应寄存器版的 `CR1 |= EOCIE; CR2 |= EXTTRIG | SWSTART`。

注意它**不自动配 NVIC**，需要在初始化里手动调 `HAL_NVIC_EnableIRQ(ADC1_2_IRQn)`。

### 6.8 `HAL_ADC_IRQHandler()` 是什么

HAL 的 ADC 中断分发函数。用户在 `ADC1_2_IRQHandler()` 中调用它，它检查 `SR.EOC`、清标志、然后调用 `HAL_ADC_ConvCpltCallback()` 用户回调。

如果 ISR 里没调这个函数，回调永远不会执行。

### 6.9 `HAL_ADC_ConvCpltCallback()` 是什么

弱函数，用户重新实现。ADC 转换完成后 HAL 自动调用。本课在这里读结果、控制 LED、再次 `Start_IT`。

参数 `hadc->Instance` 可区分是 ADC1 还是 ADC2 触发的。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

以下和 15_adc_polling 完全一致：

1. `system_clock_72mhz_init()` — HSE 8MHz → PLL x9 → 72MHz
2. `led_pc13_init()` — PC13 推挽输出，初始高电平灭
3. `pa1_adc_input_init()` — PA1 模拟输入（MODE=00, CNF=00）
4. 开 ADC1 时钟，ADCPRE = PCLK2/6 = 12MHz
5. SQR1.L=0, SQR3.SQ1=1, SMPR2.SMP1=111
6. ADON 上电 → RSTCAL → CAL 校准

### 7.2 新增：打开 EOCIE

```c
ADC1->CR1 |= ADC_CR1_EOCIE;
```

允许 `EOC` 触发中断请求。上一课这里没这句，所以只能靠轮询。

### 7.3 新增：配置 NVIC

```c
NVIC_SetPriority(ADC1_2_IRQn, 1U);
NVIC_EnableIRQ(ADC1_2_IRQn);
```

两道门都开了：ADC 内部 `EOCIE`（分闸）+ NVIC（总闸）。

### 7.4 main 启动第一次转换

```c
adc1_start_conversion();  // CR2 |= EXTTRIG | SWSTART;
```

没有第一次就没有后续中断链。后续由 ISR 持续驱动。

### 7.5 `ADC1_2_IRQHandler()`

```c
void ADC1_2_IRQHandler(void)
{
    if ((ADC1->SR & ADC_SR_EOC) != 0U) {
        g_adc_value = (uint16_t)ADC1->DR;

        if (g_adc_value > 2048U) {
            GPIOC->BRR = GPIO_BRR_BR13;     /* LED 亮 */
        } else {
            GPIOC->BSRR = GPIO_BSRR_BS13;   /* LED 灭 */
        }

        adc1_start_conversion();             /* 再启动 */
    }
}
```

关键点：
- 进入 ISR 说明 `EOC` 已经置位，不需要再等
- 读 DR 同时清除 EOC（F103 硬件特性）
- **必须再次启动**，否则单次转换模式下只采一次就停了

### 7.6 主循环空闲

```c
while (1) { }
```

主循环什么都没做。ADC 采样、LED 控制全在中断里完成。这就是中断方式的威力——CPU 不被阻塞。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

以下和 15_adc_polling HAL 版一致：

1. `HAL_Init()` + 时钟 72MHz
2. PC13 `GPIO_MODE_OUTPUT_PP`
3. PA1 `GPIO_MODE_ANALOG`
4. `__HAL_RCC_ADC1_CLK_ENABLE` + `RCC_ADCPCLK2_DIV6`
5. `hadc1.Init`（单通道、单次、软件触发、右对齐）
6. `HAL_ADCEx_Calibration_Start()`
7. `sConfig`（Channel=1, Rank=1, SamplingTime=239.5）

### 8.2 新增：NVIC 配置

```c
HAL_NVIC_SetPriority(ADC1_2_IRQn, 1, 0);
HAL_NVIC_EnableIRQ(ADC1_2_IRQn);
```

`HAL_ADC_Start_IT()` 会设 `EOCIE` 但不会自动配 NVIC，所以要手动加这一步。

### 8.3 main 启动第一次中断转换

```c
if (HAL_ADC_Start_IT(&hadc1) != HAL_OK) {
    error_handler();
}
```

对应寄存器版的 `EOCIE` + `SWSTART`。返回非 OK 说明出错了。

### 8.4 `ADC1_2_IRQHandler()` — 分发

```c
void ADC1_2_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
}
```

不要在这里写业务逻辑，交给 HAL 分发到回调。

### 8.5 `HAL_ADC_ConvCpltCallback()` — 用户业务

```c
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) {
        g_adc_value = HAL_ADC_GetValue(hadc);

        if (g_adc_value > 2048U) {
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);  /* 亮 */
        } else {
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);    /* 灭 */
        }

        if (HAL_ADC_Start_IT(hadc) != HAL_OK) {
            error_handler();
        }
    }
}
```

对应寄存器版 ISR 全部内容：读 DR → 判断阈值 → 控制 LED → 再启动。

## 9. 两个版本怎么学

寄存器版抓住两道门：

```text
EOCIE（分闸）+ NVIC（总闸）→ ADC1_2_IRQHandler → 读DR → 再SWSTART
```

HAL 版抓住三层调用：

```text
Start_IT（含EOCIE+SWSTART）→ IRQHandler（分发）→ ConvCpltCallback（业务+再Start）
```

共同要点：**中断不是"自动读取"，只是把 CPU 带到 ISR；读取 DR 和再次启动仍然要代码完成。**

## 10. 检验问题清单

### 10.1 中断采样比轮询改变了哪一步？

**答**：启动、通道、采样、校准都不变。只改变了"等待转换完成"这一步：从 CPU 轮询 `EOC` 改成 ADC 完成后触发中断。

### 10.2 只开 `EOCIE` 不开 NVIC 会怎样？

**答**：ADC 内部允许发中断请求，但 NVIC 没放行，CPU 不会进 ISR。`EOC` 正常置位，只是没人响应。

### 10.3 只开 NVIC 不开 `EOCIE` 会怎样？

**答**：CPU 中断通道放行了，但 ADC 完成转换不发中断请求，也不会进 ISR。

### 10.4 为什么 ISR 里不再轮询 `EOC`？

**答**：能进 ADC 完成中断本身就说明 `EOC` 已置位。这是中断和轮询的根本区别。

### 10.5 为什么要在 ISR 里再次启动？

**答**：本课是单次转换模式，一次转换完就停。不再次启动就只采一次。

### 10.6 `volatile` 去掉会怎样？

**答**：编译器可能把 `g_adc_value` 缓存在寄存器，主循环读不到 ISR 写的最新值。

### 10.7 HAL 版哪个函数调用用户回调？

**答**：`HAL_ADC_IRQHandler()` 检测到 `EOC` 后调用 `HAL_ADC_ConvCpltCallback()`。

### 10.8 如果回调里不判断 `hadc->Instance` 会怎样？

**答**：项目中有 ADC2 也用中断时，ADC2 完成也会进同一个回调，导致误处理。

## 11. 工程实现步骤

### 11.1 需求分析

用中断方式持续采样 PA1，根据阈值控制 PC13。要求 ADC 基础配置正确、`EOCIE` 使能、NVIC 放行、ISR 能读数据并再次启动。

### 11.2 硬件核查

同上一课：电位器中间脚接 PA1，两端接 3.3V/GND，输入不超过 3.3V，共地。

### 11.3 寄存器路线

1. 时钟 72MHz、PC13 输出、PA1 模拟输入（同上课）
2. ADC1 时钟/分频/规则组/采样时间/校准（同上课）
3. `CR1.EOCIE = 1`
4. `NVIC_EnableIRQ(ADC1_2_IRQn)`
5. main 调 `adc1_start_conversion()` 第一次
6. `ADC1_2_IRQHandler()`：判 EOC → 读 DR → 控制 LED → 再启动

### 11.4 HAL 路线

1. HAL_Init + 时钟/PC13/PA1/hadc1.Init/校准/通道（同上课）
2. `HAL_NVIC_EnableIRQ(ADC1_2_IRQn)`
3. main 调 `HAL_ADC_Start_IT(&hadc1)` 第一次
4. `ADC1_2_IRQHandler()` → `HAL_ADC_IRQHandler()`
5. `HAL_ADC_ConvCpltCallback()`：读值 → 控制 LED → 再 `Start_IT`

### 11.5 工程思维

中断释放了 CPU 等待时间，但也带来设计约束：ISR 应短小快速，避免阻塞其他中断。简单 demo 可以在 ISR 直接控 GPIO；复杂工程通常只在 ISR 存数据或置标志，主循环处理业务。

### 11.6 常见工程陷阱

1. **忘配 NVIC** — 只设了 EOCIE，中断永远不来
2. **忘设 EOCIE** — NVIC 配了但 ADC 不发请求
3. **ISR 里没再启动** — 只采一次就停了
4. **ISR 里做太多事** — 占用 CPU 太久影响其他中断
5. **HAL 版 IRQHandler 里没调 HAL_ADC_IRQHandler** — 回调永远不执行

## 12. 运行现象

电位器中间脚接 PA1，旋转电位器时：

- **电位器旋到 GND 端**：`g_adc_value` 接近 0，PC13 LED **灭**
- **电位器旋到中间位置**：`g_adc_value` 约 2048（对应 ~1.65V），PC13 LED **在此阈值切换**
- **电位器旋到 3.3V 端**：`g_adc_value` 接近 4095，PC13 LED **亮**

**关键区别于上一课**：main 函数的 `while(1)` 循环体为空，什么都不做。LED 控制完全由中断驱动，CPU 不被阻塞。用调试器观察 `g_adc_value`，旋转电位器时应看到 0~4095 平滑变化，且变化频率不受主循环影响。

如果现象和上一课一样正常工作但主循环空着也能跑，说明中断链路正确。

## 13. 常见问题排查

### 13.1 只采一次就不变了

ISR/回调里没有再次启动转换。寄存器版检查是否有 `adc1_start_conversion()`，HAL 版检查是否有 `HAL_ADC_Start_IT(hadc)`。

### 13.2 完全不进中断

三道门逐一排查：
1. `CR1.EOCIE` 是否置位
2. `NVIC_EnableIRQ(ADC1_2_IRQn)` 是否调用
3. 函数名是否严格是 `ADC1_2_IRQHandler`

HAL 版额外确认用的是 `HAL_ADC_Start_IT()` 不是 `HAL_ADC_Start()`。

### 13.3 HAL 版进了 IRQ 但不进回调

`ADC1_2_IRQHandler()` 里没调 `HAL_ADC_IRQHandler(&hadc1)`，或者调用了但句柄不对。

### 13.4 ADC 值不随电位器变化

和上一课一样的 ADC 基础问题：PA1 接线、模拟输入、规则组通道、采样时间、校准。中断方式不会修复这些底层错误。

## 14. 本课结论

1. 中断采样只改变"等待转换完成"的方式，ADC 基础配置不变
2. `EOCIE`（分闸）+ NVIC（总闸）两道门缺一不可
3. F103 中 ADC1/ADC2 共用 `ADC1_2_IRQn`，ISR 里需确认是 ADC1
4. 进入 ISR 说明 `EOC` 已置位，不需要再轮询
5. 单次转换模式必须在 ISR/回调里再次启动才能持续采样
6. `volatile` 保证中断修改的全局变量不被编译器优化掉
7. HAL 三层：Start_IT → IRQHandler → ConvCpltCallback

## 15. 阅读建议

先拿上一课轮询版对比：哪些步骤一模一样（时钟、通道、采样、校准、启动），哪里变了（等待方式）。

然后看寄存器版 `adc1_init()` 最后两步：`EOCIE` 和 NVIC。再看 `ADC1_2_IRQHandler()` 全文，理解"进来就不用等了"和"读完必须再启动"。

最后看 HAL 版的三层调用关系，理解 HAL 为什么要把业务拆到回调里。

## 16. 扩展练习

1. 主循环里读 `g_adc_value`，根据不同区间做不同 LED 闪烁节奏
2. 去掉 ISR 里的再次启动，验证是否只采一次
3. 把阈值 2048 改成 1024 或 3072，观察 LED 切换点变化
4. 回调里只设标志位，主循环里处理 LED（更接近工程做法）
5. 思考：采样频率很高时，为什么 DMA 比中断更合适？

## 17. 下一课预告

上一课：[15_adc_polling](../15_adc_polling/README.md)

下一课：[17_adc_multichannel_scan](../17_adc_multichannel_scan/README.md)

下一课学习 ADC 多通道扫描。不再只采 PA1 一个通道，而是让 ADC 按规则组序列依次采多个模拟输入。