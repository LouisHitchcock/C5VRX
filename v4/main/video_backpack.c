/* ELRS VRx backpack receiver: HDZero-protocol MSP on UART1 RX = GPIO10
 * (XIAO C5 header D10), 115200 8N1, from an ESP32-S3 running the ExpressLRS
 * Backpack HDZero VRx target (https://github.com/ExpressLRS/Backpack).
 * GPIO10 is not a strapping pin; the IQ bit it carried moved to the
 * unconnected GPIO2 pad (rf.c s_iq_pins). This task only decodes and posts
 * the requested channel; the analog_agc control task retunes, outside the
 * sample-paced path. */
#include "video_internal.h"
#include "c5vrx4.h"
#include "elrs_backpack.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#define BACKPACK_UART UART_NUM_1
#define BACKPACK_RX_GPIO GPIO_NUM_10

static int s_pending = -1;           /* C5 channel index, or -1 */
static elrs_msp_t s_msp;

bool video_backpack_take(size_t *index)
{
    int value = __atomic_exchange_n(&s_pending, -1, __ATOMIC_ACQ_REL);
    if (value < 0) return false;
    *index = (size_t)value;
    return true;
}

static void backpack_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    int last_elrs = -1;
    for (;;) {
        int n = uart_read_bytes(BACKPACK_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; ++i) {
            if (!elrs_msp_feed(&s_msp, buf[i]) || s_msp.function != ELRS_MSP_SET_CHANNEL_INDEX ||
                s_msp.size < 1) continue;
            int elrs = s_msp.payload[0];
            /* The sender repeats every 2 s. Act only on a new radio channel,
             * so a channel picked on the C5 itself sticks until the radio
             * sends a different one. The first frame after boot always counts. */
            if (elrs == last_elrs) continue;
            last_elrs = elrs;
            int c5 = elrs_backpack_c5_index((uint8_t)elrs);
            if (c5 < 0) {
                printf("[BACKPACK] Refused ELRS index %d (%u MHz): no matching C5VRX channel\n",
                       elrs, elrs_backpack_mhz((uint8_t)elrs));
                continue;
            }
            __atomic_store_n(&s_pending, c5, __ATOMIC_RELEASE);
        }
    }
}

void video_backpack_start(void)
{
    if (!c5vrx4_elrs_backpack_enabled()) {
        printf("[BACKPACK] ELRS backpack link off (menu SETUP)\n");
        return;
    }
    elrs_msp_init(&s_msp);
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BACKPACK_UART, 256, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(BACKPACK_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(BACKPACK_UART, UART_PIN_NO_CHANGE, BACKPACK_RX_GPIO,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    /* Idle high when nothing is wired, so an open pad reads as no data. */
    if (err == ESP_OK) err = gpio_pullup_en(BACKPACK_RX_GPIO);
    if (err == ESP_OK && xTaskCreate(backpack_task, "elrs_bp", 3072, NULL, 1, NULL) != pdPASS)
        err = ESP_ERR_NO_MEM;
    printf("[BACKPACK] ELRS backpack link UART1 RX=GPIO10 (D10) 115200: %s\n", esp_err_to_name(err));
}

void video_backpack_print(void)
{
    printf("[BACKPACK] enabled=%d frames=%" PRIu32 " crc_errors=%" PRIu32 " oversize=%" PRIu32 "\n",
           c5vrx4_elrs_backpack_enabled(), s_msp.frames, s_msp.crc_errors, s_msp.oversize);
}
