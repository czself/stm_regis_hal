#include "stm32f1xx.h"

/*
 * 寄存器版：运行在 0x08008000 的 app。
 *
 * 这个文件看起来和 01 课点灯很像，但它多了一个 Bootloader 场景下
 * 必须做的动作：把向量表地址改到自己身上（SCB->VTOR）。
 *
 * 如果忘记改 VTOR：
 *   SysTick 中断会按 0x08000000 处的向量表跳转，进入 bootloader 的
 *   SysTick_Handler。那个函数操作的是 bootloader 的变量 g_ms_ticks，
 *   app 自己的计数永远不增加，delay_ms() 会卡死。
 */

#ifndef APP_START_ADDR
#define APP_START_ADDR 0x08008000UL
#endif

static volatile uint32_t g_ms_ticks = 0U;

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_toggle(void);
static void uart1_init(void);
static void uart1_putc(char ch);
static void uart1_puts(const char *text);

void SysTick_Handler(void)
{
    g_ms_ticks++;
}

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

static void systick_init(void)
{
    SysTick->LOAD = 72000U - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms_ticks;
    while ((g_ms_ticks - start) < ms) {
    }
}

static void led_pc13_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;

    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;

    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void led_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

static void uart1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;

    GPIOA->CRH &= ~(GPIO_CRH_MODE9 |
                    GPIO_CRH_CNF9 |
                    GPIO_CRH_MODE10 |
                    GPIO_CRH_CNF10);

    GPIOA->CRH |= GPIO_CRH_MODE9_1;
    GPIOA->CRH |= GPIO_CRH_CNF9_1;
    GPIOA->CRH |= GPIO_CRH_CNF10_0;

    USART1->BRR = 0x0271U;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void uart1_putc(char ch)
{
    while ((USART1->SR & USART_SR_TXE) == 0U) {
    }
    USART1->DR = (uint8_t)ch;
}

static void uart1_puts(const char *text)
{
    while (*text != '\0') {
        if (*text == '\n') {
            uart1_putc('\r');
        }
        uart1_putc(*text);
        text++;
    }
}

int main(void)
{
    uint32_t last_print_ms;

    /*
     * 第一件事就是搬向量表。
     * app 由 bootloader 跳转进入，此时 VTOR 还指向 0x08000000；
     * 在打开任何中断之前改成 APP_START_ADDR，后面所有中断才属于 app。
     */
    SCB->VTOR = APP_START_ADDR;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    uart1_init();

    uart1_puts("\n[APP] bootloader jump ok, running at 0x08008000");
    uart1_puts("\n[APP] PC13 blinks at 2Hz");

    last_print_ms = g_ms_ticks;

    while (1) {
        led_toggle();
        delay_ms(250U);

        if ((g_ms_ticks - last_print_ms) >= 1000U) {
            last_print_ms = g_ms_ticks;
            uart1_puts("[APP] alive\n");
        }
    }
}
