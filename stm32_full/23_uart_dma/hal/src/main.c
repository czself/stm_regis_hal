#include "stm32f1xx_hal.h"
#include <string.h>

/*
 * 本文件是"HAL版 UART + DMA 发送实验"。
 *
 * 本课目标：
 * 1. 用 HAL 初始化 USART1 和 DMA1_Channel4
 * 2. 使用 HAL_UART_Transmit_DMA() 发送固定字符串
 * 3. 用发送完成回调通知主循环翻转 LED
 *
 * 与寄存器版的对应关系：
 *   HAL_UART_Transmit_DMA() → USART1->CR3.DMAT + DMA 配置 + DMA 使能
 *   __HAL_LINKDMA(&huart1, hdmatx, hdma_usart1_tx) → 关联 UART 和 DMA 句柄
 *   HAL_UART_TxCpltCallback() → DMA1_Channel4_IRQHandler 中的完成处理
 *
 * 为什么需要同时开 USART1_IRQn 和 DMA1_Channel4_IRQn？
 *   DMA 搬完数据后，HAL 还需要配合 USART 的 TC（传输完成）事件做收尾。
 *   所以两个中断都要使能。
 */

#define DMA_TX_PERIOD_MS 1000U

static UART_HandleTypeDef huart1;
static DMA_HandleTypeDef hdma_usart1_tx;

static volatile uint8_t g_uart_dma_busy = 0U;
static volatile uint8_t g_uart_dma_done = 0U;

static uint8_t g_dma_message[] = "[hal] USART1 DMA TX demo running...\r\n";

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void led_toggle(void);
static void usart1_gpio_init(void);
static void dma1_channel4_init(void);
static void usart1_init(void);
static void uart1_send_string_polling(const char *str);
static void error_handler(void);

/*
 * main —— 主函数
 *
 * 流程：
 *   1. HAL 初始化 + 系统时钟
 *   2. LED、USART1 GPIO、DMA、USART1 初始化
 *   3. 轮询发送欢迎信息
 *   4. 主循环：每 1 秒 DMA 发送一次，完成后翻转 LED
 *
 * HAL_UART_Transmit_DMA() 内部做了什么？
 *   1. 通过 huart1.hdmatx 找到关联的 DMA 句柄
 *   2. 配置 DMA 的源地址（字符串）、目标地址（USART1->DR）、长度
 *   3. 使能 USART1 的 DMAT（CR3 寄存器）
 *   4. 启动 DMA 通道
 *   5. 返回 HAL_OK
 *
 * 对应寄存器版 usart1_dma_send() 中的全部操作。
 */
int main(void)
{
    HAL_Init();

    system_clock_72mhz_init();
    led_pc13_init();
    usart1_gpio_init();
    dma1_channel4_init();
    usart1_init();

    uart1_send_string_polling("\r\n[hal] USART1 DMA TX demo ready.\r\n");
    uart1_send_string_polling("A DMA message will be sent every 1 second.\r\n");

    while (1) {
        /*
         * 不忙时启动一轮 DMA 发送。
         *
         * 注意：这里不要清 g_uart_dma_done。
         *   回调里是同时清 busy、置 done 的，两件事在同一个中断里发生。
         *   如果在这个分支里把 done 清掉，下一轮启动会先跑到这里，
         *   在 done 分支翻转 LED 之前就把标志抹掉，导致 LED 几乎不翻转、
         *   1 秒节奏也失效。done 只在它自己的处理分支里清。
         */
        if (g_uart_dma_busy == 0U) {
            g_uart_dma_busy = 1U;

            if (HAL_UART_Transmit_DMA(&huart1, g_dma_message,
                                      (uint16_t)(sizeof(g_dma_message) - 1U)) != HAL_OK) {
                g_uart_dma_busy = 0U;
                error_handler();
            }
        }

        /* 发送完成后翻转 LED，并间隔 1 秒再启动下一轮 */
        if (g_uart_dma_done != 0U) {
            g_uart_dma_done = 0U;
            led_toggle();
            HAL_Delay(DMA_TX_PERIOD_MS);
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

static void led_toggle(void)
{
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
}

static void usart1_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
}

/*
 * dma1_channel4_init —— HAL 版 DMA 初始化
 *
 * 与 ADC+DMA 的关键区别：
 *   1. Direction = DMA_MEMORY_TO_PERIPH（内存→外设）
 *   2. Mode = DMA_NORMAL（非循环，发完即停）
 *   3. DataAlignment = BYTE（8 位，串口是字节单位）
 *   4. Instance = DMA1_Channel4（不是 Channel1）
 *
 * __HAL_LINKDMA(&huart1, hdmatx, hdma_usart1_tx)：
 *   将 UART 句柄的 hdmatx（发送 DMA 句柄）指向 hdma_usart1_tx。
 *   这样 HAL_UART_Transmit_DMA() 才能找到对应的 DMA 通道。
 */
static void dma1_channel4_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_usart1_tx.Instance = DMA1_Channel4;
    hdma_usart1_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;    /* 内存→外设 */
    hdma_usart1_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart1_tx.Init.MemInc = DMA_MINC_ENABLE;            /* 内存地址自增 */
    hdma_usart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;   /* 8 位 */
    hdma_usart1_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;     /* 8 位 */
    hdma_usart1_tx.Init.Mode = DMA_NORMAL;                    /* 非循环 */
    hdma_usart1_tx.Init.Priority = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(&hdma_usart1_tx) != HAL_OK) {
        error_handler();
    }

    /* 关联 UART 句柄和 DMA 句柄 */
    __HAL_LINKDMA(&huart1, hdmatx, hdma_usart1_tx);

    /* DMA 通道中断 */
    HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
}

/*
 * usart1_init —— HAL 版 USART1 初始化
 *
 * 本课只用发送 tx，所以 Mode = UART_MODE_TX。
 *
 * 为什么还要开 USART1_IRQn？
 *   HAL 的 UART DMA 发送完成链路需要配合 USART 的 TC 事件。
 *   DMA 搬完数据后，HAL 需要在 USART 中断中检测 TC 完成收尾。
 *   所以 USART1 的中断也要使能。
 */
static void usart1_init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        error_handler();
    }

    /* USART1 自身的中断也要开（配合 DMA 完成收尾） */
    HAL_NVIC_SetPriority(USART1_IRQn, 1, 1);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

/*
 * 两个中断入口 —— 都需要调用 HAL 的 IRQHandler 进行分发
 */
void DMA1_Channel4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart1_tx);
}

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/*
 * HAL_UART_TxCpltCallback —— DMA 发送完成回调
 *
 * 当 DMA 把整个字符串搬完，并且最后一个字节也从 USART 发送完成后，
 * HAL 会调用这个回调函数。
 *
 * 这里我们置 g_uart_dma_done = 1，通知主循环翻转 LED。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        g_uart_dma_busy = 0U;
        g_uart_dma_done = 1U;
    }
}

static void uart1_send_string_polling(const char *str)
{
    if (HAL_UART_Transmit(&huart1, (uint8_t *)str,
                          (uint16_t)strlen(str), HAL_MAX_DELAY) != HAL_OK) {
        error_handler();
    }
}

static void error_handler(void)
{
    __disable_irq();
    while (1) {
    }
}
