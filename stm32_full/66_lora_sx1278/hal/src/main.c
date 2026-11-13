#include "stm32f1xx_hal.h"
#include <string.h>

/*
 * HAL 版：SX1278 433MHz 收发。
 *
 * 寄存器序列和寄存器版完全相同，只是：
 *   SPI 单字节交换 -> HAL_SPI_TransmitReceive
 *   NSS/RESET/DIO0 -> HAL_GPIO_WritePin / HAL_GPIO_ReadPin
 *   延时           -> HAL_Delay / HAL_GetTick
 *
 * HAL 不会替你“理解”射频参数，Frf/ModemConfig 仍然要按数据手册算。
 */

#define REG_FIFO                0x00U
#define REG_OP_MODE             0x01U
#define REG_FRF_MSB             0x06U
#define REG_FRF_MID             0x07U
#define REG_FRF_LSB             0x08U
#define REG_PA_CONFIG           0x09U
#define REG_OCP                 0x0BU
#define REG_LNA                 0x0CU
#define REG_FIFO_ADDR_PTR       0x0DU
#define REG_FIFO_TX_BASE_ADDR   0x0EU
#define REG_FIFO_RX_BASE_ADDR   0x0FU
#define REG_FIFO_RX_CURRENT     0x10U
#define REG_IRQ_FLAGS           0x12U
#define REG_RX_NB_BYTES         0x13U
#define REG_PKT_SNR_VALUE       0x19U
#define REG_PKT_RSSI_VALUE      0x1AU
#define REG_MODEM_CONFIG1       0x1DU
#define REG_MODEM_CONFIG2       0x1EU
#define REG_SYMB_TIMEOUT_LSB    0x1FU
#define REG_PREAMBLE_MSB        0x20U
#define REG_PREAMBLE_LSB        0x21U
#define REG_PAYLOAD_LENGTH      0x22U
#define REG_MODEM_CONFIG3       0x26U
#define REG_SYNC_WORD           0x39U
#define REG_DIO_MAPPING1        0x40U
#define REG_VERSION             0x42U

#define MODE_LONG_RANGE         0x80U
#define MODE_SLEEP              0x00U
#define MODE_STANDBY            0x01U
#define MODE_TX                 0x03U
#define MODE_RX_CONTINUOUS      0x05U

#define IRQ_TX_DONE             0x08U
#define IRQ_PAYLOAD_CRC_ERROR   0x20U
#define IRQ_RX_DONE             0x40U

#define LORA_FRF_433MHZ         0x6C4000UL
#define LORA_PAYLOAD_MAX        64U

static SPI_HandleTypeDef hspi1;
static UART_HandleTypeDef huart1;

static uint8_t g_rx_buffer[LORA_PAYLOAD_MAX];

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void uart1_init(void);
static void uart1_puts(const char *text);
static void uart1_put_u32(uint32_t value);
static void uart1_put_i32(int32_t value);
static void spi1_init(void);
static uint8_t spi1_transfer(uint8_t tx);
static void lora_write_reg(uint8_t address, uint8_t value);
static uint8_t lora_read_reg(uint8_t address);
static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length);
static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length);
static void lora_set_opmode(uint8_t mode);
static uint8_t lora_init(void);
static uint8_t lora_send(const uint8_t *data, uint8_t length);
static uint8_t lora_receive(uint8_t *data, uint8_t max_length, int16_t *rssi, int8_t *snr);
static uint8_t lora_wait_dio0(uint32_t timeout_ms);
static void error_handler(void);

static void spi1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* PA4 = NSS，软件片选，默认拉高 */
    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    /* PA5/PA7 = SCK/MOSI */
    gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PA6 = MISO */
    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PB0 = RESET 输出，默认拉高；PB1 = DIO0 输入 */
    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);

    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &gpio);

    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    /* APB2 72MHz / 8 = 9MHz，满足 SX1278 的 10MHz 上限 */
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 7U;

    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        error_handler();
    }
}

static uint8_t spi1_transfer(uint8_t tx)
{
    uint8_t rx = 0U;

    if (HAL_SPI_TransmitReceive(&hspi1, &tx, &rx, 1U, 100U) != HAL_OK) {
        error_handler();
    }
    return rx;
}

static void lora_write_reg(uint8_t address, uint8_t value)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address & 0x7FU);
    (void)spi1_transfer(value);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

static uint8_t lora_read_reg(uint8_t address)
{
    uint8_t value;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address | 0x80U);
    value = spi1_transfer(0x00U);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    return value;
}

static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length)
{
    uint8_t i;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address & 0x7FU);
    for (i = 0U; i < length; i++) {
        (void)spi1_transfer(data[i]);
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length)
{
    uint8_t i;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address | 0x80U);
    for (i = 0U; i < length; i++) {
        data[i] = spi1_transfer(0x00U);
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

static void lora_set_opmode(uint8_t mode)
{
    lora_write_reg(REG_OP_MODE, (uint8_t)(MODE_LONG_RANGE | mode));
}

static uint8_t lora_init(void)
{
    uint8_t version;

    /* 硬复位：RESET 低 10ms，高 20ms */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_Delay(20U);

    version = lora_read_reg(REG_VERSION);
    if (version != 0x12U) {
        return 0U;
    }

    lora_set_opmode(MODE_SLEEP);
    HAL_Delay(10U);

    lora_write_reg(REG_FRF_MSB, (uint8_t)(LORA_FRF_433MHZ >> 16));
    lora_write_reg(REG_FRF_MID, (uint8_t)(LORA_FRF_433MHZ >> 8));
    lora_write_reg(REG_FRF_LSB, (uint8_t)(LORA_FRF_433MHZ));

    lora_write_reg(REG_PA_CONFIG, 0xFFU);
    lora_write_reg(REG_OCP, 0x2BU);
    lora_write_reg(REG_LNA, 0x23U);

    lora_write_reg(REG_MODEM_CONFIG1, 0x72U);
    lora_write_reg(REG_MODEM_CONFIG2, 0x74U);
    lora_write_reg(REG_MODEM_CONFIG3, 0x04U);

    lora_write_reg(REG_PREAMBLE_MSB, 0x00U);
    lora_write_reg(REG_PREAMBLE_LSB, 0x08U);
    lora_write_reg(REG_SYMB_TIMEOUT_LSB, 0xFFU);
    lora_write_reg(REG_SYNC_WORD, 0x12U);

    lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0x00U);
    lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00U);
    lora_write_reg(REG_IRQ_FLAGS, 0xFFU);

    lora_set_opmode(MODE_STANDBY);
    return 1U;
}

static uint8_t lora_wait_dio0(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_SET) {
            return 1U;
        }
    }
    return 0U;
}

static uint8_t lora_send(const uint8_t *data, uint8_t length)
{
    uint8_t flags;

    if ((length == 0U) || (length > LORA_PAYLOAD_MAX)) {
        return 0U;
    }

    lora_set_opmode(MODE_STANDBY);
    lora_write_reg(REG_DIO_MAPPING1, 0x40U);
    lora_write_reg(REG_FIFO_ADDR_PTR, 0x00U);
    lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0x00U);
    lora_write_burst(REG_FIFO, data, length);
    lora_write_reg(REG_PAYLOAD_LENGTH, length);
    lora_write_reg(REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(MODE_TX);

    if (lora_wait_dio0(2000U) == 0U) {
        lora_set_opmode(MODE_STANDBY);
        return 0U;
    }

    flags = lora_read_reg(REG_IRQ_FLAGS);
    lora_write_reg(REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(MODE_STANDBY);

    return ((flags & IRQ_TX_DONE) != 0U) ? 1U : 0U;
}

static uint8_t lora_receive(uint8_t *data, uint8_t max_length, int16_t *rssi, int8_t *snr)
{
    uint8_t flags;
    uint8_t length;
    uint8_t current;

    lora_set_opmode(MODE_STANDBY);
    lora_write_reg(REG_DIO_MAPPING1, 0x00U);
    lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00U);
    lora_write_reg(REG_PAYLOAD_LENGTH, LORA_PAYLOAD_MAX);
    lora_write_reg(REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(MODE_RX_CONTINUOUS);

    if (lora_wait_dio0(5000U) == 0U) {
        lora_set_opmode(MODE_STANDBY);
        return 0U;
    }

    flags = lora_read_reg(REG_IRQ_FLAGS);
    if (((flags & IRQ_RX_DONE) == 0U) ||
        ((flags & IRQ_PAYLOAD_CRC_ERROR) != 0U)) {
        lora_write_reg(REG_IRQ_FLAGS, 0xFFU);
        lora_set_opmode(MODE_STANDBY);
        return 0U;
    }

    length = lora_read_reg(REG_RX_NB_BYTES);
    if (length > max_length) {
        length = max_length;
    }

    current = lora_read_reg(REG_FIFO_RX_CURRENT);
    lora_write_reg(REG_FIFO_ADDR_PTR, current);
    lora_read_burst(REG_FIFO, data, length);

    if (rssi != 0) {
        *rssi = (int16_t)((int16_t)lora_read_reg(REG_PKT_RSSI_VALUE) - 157);
    }
    if (snr != 0) {
        *snr = (int8_t)((int8_t)lora_read_reg(REG_PKT_SNR_VALUE) / 4);
    }

    lora_write_reg(REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(MODE_STANDBY);

    return length;
}

int main(void)
{
    uint32_t counter = 0U;
    uint8_t len;
    int16_t rssi = 0;
    int8_t snr = 0;

    HAL_Init();
    system_clock_72mhz_init();
    led_pc13_init();
    uart1_init();
    spi1_init();

    uart1_puts("\r\n[LORA-HAL] SX1278 433MHz BW125 SF7\r\n");

    if (lora_init() == 0U) {
        uart1_puts("[LORA-HAL] init failed, version=");
        uart1_put_u32(lora_read_reg(REG_VERSION));
        uart1_puts("\r\n");
        while (1) {
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
            HAL_Delay(100U);
        }
    }

    uart1_puts("[LORA-HAL] init ok\r\n");

    while (1) {
        uint8_t header[8] = {'P', 'I', 'N', 'G', ' ', 0U, 0U, 0U};

        header[5] = (uint8_t)('0' + ((counter / 100U) % 10U));
        header[6] = (uint8_t)('0' + ((counter / 10U) % 10U));
        header[7] = (uint8_t)('0' + (counter % 10U));

        uart1_puts("[LORA-HAL] tx PING ");
        uart1_put_u32(counter);
        uart1_puts("\r\n");

        if (lora_send(header, 8U) != 0U) {
            uart1_puts("[LORA-HAL] tx done\r\n");
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        } else {
            uart1_puts("[LORA-HAL] tx timeout\r\n");
        }

        len = lora_receive(g_rx_buffer, LORA_PAYLOAD_MAX, &rssi, &snr);
        if (len > 0U) {
            uart1_puts("[LORA-HAL] rx ");
            for (uint8_t i = 0U; i < len; i++) {
                char ch = (char)g_rx_buffer[i];
                HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1U, 20U);
            }
            uart1_puts("\r\n[LORA-HAL] rssi=");
            uart1_put_i32((int32_t)rssi);
            uart1_puts("dBm snr=");
            uart1_put_i32((int32_t)snr);
            uart1_puts("dB\r\n");
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

            if ((len >= 4U) &&
                (g_rx_buffer[0] == 'P') &&
                (g_rx_buffer[1] == 'I') &&
                (g_rx_buffer[2] == 'N') &&
                (g_rx_buffer[3] == 'G')) {
                uint8_t pong[8] = {'P', 'O', 'N', 'G', ' ', 0U, 0U, 0U};
                (void)lora_send(pong, 8U);
            }
        }

        counter++;
        HAL_Delay(2000U);
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

static void uart1_put_i32(int32_t value)
{
    if (value < 0) {
        uart1_puts("-");
        value = -value;
    }
    uart1_put_u32((uint32_t)value);
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
