#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/*
 * 寄存器版：FreeRTOS 小系统综合项目。
 *
 * 前面已经学过 GPIO、ADC、USART、队列和中断入队。
 * 本课重点不是某一个外设，而是把职责拆开：
 * - adc_task 周期采样 PA0，更新 g_adc_value
 * - key_task 轮询 PB0，只投递 EVENT_KEY
 * - USART1_IRQHandler 只把串口字节放进 g_uart_queue
 * - uart_task 解析 t/s 命令
 * - control_task 统一处理控制事件
 * - status_task 做低优先级周期状态输出
 *
 * 多个任务都会调用 uart_write_str/uart_write_u16 往串口输出，因此用一把
 * 互斥锁 g_uart_mutex 保护 UART 发送，避免多任务并发时字节交错。
 */

enum {
    EVENT_KEY = 1,
    EVENT_UART_TOGGLE = 2
};

static QueueHandle_t g_event_queue;
static QueueHandle_t g_uart_queue;
static SemaphoreHandle_t g_uart_mutex;
static volatile uint16_t g_adc_value;

static void stop_for_debug(void)
{
    taskDISABLE_INTERRUPTS();
    while (1) {
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
                   RCC_CFGR_SW |
                   RCC_CFGR_ADCPRE);
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1;
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;
    RCC->CFGR |= RCC_CFGR_PPRE2_DIV1;
    RCC->CFGR |= RCC_CFGR_PLLSRC;
    RCC->CFGR |= RCC_CFGR_PLLMULL9;
    RCC->CFGR |= RCC_CFGR_ADCPRE_DIV6;

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0U) {
    }

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

static void gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN |
                    RCC_APB2ENR_IOPBEN |
                    RCC_APB2ENR_IOPCEN |
                    RCC_APB2ENR_AFIOEN;

    /* PC13 是 control_task 的输出结果。 */
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;
    GPIOC->BSRR = GPIO_BSRR_BS13;

    /* PB0 上拉输入，按下为低。 */
    GPIOB->CRL &= ~(GPIO_CRL_MODE0 | GPIO_CRL_CNF0);
    GPIOB->CRL |= GPIO_CRL_CNF0_1;
    GPIOB->BSRR = GPIO_BSRR_BS0;

    /* PA0 模拟输入，供 adc_task 周期采样。 */
    GPIOA->CRL &= ~(GPIO_CRL_MODE0 | GPIO_CRL_CNF0);
}

static void uart1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_IOPAEN;

    GPIOA->CRH &= ~(GPIO_CRH_MODE9 |
                    GPIO_CRH_CNF9 |
                    GPIO_CRH_MODE10 |
                    GPIO_CRH_CNF10);
    GPIOA->CRH |= GPIO_CRH_MODE9_1;
    GPIOA->CRH |= GPIO_CRH_CNF9_1;
    GPIOA->CRH |= GPIO_CRH_CNF10_0;

    USART1->BRR = 0x0271U;
    USART1->CR1 = USART_CR1_TE |
                  USART_CR1_RE |
                  USART_CR1_RXNEIE |
                  USART_CR1_UE;

    NVIC_SetPriority(USART1_IRQn, 6U);
    NVIC_EnableIRQ(USART1_IRQn);
}

static void adc1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    ADC1->SMPR2 = ADC_SMPR2_SMP0;
    ADC1->SQR1 = 0U;
    ADC1->SQR3 = 0U;
    ADC1->CR2 = ADC_CR2_ADON;

    for (volatile uint32_t i = 0; i < 10000U; i++) {
    }

    ADC1->CR2 |= ADC_CR2_CAL;
    while ((ADC1->CR2 & ADC_CR2_CAL) != 0U) {
    }
}

static uint16_t adc1_read(void)
{
    ADC1->CR2 |= ADC_CR2_ADON;
    while ((ADC1->SR & ADC_SR_EOC) == 0U) {
    }

    return (uint16_t)ADC1->DR;
}

static void led_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

static uint8_t button_pressed(void)
{
    return (GPIOB->IDR & GPIO_IDR_IDR0) == 0U;
}

static void uart_write_byte(uint8_t byte)
{
    while ((USART1->SR & USART_SR_TXE) == 0U) {
    }

    USART1->DR = byte;
}

static void uart_write_str(const char *s)
{
    /*
     * 多个任务可能同时调用本函数；用互斥锁保证一次完整字符串不会被
     * 其他任务的发送打断，避免串口输出出现字节交错。
     */
    if (xSemaphoreTake(g_uart_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    while (*s != '\0') {
        uart_write_byte((uint8_t)*s);
        s++;
    }

    (void)xSemaphoreGive(g_uart_mutex);
}

static void uart_write_u16(uint16_t value)
{
    char buf[6];
    int i = 0;

    if (xSemaphoreTake(g_uart_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (value == 0U) {
        uart_write_byte('0');
        (void)xSemaphoreGive(g_uart_mutex);
        return;
    }

    while (value > 0U) {
        buf[i] = (char)('0' + (value % 10U));
        i++;
        value /= 10U;
    }

    while (i > 0) {
        i--;
        uart_write_byte((uint8_t)buf[i]);
    }

    (void)xSemaphoreGive(g_uart_mutex);
}

static void adc_task(void *argument)
{
    (void)argument;

    while (1) {
        /*
         * g_adc_value 是跨任务共享的简单 uint16_t。
         * Cortex-M3 上这种宽度的读写通常是原子的；复杂结构就不能这样裸共享。
         */
        g_adc_value = adc1_read();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void key_task(void *argument)
{
    uint8_t last = 0U;

    (void)argument;

    while (1) {
        uint8_t now = button_pressed();

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
        /*
         * control_task 是系统控制中心。
         * 按键和串口命令都转成事件到这里处理，避免多个任务到处直接翻 LED。
         */
        if (xQueueReceive(g_event_queue, &event, portMAX_DELAY) == pdPASS) {
            led_toggle();

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

void USART1_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;

    if ((USART1->SR & USART_SR_RXNE) != 0U) {
        uint8_t byte = (uint8_t)USART1->DR;
        (void)xQueueSendFromISR(g_uart_queue, &byte, &woken);
    }

    portYIELD_FROM_ISR(woken);
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

int main(void)
{
    system_clock_72mhz_init();
    gpio_init();
    uart1_init();
    adc1_init();

    g_event_queue = xQueueCreate(8, sizeof(uint8_t));
    g_uart_queue = xQueueCreate(32, sizeof(uint8_t));
    g_uart_mutex = xSemaphoreCreateMutex();

    BaseType_t adc_ok = xTaskCreate(adc_task, "adc", 128, NULL, 2, NULL);
    BaseType_t key_ok = xTaskCreate(key_task, "key", 128, NULL, 2, NULL);
    BaseType_t control_ok = xTaskCreate(control_task, "ctrl", 192, NULL, 2, NULL);
    BaseType_t uart_ok = xTaskCreate(uart_task, "uart", 192, NULL, 2, NULL);
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
