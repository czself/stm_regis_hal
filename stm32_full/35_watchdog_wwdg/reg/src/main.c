#include "stm32f1xx.h"

/*
 * ============================================================================
 * 寄存器版 WWDG（窗口看门狗）实验
 * ============================================================================
 *
 * ██████  本课核心知识点 ██████
 *
 * 1. WWDG 与 IWDG 的对比
 *
 *    | 特性       | IWDG（独立看门狗）    | WWDG（窗口看门狗）      |
 *    |------------|----------------------|------------------------|
 *    | 时钟源     | LSI（约 40kHz 独立） | APB1（PCLK1 派生）     |
 *    | 刷新限制   | 只要在超时前刷新即可 | 必须在"窗口内"刷新      |
 *    | 喂早       | 允许                | 禁止！会导致复位        |
 *    | 喂晚       | 复位                | 复位                    |
 *    | 可关闭     | 启动后无法关闭      | 启动后无法关闭          |
 *
 * 2. "窗口"的含义
 *    - WWDG 有一个 7 位递减计数器（CR.T[6:0]）
 *    - 窗口值（CFR.W[6:0]）定义了一个"只能在 T<W 时刷新"的窗口
 *    - T > W 时刷新（喂早了）→ 复位
 *    - 0x40 < T ≤ W 时刷新 → 合法
 *    - T ≤ 0x40 时刷新（喂晚了，即将到 0）→ 复位（或中断）
 *
 * 3. 关键寄存器
 *    - CR（控制寄存器）：
 *      · WDGA=1：启动 WWDG（一旦置位无法清除）
 *      · T[6:0]：当前计数器的值，读它来判断是否进入窗口
 *    - CFR（配置寄存器）：
 *      · WDGTB[1:0]：分频系数（00=1, 01=2, 10=4, 11=8）
 *      · W[6:0]：窗口上限值
 *      · EWI=1：使能提前唤醒中断（本课暂不使用）
 *
 * 4. 喂狗操作的本质
 *    刷新 WWDG = 向 CR 重新写入 WDGA|0x7F
 *    注意：不是写 CR.T，而是整个 CR 寄存器！因为 CR.T 是只读的。
 *    重新写 CR 会把计数器重置为初始值 0x7F。
 *
 * ██████  Demo 演示现象 ██████
 *
 * - 上次如果是 WWDG 复位，启动后 LED 快闪 4 次
 * - 按住 PA0：程序持续检测当前计数器，等它进入窗口后合法刷新，LED 周期闪烁
 * - 松开 PA0：不刷新，窗口看门狗超时复位
 * - 注意和 IWDG 实验的区别：如果按键按下时 T > W（还没进窗口），
 *   程序会空转等待，不会提前刷新（提前刷新会导致立即复位）
 */

#define WWDG_COUNTER_VALUE 0x7FU   /* 计数器初始值（最大） */
#define WWDG_WINDOW_VALUE  0x50U   /* 窗口上限：只有 T≤0x50 才能刷新 */

static void delay_busy(volatile uint32_t n)
{
    while (n-- > 0U) {
        __NOP();
    }
}

static void led_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;
    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void key_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    GPIOA->CRL &= ~(GPIO_CRL_MODE0 | GPIO_CRL_CNF0);
    GPIOA->CRL |= GPIO_CRL_CNF0_1;
    GPIOA->BSRR = GPIO_BSRR_BS0;
}

static uint8_t key_pressed(void)
{
    return (GPIOA->IDR & GPIO_IDR_IDR0) == 0U;
}

static void led_blink(uint32_t times)
{
    for (uint32_t i = 0; i < times; i++) {
        GPIOC->BRR = GPIO_BRR_BR13;
        delay_busy(500000U);
        GPIOC->BSRR = GPIO_BSRR_BS13;
        delay_busy(500000U);
    }
}

/*---------------------------------------------------------------------------*
 * WWDG 初始化
 *---------------------------------------------------------------------------*/
static void wwdg_init(void)
{
    /*
     * WWDG 挂在 APB1 总线上，需要打开 APB1 上的 WWDG 时钟
     * 注意：IWDG 不需要时钟使能，因为它是独立时钟域
     * 但 WWDG 使用 APB1 时钟，必须手动使能
     */
    RCC->APB1ENR |= RCC_APB1ENR_WWDGEN;

    /*
     * ██ 配置 CFR（Configuration Register）██
     *
     * WDGTB=11 表示分频系数 = 8
     * 所以 WWDG 计数时钟 = PCLK1 / 4096 / 8
     * （手册规定：WWDG 时钟 = PCLK1 / 4096 / WDGTB 分频）
     *
     * 若 PCLK1 = 36MHz：
     *   36MHz / 4096 / 8 ≈ 1098 Hz
     *   计数器从 0x7F(127) 递减到 0x3F(63) 约需 (127-63)/1098 ≈ 58ms
     *   从 0x7F 递减到 0 约需 127/1098 ≈ 115ms
     *
     * W = 0x50 表示窗口值：
     *   只有当 T 在 0x41 ~ 0x50 之间时刷新才合法
     *   T 从 0x7F 开始递减，刚启动时 T > 0x50，此时刷新会导致复位
     *   T 递减到 0x40 以下时（即 ≤0x3F），就会超时复位
     */
    WWDG->CFR = WWDG_CFR_WDGTB | WWDG_WINDOW_VALUE;

    /*
     * ██ 启动 WWDG ██
     *
     * CR 的 WDGA 位一旦置 1，窗口看门狗就启动，软件不能关闭。
     * 计数器从 0x7F 开始递减。
     *
     * 注意：写 CR 时 WDGA 必须为 1，否则不启动
     * 同时写入 WDGA 和计数器值 0x7F
     */
    WWDG->CR = WWDG_CR_WDGA | WWDG_COUNTER_VALUE;
}

/*---------------------------------------------------------------------------*
 * 窗口内刷新函数
 *
 * 这个函数实现了"等待进入窗口 → 合法刷新"的完整逻辑。
 * 正是这个函数让 WWDG 和 IWDG 的行为不同：
 * - IWDG：随时可以刷新
 * - WWDG：必须在 T ≤ W 时刷新
 *---------------------------------------------------------------------------*/
static void wwdg_refresh_in_window(void)
{
    uint32_t counter;

    /*
     * 第一步：等待计数器递减到窗口值以内
     *
     * 不断读取当前计数器值（CR.T[6:0]），
     * 直到它 ≤ 窗口值（0x50）
     *
     * 如果 T > 0x50，说明还没进入窗口，循环继续等待
     * 这就是"窗口"的含义：过早刷新的请求被软件拦截
     */
    do {
        counter = WWDG->CR & WWDG_CR_T;  /* 只读取 T 字段 */
    } while (counter > WWDG_WINDOW_VALUE);

    /*
     * 第二步：检查是否还在超时前
     *
     * 如果 T > 0x40，说明还没到超时临界点，可以合法刷新
     * 如果 T ≤ 0x40，说明已经太晚了，刷新也来不及了
     * （实际应用中这个判断可以省略，因为紧接着的刷新就是唯一机会）
     *
     * 注意：WWDG 在 T=0x3F 时会产生复位
     * 所以必须确保在 T > 0x40 时完成刷新
     */
    if (counter > 0x40U) {
        /*
         * 刷新操作：重新写 CR 寄存器
         * 把计数器重置为 0x7F
         * 注意：WDGA 必须继续保持为 1
         */
        WWDG->CR = WWDG_CR_WDGA | WWDG_COUNTER_VALUE;
    }
}

int main(void)
{
    /*
     * 检测复位来源：和 IWDG 类似，但是读 WWDGRSTF 位
     */
    uint8_t was_wwdg_reset = (RCC->CSR & RCC_CSR_WWDGRSTF) != 0U;
    RCC->CSR |= RCC_CSR_RMVF;

    led_init();
    key_init();
    if (was_wwdg_reset) {
        led_blink(4U);  /* WWDG 复位指示：快闪 4 次（和 IWDG 的 3 次区分） */
    }

    wwdg_init();  /* 配置并启动 WWDG */

    while (1) {
        if (key_pressed()) {
            /*
             * 按键按下时，调用 wwdg_refresh_in_window()
             * 这个函数内部会等待合法窗口，然后刷新
             * 如果计数器还在窗口外（T > 0x50），程序就卡在 do-while 里
             *
             * 这就是 WWDG 和 IWDG 最重要的区别：
             *   IWDG 实验：按下按键立刻喂狗
             *   WWDG 实验：按下按键后不一定能立刻刷新，
             *              必须等到 T ≤ 0x50 才能刷新
             */
            wwdg_refresh_in_window();
            led_blink(1U);
        } else {
            /*
             * 按键没按下：不做任何操作
             * WWDG 持续递减，直到 T=0x3F 触发复位
             * 这个超时时间比 IWDG 短得多（约 100ms 量级）
             */
            GPIOC->BRR = GPIO_BRR_BR13;  /* LED 亮，指示即将复位 */
        }
    }
}