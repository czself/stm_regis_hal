#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>

/*
 * HAL版：FreeRTOS 小系统综合项目。
 *
 * HAL 负责把 GPIO/ADC/UART 初始化写成结构体字段。
 * 本课真正要看的是系统数据流：输入任务或中断产生数据，队列传递事件，
 * control_task 统一控制 LED 和日志输出。
 *
 * 多个任务都会调用 uart_write_str/uart_write_u16 往串口输出，因此用一把
 * 互斥锁 g_uart_mutex 保护 UART 发送，避免多任务并发时字节交错。
 */

enum {
    EVENT_KEY = 1,
    EVENT_UART_TOGGLE = 2
};

static ADC_HandleTypeDef hadc1;
static UART_HandleTypeDef huart1;
static QueueHandle_t g_event_queue;
static QueueHandle_t g_uart_queue;
static SemaphoreHandle_t g_uart_mutex;
static volatile uint16_t g_adc_value;
static uint8_t g_rx_byte;

static void stop_for_debug(void)
{
    taskDISABLE_INTERRUPTS();
    while (1) {
    }
}

static void system_clock_72mhz_init(void);
static void gpio_init(void);
static void adc1_init(void);
static void uart1_init(void);

static void uart_write_str(const char *s)
{
    /*
     * HAL_UART_Transmit 不是线程安全的；多个任务同时调用时字节可能交错。
     * 用互斥锁保证一次完整字符串发送不会被其他任务打断。
     */
    if (xSemaphoreTake(g_uart_mutex, portMAX_DELAY) == pdTRUE) {
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 100U);
        (void)xSemaphoreGive(g_uart_mutex);
    }
}

static void uart_write_u16(uint16_t value)
{
    char buf[6];
    int i = 0;

    if (xSemaphoreTake(g_uart_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (value == 0U) {
        uint8_t c = '0';
        (void)HAL_UART_Transmit(&huart1, &c, 1U, 20U);
        (void)xSemaphoreGive(g_uart_mutex);
        return;
    }

    while (value > 0U) {
        buf[i] = (char)('0' + (value % 10U));
        i++;
        value /= 10U;
    }

    while (i > 0) {
        uint8_t c;

        i--;
        c = (uint8_t)buf[i];
        (void)HAL_UART_Transmit(&huart1, &c, 1U, 20U);
    }

    (void)xSemaphoreGive(g_uart_mutex);
}

static void adc_task(void *argument)
{
    (void)argument;

    while (1) {
        /*
         * HAL_ADC_Start/Poll/Get/Stop 对应寄存器版 ADON、EOC、DR 的轮询链路。
         * 本课没有用 DMA，目的是保持综合系统里 ADC 任务足够直观。
         */
        (void)HAL_ADC_Start(&hadc1);
        (void)HAL_ADC_PollForConversion(&hadc1, 20U);
        g_adc_value = (uint16_t)HAL_ADC_GetValue(&hadc1);
        (void)HAL_ADC_Stop(&hadc1);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void key_task(void *argument)
{
    uint8_t last = 0U;

    (void)argument;

    while (1) {
        uint8_t now = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) == GPIO_PIN_RESET);

        if ((now != 0U) && (last == 0U)) {
            uint8_t event = EVENT_KEY;
            (void)xQueueSend(g_event_queue, &event, 0);
        }

        last = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void control_task(void *argument)
{
    uint8_t event;

    (void)argument;

    while (1) {
        if (xQueueReceive(g_event_queue, &event, portMAX_DELAY) == pdPASS) {
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

            if (event == EVENT_KEY) {
                uart_write_str("key\r\n");
            } else {
                uart_write_str("toggle\r\n");
            }
        }
    }
}

static void uart_task(void *argument)
{
    uint8_t byte;

    (void)argument;

    /*
     * HAL 版 UART 中断接收必须先启动一次，回调里再续接下一字节。
     */
    if (HAL_UART_Receive_IT(&huart1, &g_rx_byte, 1U) != HAL_OK) {
        stop_for_debug();
    }

    uart_write_str("system ready\r\n");

    while (1) {
        if (xQueueReceive(g_uart_queue, &byte, portMAX_DELAY) == pdPASS) {
            if ((byte == 't') || (byte == 'T')) {
                uint8_t event = EVENT_UART_TOGGLE;
                (void)xQueueSend(g_event_queue, &event, 0);
            } else if ((byte == 's') || (byte == 'S')) {
                uart_write_str("adc=");
                uart_write_u16(g_adc_value);
                uart_write_str("\r\n");
            }
        }
    }
}

static void status_task(void *argument)
{
    (void)argument;

    while (1) {
        uart_write_str("adc=");
        uart_write_u16(g_adc_value);
        uart_write_str("\r\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

int main(void)
{
    HAL_Init();
    system_clock_72mhz_init();
    gpio_init();
    adc1_init();
    uart1_init();

    g_event_queue = xQueueCreate(8, sizeof(uint8_t));
    g_uart_queue = xQueueCreate(32, sizeof(uint8_t));
    g_uart_mutex = xSemaphoreCreateMutex();

    BaseType_t adc_ok = xTaskCreate(adc_task, "adc", 160, NULL, 2, NULL);
    BaseType_t key_ok = xTaskCreate(key_task, "key", 128, NULL, 2, NULL);
    BaseType_t control_ok = xTaskCreate(control_task, "ctrl", 192, NULL, 2, NULL);
    BaseType_t uart_ok = xTaskCreate(uart_task, "uart", 224, NULL, 2, NULL);
    BaseType_t status_ok = xTaskCreate(status_task, "stat", 192, NULL, 1, NULL);

    if ((g_event_queue == NULL) ||
        (g_uart_queue == NULL) ||
        (g_uart_mutex == NULL) ||
        (adc_ok != pdPASS) ||
        (key_ok != pdPASS) ||
        (control_ok != pdPASS) ||
        (uart_ok != pdPASS) ||
        (status_ok != pdPASS)) {
        stop_for_debug();
    }

    vTaskStartScheduler();

    stop_for_debug();
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t woken = pdFALSE;

    if (huart->Instance == USART1) {
        (void)xQueueSendFromISR(g_uart_queue, &g_rx_byte, &woken);
        (void)HAL_UART_Receive_IT(&huart1, &g_rx_byte, 1U);
    }

    portYIELD_FROM_ISR(woken);
}

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}

static void gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_ADC1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_13;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_NVIC_SetPriority(USART1_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

static void adc1_init(void)
{
    ADC_ChannelConfTypeDef channel = {0};

    hadc1.Instance = ADC1;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        stop_for_debug();
    }

    channel.Channel = ADC_CHANNEL_0;
    channel.Rank = ADC_REGULAR_RANK_1;
    channel.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    if (HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK) {
        stop_for_debug();
    }
}

static void uart1_init(void)
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        stop_for_debug();
    }
}

static void system_clock_72mhz_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    RCC_PeriphCLKInitTypeDef periph = {0};

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        stop_for_debug();
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
        stop_for_debug();
    }

    periph.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    periph.AdcClockSelection = RCC_ADCPCLK2_DIV6;
    if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) {
        stop_for_debug();
    }
}

void vApplicationMallocFailedHook(void)
{
    stop_for_debug();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    (void)task_name;

    stop_for_debug();
}
