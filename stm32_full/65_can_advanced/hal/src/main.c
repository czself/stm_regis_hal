#include "stm32f1xx_hal.h"
#include <string.h>

/*
 * HAL 版：bxCAN 进阶。
 *
 * 与寄存器版功能一一对应：
 *   CAN1->IER.FMPIE0/1         -> HAL_CAN_ActivateNotification(CAN_IT_RX_FIFO0/FIFO1_MSG_PENDING)
 *   过滤器的 FR1/FR2/FFA1R     -> HAL_CAN_ConfigFilter(CAN_FilterTypeDef)
 *   RX0/RX1 中断入口           -> HAL_CAN_IRQHandler + 两个回调
 *   ESR.BOFF 手动恢复          -> 读 ESR 后 HAL_CAN_Stop/Start
 *
 * 注意 HAL_CAN_AddTxMessage 会一次性写 TDHR/TDLR 的 8 个字节，
 * 即使 DLC=2 也要传一个至少 8 字节的数组，否则 HAL 会越界读。
 */

#define CAN_STD_ID_DATA1   0x123U
#define CAN_STD_ID_DATA2   0x456U
#define CAN_STD_ID_REJECT  0x789U
#define CAN_PERIOD_MS      500U
#define ROUNDS_PER_CHECK   10U

static CAN_HandleTypeDef hcan;
static UART_HandleTypeDef huart1;

static volatile uint32_t g_fifo0_count = 0U;
static volatile uint32_t g_fifo1_count = 0U;
static volatile uint32_t g_remote_count = 0U;
static volatile uint32_t g_other_count = 0U;
static volatile uint32_t g_busoff_count = 0U;
static volatile uint8_t g_remote_pending = 0U;

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void uart1_init(void);
static void uart1_puts(const char *text);
static void uart1_put_u32(uint32_t value);
static void can_gpio_init(void);
static void can1_init(void);
static void can1_filter_init(void);
static uint8_t can1_send_frame(uint16_t std_id, uint32_t rtr, uint8_t *data, uint8_t len);
static void can1_recover_busoff(void);
static void can_report_counts(void);
static void error_handler(void);

int main(void)
{
    uint8_t data1[8] = {0x11U, 0x22U};
    uint8_t data2[8] = {0x33U, 0x44U};
    uint8_t rejected[8] = {0xAAU, 0xBBU};
    uint8_t reply[8] = {0x5AU, 0xA5U};
    uint32_t round = 0U;
    uint32_t pass = 0U;

    HAL_Init();
    system_clock_72mhz_init();
    led_pc13_init();
    uart1_init();
    can_gpio_init();
    can1_init();
    can1_filter_init();

    if (HAL_CAN_Start(&hcan) != HAL_OK) {
        error_handler();
    }

    /*
     * 打开 FIFO0/FIFO1 的报文挂起中断。
     * 对应寄存器版 IER 的 FMPIE0/FMPIE1。
     */
    if (HAL_CAN_ActivateNotification(&hcan,
            CAN_IT_RX_FIFO0_MSG_PENDING |
            CAN_IT_RX_FIFO1_MSG_PENDING) != HAL_OK) {
        error_handler();
    }

    uart1_puts("\r\n[CAN-ADV-HAL] loopback 500k, filter 0x123->F0, 0x456->F1\r\n");

    while (1) {
        (void)can1_send_frame(CAN_STD_ID_DATA1, CAN_RTR_DATA, data1, 2U);
        (void)can1_send_frame(CAN_STD_ID_REJECT, CAN_RTR_DATA, rejected, 2U);
        (void)can1_send_frame(CAN_STD_ID_DATA2, CAN_RTR_DATA, data2, 2U);
        (void)can1_send_frame(CAN_STD_ID_DATA1, CAN_RTR_REMOTE, data1, 2U);

        HAL_Delay(20U);

        if (g_remote_pending != 0U) {
            g_remote_pending = 0U;
            (void)can1_send_frame(CAN_STD_ID_DATA1, CAN_RTR_DATA, reply, 2U);
        }

        HAL_Delay(CAN_PERIOD_MS);
        round++;

        can1_recover_busoff();

        if ((round % ROUNDS_PER_CHECK) == 0U) {
            uint32_t fifo0 = g_fifo0_count;
            uint32_t fifo1 = g_fifo1_count;
            uint32_t remote = g_remote_count;
            uint32_t other = g_other_count;

            can_report_counts();

            pass = ((fifo0 >= (ROUNDS_PER_CHECK * 3U)) &&
                    (fifo1 >= ROUNDS_PER_CHECK) &&
                    (remote >= ROUNDS_PER_CHECK) &&
                    (other == 0U)) ? 1U : 0U;

            if (pass != 0U) {
                HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
            } else {
                uart1_puts("[CAN-ADV-HAL] check failed\r\n");
            }
        }
    }
}

static void can_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
}

static void can1_init(void)
{
    __HAL_RCC_CAN1_CLK_ENABLE();

    hcan.Instance = CAN1;
    hcan.Init.Prescaler = 4U;
    hcan.Init.Mode = CAN_MODE_LOOPBACK;
    hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan.Init.TimeSeg1 = CAN_BS1_13TQ;
    hcan.Init.TimeSeg2 = CAN_BS2_4TQ;
    hcan.Init.TimeTriggeredMode = DISABLE;
    hcan.Init.AutoBusOff = DISABLE;
    hcan.Init.AutoWakeUp = DISABLE;
    hcan.Init.AutoRetransmission = ENABLE;
    hcan.Init.ReceiveFifoLocked = DISABLE;
    hcan.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan) != HAL_OK) {
        error_handler();
    }
}

/*
 * 两个过滤器组：
 *   组 0 精确匹配 0x123，进 FIFO0
 *   组 1 精确匹配 0x456，进 FIFO1
 *
 * 32 位过滤器下，StdId 位于高 16 位的 bit15~5，
 * 所以 FilterIdHigh = StdId << 5，FilterIdLow = 0。
 * 掩码 0x7FF << 5 表示 11 位 ID 全部必须匹配。
 */
static void can1_filter_init(void)
{
    CAN_FilterTypeDef filter = {0};

    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterActivation = ENABLE;

    filter.FilterBank = 0;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterIdHigh = (uint16_t)(CAN_STD_ID_DATA1 << 5);
    filter.FilterIdLow = 0x0000U;
    filter.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5);
    filter.FilterMaskIdLow = 0x0000U;
    if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK) {
        error_handler();
    }

    filter.FilterBank = 1;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO1;
    filter.FilterIdHigh = (uint16_t)(CAN_STD_ID_DATA2 << 5);
    filter.FilterIdLow = 0x0000U;
    filter.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5);
    filter.FilterMaskIdLow = 0x0000U;
    if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK) {
        error_handler();
    }
}

static uint8_t can1_send_frame(uint16_t std_id, uint32_t rtr, uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef header = {0};
    uint32_t mailbox = 0U;

    header.StdId = std_id;
    header.ExtId = 0U;
    header.IDE = CAN_ID_STD;
    header.RTR = rtr;
    header.DLC = len;
    header.TransmitGlobalTime = DISABLE;

    return (HAL_CAN_AddTxMessage(&hcan, &header, data, &mailbox) == HAL_OK) ? 1U : 0U;
}

static void can1_recover_busoff(void)
{
    if ((hcan.Instance->ESR & CAN_ESR_BOFF) == 0U) {
        return;
    }

    g_busoff_count++;

    /*
     * 真实总线上 Bus-Off 后必须重新初始化。
     * 回环模式不会走到这里，本函数用于对照 65 课文档里的总线排错流程。
     */
    (void)HAL_CAN_Stop(&hcan);
    (void)HAL_CAN_Start(&hcan);

    /*
     * Stop/Start 之后中断使能可能被清掉，必须重新打开，
     * 否则总线恢复了但再也收不到 FIFO 中断。
     */
    (void)HAL_CAN_ActivateNotification(&hcan,
        CAN_IT_RX_FIFO0_MSG_PENDING |
        CAN_IT_RX_FIFO1_MSG_PENDING);
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

    esr = hcan.Instance->ESR;

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
    uart1_puts("\r\n");
}

void CAN1_RX0_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&hcan);
}

void CAN1_RX1_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&hcan);
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan_ptr)
{
    CAN_RxHeaderTypeDef header = {0};
    uint8_t data[8] = {0};

    while (HAL_CAN_GetRxFifoFillLevel(hcan_ptr, CAN_RX_FIFO0) > 0U) {
        if (HAL_CAN_GetRxMessage(hcan_ptr, CAN_RX_FIFO0, &header, data) != HAL_OK) {
            break;
        }

        if (header.StdId == CAN_STD_ID_DATA1) {
            g_fifo0_count++;
            if (header.RTR == CAN_RTR_REMOTE) {
                g_remote_count++;
                g_remote_pending = 1U;
            }
        } else {
            g_other_count++;
        }
    }
}

void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *hcan_ptr)
{
    CAN_RxHeaderTypeDef header = {0};
    uint8_t data[8] = {0};

    while (HAL_CAN_GetRxFifoFillLevel(hcan_ptr, CAN_RX_FIFO1) > 0U) {
        if (HAL_CAN_GetRxMessage(hcan_ptr, CAN_RX_FIFO1, &header, data) != HAL_OK) {
            break;
        }

        if (header.StdId == CAN_STD_ID_DATA2) {
            g_fifo1_count++;
        } else {
            g_other_count++;
        }
    }
}

static void system_clock_72mhz_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL = RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        error_handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        error_handler();
    }
}

static void led_pc13_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_13;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
}

static void uart1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200U;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        error_handler();
    }
}

static void uart1_puts(const char *text)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)text, (uint16_t)strlen(text), 100U);
}

static void uart1_put_u32(uint32_t value)
{
    char digits[10];
    uint8_t count = 0U;

    if (value == 0U) {
        uart1_puts("0");
        return;
    }
    while ((value > 0U) && (count < 10U)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        char ch = digits[--count];
        HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1U, 20U);
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

static void error_handler(void)
{
    __disable_irq();
    while (1) {
    }
}
