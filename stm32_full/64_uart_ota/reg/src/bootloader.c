#include "stm32f1xx.h"

/*
 * 寄存器版：串口 OTA Bootloader。
 *
 * 63 课只做了“跳转”，本课补上真正把新固件写进 Flash 的完整链路：
 *
 *   PC 发 HELLO（镜像大小 + CRC32）
 *     -> Bootloader 校验参数，擦除 app 区
 *   PC 发 DATA（序号 + 最多 128 字节）
 *     -> Bootloader 按半字写 Flash，回 ACK
 *   PC 发 END
 *     -> Bootloader 重算整片 CRC32，校验通过后跳到新 app
 *
 * 协议帧（小端）：
 *   0xAA 0x55 | CMD(1) | SEQ(2) | LEN(2) | PAYLOAD(LEN) | CRC16(2)
 *   CRC16 覆盖 CMD/SEQ/LEN/PAYLOAD，多项式 0x1021，初值 0xFFFF。
 *
 * 应答帧：
 *   0xAA 0x55 | CMD|0x80(1) | SEQ(2) | 0x01 0x00 | STATUS(1) | CRC16(2)
 *   注意本实现把 LEN 放在 SEQ 之后，和请求帧一致；status=0 表示成功。
 *
 * 为什么每包都要 ACK：
 *   串口没有重传机制。停在等待 ACK 的“停止-等待”协议虽然慢，
 *   但能把丢包、Flash 写失败明确暴露给上位机，教学链路更可控。
 */

#define APP_START_ADDR      0x08008000UL
#define APP_END_ADDR        0x08010000UL
#define APP_MAX_SIZE        (APP_END_ADDR - APP_START_ADDR)
#define OTA_PAGE_SIZE       1024U
#define OTA_CHUNK_MAX       128U
#define OTA_FRAME_MAX       (7U + OTA_CHUNK_MAX + 2U)
#define OTA_WAIT_WINDOW_MS  5000U

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

static uint32_t g_image_size = 0U;
static uint32_t g_image_crc = 0U;
static uint32_t g_write_offset = 0U;
static uint16_t g_expected_seq = 0U;
static uint8_t g_ota_started = 0U;

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_toggle(void);
static void uart1_init(void);
static void uart1_putc(char ch);
static void uart1_puts(const char *text);
static void uart1_put_u32(uint32_t value);
static int uart1_getc_timeout(uint32_t timeout_ms);

static uint16_t crc16_update(uint16_t crc, uint8_t byte);
static uint32_t crc32_update(uint32_t crc, uint8_t byte);
static uint32_t crc32_flash(uint32_t address, uint32_t length);

static void flash_unlock(void);
static void flash_lock(void);
static void flash_wait_not_busy(void);
static void flash_erase_app_area(uint32_t size);
static uint8_t flash_program_halfword(uint32_t address, uint16_t value);
static uint8_t flash_program_buffer(uint32_t address, const uint8_t *data, uint16_t length);

static void ota_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_send_status(uint8_t cmd, uint16_t seq, uint8_t status);
static void ota_handle_command(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_parser_init(void);
static void ota_parser_feed(uint8_t byte);
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

/*
 * 带超时读一个字节。
 * OTA 主循环不能死等：没有数据时要靠超时判断“上位机是不是已经放弃”，
 * 否则一旦 PC 断开，bootloader 会永远卡在串口读，app 也起不来。
 */
static int uart1_getc_timeout(uint32_t timeout_ms)
{
    uint32_t start = g_ms_ticks;

    while ((g_ms_ticks - start) < timeout_ms) {
        if ((USART1->SR & USART_SR_RXNE) != 0U) {
            return (int)(uint8_t)USART1->DR;
        }
    }
    return -1;
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

/*
 * CRC32（IEEE 802.3，反射多项式 0xEDB88320）。
 * 上位机用 Python 的 zlib.crc32()，两者必须完全一致，
 * 否则表现为“传输全成功，END 校验失败”。
 */
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

/*
 * 只擦 app 用到的页，不碰 bootloader 自己所在的 0x08000000~0x08007FFF。
 * 如果这里算错地址，第一条页擦除就会把正在运行的 bootloader 擦掉，
 * 表现为擦完第一页后程序立刻跑飞。
 */
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

static void ota_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;

    uart1_putc((char)FRAME_SYNC1);
    uart1_putc((char)FRAME_SYNC2);
    uart1_putc((char)cmd);
    crc = crc16_update(crc, cmd);

    uart1_putc((char)(seq & 0xFFU));
    uart1_putc((char)((seq >> 8) & 0xFFU));
    crc = crc16_update(crc, (uint8_t)(seq & 0xFFU));
    crc = crc16_update(crc, (uint8_t)((seq >> 8) & 0xFFU));

    uart1_putc((char)(len & 0xFFU));
    uart1_putc((char)((len >> 8) & 0xFFU));
    crc = crc16_update(crc, (uint8_t)(len & 0xFFU));
    crc = crc16_update(crc, (uint8_t)((len >> 8) & 0xFFU));

    for (i = 0U; i < len; i++) {
        uart1_putc((char)payload[i]);
        crc = crc16_update(crc, payload[i]);
    }

    uart1_putc((char)(crc & 0xFFU));
    uart1_putc((char)((crc >> 8) & 0xFFU));
}

static void ota_send_status(uint8_t cmd, uint16_t seq, uint8_t status)
{
    ota_send_frame((uint8_t)(cmd | 0x80U), seq, &status, 1U);
}

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

        uart1_puts("[OTA] hello: size=");
        uart1_put_u32(image_size);
        uart1_puts(" bytes\n[OTA] erasing app area\n");

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
            uart1_puts("[OTA] crc mismatch\n");
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        /* CRC 一致还要确认向量表可跳转，防止错误镜像把板子跳死 */
        if (app_is_valid() == 0U) {
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        flash_lock();
        g_ota_started = 0U;
        uart1_puts("[OTA] verify ok, jump to new app\n");
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
 * 串口上电时可能从半包中间开始，WAIT_SYNC1 会丢掉所有非 0xAA 字节，
 * 直到重新看到帧头，具备自同步能力。
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
    uint32_t last_activity_ms;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    uart1_init();
    ota_parser_init();

    uart1_puts("\n[BOOT-OTA] STM32F103 UART OTA bootloader v1.0");
    uart1_puts("\n[BOOT-OTA] app: 0x08008000, max 32KB, chunk 128B");
    uart1_puts("\n[BOOT-OTA] waiting HELLO 15s...\n");

    last_activity_ms = g_ms_ticks;

    while (1) {
        int ch = uart1_getc_timeout(100U);

        if (ch >= 0) {
            ota_parser_feed((uint8_t)ch);
            last_activity_ms = g_ms_ticks;
            continue;
        }

        if ((g_ms_ticks - last_activity_ms) >= OTA_WAIT_WINDOW_MS) {
            if (g_ota_started != 0U) {
                /*
                 * 升级中途上位机消失：不跳转，留在 bootloader 等下一次 HELLO。
                 * 此时 app 区可能只写了一半，跳过去必死。
                 */
                g_ota_started = 0U;
                flash_lock();
                uart1_puts("[BOOT-OTA] timeout, stay in bootloader\n");
            } else if (app_is_valid() != 0U) {
                uart1_puts("[BOOT-OTA] no host, jump to app\n");
                delay_ms(20U);
                jump_to_app();
            }
            last_activity_ms = g_ms_ticks;
        }
    }
}
