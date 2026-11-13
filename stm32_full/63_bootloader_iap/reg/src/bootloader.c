#include "stm32f1xx.h"

/*
 * 寄存器版：最小 Bootloader。
 *
 * 本课只做三件事：
 *   1. 上电后先运行自己（因为它被烧在 0x08000000）
 *   2. 留出 3 秒窗口，收到 'u' 就停在 Bootloader 等待（第 64 课在这里接升级协议）
 *   3. 窗口结束且 app 有效时，把 CPU 交给 0x08008000 的 app
 *
 * 关键前提：
 *   - bootloader 链接在 0x08000000，app 链接在 0x08008000
 *   - app 的向量表必须搬到 0x08008000，否则中断会跑回 bootloader 的向量表
 *   - 跳转前要关中断、停 SysTick、把 MSP 设成 app 向量表的第一个字
 */

#define APP_START_ADDR   0x08008000UL
#define APP_END_ADDR     0x08010000UL
#define SRAM_START_ADDR  0x20000000UL
#define SRAM_END_ADDR    0x20005000UL
#define BOOT_WAIT_MS     3000U

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
static uint8_t app_is_valid(void);
static void jump_to_app(void);

void SysTick_Handler(void)
{
    g_ms_ticks++;
}

/*
 * 72MHz 时钟初始化。
 * 本步骤在 03 课已经完整讲过：HSE 8MHz -> PLL x9 -> SYSCLK 72MHz。
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
 * SysTick 1ms 中断。
 * 本课用它在 115200 的串口等待窗口里做非阻塞计时，
 * 这样等待 'u' 的同时还能按毫秒判断是否超时。
 */
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

/*
 * USART1：PA9 复用推挽输出，PA10 浮空输入，115200 8N1。
 * 21 课已讲过寄存器配置，本课只用它打印状态和接收一个触发字符。
 */
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

    /* 72MHz / 115200 = 625 = 0x0271 */
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

/*
 * 非阻塞读一个字节。
 * 返回 -1 表示当前没有数据；返回 0~255 表示读到了字节。
 * 这里必须读 DR 清 RXNE，否则下一次判断会一直认为有数据。
 */
static int uart1_try_getc(void)
{
    if ((USART1->SR & USART_SR_RXNE) == 0U) {
        return -1;
    }
    return (int)(uint8_t)USART1->DR;
}

/*
 * 检查 app 是否“看起来可跳转”。
 *
 * 两个条件都来自 app 向量表的头两个字：
 *   app[0] = 初始 MSP（栈顶），必须落在 SRAM 范围内
 *   app[1] = Reset_Handler 地址，必须落在 app 区且最低位为 1（Thumb）
 *
 * 为什么必须查：
 *   空片、擦除过的 Flash 读出来是 0xFFFFFFFF。
 *   直接跳过去会把 MSP 设成 0xFFFFFFFF，取指也落在非法地址，
 *   表现为 HardFault 后死机，看起来像“bootloader 把板子跑死了”。
 */
static uint8_t app_is_valid(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);

    if ((app_sp < SRAM_START_ADDR) || (app_sp > SRAM_END_ADDR)) {
        return 0U;
    }

    if ((app_reset < APP_START_ADDR) || (app_reset >= APP_END_ADDR)) {
        return 0U;
    }

    if ((app_reset & 1U) == 0U) {
        return 0U;
    }

    return 1U;
}

/*
 * 跳转到 app。
 *
 * 顺序不能乱：
 *   1. 关全局中断：防止跳转过程中 SysTick 或串口中断闯入
 *   2. 停 SysTick：它的向量还在 bootloader 表里，app 会重新初始化
 *   3. 清 NVIC 使能/挂起位：把 bootloader 留下的中断状态清干净
 *   4. 设 VTOR 到 app 向量表：app 的中断入口才会被 CPU 找到
 *   5. 设 MSP 为 app 栈顶：app 使用自己的栈空间
 *   6. 开中断后跳到 Reset_Handler
 *
 * 错误后果：
 *   - 不设 VTOR：app 里任何中断都会跳回 bootloader 的向量表，
 *     轻则 HAL_Delay 不走，重则进 HardFault
 *   - 不设 MSP：app 继续用 bootloader 的栈，栈空间重叠后数据被踩
 */
static void jump_to_app(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);
    uint32_t i;

    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    for (i = 0U; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    SCB->VTOR = APP_START_ADDR;
    __set_MSP(app_sp);
    __enable_irq();

    ((void (*)(void))app_reset)();
}

int main(void)
{
    uint32_t start_ms;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    uart1_init();

    uart1_puts("\n[BOOT] STM32F103 bootloader v1.0");
    uart1_puts("\n[BOOT] app region: 0x08008000 - 0x0800FFFF");

    if (app_is_valid() == 0U) {
        uart1_puts("\n[BOOT] app invalid, stay in bootloader");
        while (1) {
            led_toggle();
            delay_ms(500U);
        }
    }

    uart1_puts("\n[BOOT] send 'u' within 3s to stay, others jump to app");

    start_ms = g_ms_ticks;
    while ((g_ms_ticks - start_ms) < BOOT_WAIT_MS) {
        int ch = uart1_try_getc();

        if ((ch == 'u') || (ch == 'U')) {
            uart1_puts("\n[BOOT] stay in bootloader mode");
            while (1) {
                led_toggle();
                delay_ms(500U);
            }
        }
    }

    uart1_puts("\n[BOOT] jump to app\n");
    delay_ms(20U);

    jump_to_app();

    /* 正常永远不会到这里 */
    while (1) {
    }
}
