/* C5VRX-4: menu responsibilities. */
#include "video_internal.h"

static inline void menu_ui_pixel(int x, int y, uint8_t code);
static void menu_ui_rect(int x, int y, int w, int h, uint8_t code);
static void menu_ui_hline(int x, int y, int w, uint8_t code);
static void menu_ui_vline(int x, int y, int h, uint8_t code);
static void menu_ui_glyph(char ch, int x, int y, uint8_t code, unsigned scale);
static void menu_ui_text(const char *str, int x, int y, uint8_t code);
static void menu_ui_text_scaled(const char *str, int x, int y, uint8_t code, unsigned scale);
static void menu_ui_text_right(const char *str, int right, int y, uint8_t code);
static void menu_ui_icon(unsigned icon, int x, int y, uint8_t code);
static void menu_ui_value_box(int x, int y, int w, const char *label, const char *value);
static void menu_ui_meter(int x, int y, int w, int value, int maximum);
static void menu_ui_signal_bars(int x, int y, int quality);
static bool menu_count_segment(void *ctx, const uint8_t *data, unsigned length);
static dma_descriptor_t *menu_node(unsigned index);
static bool menu_append_segment(void *ctx, const uint8_t *data, unsigned length);
static void menu_free_nodes(void);
static esp_err_t menu_reserve_nodes(unsigned nodes);
static esp_err_t menu_init_buffers(void);
static const char *agc_state_name(void);
static const char *afc_mode_name(void);
static void menu_draw_shell(void);
static void menu_draw_page_title(const char *title, const char *tag);
static void menu_draw_band_page(void);
static void menu_draw_channel_page(void);
static void menu_draw_video_page(void);
static void menu_option_text(unsigned option, char *value, size_t n);
static const char *menu_item_text(unsigned item, char *value, size_t n);
static void menu_draw_list(const char *title);
static void menu_draw_exit_page(void);
static void menu_render_idle(void);
static void start_menu_tx(void);
#define SETUP_ITEM_COUNT (SETUP_ITEM_OPTIONS + sizeof(s_setup_options))

#define MENU_NODE_CHUNKS ((MENU_MAX_NODES + MENU_NODE_CHUNK - 1u) / MENU_NODE_CHUNK)

enum { SETUP_ITEM_AFC, SETUP_ITEM_BOOT_MENU, SETUP_ITEM_OPTIONS };

enum {
    RF_ITEM_GAIN, RF_ITEM_DIGITAL_BW, RF_ITEM_ANALOG_BW, RF_ITEM_LANES,
    RF_ITEM_CAL_BW, RF_ITEM_CAL_AGC, RF_ITEM_COUNT
};

enum {
    UI_ROOT = 22,
    UI_HEADER = 24,
    UI_PANEL = 26,
    UI_PANEL_2 = 29,
    UI_DIVIDER = 33,
    UI_MUTED = 39,
    UI_SELECTED = 43,
    UI_SELECTED_EDGE = 48,
    UI_STRONG = 54,
    UI_WHITE = 60,
};
#include "menu_font.h"
#include "heap_memory_layout.h"
#include "snr_meter_hw.h"

QueueHandle_t s_menu_commands;

/* Timing and descriptors are immutable while running; only text pixels change.
 * This raster is never linked to the RF ring.
 *
 * The raster lives in a reserved region that starts at the RF dump bank
 * (0x40830000, 64 KiB, the only address the dump writer fills), instead of
 * static RAM that runs straight through that bank. Same RAM either way. While
 * neither the menu nor the idle raster owns TX, nothing reads it and every
 * entry rebuilds it (menu_init_buffers), so the SNR meter may lend its first
 * 64 KiB to the dump writer for a reading (docs/SNR_METER.md). Every render
 * or rebuild bumps s_raster_gen so a reading that overlapped one is
 * discarded. */
#define MENU_RASTER_ADDR  0x40830000u
#define MENU_RASTER_END   (MENU_RASTER_ADDR + ((sizeof(menu_raster_t) + 63u) & ~63u))
SOC_RESERVE_MEMORY_REGION(MENU_RASTER_ADDR, MENU_RASTER_END, c5vrx4_menu_raster);
#define s_menu_raster (*(menu_raster_t *)(uintptr_t)MENU_RASTER_ADDR)
static volatile uint32_t s_raster_busy;
static volatile uint32_t s_raster_gen;
#define RASTER_WRITE_BEGIN() (++s_raster_busy)
#define RASTER_WRITE_END() do { ++s_raster_gen; --s_raster_busy; } while (0)

/* The reservation record must stay referenced or --gc-sections can drop it,
 * and static RAM must end below it (also asserted at link time). */
bool video_raster_region_ok(void)
{
    extern char _bss_end;
    const volatile soc_reserved_region_t *region = &reserved_region_c5vrx4_menu_raster;
    return region->start == MENU_RASTER_ADDR && region->end == MENU_RASTER_END &&
           (uintptr_t)&_bss_end <= MENU_RASTER_ADDR &&
           !heap_caps_check_integrity_addr(MENU_RASTER_ADDR, false) &&
           !heap_caps_check_integrity_addr(MENU_RASTER_END - 1u, false);
}

bool video_raster_idle(uint32_t *gen)
{
    if (s_menu_active || IDLE_RASTER_ACTIVE() || s_raster_busy) return false;
    *gen = s_raster_gen;
    return true;
}

/* The two-field scatter chain needs 1,499 (NTSC, 18 KiB) or 1,811 (PAL,
 * 21.7 KiB) descriptors, only while the standalone menu owns TX. GDMA follows
 * each descriptor's next pointer, so the chain need not be contiguous: it is
 * allocated as 1.5-KiB chunks of internal AHB-DMA descriptor memory, only as
 * many as the active raster counts. A single 22-KiB block failed whenever the
 * heap was fragmented (menu "unavailable" after scans/labs/captures). */

static dma_descriptor_t *s_menu_chunks[MENU_NODE_CHUNKS];

unsigned s_menu_chunk_count;

static unsigned s_menu_node_capacity;

static unsigned s_menu_node_count;

volatile bool s_menu_active;

volatile bool s_menu_boot_btn_enabled = true;

volatile int s_menu_cursor;

/* RF and SETUP pages hold an item list: long press enters it, short press
 * steps through the items (the last one is BACK), long press changes one. */
bool s_menu_edit;

unsigned s_menu_item;

/* Calibrations chosen in the menu run after it closes (labs refuse while
 * the standalone menu owns TX). */
volatile bool s_menu_bw_cal_request, s_menu_witness_request;

int s_menu_timeout_ticks;

static const uint8_t s_menu_icons[6][8] = {
    {0x10,0x38,0x54,0x10,0x10,0x38,0x7c,0x00}, /* band / antenna */
    {0x7e,0x42,0x5a,0x5a,0x5a,0x42,0x7e,0x00}, /* channel */
    {0x00,0x40,0x50,0x54,0x55,0x55,0x55,0x00}, /* RF bars */
    {0x10,0x10,0x54,0x38,0x54,0x10,0x10,0x00}, /* AFC crosshair */
    {0x7e,0x42,0x42,0x42,0x7e,0x18,0x3c,0x00}, /* video */
    {0x7c,0x44,0x04,0x1f,0x04,0x44,0x7c,0x00}, /* exit */
};

static const char *const s_menu_nav[6] = {
    "BAND", "CHANNEL", "RF", "SETUP", "VIDEO", "EXIT"
};

/* Only options that do something in this firmware are selectable (menu
 * audit 2026-10-06). DC RECENTER and LEVEL SERVO rewrite the running
 * decoder LUT, which is refused (random read-back while the engine runs);
 * they keep their stored value and stay console-only. */
static const uint8_t s_setup_options[] = {
    C5VRX4_OPT_AGC_MASK, C5VRX4_OPT_SPHASE, C5VRX4_OPT_IDLE_RASTER,
    C5VRX4_OPT_RADIUS_BOOST, C5VRX4_OPT_SYNC_FW, C5VRX4_OPT_CVBS,
    C5VRX4_OPT_HISTORY, C5VRX4_OPT_NATIVE_PATCH, C5VRX4_OPT_HW_DCO,
    C5VRX4_OPT_LINE_FIX, C5VRX4_OPT_EDGE_GEAR,
};

static inline void menu_ui_pixel(int x, int y, uint8_t code)
{
    if ((unsigned)x >= MENU_UI_WIDTH || (unsigned)y >= MENU_UI_LINES) return;
    unsigned x0 = (unsigned)x * MENU_UI_X_SCALE_NUM / MENU_UI_X_SCALE_DEN;
    unsigned x1 = (unsigned)(x + 1) * MENU_UI_X_SCALE_NUM / MENU_UI_X_SCALE_DEN;
    memset(&s_menu_raster.ui[y][x0], code, x1 - x0);
}

static void menu_ui_rect(int x, int y, int w, int h, uint8_t code)
{
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)MENU_UI_WIDTH ? (int)MENU_UI_WIDTH : x + w;
    int y1 = y + h > (int)MENU_UI_LINES ? (int)MENU_UI_LINES : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; ++yy) {
        unsigned sx0 = (unsigned)x0 * MENU_UI_X_SCALE_NUM / MENU_UI_X_SCALE_DEN;
        unsigned sx1 = (unsigned)x1 * MENU_UI_X_SCALE_NUM / MENU_UI_X_SCALE_DEN;
        memset(&s_menu_raster.ui[yy][sx0], code, sx1 - sx0);
    }
}

static void menu_ui_hline(int x, int y, int w, uint8_t code)
{
    menu_ui_rect(x, y, w, 1, code);
}

static void menu_ui_vline(int x, int y, int h, uint8_t code)
{
    menu_ui_rect(x, y, 1, h, code);
}

static void menu_ui_glyph(char ch, int x, int y, uint8_t code, unsigned scale)
{
    unsigned index = (ch >= 32 && ch <= 126) ? (unsigned)ch - 32u : 0u;
    if (scale == 0u) scale = 1u;
    for (unsigned gy = 0; gy < 8u; ++gy) {
        uint8_t bits = s_font8x8[index][gy];
        for (unsigned gx = 0; gx < 8u; ++gx) {
            if (bits & (0x80u >> gx)) {
                menu_ui_rect(x + (int)(gx * scale), y + (int)(gy * scale),
                             (int)scale, (int)scale, code);
            }
        }
    }
}

static void menu_ui_text(const char *str, int x, int y, uint8_t code)
{
    if (!str) return;
    for (; *str && x < (int)MENU_UI_WIDTH; ++str, x += 8) {
        menu_ui_glyph(*str, x, y, code, 1u);
    }
}

static void menu_ui_text_scaled(const char *str, int x, int y, uint8_t code, unsigned scale)
{
    if (!str || scale == 0u) return;
    int advance = (int)(8u * scale);
    for (; *str && x < (int)MENU_UI_WIDTH; ++str, x += advance) {
        menu_ui_glyph(*str, x, y, code, scale);
    }
}

static void menu_ui_text_right(const char *str, int right, int y, uint8_t code)
{
    size_t n = str ? strlen(str) : 0u;
    menu_ui_text(str, right - (int)(n * 8u), y, code);
}

static void menu_ui_icon(unsigned icon, int x, int y, uint8_t code)
{
    if (icon >= 6u) return;
    for (unsigned gy = 0; gy < 8u; ++gy) {
        uint8_t bits = s_menu_icons[icon][gy];
        for (unsigned gx = 0; gx < 8u; ++gx) {
            if (bits & (0x80u >> gx)) menu_ui_pixel(x + (int)gx, y + (int)gy, code);
        }
    }
}

static void menu_ui_value_box(int x, int y, int w, const char *label, const char *value)
{
    menu_ui_rect(x, y, w, 10, UI_PANEL_2);
    menu_ui_hline(x, y, w, UI_DIVIDER);
    menu_ui_text(label, x + 3, y + 1, UI_MUTED);
    menu_ui_text_right(value, x + w - 3, y + 1, UI_WHITE);
}

static void menu_ui_meter(int x, int y, int w, int value, int maximum)
{
    if (maximum <= 0) maximum = 1;
    if (value < 0) value = 0;
    if (value > maximum) value = maximum;
    menu_ui_rect(x, y, w, 5, UI_ROOT);
    menu_ui_rect(x + 1, y + 1, w - 2, 3, UI_PANEL_2);
    int fill = (w - 2) * value / maximum;
    if (fill > 0) menu_ui_rect(x + 1, y + 1, fill, 3, UI_STRONG);
}

static void menu_ui_signal_bars(int x, int y, int quality)
{
    int bars = quality <= 0 ? 0 : (quality >= 100 ? 5 : (quality + 19) / 20);
    for (int i = 0; i < 5; ++i) {
        int h = 2 + i;
        menu_ui_rect(x + i * 3, y + 7 - h, 2, h,
                     i < bars ? UI_WHITE : UI_DIVIDER);
    }
}

static bool menu_count_segment(void *ctx, const uint8_t *data, unsigned length)
{
    unsigned *count = (unsigned *)ctx;
    if (!count || *count >= MENU_MAX_NODES || length > 4092 ||
        ((uintptr_t)data & 3) || (length & 3)) return false;
    ++*count;
    return true;
}

static dma_descriptor_t *menu_node(unsigned index)
{
    return &s_menu_chunks[index / MENU_NODE_CHUNK][index % MENU_NODE_CHUNK];
}

static bool menu_append_segment(void *ctx, const uint8_t *data, unsigned length)
{
    (void)ctx;
    if (s_menu_node_count >= s_menu_node_capacity ||
        length > 4092 || ((uintptr_t)data & 3) || (length & 3)) return false;
    dma_descriptor_t *node = menu_node(s_menu_node_count++);
    memset(node, 0, sizeof(*node));
    node->dw0.size = length;
    node->dw0.length = length;
    node->dw0.owner = 1;
    node->buffer = (void *)data;
    return true; /* Linked after the whole chain is known. */
}

static void menu_free_nodes(void)
{
    for (unsigned k = 0; k < s_menu_chunk_count; ++k) {
        heap_caps_free(s_menu_chunks[k]);
        s_menu_chunks[k] = NULL;
    }
    s_menu_chunk_count = 0;
    s_menu_node_capacity = 0;
    s_menu_node_count = 0;
}

/* Grow only: chunks still referenced by a running menu chain are never
 * freed here, and a failed growth keeps the previous (smaller) chain. */
static esp_err_t menu_reserve_nodes(unsigned nodes)
{
    unsigned chunks = (nodes + MENU_NODE_CHUNK - 1u) / MENU_NODE_CHUNK;
    if (chunks > MENU_NODE_CHUNKS) return ESP_ERR_INVALID_SIZE;
    while (s_menu_chunk_count < chunks) {
        dma_descriptor_t *chunk = (dma_descriptor_t *)heap_caps_aligned_alloc(
            64u, MENU_NODE_CHUNK * sizeof(dma_descriptor_t),
            MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL);
        if (!chunk) return ESP_ERR_NO_MEM;
        s_menu_chunks[s_menu_chunk_count++] = chunk;
        s_menu_node_capacity += MENU_NODE_CHUNK;
    }
    return ESP_OK;
}

static esp_err_t menu_init_buffers_body(void);

static esp_err_t menu_init_buffers(void)
{
    RASTER_WRITE_BEGIN();
    esp_err_t err = menu_init_buffers_body();
    RASTER_WRITE_END();
    return err;
}

static esp_err_t menu_init_buffers_body(void)
{
    menu_raster_init(&s_menu_raster, s_video_std);

    /* Count the active raster first; reserve only that many descriptors.
     * The chain may grow on an NTSC -> PAL switch inside the menu. */
    unsigned required_nodes = 0;
    if (!menu_raster_emit(&s_menu_raster, s_video_std,
                          menu_count_segment, &required_nodes) ||
        required_nodes == 0 || required_nodes > MENU_MAX_NODES) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = menu_reserve_nodes(required_nodes);
    if (err != ESP_OK) return err;

    s_menu_node_count = 0;
    if (!menu_raster_emit(&s_menu_raster, s_video_std,
                          menu_append_segment, NULL) ||
        s_menu_node_count != required_nodes) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (unsigned i = 0; i < s_menu_node_count; ++i)
        menu_node(i)->next = menu_node((i + 1u) % s_menu_node_count);
    menu_render_menu();
    sync_dma_c2m(&s_menu_raster, sizeof(s_menu_raster));
    for (unsigned k = 0; k < s_menu_chunk_count; ++k)
        sync_dma_c2m(s_menu_chunks[k], MENU_NODE_CHUNK * sizeof(dma_descriptor_t));
    return ESP_OK;
}

const char *video_standard_name(video_standard_t standard)
{
    return standard == VIDEO_STD_PAL ? "PAL 625i" : "NTSC 525i";
}

video_standard_t resolved_menu_standard(void)
{
    if (s_video_std_mode == VIDEO_STD_MODE_PAL) return VIDEO_STD_PAL;
    if (s_video_std_mode == VIDEO_STD_MODE_NTSC) return VIDEO_STD_NTSC;
    /* AUTO with no live detection (no carrier, menu after reboot): start in
     * the last stable live standard so the goggles need no PAL/NTSC switch. */
    if (!s_detected_video_std_valid && c5vrx4_last_standard() != C5VRX4_STD_UNKNOWN)
        return c5vrx4_last_standard() ? VIDEO_STD_PAL : VIDEO_STD_NTSC;
    return s_detected_video_std_valid ? s_detected_video_std : s_video_std;
}

static const char *agc_state_name(void)
{
    return s_agc_state == AGC_STATE_TRACK ? "TRACK" :
           s_agc_state == AGC_STATE_LEARN ? "LEARN" : "SEARCH";
}

static const char *afc_mode_name(void)
{
    return s_afc_mode == AFC_MODE_AUTO ? "AUTO" :
           s_afc_mode == AFC_MODE_HOLD ? "HOLD" : "OFF";
}

static void menu_draw_shell(void)
{
    menu_ui_rect(0, 0, MENU_UI_WIDTH, MENU_UI_LINES, UI_PANEL);

    menu_ui_rect(0, 0, MENU_UI_WIDTH, 8, UI_HEADER);
    menu_ui_rect(4, 1, 6, 6, UI_WHITE);
    menu_ui_rect(6, 3, 2, 2, UI_HEADER);
    menu_ui_text("C5VRX", 14, 0, UI_WHITE);

    const fpv_channel_t *ch = rf_get_current_channel();
    char buf[24];
    menu_ui_text(ch->name, 96, 0, UI_WHITE);
    snprintf(buf, sizeof(buf), "%uM", ch->freq_mhz);
    menu_ui_text(buf, 128, 0, UI_MUTED);
    /* Native gain is the vendor AGC's own; s_current_gain is not it. */
    if (rf_native_agc_active()) snprintf(buf, sizeof(buf), "NAT");
    else snprintf(buf, sizeof(buf), "G%u", s_current_gain);
    menu_ui_text(buf, 208, 0, UI_WHITE);
    menu_ui_text(agc_state_name(), 244, 0, UI_MUTED);
    menu_ui_signal_bars(320, 0, s_signal_strength);
    menu_ui_text(s_video_std == VIDEO_STD_PAL ? "PAL" : "NTSC", 344, 0, UI_WHITE);

    menu_ui_rect(0, 8, 92, MENU_UI_LINES - 8, UI_ROOT);
    menu_ui_vline(91, 8, MENU_UI_LINES - 8, UI_DIVIDER);
    for (unsigned i = 0; i < 6u; ++i) {
        int y = 8 + (int)i * 8;
        bool selected = (int)i == s_menu_cursor;
        if (selected) {
            menu_ui_rect(0, y, 91, 8, UI_SELECTED);
            menu_ui_rect(0, y, 3, 8, UI_WHITE);
            menu_ui_hline(3, y, 88, UI_SELECTED_EDGE);
        }
        uint8_t ink = selected ? UI_WHITE : UI_MUTED;
        menu_ui_icon(i, 5, y, ink);
        menu_ui_text(s_menu_nav[i], 18, y, ink);
    }
}

static void menu_draw_page_title(const char *title, const char *tag)
{
    menu_ui_text(title, 100, 10, UI_WHITE);
    if (tag && *tag) menu_ui_text_right(tag, 376, 10, UI_MUTED);
    menu_ui_hline(100, 19, 276, UI_DIVIDER);
}

static void menu_draw_band_page(void)
{
    char value[24];
    menu_draw_page_title("RF BAND", "48 CHANNELS");

    /* Band names are long enough to collide with the label when both are
     * right/left aligned inside the old 130 px value box. Give the active band
     * its own full-width row so every built-in band name remains readable. */
    snprintf(value, sizeof(value), "%s", rf_get_band_name(rf_get_current_band()));
    menu_ui_value_box(100, 22, 276, "ACTIVE BAND", value);

    snprintf(value, sizeof(value), "%s", rf_get_current_channel()->name);
    menu_ui_value_box(100, 34, 130, "CHANNEL", value);
    menu_ui_text("LONG: NEXT BAND", 238, 35, UI_WHITE);
    menu_ui_text("SHORT PRESS MOVES CURSOR", 100, 47, UI_MUTED);
}

static void menu_draw_channel_page(void)
{
    const fpv_channel_t *ch = rf_get_current_channel();
    char buf[24];
    menu_draw_page_title("CHANNEL", s_channel_scan_active ? "SCANNING" : "LONG: NEXT / HOLD: SCAN");

    menu_ui_rect(100, 22, 108, 23, UI_PANEL_2);
    menu_ui_vline(100, 22, 23, UI_WHITE);
    menu_ui_text("ACTIVE", 108, 23, UI_MUTED);
    menu_ui_text_scaled(ch->name, 108, 29, UI_WHITE, 2u);

    menu_ui_rect(216, 22, 160, 23, UI_ROOT);
    menu_ui_text("CENTER", 224, 23, UI_MUTED);
    snprintf(buf, sizeof(buf), "%u", ch->freq_mhz);
    menu_ui_text_scaled(buf, 224, 29, UI_WHITE, 2u);
    menu_ui_text("MHZ", 304, 36, UI_MUTED);

    menu_ui_text("SIGNAL", 100, 47, UI_MUTED);
    menu_ui_meter(156, 48, 100,
                  s_channel_scan_active ? (int)s_channel_scan_progress : s_signal_strength, 100);
    snprintf(buf, sizeof(buf), s_channel_scan_active ? "%u%%" : "S%u",
             s_channel_scan_active ? s_channel_scan_progress : (unsigned)s_signal_strength);
    menu_ui_text(buf, 264, 47, UI_WHITE);
    if (rf_native_agc_active()) snprintf(buf, sizeof(buf), "NATIVE");
    else snprintf(buf, sizeof(buf), "G%u", s_current_gain);
    menu_ui_text_right(buf, 376, 47, UI_WHITE);
}

static void menu_draw_video_page(void)
{
    char detected[24];
    menu_draw_page_title("VIDEO OUTPUT", "DEFAULT");
    menu_ui_value_box(100, 22, 130, "DAC", output_mode_name());
    menu_ui_value_box(238, 22, 138, "DEMOD", demod_mode_name());
    menu_ui_value_box(100, 34, 130, "STANDARD",
                      s_video_std == VIDEO_STD_PAL ? "PAL" : "NTSC");
    if (s_detected_video_std_valid) {
        snprintf(detected, sizeof(detected), "%s %u",
                 s_detected_video_std == VIDEO_STD_PAL ? "PAL" : "NTSC",
                 s_last_line_period_20m);
    } else {
        snprintf(detected, sizeof(detected), "SEARCHING");
    }
    menu_ui_value_box(238, 34, 138, "DETECTED", detected);
    menu_ui_text(s_video_std_mode == VIDEO_STD_MODE_AUTO ? "LONG: STANDARD (NOW AUTO)" :
                 "LONG: STANDARD (FIXED)", 100, 47, UI_MUTED);
}

unsigned menu_item_count(void)
{
    return s_menu_cursor == 2 ? RF_ITEM_COUNT :
           s_menu_cursor == 3 ? SETUP_ITEM_COUNT : 0u;
}

/* Boot-time choices written in this menu session but not yet running. */
bool menu_changes_pending(void)
{
    return c5vrx4_options_pending() || rf_native_agc_requested() != rf_native_agc_active();
}

static void menu_option_text(unsigned option, char *value, size_t n)
{
    /* Native-AGC-only options do nothing under Direct V5, and line repair
     * runs inside the sync flywheel: say so. */
    bool native_only = option == C5VRX4_OPT_AGC_MASK || option == C5VRX4_OPT_NATIVE_PATCH;
    bool needs_fw = option == C5VRX4_OPT_LINE_FIX &&
                    !strcmp(c5vrx4_option_value(C5VRX4_OPT_SYNC_FW), "OFF");
    snprintf(value, n, "%s%s%s", c5vrx4_option_value(option),
             native_only && !rf_native_agc_requested() ? " (NATIVE)" :
             needs_fw ? " (FLYWHEEL)" : "",
             c5vrx4_option_pending(option) ? " *" : "");
}

static const char *menu_item_text(unsigned item, char *value, size_t n)
{
    if (s_menu_cursor == 2) {
        switch (item) {
        case RF_ITEM_GAIN: {
            bool next = rf_native_agc_requested();
            snprintf(value, n, "%s%s", next ? "NATIVE AGC" : "DIRECT V5",
                     next != rf_native_agc_active() ? " *" : "");
            return "GAIN";
        }
        case RF_ITEM_DIGITAL_BW:
            snprintf(value, n, "%s %s", rf_bw_mode_name(), s_current_bw40 ? "40" : "20");
            return "DIGITAL BW";
        case RF_ITEM_ANALOG_BW:
            if (c5vrx4_option_pending(C5VRX4_OPT_FIXED_BW) || !c5vrx4_fixed_bw_enabled())
                menu_option_text(C5VRX4_OPT_FIXED_BW, value, n);
            else if (c5vrx4_bw_code() == C5VRX4_BW_UNCALIBRATED)
                snprintf(value, n, "FIXED UNCAL");
            else
                snprintf(value, n, "FIXED C%u %uM", c5vrx4_bw_code(),
                         (c5vrx4_bw_width_khz() + 500u) / 1000u);
            return "ANALOG BW";
        case RF_ITEM_LANES:
            menu_option_text(C5VRX4_OPT_LANES, value, n);
            return "IQ LANES";
        case RF_ITEM_CAL_BW:
            snprintf(value, n, "VTX OFF");
            return "CALIBRATE BW";
        default:
            snprintf(value, n, "%s", rf_native_agc_active() ? "VTX ON" : "NATIVE ONLY");
            return "CALIBRATE AGC";
        }
    }
    if (item == SETUP_ITEM_AFC) {
        if (c5vrx4_reference_demod()) {
            snprintf(value, n, "OFF (REFERENCE)");
            return "AFC";
        }
        snprintf(value, n, "%s", afc_mode_name());
        return "AFC";
    }
    if (item == SETUP_ITEM_BOOT_MENU) {
        snprintf(value, n, "%s", s_menu_boot_btn_enabled ? "ON" : "OFF");
        return "BOOT MENU";
    }
    unsigned option = s_setup_options[item - SETUP_ITEM_OPTIONS];
    menu_option_text(option, value, n);
    return c5vrx4_option_label(option);
}

static void menu_draw_list(const char *title)
{
    const unsigned count = menu_item_count(), rows = count + 1u;
    menu_draw_page_title(title, s_menu_edit ? "SHORT:NEXT LONG:SET" : "LONG: OPEN");
    /* Four rows fit between the title and the bottom of the UI band. */
    unsigned first = s_menu_edit && s_menu_item >= 4u ? s_menu_item - 3u : 0u;
    for (unsigned r = 0; r < 4u && first + r < rows; ++r) {
        unsigned item = first + r;
        int y = 22 + (int)r * 8;
        bool selected = s_menu_edit && item == s_menu_item;
        if (selected) menu_ui_rect(100, y, 276, 8, UI_SELECTED);
        uint8_t ink = selected ? UI_WHITE : UI_MUTED;
        if (item == count) { menu_ui_text("BACK", 104, y, ink); continue; }
        char value[24];
        menu_ui_text(menu_item_text(item, value, sizeof(value)), 104, y, ink);
        menu_ui_text_right(value, 372, y, UI_WHITE);
    }
}

/* Returns false when the item closed the menu (nothing left to render). */
bool menu_item_apply(unsigned item)
{
    if (s_menu_cursor == 2) {
        switch (item) {
        case RF_ITEM_GAIN: {
            bool next = !rf_native_agc_requested();
            esp_err_t err = rf_request_native_agc_boot(next);
            printf("[MENU: GAIN] next=%s err=%s (applies on SAVE AND EXIT)\n",
                   next ? "native" : "direct_v5", esp_err_to_name(err));
            return true;
        }
        case RF_ITEM_DIGITAL_BW:
            cycle_rf_bandwidth_mode();
            settings_save();
            printf("[MENU: RF BW] Mode -> %s (active %s)\n",
                   rf_bw_mode_name(), s_current_bw40 ? "BW40" : "BW20");
            return true;
        case RF_ITEM_ANALOG_BW:
            (void)c5vrx4_option_cycle(C5VRX4_OPT_FIXED_BW);
            return true;
        case RF_ITEM_LANES:
            (void)c5vrx4_option_cycle(C5VRX4_OPT_LANES);
            return true;
        case RF_ITEM_CAL_BW:
        case RF_ITEM_CAL_AGC:
            if (item == RF_ITEM_CAL_AGC && !rf_native_agc_active()) {
                printf("[MENU: CAL AGC] refused: native AGC only (GAIN -> NATIVE AGC, save and exit)\n");
                return true;
            }
            if (item == RF_ITEM_CAL_BW) s_menu_bw_cal_request = true;
            else s_menu_witness_request = true;
            settings_save();
            video_set_menu_mode(false);
            printf("[MENU: CAL %s] runs now on live RX\n", item == RF_ITEM_CAL_BW ? "BW" : "AGC");
            return false;
        default:
            return true;
        }
    }
    if (item == SETUP_ITEM_AFC) {
        if (s_afc_mode == AFC_MODE_AUTO) {
            if (c5vrx4_reference_demod()) { s_afc_mode = AFC_MODE_OFF; return true; }
            s_afc_mode = AFC_MODE_HOLD;
        } else if (s_afc_mode == AFC_MODE_HOLD) {
            s_afc_mode = AFC_MODE_OFF;
            apply_frequency_offset_khz_tracked(0);
        } else {
            s_afc_mode = c5vrx4_reference_demod() ? AFC_MODE_OFF : AFC_MODE_AUTO;
        }
        printf("[MENU: AFC] Mode -> %s\n", afc_mode_name());
        settings_save();
        return true;
    }
    if (item == SETUP_ITEM_BOOT_MENU) {
        s_menu_boot_btn_enabled = !s_menu_boot_btn_enabled;
        settings_save();
        printf("[MENU: BOOT MENU] %s (3 s BOOT hold always opens recovery)\n",
               s_menu_boot_btn_enabled ? "on" : "off");
        return true;
    }
    (void)c5vrx4_option_cycle(s_setup_options[item - SETUP_ITEM_OPTIONS]);
    return true;
}

static void menu_draw_exit_page(void)
{
    menu_draw_page_title("SAVE AND EXIT", "");
    menu_ui_rect(100, 23, 276, 20, UI_PANEL_2);
    menu_ui_vline(100, 23, 20, UI_WHITE);
    menu_ui_text("RETURN TO LIVE VIDEO", 116, 25, UI_WHITE);
    menu_ui_text("LONG PRESS TO EXIT", 116, 34, UI_MUTED);
    if (menu_changes_pending()) {
        menu_ui_text("REBOOTS TO APPLY (*) CHANGES", 100, 47, UI_WHITE);
        return;
    }
    menu_ui_text("12S AUTO EXIT ENABLED", 100, 47, UI_MUTED);
}

/* Idle raster picture: blanking-level black (the raster's own blank code)
 * with one dim status line. Sync, equalizing/broad pulses and burst come
 * unchanged from the BT.470 menu raster. */
static void menu_render_idle(void)
{
    memset(s_menu_raster.ui, 20, sizeof(s_menu_raster.ui));
    const fpv_channel_t *ch = rf_get_current_channel();
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "C5VRX %s %uM NO SIGNAL", ch->name, ch->freq_mhz);
    if (n < 0) n = 0;
    if (n > (int)(MENU_UI_WIDTH / 8u)) n = (int)(MENU_UI_WIDTH / 8u);
    menu_ui_text(buf, ((int)MENU_UI_WIDTH - n * 8) / 2, (int)MENU_UI_LINES / 2 - 4, UI_MUTED);
    sync_dma_c2m(s_menu_raster.ui, sizeof(s_menu_raster.ui));
}

static void menu_render_menu_body(void);

void menu_render_menu(void)
{
    RASTER_WRITE_BEGIN();
    menu_render_menu_body();
    RASTER_WRITE_END();
}

static void menu_render_menu_body(void)
{
    if (s_idle.active) { menu_render_idle(); return; }
    memset(s_menu_raster.ui, UI_ROOT, sizeof(s_menu_raster.ui));
    menu_draw_shell();

    switch (s_menu_cursor) {
    case 0: menu_draw_band_page(); break;
    case 1: menu_draw_channel_page(); break;
    case 2: menu_draw_list("RF FRONTEND"); break;
    case 3: menu_draw_list("SETUP"); break;
    case 4: menu_draw_video_page(); break;
    default: menu_draw_exit_page(); break;
    }

    sync_dma_c2m(s_menu_raster.ui, sizeof(s_menu_raster.ui));
}

/* Directly test whether C5VRX's own 40/80 MHz parallel video output raises the
 * pre-Q4 receiver noise floor. RX remains the measurement source while TX is
 * removed and all six resistor-DAC GPIOs are held static low. */

static void start_menu_tx(void)
{
    /* IDF owns the channel allocation and stop/reset lifecycle. While its TX
     * transaction queue is empty, start our SRAM scatter chain directly.
     * No driver-private structure access and no active descriptor rewiring. */
    ESP_ERROR_CHECK(parlio_tx_unit_enable(s_tx));

    /* A 4-bit->menu reconfiguration can allocate a different GDMA channel. */
    s_tx_dma_ch = -1;
    for (int i = 0; i < 3; ++i) {
        if (AHB_DMA.channel[i].out.out_peri_sel.peri_out_sel_chn == 9) {
            s_tx_dma_ch = i;
            break;
        }
    }
    ESP_ERROR_CHECK(s_tx_dma_ch >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND);

    quiet_tx_interrupts();
    parlio_ll_tx_enable_clock(&PARL_IO, false);
    parlio_ll_tx_reset_clock(&PARL_IO);
    parlio_ll_tx_reset_fifo(&PARL_IO);
    parlio_ll_tx_set_idle_data_value(&PARL_IO, DAC_IDLE_CODE);
    parlio_ll_tx_set_eof_condition(&PARL_IO, PARLIO_LL_TX_EOF_COND_DATA_LEN);
    parlio_ll_tx_set_trans_bit_len(&PARL_IO, 1);
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    AHB_DMA.out_link_addr[s_tx_dma_ch].val = (uint32_t)menu_node(0);
    AHB_DMA.channel[s_tx_dma_ch].out.out_link.outlink_start_chn = 1;
    /* Board 2026-10-07: a 1 ms busy-wait with ESP_ERROR_CHECK rebooted the
     * receiver when the idle raster started while its task (priority 3,
     * sharing time slices with the V5 observer, below the flywheel) lost one
     * 1 ms slice. Wait up to 50 ms, yielding; if TX is still not ready, start
     * anyway (PARLIO outputs the idle code until data arrives) - never abort. */
    int64_t deadline = esp_timer_get_time() + 50000;
    while (!parlio_ll_tx_is_ready(&PARL_IO)) {
        if (esp_timer_get_time() >= deadline) {
            printf("MENU_TX not_ready_after_us=50000 action=start_anyway\n");
            break;
        }
        taskYIELD();
    }
    parlio_ll_tx_start(&PARL_IO, true);
    parlio_ll_tx_enable_clock(&PARL_IO, true);
}

void video_set_menu_mode(bool active)
{
    if (s_pre_q4_probe_active) return;
    if (active && !MENU_RUNTIME_ENABLED) return;
    if (s_menu_active == active) return;

    if (active) {
        /* Allocate/render before touching the live pipeline. If memory is
         * unavailable, keep flight video running instead of rebooting. */
        s_video_std = resolved_menu_standard();
        s_menu_edit = false;
        s_menu_item = 0;
        esp_err_t menu_err = menu_init_buffers();
        if (menu_err != ESP_OK) {
            ESP_LOGE(TAG, "menu unavailable: %s (dma_desc free=%u largest=%u, need %u x %u B chunks)",
                     esp_err_to_name(menu_err),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL),
                     (unsigned)MENU_NODE_CHUNKS,
                     (unsigned)(MENU_NODE_CHUNK * sizeof(dma_descriptor_t)));
            menu_free_nodes();
            return;
        }

        ESP_ERROR_CHECK(parlio_tx_unit_disable(s_tx));
        /* Live -> menu: stop the live producer once. */
    c5v4_level_hw_stop();
    ESP_ERROR_CHECK(bitscrambler_disable(s_flight_bs));

        /* Menu and live output use the same six-bit DAC wiring. */
        start_menu_tx();
    } else {
        idle_raster_abandon(&s_idle);
        ESP_ERROR_CHECK(parlio_tx_unit_disable(s_tx));
        /* Menu -> live: the BitScrambler is already disabled; do not disable twice. */
        start_flight_demodulator();
        ESP_ERROR_CHECK(parlio_rx_unit_disable(s_rx));
        ESP_ERROR_CHECK(parlio_rx_unit_enable(s_rx, false));
        ESP_ERROR_CHECK(parlio_tx_unit_enable(s_tx));

        s_tx_dma_ch = -1;
        for (int i = 0; i < 3; ++i) {
            if (AHB_DMA.channel[i].out.out_peri_sel.peri_out_sel_chn == 9) {
                s_tx_dma_ch = i;
                break;
            }
        }
        ESP_ERROR_CHECK(s_tx_dma_ch >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND);

        quiet_tx_interrupts();
        ESP_ERROR_CHECK(start_rx());
        AHB_DMA.in_intr[s_rx_dma_ch].ena.val = 0;
        PARL_IO.rx_genrl_cfg.rx_eof_gen_sel = 1;
        esp_rom_delay_us((RAW_RING_BYTES / 2ULL) * 1000000ULL / IQ_RATE_HZ);
        ESP_ERROR_CHECK(start_tx());
        quiet_tx_interrupts();
        patch_descriptors_clear_eof(s_rx_dma_ch, true);
        patch_descriptors_clear_eof(s_tx_dma_ch, false);

        /* Live TX now owns its own driver descriptors; the standalone menu
         * scatter chain is no longer referenced by GDMA. Return its large
         * descriptor allocation to internal heap for normal flight. */
        menu_free_nodes();
    }
    s_menu_timeout_ticks = 0;
    s_menu_active = active;
}

/* Open the user menu; from the idle raster this only redraws (TX already
 * belongs to the standalone raster). */
void video_open_menu(void)
{
    if (s_idle.active) {
        idle_raster_abandon(&s_idle);
        s_menu_timeout_ticks = 0;
        menu_render_menu();
        return;
    }
    video_set_menu_mode(true);
}

void menu_cycle_standard_mode(void)
{
    const video_standard_mode_t previous_mode = s_video_std_mode;
    const video_standard_t previous_std = s_video_std;
    if (s_menu_active) ESP_ERROR_CHECK(parlio_tx_unit_disable(s_tx));
    if (s_video_std_mode == VIDEO_STD_MODE_AUTO) {
        s_video_std_mode = VIDEO_STD_MODE_NTSC;
        s_video_std = VIDEO_STD_NTSC;
    } else if (s_video_std_mode == VIDEO_STD_MODE_NTSC) {
        s_video_std_mode = VIDEO_STD_MODE_PAL;
        s_video_std = VIDEO_STD_PAL;
    } else {
        s_video_std_mode = VIDEO_STD_MODE_AUTO;
        s_video_std = resolved_menu_standard();
    }
    if (s_menu_active) {
        esp_err_t err = menu_init_buffers();
        if (err != ESP_OK) {
            /* PAL needs ~312 more descriptors than NTSC. If they cannot be
             * allocated, keep the previous standard: its chain still fits
             * (capacity only grows) instead of rebooting. */
            printf("[MENU] %s unavailable: %s (free=%u largest=%u); standard kept\n",
                   video_standard_name(s_video_std), esp_err_to_name(err),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL));
            s_video_std_mode = previous_mode;
            s_video_std = previous_std;
            ESP_ERROR_CHECK(menu_init_buffers());
        }
        start_menu_tx();
    }
    settings_save();
}
