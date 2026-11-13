#include "stm32f1xx.h"

/*
 * 寄存器版：SX1278（Ra-01）LoRa 收发。
 *
 * 30/31 课已经讲过 SPI 主机和“片选边界”。本课的新东西是：
 *   1. SX1278 的寄存器通过 SPI 读写，地址最高位=1 表示读
 *   2. 上电必须先复位，再确认 RegVersion(0x42) == 0x12
 *   3. 433MHz 的载频由 Frf 寄存器决定：Frf = fRF * 2^19 / 32MHz
 *   4. 发包流程：Standby -> 写 FIFO -> TX -> 等 DIO0/TxDone -> Standby
 *   5. 收包流程：Standby -> 清标志 -> RXCONT -> 等 DIO0/RxDone -> 读 FIFO
 *
 * 接线（BluePill）：
 *   PA4 -> NSS   PA5 -> SCK   PA6 -> MISO   PA7 -> MOSI
 *   PB0 -> RESET
 *   PB1 -> DIO0
 *   SX1278 3.3V 供电，必须接天线，否则可能损坏射频前端。
 *
 * 单板只能看到“发送完成”，收包需要第二块板或第二个 LoRa 节点。
 */

#define LORA_NSS_LOW()      (GPIOA->BRR = GPIO_BRR_BR4)
#define LORA_NSS_HIGH()     (GPIOA->BSRR = GPIO_BSRR_BS4)

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
#define REG_IRQ_FLAGS_MASK      0x11U
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

#define LORA_FRF_433MHZ         0x6C4000UL  /* 433MHz x 2^19 / 32MHz */
#define LORA_PAYLOAD_MAX        64U

static volatile uint32_t g_ms_ticks = 0U;

static uint8_t g_rx_buffer[LORA_PAYLOAD_MAX];

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_toggle(void);
static void uart1_init(void);
static void uart1_puts(const char *text);
static void uart1_put_u32(uint32_t value);
static void uart1_put_i32(int32_t value);
static void spi1_gpio_init(void);
static void spi1_init(void);
static uint8_t spi1_transfer(uint8_t tx);
static void lora_reset_pin_init(void);
static void lora_reset(void);
static void lora_write_reg(uint8_t address, uint8_t value);
static uint8_t lora_read_reg(uint8_t address);
static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length);
static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length);
static void lora_set_opmode(uint8_t mode);
static uint8_t lora_init(void);
static uint8_t lora_send(const uint8_t *data, uint8_t length);
static uint8_t lora_receive(uint8_t *data, uint8_t max_length, int16_t *rssi, int8_t *snr);
static uint8_t lora_wait_dio0(uint32_t timeout_ms);

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

static void uart1_put_i32(int32_t value)
{
    if (value < 0) {
        uart1_putc('-');
        value = -value;
    }
    uart1_put_u32((uint32_t)value);
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
 * SPI1 + 控制引脚。
 * NSS 由软件控制：每笔寄存器事务单独拉低/拉高。
 * RESET/DIO0 使用 PB0/PB1，避开 SPI1 的 PA4~PA7。
 */
static void spi1_gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;

    GPIOA->CRL &= ~(GPIO_CRL_MODE4 |
                    GPIO_CRL_CNF4 |
                    GPIO_CRL_MODE5 |
                    GPIO_CRL_CNF5 |
                    GPIO_CRL_MODE6 |
                    GPIO_CRL_CNF6 |
                    GPIO_CRL_MODE7 |
                    GPIO_CRL_CNF7);

    GPIOA->CRL |= GPIO_CRL_MODE4_1;
    GPIOA->CRL |= GPIO_CRL_MODE5_1 | GPIO_CRL_CNF5_1;
    GPIOA->CRL |= GPIO_CRL_CNF6_0;
    GPIOA->CRL |= GPIO_CRL_MODE7_1 | GPIO_CRL_CNF7_1;

    LORA_NSS_HIGH();
}

static void lora_reset_pin_init(void)
{
    GPIOB->CRL &= ~(GPIO_CRL_MODE0 | GPIO_CRL_CNF0 |
                    GPIO_CRL_MODE1 | GPIO_CRL_CNF1);

    /* PB0 输出推挽，PB1 浮空输入（DIO0） */
    GPIOB->CRL |= GPIO_CRL_MODE0_1;
    GPIOB->CRL |= GPIO_CRL_CNF1_0;

    GPIOB->BSRR = GPIO_BSRR_BS0;
}

static void spi1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

    /*
     * 主机、软件 NSS、Mode0（CPOL=0/CPHA=0）、8 位。
     * BR_1 -> PCLK2/8 = 9MHz，低于 SX1278 的 10MHz 上限。
     */
    SPI1->CR1 = SPI_CR1_MSTR |
                SPI_CR1_SSM |
                SPI_CR1_SSI |
                SPI_CR1_BR_1 |
                SPI_CR1_SPE;
}

static uint8_t spi1_transfer(uint8_t tx)
{
    while ((SPI1->SR & SPI_SR_TXE) == 0U) {
    }
    *(__IO uint8_t *)&SPI1->DR = tx;

    while ((SPI1->SR & SPI_SR_RXNE) == 0U) {
    }
    return (uint8_t)SPI1->DR;
}

/*
 * 硬复位 SX1278：RESET 拉低 10ms 再拉高，等待内部振荡器稳定。
 * 不复位的典型现象是 RegVersion 读到 0x00 或 0xFF。
 */
static void lora_reset(void)
{
    GPIOB->BRR = GPIO_BRR_BR0;
    delay_ms(10U);
    GPIOB->BSRR = GPIO_BSRR_BS0;
    delay_ms(20U);
}

static void lora_write_reg(uint8_t address, uint8_t value)
{
    LORA_NSS_LOW();
    (void)spi1_transfer(address & 0x7FU);
    (void)spi1_transfer(value);
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    LORA_NSS_HIGH();
}

static uint8_t lora_read_reg(uint8_t address)
{
    uint8_t value;

    LORA_NSS_LOW();
    (void)spi1_transfer(address | 0x80U);
    value = spi1_transfer(0x00U);
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    LORA_NSS_HIGH();

    return value;
}

static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length)
{
    uint8_t i;

    LORA_NSS_LOW();
    (void)spi1_transfer(address & 0x7FU);
    for (i = 0U; i < length; i++) {
        (void)spi1_transfer(data[i]);
    }
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    LORA_NSS_HIGH();
}

static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length)
{
    uint8_t i;

    LORA_NSS_LOW();
    (void)spi1_transfer(address | 0x80U);
    for (i = 0U; i < length; i++) {
        data[i] = spi1_transfer(0x00U);
    }
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    LORA_NSS_HIGH();
}

static void lora_set_opmode(uint8_t mode)
{
    lora_write_reg(REG_OP_MODE, (uint8_t)(MODE_LONG_RANGE | mode));
}

/*
 * 初始化到 433MHz / BW125 / SF7 / CR4/5 / CRC on。
 * 所有参数只能在 Sleep/Standby 下修改；TX 或 RX 中写寄存器会被忽略。
 */
static uint8_t lora_init(void)
{
    uint8_t version;

    spi1_gpio_init();
    lora_reset_pin_init();
    spi1_init();

    lora_reset();

    version = lora_read_reg(REG_VERSION);
    if (version != 0x12U) {
        return 0U;
    }

    lora_set_opmode(MODE_SLEEP);
    delay_ms(10U);

    lora_write_reg(REG_FRF_MSB, (uint8_t)(LORA_FRF_433MHZ >> 16));
    lora_write_reg(REG_FRF_MID, (uint8_t)(LORA_FRF_433MHZ >> 8));
    lora_write_reg(REG_FRF_LSB, (uint8_t)(LORA_FRF_433MHZ));

    /* PA_BOOST，最大功率档，输出 +17dBm */
    lora_write_reg(REG_PA_CONFIG, 0xFFU);
    lora_write_reg(REG_OCP, 0x2BU);
    lora_write_reg(REG_LNA, 0x23U);

    /* BW=125kHz, CR=4/5, 显式包头 */
    lora_write_reg(REG_MODEM_CONFIG1, 0x72U);
    /* SF=7, CRC 开启 */
    lora_write_reg(REG_MODEM_CONFIG2, 0x74U);
    /* 自动 AGC */
    lora_write_reg(REG_MODEM_CONFIG3, 0x04U);

    lora_write_reg(REG_PREAMBLE_MSB, 0x00U);
    lora_write_reg(REG_PREAMBLE_LSB, 0x08U);
    lora_write_reg(REG_SYMB_TIMEOUT_LSB, 0xFFU);
    /* 私有网络同步字，收发双方必须一致 */
    lora_write_reg(REG_SYNC_WORD, 0x12U);

    lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0x00U);
    lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00U);
    lora_write_reg(REG_IRQ_FLAGS, 0xFFU);

    lora_set_opmode(MODE_STANDBY);
    return 1U;
}

static uint8_t lora_wait_dio0(uint32_t timeout_ms)
{
    uint32_t start = g_ms_ticks;

    while ((g_ms_ticks - start) < timeout_ms) {
        if ((GPIOB->IDR & GPIO_IDR_IDR1) != 0U) {
            return 1U;
        }
    }
    return 0U;
}

/*
 * 发送一包。
 * DIO0 在本函数里被映射为 TxDone（DioMapping1=0x40），
 * 收到上升沿后再读 IrqFlags 确认，最后清标志回 Standby。
 */
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

/*
 * 进入连续接收并等待一包。
 * 返回 1 表示收到，0 表示超时或 CRC 错。
 * RSSI 换算：PacketRssi - 157（HF 端口）。
 */
static uint8_t lora_receive(uint8_t *data, uint8_t max_length, int16_t *rssi, int8_t *snr)
{
    uint8_t flags;
    uint8_t length;
    uint8_t current;
    int16_t rssi_raw;
    int8_t snr_raw;

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

    rssi_raw = (int16_t)lora_read_reg(REG_PKT_RSSI_VALUE);
    snr_raw = (int8_t)lora_read_reg(REG_PKT_SNR_VALUE);

    if (rssi != 0) {
        *rssi = (int16_t)(rssi_raw - 157);
    }
    if (snr != 0) {
        *snr = (int8_t)(snr_raw / 4);
    }

    lora_write_reg(REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(MODE_STANDBY);

    return length;
}

int main(void)
{
    uint32_t counter = 0U;
    uint8_t len;
    uint8_t version;
    int16_t rssi = 0;
    int8_t snr = 0;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    uart1_init();

    uart1_puts("\n[LORA] SX1278 433MHz BW125 SF7 CR4/5\n");

    if (lora_init() == 0U) {
        version = lora_read_reg(REG_VERSION);
        uart1_puts("[LORA] init failed, version=");
        uart1_put_u32(version);
        uart1_puts("\n[LORA] check 3.3V, SPI wiring, RESET\n");
        while (1) {
            led_toggle();
            delay_ms(100U);
        }
    }

    uart1_puts("[LORA] init ok, version=0x12\n");

    while (1) {
        uint8_t header[8] = {'P', 'I', 'N', 'G', ' ', 0U, 0U, 0U};

        header[5] = (uint8_t)('0' + ((counter / 100U) % 10U));
        header[6] = (uint8_t)('0' + ((counter / 10U) % 10U));
        header[7] = (uint8_t)('0' + (counter % 10U));

        uart1_puts("[LORA] tx PING ");
        uart1_put_u32(counter);
        uart1_puts("\n");

        if (lora_send(header, 8U) != 0U) {
            uart1_puts("[LORA] tx done\n");
            led_toggle();
        } else {
            uart1_puts("[LORA] tx timeout\n");
        }

        len = lora_receive(g_rx_buffer, LORA_PAYLOAD_MAX, &rssi, &snr);
        if (len > 0U) {
            uart1_puts("[LORA] rx ");
            for (uint8_t i = 0U; i < len; i++) {
                uart1_putc((char)g_rx_buffer[i]);
            }
            uart1_puts("\n[LORA] rssi=");
            uart1_put_i32((int32_t)rssi);
            uart1_puts("dBm snr=");
            uart1_put_i32((int32_t)snr);
            uart1_puts("dB\n");
            led_toggle();

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
        delay_ms(2000U);
    }
}
