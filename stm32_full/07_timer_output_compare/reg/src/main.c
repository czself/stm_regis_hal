#include "stm32f1xx.h"

/*
 * 寄存器版：TIM 输出比较。
 *
 * 06 已经讲过 PSC、ARR、更新事件和定时器中断。
 * 本课的新重点是“比较匹配”：
 * - CNT 一直按定时器时钟计数
 * - CCR1 保存一个比较值
 * - 当 CNT == CCR1 时，TIM2_CH1 可以自动改变 PA0 输出
 *
 * 这和中断里翻转 GPIO 不同。这里 PA0 的翻转由定时器通道硬件完成，
 * CPU 主循环不需要参与 TIM2_CH1 的每次边沿。
 */

static void system_clock_72mhz_init(void)
{
    /* 时钟树前面已经讲过，这里保留 72MHz 配置作为定时器频率前提。 */
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

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

static void pc13_led_init(void)
{
    /* PC13 只是心跳指示灯，GPIO 输出细节已经在 01 讲过。 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;

    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;

    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void pc13_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

static void delay_cycles(volatile uint32_t cycles)
{
    while (cycles-- != 0U) {
        __NOP();
    }
}

static void pa1_input_init(void)
{
    /*
     * PA1 配成浮空输入，用来读取 PA0 输出的方波电平。
     * 用杜邦线把 PA0 接到 PA1，PA1 读到的就是 TIM2_CH1 的输出状态。
     * MODE1=00（输入），CNF1=01（浮空输入）。
     *
     * 注意：CNF1=00 是模拟输入，施密特触发器被关掉，IDR 读不到正确值！
     * 必须设 CNF1=01 才是浮空输入，数字读 IDR 才有效。
     */
    GPIOA->CRL &= ~(GPIO_CRL_MODE1 | GPIO_CRL_CNF1);
    GPIOA->CRL |= GPIO_CRL_CNF1_0;
}

static void uart1_init(void)
{
    /*
     * USART1 挂在 APB2 总线上，用于把 PA1 电平值发给 VOFA+ 观察波形。
     * PA9 = TX（复用推挽），PA10 = RX（浮空输入，本课不用接收）。
     */
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    GPIOA->CRH &= ~(GPIO_CRH_MODE9 | GPIO_CRH_CNF9);
    GPIOA->CRH |= GPIO_CRH_MODE9_1 | GPIO_CRH_CNF9_1;

    GPIOA->CRH &= ~(GPIO_CRH_MODE10 | GPIO_CRH_CNF10);

    /*
     * 波特率 115200，USART1 时钟 = 72MHz（APB2）。
     * BRR = 72MHz / 115200 = 625 = 0x271
     * 整数部分 625 → 0x27，小数部分 0 → 0x0
     * BRR = (625 << 0) = 0x0271
     */
    USART1->BRR = 0x0271U;

    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void uart1_send_byte(uint8_t ch)
{
    USART1->DR = ch;
    while ((USART1->SR & USART_SR_TXE) == 0U) {
    }
}

static void uart1_send_str(const char *s)
{
    while (*s != '\0') {
        uart1_send_byte((uint8_t)*s);
        s++;
    }
}

static void pa0_tim2_ch1_pin_init(void)
{
    /*
     * PA0 要交给 TIM2_CH1 驱动，所以 GPIOA 时钟必须打开。
     * AFIO 是 F1 的复用功能模块；即使这里没有重映射，打开它能让“复用输出”
     * 这条链路更清楚。
     */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;

    /*
     * PA0 配成复用推挽输出：
     * - MODE0 = 10：输出速度 2MHz
     * - CNF0  = 10：复用推挽输出
     *
     * 配成普通推挽输出也能由 CPU 写电平，但 TIM2_CH1 的比较输出就接不到引脚上。
     */
    GPIOA->CRL &= ~(GPIO_CRL_MODE0 | GPIO_CRL_CNF0);
    GPIOA->CRL |= GPIO_CRL_MODE0_1 | GPIO_CRL_CNF0_1;
}

static void tim2_ch1_output_compare_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    /*
     * 沿用 06 的定时器频率计算：
     * - TIM2 输入时钟 = 72MHz
     * - PSC = 7200 - 1 后，CNT 计数频率为 10kHz
     * - ARR = 10000 - 1 后，一个完整周期为 1 秒
     */
    TIM2->PSC = 7200U - 1U;
    TIM2->ARR = 10000U - 1U;

    /*
     * CCR1 是通道 1 的比较值。
     *
     * CNT 从 0 数到 9999。
     * 当 CNT 数到 5000 时发生通道 1 比较匹配，相当于周期中间的 0.5 秒位置。
     */
    TIM2->CCR1 = 5000U;

    /*
     * CC1S = 00：通道 1 工作在输出模式，不是输入捕获。
     * OC1M = 011：比较匹配时翻转输出电平，也就是 toggle mode。
     */
    TIM2->CCMR1 &= ~(TIM_CCMR1_CC1S | TIM_CCMR1_OC1M);
    TIM2->CCMR1 |= TIM_CCMR1_OC1M_0;
    TIM2->CCMR1 |= TIM_CCMR1_OC1M_1;

    /*
     * CC1E 打开通道 1 输出。
     * 如果漏掉这一步，定时器内部会比较成功，但 PA0 看不到输出变化。
     */
    TIM2->CCER |= TIM_CCER_CC1E;

    /*
     * 产生一次更新事件，把 PSC/ARR 这些缓冲配置装入实际计数逻辑。
     */
    TIM2->EGR = TIM_EGR_UG;

    TIM2->CR1 |= TIM_CR1_CEN;
}

int main(void)
{
    system_clock_72mhz_init();
    pc13_led_init();
    pa0_tim2_ch1_pin_init();
    pa1_input_init();
    uart1_init();
    tim2_ch1_output_compare_init();

    while (1) {
        /*
         * 读 PA1 电平：PA0 通过杜邦线接到 PA1，
         * PA1 读到的就是 TIM2_CH1 输出比较产生的方波。
         * IDR1 = 1 表示高电平，IDR1 = 0 表示低电平。
         */
        uint8_t level = (GPIOA->IDR & GPIO_IDR_IDR1) ? 1U : 0U;

        /*
         * FireWater 协议：每行一个数值，以 \n 结尾。
         * VOFA+ 收到 "0\n" 或 "1\n" 后在波形窗口显示跳变。
         */
        uart1_send_byte('0' + level);
        uart1_send_byte('\n');

        pc13_toggle();
        delay_cycles(360000U);
    }
}