#include "display.h"
#include "rtc_clock.h"
#include "appconfig.h"
#include "wifi.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>

#include <lvgl.h>
#include <string.h>


LOG_MODULE_DECLARE(app);
#define MSG_SIZE 256
K_MSGQ_DEFINE(ui_msgq, sizeof(char[MSG_SIZE]), 20, 4);


#ifdef CONFIG_DISPLAY_LOG_BACKEND
// output
static int display_char_out(uint8_t *data, size_t length, void *ctx)
{
    static char line[MSG_SIZE];
    static size_t pos;

    for (size_t i = 0; i < length; i++) {
        if (pos < MSG_SIZE - 1) {
            line[pos++] = data[i];
        }

        if (data[i] == '\n') {
            line[pos] = '\0';
            k_msgq_put(&ui_msgq, line, K_NO_WAIT);
            pos = 0;
        }
    }

    return length;
}

static uint8_t log_buf[MSG_SIZE];
LOG_OUTPUT_DEFINE(display_log_output, display_char_out, log_buf, sizeof(log_buf));

// backend
static void display_log_backend_process(
    const struct log_backend *const backend,
    union log_msg_generic *msg)
{
    ARG_UNUSED(backend);
    log_output_msg_process(&display_log_output, &msg->log, 0);
}

static void display_log_backend_dropped(const struct log_backend *const backend, uint32_t cnt)
{
    ARG_UNUSED(backend);
    ARG_UNUSED(cnt);
}

static void display_log_backend_panic(const struct log_backend *const backend)
{
    ARG_UNUSED(backend);
}

static const struct log_backend_api display_log_backend_api = {
    .process = display_log_backend_process,
    .dropped = display_log_backend_dropped,
    .panic = display_log_backend_panic,
};

LOG_BACKEND_DEFINE(display_log_backend, display_log_backend_api, true);
#endif

void display_printf(const char* format, ...) {
  char buf[MSG_SIZE];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  k_msgq_put(&ui_msgq, buf, K_NO_WAIT);
  va_end(args);
}

static void ui_worker(void *arg1, void *arg2, void *arg3);

/* WiFi icon */
static const uint8_t icon_wifi_map[] = {
    0b00000000,
    0b01111110,
    0b10000001,
    0b00111100,
    0b01000010,
    0b00011000,
    0b00011000,
    0b00000000,
};

const lv_image_dsc_t icon_wifi = {
    .header = {
        .w = 8,
        .h = 8,
        .cf = LV_COLOR_FORMAT_A1,
    },
    .data_size = sizeof(icon_wifi_map),
    .data = icon_wifi_map,
};

#define UI_THREAD_STACK_SIZE 8192
#define UI_THREAD_PRIORITY   4
static K_THREAD_STACK_DEFINE(ui_stack, UI_THREAD_STACK_SIZE);
static struct k_thread ui_thread;

int display_start(void) {
  LOG_INF("Starting UI thread");
  k_thread_create(&ui_thread,
              ui_stack,
              K_THREAD_STACK_SIZEOF(ui_stack),
              ui_worker,
              NULL, NULL, NULL,
              UI_THREAD_PRIORITY,
              0,
              K_NO_WAIT);
  return 0;
}

static int ui_wdt_channel;
void ui_wdt_cb(int channel_id, void *user_data)
{
    ARG_UNUSED(channel_id);
    ARG_UNUSED(user_data);

    LOG_ERR("UI thread stuck, rebooting");
    sys_reboot(SYS_REBOOT_COLD);
}

const char *get_last_utf8_offset(const char *text, size_t offset)
{
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *p = start + offset;

    // Walk backward while we're on continuation bytes (10xxxxxx)
    while (p > start && ((*p & 0xC0) == 0x80)) {
        p--;
    }

    return (const char *)p;
}

LV_FONT_DECLARE(font_tom_thumb);
static void ui_worker(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    LOG_INF("UI thread started");

    if(!lv_scr_act()) {
        LOG_ERR("No LVGL active screen");
        return;
    }

    // Set the default font
    lv_obj_set_style_text_font(lv_scr_act(), &font_tom_thumb, 0);

    // Root layout
    lv_obj_t *lvobj_flex = lv_obj_create(lv_scr_act());
    lv_obj_set_size(lvobj_flex, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(lvobj_flex, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lvobj_flex, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(lvobj_flex, 0, 0);

    // Status bar (top, 8px, white)
    lv_obj_t *lvobj_bar = lv_obj_create(lvobj_flex);
    lv_obj_set_size(lvobj_bar, LV_PCT(100), 8);
    lv_obj_set_style_bg_color(lvobj_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lvobj_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(lvobj_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(lvobj_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lvobj_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lvobj_bar,
                        LV_FLEX_ALIGN_END,    // main axis (horizontal → right)
                        LV_FLEX_ALIGN_CENTER, // cross axis (vertical → center)
                        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(lvobj_bar, 0, 0);
    lv_obj_set_style_pad_right(lvobj_bar, 1, 0);
    lv_obj_set_style_pad_column(lvobj_bar, 1, 0);

    // Status bar items
    // Wifi icon
    lv_obj_t *lvobj_imgwifi = lv_img_create(lvobj_bar);
    lv_img_set_src(lvobj_imgwifi, &icon_wifi);
    lv_obj_set_style_img_recolor(lvobj_imgwifi, lv_color_white(), 0);
    lv_obj_add_flag(lvobj_imgwifi, LV_OBJ_FLAG_HIDDEN);

    // Clock
    lv_obj_t *lvobj_lbtime = lv_label_create(lvobj_bar);
    lv_obj_set_style_pad_top(lvobj_lbtime, 2, 0);
    lv_obj_set_style_pad_left(lvobj_lbtime, 2, 0);
    lv_obj_set_style_text_color(lvobj_lbtime, lv_color_white(), 0);
    lv_obj_set_style_text_font(lvobj_lbtime, &font_tom_thumb, 0);

    // Text area (log messages)
    lv_obj_t *lvobj_textarea = lv_textarea_create(lvobj_flex);
    lv_obj_set_width(lvobj_textarea, LV_PCT(100));
    lv_obj_set_flex_grow(lvobj_textarea, 1);
    lv_textarea_set_one_line(lvobj_textarea, false);
    lv_obj_clear_flag(lvobj_textarea, LV_OBJ_FLAG_CLICKABLE);
    lv_textarea_set_cursor_click_pos(lvobj_textarea, false);
    lv_obj_set_style_text_opa(lvobj_textarea, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_scrollbar_mode(lvobj_textarea, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_text_color(lvobj_textarea, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lvobj_textarea, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lvobj_textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(lvobj_textarea, 1, 0);
    lv_obj_set_style_pad_bottom(lvobj_textarea, 2, 0);
    lv_textarea_add_text(lvobj_textarea, "UI thread started\n");

    // Task WDT (to reboot if the UI thread is stuck)
    ui_wdt_channel = task_wdt_add(500, ui_wdt_cb, NULL); 
    char msg[MSG_SIZE];
    char clock_buf[6];
    uint32_t last_update_clock = 0;
    while (1) {
        bool updated = false;
        while (k_msgq_get(&ui_msgq, &msg, K_NO_WAIT) == 0) {
            lv_textarea_add_text(lvobj_textarea, msg);
            updated = true;
        }

        uint32_t uptime = k_uptime_get_32();

        if (uptime - last_update_clock > 1000) {
            struct rtc_time   tm;
            
            if(!rtc_get_time_eu_local(&tm, app_config.tz_offset)) {
                snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
                lv_label_set_text(lvobj_lbtime, clock_buf);
            } else {
                lv_label_set_text(lvobj_lbtime, "??:??");
            }

            if (wifi_is_connected()) {
                lv_obj_clear_flag(lvobj_imgwifi, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(lvobj_imgwifi, LV_OBJ_FLAG_HIDDEN);
            }

            last_update_clock = uptime;
        }

        if (updated) {
            const char *txt = lv_textarea_get_text(lvobj_textarea);
            if (strlen(txt) > 1024 + 512 + 128) {
                const char *keep = get_last_utf8_offset(txt, 128);
                if (keep > txt) {
                    char tmp[1526];
                    strncpy(tmp, keep, sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = 0;
                    lv_textarea_set_text(lvobj_textarea, tmp);
                }
            }
            lv_obj_update_layout(lvobj_textarea);
            lv_textarea_set_cursor_pos(lvobj_textarea, LV_TEXTAREA_CURSOR_LAST);
            lv_obj_scroll_to_y(lvobj_textarea, LV_COORD_MAX, LV_ANIM_OFF);
        }

      lv_timer_handler();
      task_wdt_feed(ui_wdt_channel);
      k_sleep(K_MSEC(50));
    }
}