#include "stm32f1xx_hal.h"
#include <string.h>

/*
 * HAL 版：串口 OTA Bootloader。
 *
 * 和寄存器版的协议、CRC、状态机完全一致，区别只在底层：
 *   串口收发   -> HAL_UART_Transmit / HAL_UART_Receive
 *   Flash 擦除 -> HAL_FLASHEx_Erase
 *   Flash 编程 -> HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, ...)
 *   计时       -> HAL_GetTick
 *
 * HAL 帮你处理了 BSY 等待、错误标志和寄存器顺序，
 * 但它不会替你判断“这个地址是不是 app 区”，也不会校验固件 CRC。
 * 安全边界仍然要自己写。
 */

#define APP_START_ADDR      0x08008000UL
#define APP_END_ADDR        0x08010000UL
#define APP_MAX_SIZE        (APP_END_ADDR - APP_START_ADDR)
#define OTA_PAGE_SIZE       1024U
#define OTA_CHUNK_MAX       128U
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

static UART_HandleTypeDef huart1;
static OtaParser g_parser;

static uint32_t g_image_size = 0U;
static uint32_t g_image_crc = 0U;
static uint32_t g_write_offset = 0U;
static uint16_t g_expected_seq = 0U;
static uint8_t g_ota_started = 0U;

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void uart1_init(void);
static void boot_print(const char *text);
static void boot_print_u32(uint32_t value);
static uint16_t crc16_update(uint16_t crc, uint8_t byte);
static uint32_t crc32_update(uint32_t crc, uint8_t byte);
static uint32_t crc32_flash(uint32_t address, uint32_t length);
static uint8_t flash_erase_app_area(uint32_t size);
static uint8_t flash_program_buffer(uint32_t address, const uint8_t *data, uint16_t length);
static void ota_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_send_status(uint8_t cmd, uint16_t seq, uint8_t status);
static void ota_handle_command(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_parser_init(void);
static void ota_parser_feed(uint8_t byte);
static uint8_t app_is_valid(void);
static void jump_to_app(void);
static void error_handler(void);

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

static void boot_print(const char *text)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)text, (uint16_t)strlen(text), 200U);
}

static void boot_print_u32(uint32_t value)
{
    char digits[10];
    uint8_t count = 0U;

    if (value == 0U) {
        boot_print("0");
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

/*
 * 擦除 app 区。
 * NbPages 按镜像大小向上取整，避免擦到 app 区之外。
 */
static uint8_t flash_erase_app_area(uint32_t size)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0U;
    uint32_t pages = (size + OTA_PAGE_SIZE - 1U) / OTA_PAGE_SIZE;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_START_ADDR;
    erase.NbPages = pages;

    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
        return 0U;
    }
    return 1U;
}

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

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              address + i,
                              (uint64_t)value) != HAL_OK) {
            return 0U;
        }
    }
    return 1U;
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

    HAL_UART_Transmit(&huart1, frame, index, 500U);
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

        boot_print("[OTA-HAL] hello: size=");
        boot_print_u32(image_size);
        boot_print(" bytes, erasing\r\n");

        if (HAL_FLASH_Unlock() != HAL_OK) {
            ota_send_status(cmd, seq, ST_ERR_FLASH);
            return;
        }
        if (flash_erase_app_area(image_size) == 0U) {
            HAL_FLASH_Lock();
            ota_send_status(cmd, seq, ST_ERR_FLASH);
            return;
        }

        g_image_size = image_size;
        g_image_crc = image_crc;
        g_write_offset = 0U;
        g_expected_seq = 0U;
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
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
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
            boot_print("[OTA-HAL] crc mismatch\r\n");
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        /* CRC 一致还要确认向量表可跳转，防止错误镜像把板子跳死 */
        if (app_is_valid() == 0U) {
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        HAL_FLASH_Lock();
        g_ota_started = 0U;

        boot_print("[OTA-HAL] verify ok, jump\r\n");
        ota_send_status(cmd, seq, ST_OK);
        HAL_Delay(200U);
        jump_to_app();
        return;
    }

    if (cmd == CMD_ABORT) {
        HAL_FLASH_Lock();
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

    HAL_UART_DeInit(&huart1);

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
    uint32_t last_activity_tick;

    HAL_Init();
    system_clock_72mhz_init();
    led_pc13_init();
    uart1_init();
    ota_parser_init();

    boot_print("\r\n[BOOT-OTA-HAL] v1.0, app 0x08008000, chunk 128B\r\n");
    boot_print("[BOOT-OTA-HAL] waiting HELLO 5s...\r\n");

    last_activity_tick = HAL_GetTick();

    while (1) {
        uint8_t ch = 0U;

        if (HAL_UART_Receive(&huart1, &ch, 1U, 100U) == HAL_OK) {
            ota_parser_feed(ch);
            last_activity_tick = HAL_GetTick();
            continue;
        }

        if ((HAL_GetTick() - last_activity_tick) >= OTA_WAIT_WINDOW_MS) {
            if (g_ota_started != 0U) {
                g_ota_started = 0U;
                HAL_FLASH_Lock();
                boot_print("[BOOT-OTA-HAL] timeout, still in bootloader\r\n");
            } else if (app_is_valid() != 0U) {
                boot_print("[BOOT-OTA-HAL] no host, jump to app\r\n");
                HAL_Delay(20U);
                jump_to_app();
            }
            last_activity_tick = HAL_GetTick();
        }
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
