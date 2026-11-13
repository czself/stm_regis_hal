#include "stm32f1xx.h"

/*
 * 寄存器版：bxCAN 进阶。
 *
 * 32 课已经讲过“能发能收”：初始化、BTR、全放行过滤器、邮箱/FIFO 读写。
 * 本课不再重复这些步骤，只补四个真实总线必须会用的能力：
 *
 *   1. 精确过滤器：只让 0x123 / 0x456 进 FIFO，0x789 必须被硬件丢弃
 *   2. 接收中断：FIFO0/FIFO1 有报文才进 ISR，CPU 不再轮询
 *   3. 远程帧：收到 RTR 帧后，用同一 ID 回一帧数据
 *   4. Bus-Off 监测：读 ESR 的 BOFF，必要时重新初始化控制器
 *
 * 为了不依赖外部收发器，本课仍用内部回环模式（BTR.LBKM=1）。
 * 回环模式下发送帧会完整经过“邮箱 -> 总线(内部) -> 过滤器 -> FIFO”，
 * 所以过滤器和中断逻辑与真实总线一致，只是不需要 CANH/CANL 接线。
 */

#define CAN_STD_ID_DATA1   0x123U
#define CAN_STD_ID_DATA2   0x456U
#define CAN_STD_ID_REJECT  0x789U
#define CAN_PERIOD_MS      500U
#define ROUNDS_PER_CHECK   10U
#define CAN_TIMEOUT_COUNT  100000U

static volatile uint32_t g_ms_ticks = 0U;
static volatile uint32_t g_fifo0_count = 0U;
static volatile uint32_t g_fifo1_count = 0U;
static volatile uint32_t g_remote_count = 0U;
static volatile uint32_t g_other_count = 0U;
static volatile uint32_t g_busoff_count = 0U;
static volatile uint8_t g_remote_pending = 0U;

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_toggle(void);
static void uart1_init(void);
static void uart1_puts(const char *text);
static void uart1_put_u32(uint32_t value);
static void can_gpio_init(void);
static uint8_t can1_enter_init_mode(void);
static uint8_t can1_leave_init_mode(void);
static void can1_filter_init(void);
static uint8_t can1_init(void);
static uint8_t can1_send_frame(uint16_t std_id, uint8_t rtr, const uint8_t *data, uint8_t len);
static void can1_recover_busoff(void);
static void can_report_counts(void);

void SysTick_Handler(void)
{
    g_ms_ticks++;
}

/*
 * FIFO0 中断：精确过滤器 0x123 和远程帧回应的接收端。
 *
 * 为什么先读 RIR 再判断 RTR：
 *   RIR 的 RTR 位告诉 CPU 这一帧是数据帧还是远程帧。
 *   远程帧没有数据，DLC 表示“请求方希望回多少字节”，
 *   如果把它当数据帧解析 RDTR/RDLR，会读到无意义内容。
 */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    while ((CAN1->RF0R & CAN_RF0R_FMP0_Msk) != 0U) {
        uint32_t rir = CAN1->sFIFOMailBox[0].RIR;
        uint16_t std_id = (uint16_t)(rir >> 21);
        uint8_t is_remote = ((rir & CAN_RI0R_RTR) != 0U) ? 1U : 0U;

        if (std_id == CAN_STD_ID_DATA1) {
            g_fifo0_count++;
            if (is_remote != 0U) {
                g_remote_count++;
                g_remote_pending = 1U;
            }
        } else {
            g_other_count++;
        }

        CAN1->RF0R |= CAN_RF0R_RFOM0;
    }
}

/*
 * FIFO1 中断：只接了 0x456 的过滤器。
 * 如果这里收到 0x123，说明过滤器组配置串了。
 *
 * 中断入口名必须是启动文件里的 CAN1_RX1_IRQHandler。
 * 写成 USB_HP_CAN1_TX_IRQHandler 会变成“发送中断”入口，
 * FIFO1 的挂起位永远没人清，程序会反复进同一个中断。
 */
void CAN1_RX1_IRQHandler(void)
{
    while ((CAN1->RF1R & CAN_RF1R_FMP1_Msk) != 0U) {
        uint32_t rir = CAN1->sFIFOMailBox[1].RIR;
        uint16_t std_id = (uint16_t)(rir >> 21);

        if (std_id == CAN_STD_ID_DATA2) {
            g_fifo1_count++;
        } else {
            g_other_count++;
        }

        CAN1->RF1R |= CAN_RF1R_RFOM1;
    }
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

static void uart1_put_u32(uint32_t value)
{
    char digits[10];
    uint8_t count = 0U;

    if (value == 0U) {
        uart1_putc('0');
        return;
    }
    while ((value > 0U) && (count < 10U)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        uart1_putc(digits[--count]);
    }
}

/*
 * PA11/PA12 + CAN1 时钟。
 * 即使使用内部回环，也按真实引脚配置，方便切换到总线模式。
 */
static void can_gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;
    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;

    GPIOA->CRH &= ~(GPIO_CRH_MODE12 | GPIO_CRH_CNF12 |
                    GPIO_CRH_MODE11 | GPIO_CRH_CNF11);

    GPIOA->CRH |= GPIO_CRH_MODE12;
    GPIOA->CRH |= GPIO_CRH_CNF12_1;
    GPIOA->CRH |= GPIO_CRH_CNF11_0;
}

static uint8_t can1_enter_init_mode(void)
{
    uint32_t timeout = CAN_TIMEOUT_COUNT;

    CAN1->MCR |= CAN_MCR_INRQ;
    while (((CAN1->MSR & CAN_MSR_INAK) == 0U) && (timeout > 0U)) {
        timeout--;
    }
    return (timeout > 0U) ? 1U : 0U;
}

static uint8_t can1_leave_init_mode(void)
{
    uint32_t timeout = CAN_TIMEOUT_COUNT;

    CAN1->MCR &= ~CAN_MCR_INRQ;
    while (((CAN1->MSR & CAN_MSR_INAK) != 0U) && (timeout > 0U)) {
        timeout--;
    }
    return (timeout > 0U) ? 1U : 0U;
}

/*
 * 两个 32 位掩码过滤器：
 *
 *   组 0：STID = 0x123，掩码 0x7FF -> 只匹配 0x123，送 FIFO0
 *   组 1：STID = 0x456，掩码 0x7FF -> 只匹配 0x456，送 FIFO1
 *
 * 32 位过滤器里 STDID 位于 bit31~21，所以 FR1 要左移 21 位。
 * 掩码位为 1 表示“这一位必须和 ID 相同”，为 0 表示“不关心”。
 * 掩码写成 0 就是全放行；漏写移位则 ID 落到保留位，永远匹配不到。
 */
static void can1_filter_init(void)
{
    CAN1->FMR |= CAN_FMR_FINIT;

    /* 组 0 -> FIFO0 */
    CAN1->FA1R &= ~CAN_FA1R_FACT0;
    CAN1->FS1R |= CAN_FS1R_FSC0;
    CAN1->FM1R &= ~CAN_FM1R_FBM0;
    CAN1->FFA1R &= ~CAN_FFA1R_FFA0;
    CAN1->sFilterRegister[0].FR1 = (uint32_t)CAN_STD_ID_DATA1 << 21;
    CAN1->sFilterRegister[0].FR2 = (uint32_t)0x7FFU << 21;
    CAN1->FA1R |= CAN_FA1R_FACT0;

    /* 组 1 -> FIFO1 */
    CAN1->FA1R &= ~CAN_FA1R_FACT1;
    CAN1->FS1R |= CAN_FS1R_FSC1;
    CAN1->FM1R &= ~CAN_FM1R_FBM1;
    CAN1->FFA1R |= CAN_FFA1R_FFA1;
    CAN1->sFilterRegister[1].FR1 = (uint32_t)CAN_STD_ID_DATA2 << 21;
    CAN1->sFilterRegister[1].FR2 = (uint32_t)0x7FFU << 21;
    CAN1->FA1R |= CAN_FA1R_FACT1;

    CAN1->FMR &= ~CAN_FMR_FINIT;
}

static uint8_t can1_init(void)
{
    if (can1_enter_init_mode() == 0U) {
        return 0U;
    }

    CAN1->MCR = CAN_MCR_INRQ;

    /* 500kbps：36MHz / (4 x 18) */
    CAN1->BTR = CAN_BTR_LBKM |
                (3U << CAN_BTR_BRP_Pos) |
                (12U << CAN_BTR_TS1_Pos) |
                (3U << CAN_BTR_TS2_Pos);

    can1_filter_init();

    /*
     * 打开 FIFO0/FIFO1 的“报文挂起”中断。
     * FMPIE0/FMPIE1 置 1 后，只要 FIFO 里出现新报文就触发 CAN1_RX0/RX1。
     */
    CAN1->IER |= CAN_IER_FMPIE0 | CAN_IER_FMPIE1;

    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1U);
    NVIC_SetPriority(CAN1_RX1_IRQn, 1U);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    NVIC_EnableIRQ(CAN1_RX1_IRQn);

    if (can1_leave_init_mode() == 0U) {
        return 0U;
    }
    return 1U;
}

/*
 * 发送一帧。rtr=1 表示远程帧（不带数据，只请求对方回数据）。
 * 远程帧的 DLC 表示“我希望你回几个字节”，不是实际数据长度。
 */
static uint8_t can1_send_frame(uint16_t std_id, uint8_t rtr, const uint8_t *data, uint8_t len)
{
    uint32_t timeout = CAN_TIMEOUT_COUNT;
    uint32_t tir;
    uint8_t i;

    if (len > 8U) {
        return 0U;
    }

    while (((CAN1->TSR & CAN_TSR_TME0) == 0U) && (timeout > 0U)) {
        timeout--;
    }
    if (timeout == 0U) {
        return 0U;
    }

    tir = ((uint32_t)std_id << 21);
    if (rtr != 0U) {
        tir |= CAN_TI0R_RTR;
    }
    CAN1->sTxMailBox[0].TIR = tir;
    CAN1->sTxMailBox[0].TDTR = len;

    CAN1->sTxMailBox[0].TDLR = 0U;
    CAN1->sTxMailBox[0].TDHR = 0U;

    /*
     * 远程帧不携带数据，调用者可以传 NULL。
     * 如果这里无条件读 data[i]，发远程帧会直接访问空指针。
     */
    if (rtr == 0U) {
        for (i = 0U; i < len; i++) {
            if (i < 4U) {
                CAN1->sTxMailBox[0].TDLR |= ((uint32_t)data[i] << (8U * i));
            } else {
                CAN1->sTxMailBox[0].TDHR |= ((uint32_t)data[i] << (8U * (i - 4U)));
            }
        }
    }

    CAN1->sTxMailBox[0].TIR |= CAN_TI0R_TXRQ;
    return 1U;
}

/*
 * Bus-Off 恢复。
 *
 * TEC（发送错误计数）超过 255 时，bxCAN 自动进入 Bus-Off：
 * 控制器把自己从总线上摘下来，TSR/邮箱都不再工作。
 * 恢复步骤：请求进入初始化 -> 重新配置 BTR/过滤器 -> 退出初始化。
 *
 * 回环模式不会产生 Bus-Off，本函数是给真实总线用的。
 */
static void can1_recover_busoff(void)
{
    if ((CAN1->ESR & CAN_ESR_BOFF) == 0U) {
        return;
    }

    g_busoff_count++;

    if (can1_enter_init_mode() == 0U) {
        return;
    }

    CAN1->MCR = CAN_MCR_INRQ;
    CAN1->BTR = CAN_BTR_LBKM |
                (3U << CAN_BTR_BRP_Pos) |
                (12U << CAN_BTR_TS1_Pos) |
                (3U << CAN_BTR_TS2_Pos);
    can1_filter_init();
    CAN1->IER |= CAN_IER_FMPIE0 | CAN_IER_FMPIE1;

    (void)can1_leave_init_mode();
}

static void can_report_counts(void)
{
    uint32_t fifo0;
    uint32_t fifo1;
    uint32_t remote;
    uint32_t other;
    uint32_t busoff;
    uint32_t esr;

    __disable_irq();
    fifo0 = g_fifo0_count;
    fifo1 = g_fifo1_count;
    remote = g_remote_count;
    other = g_other_count;
    busoff = g_busoff_count;
    __enable_irq();

    esr = CAN1->ESR;

    uart1_puts("fifo0=");
    uart1_put_u32(fifo0);
    uart1_puts(" fifo1=");
    uart1_put_u32(fifo1);
    uart1_puts(" rtr=");
    uart1_put_u32(remote);
    uart1_puts(" other=");
    uart1_put_u32(other);
    uart1_puts(" tec=");
    uart1_put_u32((esr >> 16) & 0xFFU);
    uart1_puts(" rec=");
    uart1_put_u32((esr >> 24) & 0xFFU);
    uart1_puts(" busoff=");
    uart1_put_u32(busoff);
    uart1_puts("\n");
}

int main(void)
{
    uint8_t data1[2] = {0x11U, 0x22U};
    uint8_t data2[2] = {0x33U, 0x44U};
    uint8_t rejected[2] = {0xAAU, 0xBBU};
    uint8_t reply[2] = {0x5AU, 0xA5U};
    uint32_t round = 0U;
    uint32_t pass = 0U;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    uart1_init();
    can_gpio_init();

    uart1_puts("\n[CAN-ADV] loopback 500kbps, filter 0x123->FIFO0, 0x456->FIFO1\n");

    if (can1_init() == 0U) {
        uart1_puts("[CAN-ADV] init failed\n");
        while (1) {
            led_toggle();
            delay_ms(100U);
        }
    }

    while (1) {
        /* 1. 0x123 数据帧：应进 FIFO0 */
        (void)can1_send_frame(CAN_STD_ID_DATA1, 0U, data1, 2U);

        /* 2. 0x789 数据帧：过滤器不接受，应被硬件丢弃 */
        (void)can1_send_frame(CAN_STD_ID_REJECT, 0U, rejected, 2U);

        /* 3. 0x456 数据帧：应进 FIFO1 */
        (void)can1_send_frame(CAN_STD_ID_DATA2, 0U, data2, 2U);

        /* 4. 0x123 远程帧：ISR 记录，主循环用同 ID 回应 */
        (void)can1_send_frame(CAN_STD_ID_DATA1, 1U, 0, 2U);

        delay_ms(20U);

        if (g_remote_pending != 0U) {
            g_remote_pending = 0U;
            (void)can1_send_frame(CAN_STD_ID_DATA1, 0U, reply, 2U);
        }

        delay_ms(CAN_PERIOD_MS);
        round++;

        can1_recover_busoff();

        if ((round % ROUNDS_PER_CHECK) == 0U) {
            uint32_t fifo0 = g_fifo0_count;
            uint32_t fifo1 = g_fifo1_count;
            uint32_t remote = g_remote_count;
            uint32_t other = g_other_count;

            can_report_counts();

            if ((fifo0 >= (ROUNDS_PER_CHECK * 3U)) &&
                (fifo1 >= ROUNDS_PER_CHECK) &&
                (remote >= ROUNDS_PER_CHECK) &&
                (other == 0U)) {
                pass = 1U;
            } else {
                pass = 0U;
            }

            if (pass != 0U) {
                led_toggle();
            } else {
                uart1_puts("[CAN-ADV] check failed\n");
            }
        }
    }
}
