#include "stm32f1xx.h"

/*
 * 寄存器版：OTA 演示用的 app。
 *
 * 它和 63 课 app 的区别只有两点：
 *   1. 串口收到 'r' 时执行 NVIC_SystemReset()，重新回到 bootloader 的 5 秒窗口
 *   2. 打印自己的版本号，升级后能直观看出“新固件真的跑起来了”
 *
 * 注意：NVIC_SystemReset() 是内核复位，不会断开 SWD，
 * 也不会像按复位键那样让 ST-Link 掉线，适合 OTA 调试。
 */

#ifndef APP_START_ADDR
#define APP_START_ADDR 0x08008000UL
#endif

#define APP_VERSION "v2.0"

static volatile uint32_t g_ms_ticks = 0U;

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_toggle(void);
static void uart1_init(void);
static void uart1_putc(char ch);
static void uart1_puts(const char *text);
static int uart1_try_getc(void);

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

static int uart1_try_getc(void)
{
    if ((USART1->SR & USART_SR_RXNE) == 0U) {
        return -1;
    }
    return (int)(uint8_t)USART1->DR;
}

int main(void)
{
    uint32_t last_print_ms;

    SCB->VTOR = APP_START_ADDR;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    uart1_init();

    uart1_puts("\n[APP-OTA67] " APP_VERSION " running at 0x08008000");
    uart1_puts("\n[APP-OTA67] send 'r' to reboot into bootloader\n");

    last_print_ms = g_ms_ticks;

    while (1) {
        int ch = uart1_try_getc();

        if ((ch == 'r') || (ch == 'R')) {
            uart1_puts("[APP-OTA67] reboot request\n");
            delay_ms(20U);
            NVIC_SystemReset();
        }

        led_toggle();
        delay_ms(250U);

        if ((g_ms_ticks - last_print_ms) >= 1000U) {
            last_print_ms = g_ms_ticks;
            uart1_puts("[APP-OTA67] alive\n");
        }
    }
}
