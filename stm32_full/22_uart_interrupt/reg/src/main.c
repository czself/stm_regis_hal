#include "stm32f1xx.h"

/*
 * 本文件是"寄存器版 UART 中断接收实验"。
 *
 * 本课目标：
 * 1. 配置 USART1 为 115200 8N1
 * 2. 使用 RXNE 中断接收单字节
 * 3. 在中断里只做"收数据 + 置标志"
 * 4. 在主循环里做命令解析和 LED 控制
 *
 * 与上一课（UART 轮询）的核心区别：
 *   轮询：CPU 主动检查 RXNE，不知道数据何时来，一直在问。
 *   中断：USART 收到数据后主动通知 CPU，CPU 不必一直盯着。
 *
 * 中断链路：
 *   串口线有数据进入 → USART1 接收完成 → RXNE 置位
 *   → RXNEIE 已使能 → 硬件触发中断请求
 *   → NVIC 放行 → CPU 进入 USART1_IRQHandler()
 *   → 读取 DR → 保存数据 → 置标志 → 退出中断
 *   → 主循环检测到标志 → 处理命令
 *
 * 为什么要有"标志位 + 主循环处理"这种结构？
 *   中断原则：快进快出。
 *   如果中断里做大量字符串处理和 LED 控制，
 *   会拖慢系统实时性，阻塞其他中断。
 *   所以中断只保存数据，主循环做业务逻辑。
 */

/*
 * 全局变量 —— 中断与主循环之间的数据交换区
 *
 * g_rx_byte：中断中存入收到的字节，主循环中读取
 * g_rx_ready：中断中置 1，主循环检测到后清 0
 *
 * 为什么需要两个变量？
 *   g_rx_byte 保存数据，g_rx_ready 表示"有新数据"。
 *   分离数据和状态，逻辑更清晰，也方便扩展（如环形缓冲区）。
 */
static volatile uint8_t g_rx_byte = 0U;
static volatile uint8_t g_rx_ready = 0U;

/* 函数前置声明 */
static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void usart1_gpio_init(void);
static void usart1_init(void);
static void usart1_send_byte(uint8_t byte);
static void usart1_send_string(const char *str);
static void led_on(void);
static void led_off(void);
static void led_toggle(void);

/*
 * system_clock_72mhz_init —— 系统时钟 72MHz
 */
static void system_clock_72mhz_init(void)
{
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0U) {
    }

    RCC->CFGR &= ~(RCC_CFGR_HPRE |
                   RCC_CFGR_PPRE1 |
                   RCC_CFGR_PPRE2 |
                   RCC_CFGR_PLLSRC |
                   RCC_CFGR_PLLXTPRE |
                   RCC_CFGR_PLLMULL |
                   RCC_CFGR_SW);

    RCC->CFGR |= RCC_CFGR_HPRE_DIV1;
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;
    RCC->CFGR |= RCC_CFGR_PPRE2_DIV1;
    RCC->CFGR |= RCC_CFGR_PLLSRC;
    RCC->CFGR |= RCC_CFGR_PLLMULL9;

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0U) {
    }

    RCC->CFGR &= ~RCC_CFGR_SW;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

/*
 * led_pc13_init —— 初始化 PC13 LED
 */
static void led_pc13_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;

    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;

    GPIOC->BSRR = GPIO_BSRR_BS13;
}

/*
 * usart1_gpio_init —— 配置 USART1 引脚
 *
 * 与上一课（轮询版）完全相同的配置：
 *   PA9 = USART1_TX → 复用推挽输出
 *   PA10 = USART1_RX → 浮空输入
 */
static void usart1_gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /*
     * PA9 → USART1_TX → 复用推挽输出
     */
    GPIOA->CRH &= ~(GPIO_CRH_MODE9 | GPIO_CRH_CNF9);
    GPIOA->CRH |= GPIO_CRH_MODE9;
    GPIOA->CRH |= GPIO_CRH_CNF9_1;

    /*
     * PA10 → USART1_RX → 浮空输入
     */
    GPIOA->CRH &= ~(GPIO_CRH_MODE10 | GPIO_CRH_CNF10);
    GPIOA->CRH |= GPIO_CRH_CNF10_0;
}

/*
 * usart1_init —— 初始化 USART1（中断模式）
 *
 * 与上一课（轮询版）相比，新增了两个关键配置：
 *   1. RXNEIE = 1：使能接收中断（CR1 bit 5）
 *   2. NVIC 配置：使能 USART1_IRQn
 *
 * 轮询版 vs 中断版的 CR1 配置差异：
 *   轮询版：CR1 = TE | RE（只有发送和接收使能）
 *   中断版：CR1 = TE | RE | RXNEIE（新增接收中断使能）
 *
 * RXNEIE（RXNE Interrupt Enable）：
 *   0 = RXNE 只作为状态位（轮询方式）
 *   1 = RXNE 置位时触发中断请求
 *
 * 中断使能的两层配置（与 ADC 中断完全相同的原理）：
 *   第 1 层（外设侧）：CR1.RXNEIE = 1，允许 USART 产生中断请求
 *   第 2 层（NVIC 侧）：NVIC_EnableIRQ(USART1_IRQn)，允许 CPU 接收中断
 *   缺任何一层，中断都不会被执行。
 */
static void usart1_init(void)
{
    /*
     * 设置波特率：72MHz / (16 × 115200) → BRR = 0x0271
     */
    USART1->BRR = 0x0271U;

    /*
     * ★ 关键区别：配置 CR1
     *
     * TE：发送器使能
     * RE：接收器使能
     * RXNEIE：接收数据寄存器非空中断使能 ← 新增！
     *
     * 当收到一个字节时：
     *   1. 硬件把数据移入 DR
     *   2. RXNE 标志置 1
     *   3. 因 RXNEIE = 1，USART 向 NVIC 发送中断请求
     *   4. CPU 进入 USART1_IRQHandler()
     */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

    /*
     * 开启 USART 总使能
     */
    USART1->CR1 |= USART_CR1_UE;

    /*
     * ★ 关键区别：NVIC 配置
     *
     * USART1_IRQn = USART1 的中断号（查手册或 stm32f1xx.h）
     * 与定时器、ADC 中断一样，NVIC 是"总闸"。
     *
     * 如果只设了 RXNEIE 但不开 NVIC：
     *   USART 能发出中断请求，但 CPU 不会响应。
     *
     * 如果只开了 NVIC 但没设 RXNEIE：
     *   NVIC 允许了 USART1 中断，但 USART 不会产生中断请求。
     */
    NVIC_SetPriority(USART1_IRQn, 1U);
    NVIC_EnableIRQ(USART1_IRQn);
}

/*
 * usart1_send_byte —— 发送一个字节（轮询，与上一课相同）
 *
 * 发送仍然用轮询，因为发送是 CPU 主动的，不需要中断。
 * 如果使用中断发送，复杂度会增加，收益却不大（等待时间约 86us 一个字节）。
 * 本课只做"中断接收"——这是最常用、收益最大的场景。
 */
static void usart1_send_byte(uint8_t byte)
{
    while ((USART1->SR & USART_SR_TXE) == 0U) {
    }

    USART1->DR = byte;
}

/*
 * usart1_send_string —— 发送字符串
 */
static void usart1_send_string(const char *str)
{
    while (*str != '\0') {
        usart1_send_byte((uint8_t)*str);
        str++;
    }
}

/* LED 控制辅助函数 */
static void led_on(void)
{
    GPIOC->BRR = GPIO_BRR_BR13;
}

static void led_off(void)
{
    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void led_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        led_on();
    } else {
        led_off();
    }
}

/*
 * USART1_IRQHandler —— USART1 中断服务函数
 *
 * 什么时候进入这个函数？
 *   当 RXNEIE = 1 且 USART1 收到一个字节时，CPU 跳转到这里。
 *
 * 为什么函数名必须是 USART1_IRQHandler？
 *   这个名称在启动文件（startup_stm32f103xb.s）的中断向量表中定义。
 *   CPU 根据中断向量表跳转，所以函数名必须与向量表一致。
 *
 * 中断处理原则：
 *   1. 快速判断中断源（检查 SR 标志）
 *   2. 尽快读取数据（读 DR）
 *   3. 设置标志通知主循环
 *   4. 不要在这里做复杂逻辑
 */
void USART1_IRQHandler(void)
{
    /*
     * 确认本次中断确实是因为 RXNE 置位
     *
     * USART1 的中断可能由多个来源触发：
     *   RXNE：收到新数据 ← 本课关注的
     *   TC：发送完成
     *   TXE：发送寄存器空
     *   IDLE：总线空闲
     *   等等
     *
     * 所以先检查 SR.RXNE，确认是"收到数据"引起的中断。
     */
    if ((USART1->SR & USART_SR_RXNE) != 0U) {
        /*
         * 读取 DR：
         *   1. 取出收到的字节
         *   2. 读取 DR 后，硬件自动清除 RXNE 标志
         *      （与 ADC 的 EOC 类似，读 DR 即清标志）
         *
         * 为什么不先读 SR 再读 DR？
         *   先判断 SR.RXNE，确认有数据，再读 DR。
         *   这是标准写法——防止 RXNE=0 时误读无效数据。
         */
        g_rx_byte = (uint8_t)USART1->DR;

        /*
         * 设置"收到新数据"标志
         *
         * 主循环检测到 g_rx_ready = 1 后，会来取走数据并处理。
         * 这就完成了"中断保存 → 主循环处理"的交接。
         *
         * 思考：如果主循环处理速度慢，而中断频繁进入，
         * 数据会被覆盖。更健壮的做法是用环形缓冲区。
         */
        g_rx_ready = 1U;
    }
}

/*
 * main —— 主函数
 *
 * 与上一课（轮询版）的区别：
 *   轮询版：主循环中不断调用 usart1_receive_byte_nonblocking() 检查 RXNE。
 *   中断版：主循环等待 g_rx_ready 标志，由中断通知有新数据。
 *
 *   轮询版：CPU 主动去问"有数据吗？"
 *   中断版：CPU 做其他事（甚至空闲），数据到了由中断告知。
 *
 * 注意：
 *   本课主循环"空闲"是因为没有其他任务。
 *   在真实项目中，主循环可以处理传感器、刷新显示等，
 *   收到串口命令后自动响应。
 */
int main(void)
{
    uint8_t ch;

    system_clock_72mhz_init();
    led_pc13_init();
    usart1_gpio_init();
    usart1_init();

    usart1_send_string("\r\n[reg] USART1 interrupt demo ready.\r\n");
    usart1_send_string("Send '1' to LED ON, '0' to LED OFF, 't' to TOGGLE.\r\n");

    while (1) {
        /*
         * 检查是否有新数据从中断中送达
         *
         * 当 USART1 收到一个字节时：
         *   中断函数 USART1_IRQHandler() 被调用
         *   → g_rx_byte 被更新
         *   → g_rx_ready 被置 1
         *
         * 主循环检测到 g_rx_ready = 1 后：
         *   → 取走 g_rx_byte 到局部变量 ch
         *   → 清 g_rx_ready = 0
         *   → 处理命令
         *
         * 为什么不直接在 g_rx_byte 上操作？
         *   把数据从"共享变量"搬到"局部变量"，
         *   可以避免在后续处理中再次被中断修改。
         */
        if (g_rx_ready != 0U) {
            ch = g_rx_byte;
            g_rx_ready = 0U;

            /* 回显收到的字符 */
            usart1_send_string("RX: ");
            usart1_send_byte(ch);
            usart1_send_string("\r\n");

            /* 命令解析 */
            if (ch == '1') {
                led_on();
                usart1_send_string("LED ON\r\n");
            } else if (ch == '0') {
                led_off();
                usart1_send_string("LED OFF\r\n");
            } else if (ch == 't' || ch == 'T') {
                led_toggle();
                usart1_send_string("LED TOGGLE\r\n");
            } else {
                usart1_send_string("Unknown command. Use 1 / 0 / t\r\n");
            }
        }

        /*
         * 没有收到数据时，CPU 可以在这里做其他任务。
         * 这就是中断方式解放 CPU 的意义所在。
         */
    }
}