#include "stm32f1xx.h"

/*
 * 寄存器版：TIM1 高级定时器 PWM。
 *
 * 前面已经学过 TIM2/TIM3 的 PWM 输出。本课的新重点是：
 * TIM1 是高级定时器，除了通道使能 CC1E 之外，还必须打开 BDTR.MOE。
 *
 * 可以把 CC1E 理解成“通道 1 的门”，把 MOE 理解成“高级定时器总闸”。
 * 只开 CC1E 不开 MOE，TIM1 内部在跑，PA8 也可能没有 PWM 输出。
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

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

static void pc13_led_init(void)
{
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

static void pa8_tim1_ch1_pin_init(void)
{
    /*
     * PA8 是 TIM1_CH1 的默认复用输出脚。
     * AFIO 用于 F1 的复用功能体系；本课不重映射，但打开它让链路完整。
     */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;

    GPIOA->CRH &= ~(GPIO_CRH_MODE8 | GPIO_CRH_CNF8);
    GPIOA->CRH |= GPIO_CRH_MODE8_1 | GPIO_CRH_CNF8_1;
}

static void tim1_ch1_pwm_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;

    /*
     * PWM 参数沿用前面课程的思路：
     * 72MHz / 72 = 1MHz，ARR=999 得到 1kHz PWM。
     */
    TIM1->PSC = 72U - 1U;
    TIM1->ARR = 1000U - 1U;

    /*
     * CCR1=300 表示 PWM mode 1 下约 30% 占空比。
     */
    TIM1->CCR1 = 300U;

    /*
     * OC1M = 110：PWM mode 1。
     * OC1PE = 1：开启比较值预装载，让 CCR1 更新更稳。
     */
    TIM1->CCMR1 &= ~(TIM_CCMR1_CC1S | TIM_CCMR1_OC1M);
    TIM1->CCMR1 |= TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE;

    /*
     * CC1E 只打开通道 1 输出路径。
     * 对 TIM1 这种高级定时器，这还不够，下面还必须打开 MOE。
     */
    TIM1->CCER |= TIM_CCER_CC1E;

    /*
     * BDTR.MOE 是高级定时器的主输出使能。
     * 它存在的意义是给电机/功率驱动一个总安全门：刹车、死区、互补输出都要经过这里。
     *
     * 本课不展开死区和刹车，但必须记住：
     * TIM1/TIM8 输出 PWM 到引脚前，MOE 是最后一道闸。
     */
    TIM1->BDTR |= TIM_BDTR_MOE;

    TIM1->CR1 |= TIM_CR1_ARPE;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->CR1 |= TIM_CR1_CEN;
}

int main(void)
{
    system_clock_72mhz_init();
    pc13_led_init();
    pa8_tim1_ch1_pin_init();
    tim1_ch1_pwm_init();

    while (1) {
        pc13_toggle();
        delay_cycles(3600000U);
    }
}