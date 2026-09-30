/* Standalone bench meter. No live DMA or video BSS in this image. */
#include <stdint.h>
#include <stdio.h>
#include "heap_memory_layout.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf.h"
#include "phy_phase_tap_probe.h"

/* The allocation selector disconnects BOTH 64 KiB banks from the CPU.
 * Proven reservation from legacy/c5vrx2/main/rf_dump.c. */
SOC_RESERVE_MEMORY_REGION(0x4082ffc0, 0x40850040, c5vrx_agc_meter_ram);
extern char _bss_end;

void app_main(void)
{
    if ((uintptr_t)&_bss_end > 0x4082ffc0u) {
        printf("AGC_CAPTURE ERROR bss_overlaps_dump_banks\n");
        return;
    }
    /* CPU-only resets can leave dump ownership latched. Sanitize before PHY. */
    volatile uint32_t *ctrl = (volatile uint32_t *)0x600a9004u;
    volatile uint32_t *usage = (volatile uint32_t *)0x60095004u;
    *ctrl &= ~(0x80000000u | 0x00080000u | 0x00040000u | 0x00020000u);
    *usage &= ~0x00010f00u;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    ESP_ERROR_CHECK(rf_start());
    (void)phy_agc_capture_boot_run();
    printf("AGC_METER READY no_video=1 commands=@capture,{next,}vendor\n");
    fflush(stdout);
    for (;;) {
        int c = getchar();
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (c == '@') err = phy_agc_capture_arm();
        else if (c == '{' || c == '}') err = rf_native_acq_arm(c == '{');
        else { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        printf("AGC_METER ARMED err=%s\n", esp_err_to_name(err));
        fflush(stdout);
        if (err == ESP_OK) { vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
    }
}
