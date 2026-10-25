/* C5VRX by Twotoz and the C5VRX contributors, GPL-3.0-only.
 * Port of the earlier C5VRX-4 UART/MSP control link, not an RF data path.
 * No extra task: the existing 50 ms control loop reads a bounded UART batch. */
#include "backpack.h"
#include "elrs_backpack.h"
#include "rf.h"
#include <inttypes.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BACKPACK_UART UART_NUM_1
#define BACKPACK_RX GPIO_NUM_10
static elrs_backpack_t s_link = {.last_index = -1, .pending_index = -1};
static esp_err_t s_error = ESP_ERR_INVALID_STATE;
static QueueHandle_t s_events;
static uint32_t s_uart_errors;

void backpack_start(void)
{
    elrs_backpack_init(&s_link);
    const int iq_pins[] = RF_IQ_GPIO_NUMS;
    for (unsigned k = 0; k < sizeof(iq_pins) / sizeof(iq_pins[0]); ++k) {
        if (iq_pins[k] == BACKPACK_RX) {
            printf("[BACKPACK] disabled: GPIO10 belongs to IQ\n");
            return;
        }
    }
    const uart_config_t cfg = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    s_error = uart_driver_install(BACKPACK_UART, 256, 0, 8, &s_events, 0);
    if (s_error != ESP_OK) goto done;
    s_error = uart_param_config(BACKPACK_UART, &cfg);
    if (s_error == ESP_OK)
        s_error = uart_set_pin(BACKPACK_UART, UART_PIN_NO_CHANGE, BACKPACK_RX,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (s_error == ESP_OK) s_error = gpio_pullup_en(BACKPACK_RX);
    if (s_error != ESP_OK) { uart_driver_delete(BACKPACK_UART); s_events = NULL; }
done:
    printf("[BACKPACK] UART1 RX=GPIO10 (D10) 115200: %s\n", esp_err_to_name(s_error));
}

void backpack_poll(void)
{
    if (s_error != ESP_OK) return;
    uart_event_t event;
    for (unsigned k = 0; k < 8 && xQueueReceive(s_events, &event, 0) == pdTRUE; ++k) {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
            ++s_uart_errors;
            uart_flush_input(BACKPACK_UART);
            s_link.state = 0;   /* discard partial frame, preserve counters/dedupe */
        }
    }
    uint8_t buf[64];
    int n = uart_read_bytes(BACKPACK_UART, buf, sizeof(buf), 0);
    uint32_t rejected = s_link.rejected;
    for (int k = 0; k < n; ++k) elrs_backpack_feed(&s_link, buf[k]);
    if (s_link.rejected != rejected)
        printf("[BACKPACK] rejected=%" PRIu32 " last_elrs=%d mhz=%u (unsupported channel/flags/size)\n",
               s_link.rejected, s_link.last_index,
               s_link.last_index >= 0 ? elrs_backpack_mhz((uint8_t)s_link.last_index) : 0);
}

bool backpack_take(uint8_t *index)
{
    return s_error == ESP_OK && elrs_backpack_take(&s_link, index);
}

void backpack_print(void)
{
    /* Parser is control-task-owned; copy before printing from the console. */
    vTaskSuspendAll();
    const elrs_backpack_t link = s_link;
    const uint32_t uart_errors = s_uart_errors;
    (void)xTaskResumeAll();
    printf("[BACKPACK] ready=%u frames=%" PRIu32 " commands=%" PRIu32 " crc_errors=%" PRIu32
           " oversize=%" PRIu32 " rejected=%" PRIu32 " uart_errors=%" PRIu32
           " last_elrs=%d pending=%d\n", s_error == ESP_OK, link.frames, link.commands,
           link.crc_errors, link.oversize, link.rejected, uart_errors, link.last_index, link.pending_index);
}
