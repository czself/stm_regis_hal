#include "stm32f1xx.h"

/*
 * 寄存器版：UART / CAN / LoRa 三通道 OTA Bootloader。
 *
 * 前三课分别解决了：
 *   63：跳转与内存分区
 *   64：UART OTA 协议、Flash 擦写、CRC32
 *   65：CAN 过滤器、中断、远程帧
 *   66：SX1278 收发
 *
 * 本课把它们接成一条“任意通道都能升级”的链路：
 *   上电 5 秒内，哪个通道先收到数据，就锁定哪个通道；
 *   之后所有 HELLO/DATA/END 帧都从该通道进、ACK 从该通道出；
 *   升级完成后统一校验并跳转到 0x08008000。
 *
 * 帧格式与 64 课一致，只是 CHUNK 降到 32 字节，
 * 让一帧最多 41 字节，能塞进 LoRa 的 64 字节包。
 *
 * 通道差异：
 *   UART：字节流，直接喂解析器
 *   CAN ：CAN 帧只有 8 字节，用 [seq][7B 数据] 分片重组
 *   LoRa：一个 LoRa 包 = [seq][最多 63B 数据]
 */

#define APP_START_ADDR      0x08008000UL
#define APP_END_ADDR        0x08010000UL
#define APP_MAX_SIZE        (APP_END_ADDR - APP_START_ADDR)
#define OTA_PAGE_SIZE       1024U
#define OTA_CHUNK_MAX       32U
#define OTA_WAIT_WINDOW_MS  5000U
#define OTA_IDLE_ABORT_MS   15000U

#define FRAME_SYNC1         0xAAU
#define FRAME_SYNC2         0x55U

#define CMD_HELLO           0x01U
#define CMD_DATA            0x02U
#define CMD_END             0x03U
#define CMD_ABORT           0x04U

#define ST_OK               0x00U
#define ST_ERR_CRC16        0x01U
#define ST_ERR_LEN          0x02U
#define ST_ERR_SEQ          0x03U
#define ST_ERR_STATE        0x04U
#define ST_ERR_FLASH        0x05U
#define ST_ERR_SIZE         0x06U
#define ST_ERR_VERIFY       0x07U

#define CH_NONE             0U
#define CH_UART             1U
#define CH_CAN              2U
#define CH_LORA             3U

#define CAN_HOST_ID         0x6A1U
#define CAN_TARGET_ID       0x6A2U

#define LORA_PAYLOAD_MAX    64U

/* SX1278 寄存器（66 课已逐个讲过，这里只保留用到的） */
#define LR_REG_FIFO         0x00U
#define LR_REG_OP_MODE      0x01U
#define LR_REG_FRF_MSB      0x06U
#define LR_REG_FRF_MID      0x07U
#define LR_REG_FRF_LSB      0x08U
#define LR_REG_PA_CONFIG    0x09U
#define LR_REG_OCP          0x0BU
#define LR_REG_LNA          0x0CU
#define LR_REG_FIFO_ADDR    0x0DU
#define LR_REG_FIFO_TX_BASE 0x0EU
#define LR_REG_FIFO_RX_BASE 0x0FU
#define LR_REG_FIFO_RX_CUR  0x10U
#define LR_REG_IRQ_FLAGS    0x12U
#define LR_REG_RX_NB_BYTES  0x13U
#define LR_REG_MODEM1       0x1DU
#define LR_REG_MODEM2       0x1EU
#define LR_REG_PREAMBLE_MSB 0x20U
#define LR_REG_PREAMBLE_LSB 0x21U
#define LR_REG_PAYLOAD_LEN  0x22U
#define LR_REG_MODEM3       0x26U
#define LR_REG_SYNC_WORD    0x39U
#define LR_REG_DIO_MAPPING  0x40U
#define LR_REG_VERSION      0x42U

typedef enum {
    RX_WAIT_SYNC1 = 0,
    RX_WAIT_SYNC2,
    RX_CMD,
    RX_SEQ_L,
    RX_SEQ_H,
    RX_LEN_L,
    RX_LEN_H,
    RX_PAYLOAD,
    RX_CRC_L,
    RX_CRC_H
} RxState;

typedef struct {
    RxState state;
    uint8_t cmd;
    uint16_t seq;
    uint16_t len;
    uint16_t payload_index;
    uint16_t crc_calc;
    uint16_t crc_recv;
    uint8_t payload[OTA_CHUNK_MAX];
} OtaParser;

static volatile uint32_t g_ms_ticks = 0U;

static OtaParser g_parser;
static uint8_t g_channel = CH_NONE;
static uint8_t g_can_ok = 0U;
static uint8_t g_lora_ok = 0U;

static uint32_t g_image_size = 0U;
static uint32_t g_image_crc = 0U;
static uint32_t g_write_offset = 0U;
static uint16_t g_expected_seq = 0U;
static uint8_t g_ota_started = 0U;

static uint8_t g_can_rx_seq = 0U;
static uint8_t g_can_tx_seq = 0U;
static uint8_t g_lora_rx_seq = 0U;
static uint8_t g_lora_rx_ready = 0U;

static uint8_t g_lora_rx_packet[LORA_PAYLOAD_MAX];

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_toggle(void);
static void uart1_init(void);
static void uart1_putc(char ch);
static void uart1_puts(const char *text);
static void uart1_put_u32(uint32_t value);
static int uart1_try_getc(void);

static uint16_t crc16_update(uint16_t crc, uint8_t byte);
static uint32_t crc32_update(uint32_t crc, uint8_t byte);
static uint32_t crc32_flash(uint32_t address, uint32_t length);

static void flash_unlock(void);
static void flash_lock(void);
static void flash_wait_not_busy(void);
static void flash_erase_app_area(uint32_t size);
static uint8_t flash_program_buffer(uint32_t address, const uint8_t *data, uint16_t length);

static void ota_parser_init(void);
static void ota_parser_feed(uint8_t byte);
static void ota_handle_command(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_send_status(uint8_t cmd, uint16_t seq, uint8_t status);

static void can_gpio_init(void);
static uint8_t can1_init(void);
static void can_send_bytes(const uint8_t *data, uint16_t length);
static uint8_t can_poll_byte_input(void);
static void can_rx_frame_process(void);

static void lora_pins_init(void);
static void spi1_init(void);
static uint8_t spi1_transfer(uint8_t tx);
static void lora_write_reg(uint8_t address, uint8_t value);
static uint8_t lora_read_reg(uint8_t address);
static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length);
static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length);
static uint8_t lora_init(void);
static void lora_enter_rx(void);
static void lora_send_bytes(const uint8_t *data, uint16_t length);
static uint8_t lora_poll_byte_input(void);

static void channel_send_bytes(const uint8_t *data, uint16_t length);
static uint8_t app_is_valid(void);
static void jump_to_app(void);

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

static int uart1_try_getc(void)
{
    if ((USART1->SR & USART_SR_RXNE) == 0U) {
        return -1;
    }
    return (int)(uint8_t)USART1->DR;
}

static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    uint8_t i;

    crc ^= (uint16_t)byte << 8;
    for (i = 0U; i < 8U; i++) {
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)((crc << 1) ^ 0x1021U);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
    uint8_t i;

    crc ^= byte;
    for (i = 0U; i < 8U; i++) {
        if ((crc & 1U) != 0U) {
            crc = (crc >> 1) ^ 0xEDB88320U;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

static uint32_t crc32_flash(uint32_t address, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t i;

    for (i = 0U; i < length; i++) {
        crc = crc32_update(crc, *(volatile uint8_t *)(address + i));
    }
    return crc ^ 0xFFFFFFFFU;
}

static void flash_wait_not_busy(void)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
    }
}

static void flash_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = 0x45670123U;
        FLASH->KEYR = 0xCDEF89ABU;
    }
}

static void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static void flash_erase_page(uint32_t address)
{
    flash_wait_not_busy();
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = address;
    FLASH->CR |= FLASH_CR_STRT;
    flash_wait_not_busy();
    FLASH->CR &= ~FLASH_CR_PER;
}

static void flash_erase_app_area(uint32_t size)
{
    uint32_t offset;

    for (offset = 0U; offset < size; offset += OTA_PAGE_SIZE) {
        flash_erase_page(APP_START_ADDR + offset);
        led_toggle();
    }
}

static uint8_t flash_program_halfword(uint32_t address, uint16_t value)
{
    flash_wait_not_busy();
    FLASH->CR |= FLASH_CR_PG;
    *(__IO uint16_t *)address = value;
    flash_wait_not_busy();

    if ((FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) != 0U) {
        FLASH->CR &= ~FLASH_CR_PG;
        return 0U;
    }

    FLASH->CR &= ~FLASH_CR_PG;
    return 1U;
}

/*
 * 按半字写一段数据。
 * STM32F1 只能 16 位编程，最后一个字节要补 0xFF 组成半字，
 * 否则奇数字节长度的固件最后一包会写失败。
 */
static uint8_t flash_program_buffer(uint32_t address, const uint8_t *data, uint16_t length)
{
    uint16_t i;

    for (i = 0U; i < length; i += 2U) {
        uint16_t value;

        if ((i + 1U) < length) {
            value = (uint16_t)data[i] | ((uint16_t)data[i + 1U] << 8);
        } else {
            value = (uint16_t)data[i] | 0xFF00U;
        }

        if (flash_program_halfword(address + i, value) == 0U) {
            return 0U;
        }
    }
    return 1U;
}

/*
 * ---------------- UART / CAN / LoRa 三个通道的发送 ----------------
 */

static void channel_send_bytes(const uint8_t *data, uint16_t length)
{
    if (g_channel == CH_UART) {
        uint16_t i;
        for (i = 0U; i < length; i++) {
            uart1_putc((char)data[i]);
        }
        return;
    }

    if (g_channel == CH_CAN) {
        can_send_bytes(data, length);
        return;
    }

    if (g_channel == CH_LORA) {
        lora_send_bytes(data, length);
        return;
    }
}

static void ota_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[7U + OTA_CHUNK_MAX + 2U];
    uint16_t index = 0U;
    uint16_t crc = 0xFFFFU;
    uint16_t i;

    frame[index++] = FRAME_SYNC1;
    frame[index++] = FRAME_SYNC2;
    frame[index++] = cmd;
    crc = crc16_update(crc, cmd);

    frame[index++] = (uint8_t)(seq & 0xFFU);
    frame[index++] = (uint8_t)((seq >> 8) & 0xFFU);
    crc = crc16_update(crc, (uint8_t)(seq & 0xFFU));
    crc = crc16_update(crc, (uint8_t)((seq >> 8) & 0xFFU));

    frame[index++] = (uint8_t)(len & 0xFFU);
    frame[index++] = (uint8_t)((len >> 8) & 0xFFU);
    crc = crc16_update(crc, (uint8_t)(len & 0xFFU));
    crc = crc16_update(crc, (uint8_t)((len >> 8) & 0xFFU));

    for (i = 0U; i < len; i++) {
        frame[index++] = payload[i];
        crc = crc16_update(crc, payload[i]);
    }

    frame[index++] = (uint8_t)(crc & 0xFFU);
    frame[index++] = (uint8_t)((crc >> 8) & 0xFFU);

    channel_send_bytes(frame, index);
}

static void ota_send_status(uint8_t cmd, uint16_t seq, uint8_t status)
{
    ota_send_frame((uint8_t)(cmd | 0x80U), seq, &status, 1U);
}

/*
 * ---------------- CAN 传输 ----------------
 *
 * CAN 一帧只有 8 字节。约定：
 *   ID 0x6A1：主机 -> 目标，数据 = [seq][7 字节协议流]
 *   ID 0x6A2：目标 -> 主机，数据 = [seq][7 字节协议流]
 *
 * 接收端按 seq 递增拼接。seq 不连续说明中间丢帧，
 * 直接重置协议状态机，等主机重传整个协议帧。
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

static uint8_t can1_init(void)
{
    uint32_t timeout = 100000U;

    CAN1->MCR |= CAN_MCR_INRQ;
    while (((CAN1->MSR & CAN_MSR_INAK) == 0U) && (timeout > 0U)) {
        timeout--;
    }
    if (timeout == 0U) {
        return 0U;
    }

    CAN1->MCR = CAN_MCR_INRQ;

    /* 500kbps，正常模式：需要 TJA1050 等收发器才能上总线 */
    CAN1->BTR = (3U << CAN_BTR_BRP_Pos) |
                (12U << CAN_BTR_TS1_Pos) |
                (3U << CAN_BTR_TS2_Pos);

    /* 全放行过滤器到 FIFO0 */
    CAN1->FMR |= CAN_FMR_FINIT;
    CAN1->FA1R &= ~CAN_FA1R_FACT0;
    CAN1->FS1R |= CAN_FS1R_FSC0;
    CAN1->FM1R &= ~CAN_FM1R_FBM0;
    CAN1->FFA1R &= ~CAN_FFA1R_FFA0;
    CAN1->sFilterRegister[0].FR1 = 0U;
    CAN1->sFilterRegister[0].FR2 = 0U;
    CAN1->FA1R |= CAN_FA1R_FACT0;
    CAN1->FMR &= ~CAN_FMR_FINIT;

    CAN1->MCR &= ~CAN_MCR_INRQ;
    timeout = 100000U;
    while (((CAN1->MSR & CAN_MSR_INAK) != 0U) && (timeout > 0U)) {
        timeout--;
    }

    return (timeout > 0U) ? 1U : 0U;
}

static void can_send_frame_raw(uint16_t std_id, const uint8_t *data)
{
    uint32_t timeout = 100000U;
    uint8_t i;

    while (((CAN1->TSR & CAN_TSR_TME0) == 0U) && (timeout > 0U)) {
        timeout--;
    }
    if (timeout == 0U) {
        return;
    }

    CAN1->sTxMailBox[0].TIR = ((uint32_t)std_id << 21);
    CAN1->sTxMailBox[0].TDTR = 8U;
    CAN1->sTxMailBox[0].TDLR = 0U;
    CAN1->sTxMailBox[0].TDHR = 0U;

    for (i = 0U; i < 8U; i++) {
        if (i < 4U) {
            CAN1->sTxMailBox[0].TDLR |= ((uint32_t)data[i] << (8U * i));
        } else {
            CAN1->sTxMailBox[0].TDHR |= ((uint32_t)data[i] << (8U * (i - 4U)));
        }
    }
    CAN1->sTxMailBox[0].TIR |= CAN_TI0R_TXRQ;
}

static void can_send_bytes(const uint8_t *data, uint16_t length)
{
    uint16_t offset = 0U;

    while (offset < length) {
        uint8_t frame[8];
        uint8_t count = (uint8_t)((length - offset) > 7U ? 7U : (length - offset));
        uint8_t i;

        frame[0] = g_can_tx_seq++;
        for (i = 0U; i < 7U; i++) {
            frame[1U + i] = (i < count) ? data[offset + i] : 0xFFU;
        }
        can_send_frame_raw(CAN_TARGET_ID, frame);
        offset += count;
    }
}

static void can_rx_frame_process(void)
{
    uint32_t rir;
    uint32_t rdtr;
    uint32_t rdlr;
    uint32_t rdhr;
    uint16_t std_id;
    uint8_t data[8];
    uint8_t seq;
    uint8_t i;

    rir = CAN1->sFIFOMailBox[0].RIR;
    rdtr = CAN1->sFIFOMailBox[0].RDTR;
    rdlr = CAN1->sFIFOMailBox[0].RDLR;
    rdhr = CAN1->sFIFOMailBox[0].RDHR;
    std_id = (uint16_t)(rir >> 21);

    data[0] = (uint8_t)(rdlr & 0xFFU);
    data[1] = (uint8_t)((rdlr >> 8) & 0xFFU);
    data[2] = (uint8_t)((rdlr >> 16) & 0xFFU);
    data[3] = (uint8_t)((rdlr >> 24) & 0xFFU);
    data[4] = (uint8_t)(rdhr & 0xFFU);
    data[5] = (uint8_t)((rdhr >> 8) & 0xFFU);
    data[6] = (uint8_t)((rdhr >> 16) & 0xFFU);
    data[7] = (uint8_t)((rdhr >> 24) & 0xFFU);

    CAN1->RF0R |= CAN_RF0R_RFOM0;

    if ((std_id != CAN_HOST_ID) || ((rdtr & 0x0FU) != 8U)) {
        return;
    }

    seq = data[0];
    if (seq != g_can_rx_seq) {
        ota_parser_init();
    }
    g_can_rx_seq = (uint8_t)(seq + 1U);

    for (i = 0U; i < 7U; i++) {
        ota_parser_feed(data[1U + i]);
    }
}

static uint8_t can_poll_byte_input(void)
{
    if ((CAN1->RF0R & CAN_RF0R_FMP0_Msk) == 0U) {
        return 0U;
    }
    can_rx_frame_process();
    return 1U;
}

/*
 * ---------------- LoRa 传输 ----------------
 *
 * 一个 LoRa 包 = [seq][最多 63 字节协议流]。
 * 包之间 seq 必须连续，否则重置解析器等待主机重传。
 */
static void lora_pins_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

    GPIOA->CRL &= ~(GPIO_CRL_MODE4 | GPIO_CRL_CNF4 |
                    GPIO_CRL_MODE5 | GPIO_CRL_CNF5 |
                    GPIO_CRL_MODE6 | GPIO_CRL_CNF6 |
                    GPIO_CRL_MODE7 | GPIO_CRL_CNF7);

    GPIOA->CRL |= GPIO_CRL_MODE4_1;
    GPIOA->CRL |= GPIO_CRL_MODE5_1 | GPIO_CRL_CNF5_1;
    GPIOA->CRL |= GPIO_CRL_CNF6_0;
    GPIOA->CRL |= GPIO_CRL_MODE7_1 | GPIO_CRL_CNF7_1;
    GPIOA->BSRR = GPIO_BSRR_BS4;

    GPIOB->CRL &= ~(GPIO_CRL_MODE0 | GPIO_CRL_CNF0 |
                    GPIO_CRL_MODE1 | GPIO_CRL_CNF1);
    GPIOB->CRL |= GPIO_CRL_MODE0_1;
    GPIOB->CRL |= GPIO_CRL_CNF1_0;
    GPIOB->BSRR = GPIO_BSRR_BS0;
}

static void spi1_init(void)
{
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

static void lora_write_reg(uint8_t address, uint8_t value)
{
    GPIOA->BRR = GPIO_BRR_BR4;
    (void)spi1_transfer(address & 0x7FU);
    (void)spi1_transfer(value);
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    GPIOA->BSRR = GPIO_BSRR_BS4;
}

static uint8_t lora_read_reg(uint8_t address)
{
    uint8_t value;

    GPIOA->BRR = GPIO_BRR_BR4;
    (void)spi1_transfer(address | 0x80U);
    value = spi1_transfer(0x00U);
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    GPIOA->BSRR = GPIO_BSRR_BS4;
    return value;
}

static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length)
{
    uint8_t i;

    GPIOA->BRR = GPIO_BRR_BR4;
    (void)spi1_transfer(address & 0x7FU);
    for (i = 0U; i < length; i++) {
        (void)spi1_transfer(data[i]);
    }
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    GPIOA->BSRR = GPIO_BSRR_BS4;
}

static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length)
{
    uint8_t i;

    GPIOA->BRR = GPIO_BRR_BR4;
    (void)spi1_transfer(address | 0x80U);
    for (i = 0U; i < length; i++) {
        data[i] = spi1_transfer(0x00U);
    }
    while ((SPI1->SR & SPI_SR_BSY) != 0U) {
    }
    GPIOA->BSRR = GPIO_BSRR_BS4;
}

static void lora_set_opmode(uint8_t mode)
{
    lora_write_reg(LR_REG_OP_MODE, (uint8_t)(0x80U | mode));
}

static uint8_t lora_init(void)
{
    uint8_t version;

    lora_pins_init();
    spi1_init();

    GPIOB->BRR = GPIO_BRR_BR0;
    delay_ms(10U);
    GPIOB->BSRR = GPIO_BSRR_BS0;
    delay_ms(20U);

    version = lora_read_reg(LR_REG_VERSION);
    if (version != 0x12U) {
        return 0U;
    }

    lora_set_opmode(0x00U);  /* sleep */
    delay_ms(10U);

    lora_write_reg(LR_REG_FRF_MSB, 0x6CU);
    lora_write_reg(LR_REG_FRF_MID, 0x40U);
    lora_write_reg(LR_REG_FRF_LSB, 0x00U);
    lora_write_reg(LR_REG_PA_CONFIG, 0xFFU);
    lora_write_reg(LR_REG_OCP, 0x2BU);
    lora_write_reg(LR_REG_LNA, 0x23U);
    lora_write_reg(LR_REG_MODEM1, 0x72U);
    lora_write_reg(LR_REG_MODEM2, 0x74U);
    lora_write_reg(LR_REG_MODEM3, 0x04U);
    lora_write_reg(LR_REG_PREAMBLE_MSB, 0x00U);
    lora_write_reg(LR_REG_PREAMBLE_LSB, 0x08U);
    lora_write_reg(LR_REG_SYNC_WORD, 0x12U);
    lora_write_reg(LR_REG_FIFO_TX_BASE, 0x00U);
    lora_write_reg(LR_REG_FIFO_RX_BASE, 0x00U);
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);

    lora_set_opmode(0x01U);  /* standby */
    return 1U;
}

static void lora_send_bytes(const uint8_t *data, uint16_t length)
{
    uint8_t packet[LORA_PAYLOAD_MAX];
    uint8_t count = (uint8_t)((length > (LORA_PAYLOAD_MAX - 1U)) ? (LORA_PAYLOAD_MAX - 1U) : length);
    uint8_t flags;
    uint32_t start;

    packet[0] = 0U;  /* ACK 帧很短，固定 seq=0 */
    for (uint8_t i = 0U; i < count; i++) {
        packet[1U + i] = data[i];
    }

    lora_set_opmode(0x01U);
    lora_write_reg(LR_REG_DIO_MAPPING, 0x40U);
    lora_write_reg(LR_REG_FIFO_ADDR, 0x00U);
    lora_write_reg(LR_REG_FIFO_TX_BASE, 0x00U);
    lora_write_burst(LR_REG_FIFO, packet, (uint8_t)(count + 1U));
    lora_write_reg(LR_REG_PAYLOAD_LEN, (uint8_t)(count + 1U));
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(0x03U);  /* TX */

    start = g_ms_ticks;
    while ((g_ms_ticks - start) < 2000U) {
        if ((GPIOB->IDR & GPIO_IDR_IDR1) != 0U) {
            break;
        }
    }
    flags = lora_read_reg(LR_REG_IRQ_FLAGS);
    (void)flags;
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);

    /*
     * 发完 ACK 必须回到接收：
     * 如果直接停在 Standby，下一包 DATA 永远等不到 DIO0。
     */
    if (g_lora_rx_ready != 0U) {
        lora_enter_rx();
    } else {
        lora_set_opmode(0x01U);
    }
}

/*
 * 进入连续接收只需要做一次。
 * 如果每次轮询都重新 Standby -> RXCONT，会不断清 IRQ 标志，
 * 正在空中的包会被打断，表现为“偶尔才能收到”。
 */
static void lora_enter_rx(void)
{
    lora_set_opmode(0x01U);
    lora_write_reg(LR_REG_DIO_MAPPING, 0x00U);
    lora_write_reg(LR_REG_FIFO_RX_BASE, 0x00U);
    lora_write_reg(LR_REG_PAYLOAD_LEN, LORA_PAYLOAD_MAX);
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(0x05U);  /* RX continuous */
    g_lora_rx_ready = 1U;
}

static uint8_t lora_poll_byte_input(void)
{
    uint8_t flags;
    uint8_t length;
    uint8_t current;
    uint8_t seq;

    if (g_lora_rx_ready == 0U) {
        lora_enter_rx();
    }

    if ((GPIOB->IDR & GPIO_IDR_IDR1) == 0U) {
        return 0U;
    }

    flags = lora_read_reg(LR_REG_IRQ_FLAGS);
    if (((flags & 0x40U) == 0U) || ((flags & 0x20U) != 0U)) {
        lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
        return 0U;
    }

    length = lora_read_reg(LR_REG_RX_NB_BYTES);
    if ((length == 0U) || (length > LORA_PAYLOAD_MAX)) {
        lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
        return 0U;
    }

    current = lora_read_reg(LR_REG_FIFO_RX_CUR);
    lora_write_reg(LR_REG_FIFO_ADDR, current);
    lora_read_burst(LR_REG_FIFO, g_lora_rx_packet, length);
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);

    seq = g_lora_rx_packet[0];
    if (seq != g_lora_rx_seq) {
        ota_parser_init();
    }
    g_lora_rx_seq = (uint8_t)(seq + 1U);

    for (uint8_t i = 1U; i < length; i++) {
        ota_parser_feed(g_lora_rx_packet[i]);
    }
    return 1U;
}

/*
 * ---------------- OTA 协议与 Flash ----------------
 */

static void ota_handle_command(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    if (cmd == CMD_HELLO) {
        uint32_t image_size;
        uint32_t image_crc;

        if (g_ota_started != 0U) {
            ota_send_status(cmd, seq, ST_ERR_STATE);
            return;
        }
        if (len != 8U) {
            ota_send_status(cmd, seq, ST_ERR_LEN);
            return;
        }

        image_size = (uint32_t)payload[0] |
                     ((uint32_t)payload[1] << 8) |
                     ((uint32_t)payload[2] << 16) |
                     ((uint32_t)payload[3] << 24);
        image_crc = (uint32_t)payload[4] |
                    ((uint32_t)payload[5] << 8) |
                    ((uint32_t)payload[6] << 16) |
                    ((uint32_t)payload[7] << 24);

        if ((image_size == 0U) || (image_size > APP_MAX_SIZE)) {
            ota_send_status(cmd, seq, ST_ERR_SIZE);
            return;
        }

        g_image_size = image_size;
        g_image_crc = image_crc;
        g_write_offset = 0U;
        g_expected_seq = 0U;

        flash_unlock();
        flash_erase_app_area(image_size);
        g_ota_started = 1U;

        ota_send_status(cmd, seq, ST_OK);
        return;
    }

    if (cmd == CMD_DATA) {
        if (g_ota_started == 0U) {
            ota_send_status(cmd, seq, ST_ERR_STATE);
            return;
        }
        if (seq != g_expected_seq) {
            /*
             * 上一包的 ACK 可能在串口/CAN/LoRa 上丢了，上位机会重发同一包。
             * 此时数据已经写过，不能再写一次，只补一个 ACK。
             */
            if ((g_expected_seq != 0U) &&
                (seq == (uint16_t)(g_expected_seq - 1U))) {
                ota_send_status(cmd, seq, ST_OK);
                return;
            }
            ota_send_status(cmd, seq, ST_ERR_SEQ);
            return;
        }
        if ((len == 0U) || (len > OTA_CHUNK_MAX)) {
            ota_send_status(cmd, seq, ST_ERR_LEN);
            return;
        }
        if ((g_write_offset + len) > g_image_size) {
            ota_send_status(cmd, seq, ST_ERR_SIZE);
            return;
        }

        if (flash_program_buffer(APP_START_ADDR + g_write_offset, payload, len) == 0U) {
            ota_send_status(cmd, seq, ST_ERR_FLASH);
            return;
        }

        g_write_offset += len;
        g_expected_seq++;
        led_toggle();
        ota_send_status(cmd, seq, ST_OK);
        return;
    }

    if (cmd == CMD_END) {
        uint32_t crc;

        if (g_ota_started == 0U) {
            ota_send_status(cmd, seq, ST_ERR_STATE);
            return;
        }
        if (g_write_offset != g_image_size) {
            ota_send_status(cmd, seq, ST_ERR_SIZE);
            return;
        }

        crc = crc32_flash(APP_START_ADDR, g_image_size);
        if (crc != g_image_crc) {
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        /*
         * CRC 只证明“内容和上位机一致”，不证明“这个镜像能运行”。
         * 再查一次向量表，防止把错误文件写进 app 区后直接跳死。
         */
        if (app_is_valid() == 0U) {
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        flash_lock();
        g_ota_started = 0U;
        ota_send_status(cmd, seq, ST_OK);
        delay_ms(200U);
        jump_to_app();
        return;
    }

    if (cmd == CMD_ABORT) {
        flash_lock();
        g_ota_started = 0U;
        ota_send_status(cmd, seq, ST_OK);
        return;
    }

    ota_send_status(cmd, seq, ST_ERR_STATE);
}

static void ota_parser_init(void)
{
    g_parser.state = RX_WAIT_SYNC1;
    g_parser.cmd = 0U;
    g_parser.seq = 0U;
    g_parser.len = 0U;
    g_parser.payload_index = 0U;
    g_parser.crc_calc = 0xFFFFU;
    g_parser.crc_recv = 0U;
}

/*
 * 逐字节状态机。
 * WAIT_SYNC1 会丢弃所有非 0xAA 字节，因此可以从半包或噪声中重新同步；
 * 长度/CRC16 错误只回 NACK，不推进 Flash 写入状态。
 */
static void ota_parser_feed(uint8_t byte)
{
    switch (g_parser.state) {
    case RX_WAIT_SYNC1:
        if (byte == FRAME_SYNC1) {
            g_parser.state = RX_WAIT_SYNC2;
        }
        break;

    case RX_WAIT_SYNC2:
        if (byte == FRAME_SYNC2) {
            g_parser.state = RX_CMD;
            g_parser.crc_calc = 0xFFFFU;
        } else {
            g_parser.state = RX_WAIT_SYNC1;
        }
        break;

    case RX_CMD:
        g_parser.cmd = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_SEQ_L;
        break;

    case RX_SEQ_L:
        g_parser.seq = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_SEQ_H;
        break;

    case RX_SEQ_H:
        g_parser.seq |= (uint16_t)byte << 8;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_LEN_L;
        break;

    case RX_LEN_L:
        g_parser.len = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_LEN_H;
        break;

    case RX_LEN_H:
        g_parser.len |= (uint16_t)byte << 8;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);

        if (g_parser.len > OTA_CHUNK_MAX) {
            ota_send_status(g_parser.cmd, g_parser.seq, ST_ERR_LEN);
            ota_parser_init();
            break;
        }

        g_parser.payload_index = 0U;
        g_parser.state = (g_parser.len == 0U) ? RX_CRC_L : RX_PAYLOAD;
        break;

    case RX_PAYLOAD:
        g_parser.payload[g_parser.payload_index++] = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        if (g_parser.payload_index >= g_parser.len) {
            g_parser.state = RX_CRC_L;
        }
        break;

    case RX_CRC_L:
        g_parser.crc_recv = byte;
        g_parser.state = RX_CRC_H;
        break;

    case RX_CRC_H:
        g_parser.crc_recv |= (uint16_t)byte << 8;
        if (g_parser.crc_recv != g_parser.crc_calc) {
            ota_send_status(g_parser.cmd, g_parser.seq, ST_ERR_CRC16);
        } else {
            ota_handle_command(g_parser.cmd, g_parser.seq,
                               g_parser.payload, g_parser.len);
        }
        ota_parser_init();
        break;

    default:
        ota_parser_init();
        break;
    }
}

static uint8_t app_is_valid(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);

    if ((app_sp < 0x20000000U) || (app_sp > 0x20005000U)) {
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
    uint32_t last_activity_ms;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    uart1_init();
    ota_parser_init();

    can_gpio_init();
    g_can_ok = can1_init();

    g_lora_ok = lora_init();

    uart1_puts("\n[OTA67] multi-channel bootloader: UART/CAN/LoRa\n");
    uart1_puts("[OTA67] can=");
    uart1_put_u32(g_can_ok);
    uart1_puts(" lora=");
    uart1_put_u32(g_lora_ok);
    uart1_puts(", chunk=32B, wait 5s\n");

    start_ms = g_ms_ticks;
    last_activity_ms = start_ms;

    /*
     * 通道选择阶段：
     * 先收到数据的通道获得升级会话，避免三个通道的半包互相破坏状态机。
     */
    while (g_channel == CH_NONE) {
        int ch = uart1_try_getc();
        if (ch >= 0) {
            g_channel = CH_UART;
            uart1_puts("[OTA67] channel=UART\n");
            ota_parser_init();
            ota_parser_feed((uint8_t)ch);
            last_activity_ms = g_ms_ticks;
            break;
        }

        if ((g_can_ok != 0U) && (can_poll_byte_input() != 0U)) {
            g_channel = CH_CAN;
            uart1_puts("[OTA67] channel=CAN\n");
            last_activity_ms = g_ms_ticks;
            break;
        }

        if ((g_lora_ok != 0U) && (lora_poll_byte_input() != 0U)) {
            g_channel = CH_LORA;
            uart1_puts("[OTA67] channel=LoRa\n");
            lora_enter_rx();
            last_activity_ms = g_ms_ticks;
            break;
        }

        if ((g_ms_ticks - start_ms) >= OTA_WAIT_WINDOW_MS) {
            if (app_is_valid() != 0U) {
                uart1_puts("[OTA67] no channel, jump to app\n");
                delay_ms(20U);
                jump_to_app();
            }
            start_ms = g_ms_ticks;
        }
    }

    while (1) {
        if (g_channel == CH_UART) {
            int ch = uart1_try_getc();
            if (ch >= 0) {
                ota_parser_feed((uint8_t)ch);
                last_activity_ms = g_ms_ticks;
            }
        } else if (g_channel == CH_CAN) {
            if (can_poll_byte_input() != 0U) {
                last_activity_ms = g_ms_ticks;
            }
        } else if (g_channel == CH_LORA) {
            if (lora_poll_byte_input() != 0U) {
                last_activity_ms = g_ms_ticks;
            }
        }

        if ((g_ms_ticks - last_activity_ms) >= OTA_IDLE_ABORT_MS) {
            if (g_ota_started != 0U) {
                g_ota_started = 0U;
                flash_lock();
                uart1_puts("[OTA67] idle timeout, stay in bootloader\n");
            } else if (app_is_valid() != 0U) {
                uart1_puts("[OTA67] idle, jump to app\n");
                delay_ms(20U);
                jump_to_app();
            }
            last_activity_ms = g_ms_ticks;
        }
    }
}
