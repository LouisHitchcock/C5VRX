/* C5VRX-5 by Twotoz and the C5VRX contributors: menu renderer for the host.
 *
 * Wraps main/menu_ui.c for menu_mock.py (ctypes). shim_view_t holds only
 * ints and strings so Python can mirror it without knowing menu_view_t's
 * bool layout; a field added to menu_view_t defaults to zero here until it
 * is added to both. menu_mock.py builds this into build/menu_ui.dll (or .so). */
#include <string.h>
#include "menu_ui.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    int cursor, cal_sub;
    const char *channel;
    int freq_mhz, band, channel_in_band;
    const char *band_name;
    int signal, video, afc_locked, afc_off;
    int level_valid, level_dbm;
    int scan_active, scan_progress;
    int std_mode, std_out_pal, detected_valid, detected_pal;
    int output;
    const char *output_name;
    const char *gain_mode;
    int gain_auto, gain, gain_max;
    const char *filter_mode, *filter_active, *filter_recommended;
    int filter_width_khz, narrow_ok, wide_ok;
    int dfilt_mode, dfilt_default;
    int deviation_x100, offset_khz, cn_x10;
    const char *firmware;
} shim_view_t;

static menu_ui_rows_t s_ui;

EXPORT unsigned shim_lines(void) { return MENU_UI_LINES; }
EXPORT unsigned shim_bytes(void) { return MENU_UI_BYTES; }
EXPORT unsigned shim_items(void) { return MENU_ITEMS; }
EXPORT unsigned shim_cal_items(void) { return MENU_CAL_ITEMS; }
EXPORT unsigned shim_overflow(void) { return menu_ui_overflow; }

EXPORT void shim_render(uint8_t *out, const shim_view_t *s)
{
    const menu_view_t v = {
        .cursor = s->cursor, .cal_sub = s->cal_sub,
        .channel = s->channel, .freq_mhz = (unsigned)s->freq_mhz, .band = (unsigned)s->band,
        .channel_in_band = (unsigned)s->channel_in_band, .band_name = s->band_name,
        .band_letters = "RABEFL", .band_count = 6,
        .signal = s->signal, .video = s->video, .afc_locked = s->afc_locked, .afc_off = s->afc_off,
        .level_valid = s->level_valid, .level_above_noise = s->level_valid, .level_dbm = s->level_dbm,
        .dc_cal = false, .dc_need = "", .gain_fixed = !s->gain_auto,
        .scan_active = s->scan_active, .scan_progress = (unsigned)s->scan_progress,
        .std_mode = s->std_mode, .std_out_pal = s->std_out_pal,
        .detected_valid = s->detected_valid, .detected_pal = s->detected_pal,
        .output = (unsigned)s->output, .output_name = s->output_name,
        .gain_mode = s->gain_mode, .gain_auto = s->gain_auto, .gain = (unsigned)s->gain,
        .gain_max = (unsigned)s->gain_max,
        .filter_mode = s->filter_mode, .filter_active = s->filter_active,
        .filter_recommended = s->filter_recommended, .filter_width_khz = (unsigned)s->filter_width_khz,
        .narrow_ok = s->narrow_ok, .wide_ok = s->wide_ok,
        .dfilt_mode = s->dfilt_mode, .dfilt_default = s->dfilt_default,
        .deviation_x100 = s->deviation_x100, .offset_khz = s->offset_khz, .cn_x10 = s->cn_x10,
        .firmware = s->firmware,
    };
    menu_ui_render(s_ui, &v);
    memcpy(out, s_ui, sizeof(s_ui));
}

EXPORT void shim_render_status(uint8_t *out, const char *title, const char *line1, const char *line2,
                               int percent, const char *channel, unsigned freq_mhz)
{
    const menu_status_view_t v = {
        .title = title, .line1 = line1, .line2 = line2, .percent = percent, .channel = channel,
        .freq_mhz = freq_mhz,
    };
    menu_ui_render_status(s_ui, &v);
    memcpy(out, s_ui, sizeof(s_ui));
}

EXPORT void shim_render_splash(uint8_t *out, int percent)
{
    menu_ui_render_splash_health(s_ui, percent, false);
    memcpy(out, s_ui, sizeof(s_ui));
}
