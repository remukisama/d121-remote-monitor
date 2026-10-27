/* Dark A, LVGL 9.1: real collection data in the approved 480x272 layout. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <rtthread.h>
#include "lvgl.h"
#include "bb.h"
#include "bb_system.h"
LV_FONT_DECLARE(mon_zh_12)
LV_FONT_DECLARE(mon_zh_14)
#if !LV_FONT_MONTSERRAT_28
#error "Dark A requires LV_FONT_MONTSERRAT_28"
#endif
#define BG 0x17212b
#define PANEL 0x202d38
#define FG 0xe4edf3
#define MUTED 0x96a9b8
#define ACCENT 0x63d5cc
#define BORDER 0x354654
#define SOFT 0x263e42
#define WARN 0xffc06b
#define BAD 0xff8a86
static lv_obj_t *scr, *pages[4], *nav[4], *head, *dot;
static lv_obj_t *values[2], *units[2], *sources[2], *record_badge, *record_info, *record_link;
static lv_obj_t *recent_name, *recent_time, *spark, *spark_left, *spark_mid;
static lv_obj_t *chart, *trend_value, *trend_mode, *trend_left, *trend_span, *axis_hi, *axis_lo;
static lv_obj_t *channels[3], *range_btn, *pause_btn;
static lv_obj_t *event_list, *event_detail, *event_rows[3], *event_names[3], *event_codes[3], *event_times[3], *event_note;
static lv_obj_t *detail_name, *detail_time, *detail_mode, *detail_value, *detail_offset;
static lv_obj_t *detail_chart, *detail_slider, *detail_hi, *detail_lo, *detail_left, *detail_right;
static lv_obj_t *collector_btn, *storage_info, *storage_note, *terminal_info, *control_note;
static lv_chart_series_t *series, *spark_series, *detail_series;
static lv_chart_cursor_t *detail_cursor;
static lv_timer_t *timer;
static bb_frame_t frames[BB_RING_LEN], frozen[BB_RING_LEN];
static bb_event_t events[BB_EVENT_RING_LEN], live_events[BB_EVENT_RING_LEN], history_cache[BB_EVENT_RING_LEN], selected_event;
static lv_obj_t *history_toggle, *event_tag, *older_btn, *newer_btn;
static int history_mode, event_page, history_error, detail_disk;
static volatile int history_result;
static volatile int history_busy, history_ready;
static bb_status_t status;
static int count, event_count, frozen_count, detail_index, detail_has_window;
static int page, metric, points, paused, detail_open, start_requested;
static volatile int busy, control_result;
static volatile int control_test_left, control_test_failed;
static unsigned control_stack_used, history_stack_used;
static int node2;
static volatile int requested_node = -1;
static volatile int requested_page = -1, requested_action;
static lv_obj_t *node_button, *card_titles[2];
static lv_obj_t *system_hint, *spark_title;
static bb_system_status_t system_status;
static bb_system_frame_t system_frames[BB_SYSTEM_RING_LEN];
static bb_system_event_t system_events[BB_SYSTEM_EVENTS], system_live_events[BB_SYSTEM_EVENTS], system_history_cache[BB_SYSTEM_EVENTS], selected_system_event;
static bb_system_frame_t system_frozen[BB_SYSTEM_RING_LEN];
static void present_system_window(void);
static void system_detail_refresh(void);
static int system_count, system_event_count;
static void refresh_system(void);
static void refresh(lv_timer_t *t);
static void show_page(int target);
static void present_event_window(void);
static void history_worker(void *arg)
{
    int action = (int)(intptr_t)arg;
    if (node2) history_result = action == 1 ? bb_system_history_events(system_history_cache, BB_SYSTEM_EVENTS) :
                                            bb_system_history_window(&selected_system_event, system_frozen, BB_SYSTEM_RING_LEN);
    else history_result = action == 1 ? bb_history_events(history_cache, BB_EVENT_RING_LEN) :
                                  bb_history_window(&selected_event, frozen, BB_RING_LEN);
#ifndef MONITOR_HOST_RTTHREAD_H
    rt_thread_t self = rt_thread_self(); unsigned unused = 0;
    while (unused < self->stack_size && ((unsigned char *)self->stack_addr)[unused] == '#') unused++;
    unsigned used = self->stack_size - unused; if (used > history_stack_used) history_stack_used = used;
#endif
    history_ready = action;
    history_busy = 0;
}
static void begin_history(int action)
{
    history_busy = 1; history_ready = 0;
    rt_thread_t worker = rt_thread_create("bb_hist", history_worker, (void *)(intptr_t)action, 16384, 22, 10);
    if (!worker || rt_thread_startup(worker) != RT_EOK) {
        if (worker) rt_thread_delete(worker);
        history_result = -RT_ENOMEM; history_ready = action; history_busy = 0;
    }
}
static void history_cb(lv_event_t *e)
{
    (void)e; if (busy || history_busy || history_ready) return;
    history_mode = !history_mode; event_page = 0; history_error = 0;
    if (history_mode) { event_count = 0; if (node2) system_event_count = 0; begin_history(1); }
    refresh(NULL);
}
static void event_page_cb(lv_event_t *e)
{
    if (history_busy) return;
    int next = event_page + (int)(intptr_t)lv_event_get_user_data(e);
    if (next >= 0 && next < event_count) event_page = next;
    refresh(NULL);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int border)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, border, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(BORDER), 0);
    lv_obj_set_style_radius(o, border ? 5 : 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
static lv_obj_t *text(lv_obj_t *parent, int x, int y, int w, const char *s, uint32_t color)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_remove_style_all(o); lv_obj_set_pos(o, x, y); lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, &mon_zh_12, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(o, 3, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP); lv_label_set_text(o, s);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *s, lv_event_cb_t cb, int arg)
{
    lv_obj_t *o = lv_button_create(parent);
    lv_obj_remove_style_all(o); lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(SOFT), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(BORDER), LV_STATE_PRESSED);
    lv_obj_set_style_radius(o, 4, 0); lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = text(o, 0, 0, w, s, ACCENT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(l);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, (void *)(intptr_t)arg);
    return o;
}
static void button_text(lv_obj_t *o, const char *s)
{
    lv_label_set_text(lv_obj_get_child(o, 0), s); lv_obj_center(lv_obj_get_child(o, 0));
}
static void decimal(char *out, int size, int value, int scale, int digits)
{
    int a = value < 0 ? -value : value;
    if (digits == 1) rt_snprintf(out, size, "%s%d.%d", value < 0 ? "-" : "", a / scale, a % scale * 10 / scale);
    else rt_snprintf(out, size, "%s%d.%02d", value < 0 ? "-" : "", a / scale, a % scale * 100 / scale);
}
static const char *event_name(bb_event_type_t type)
{
    static const char *names[] = {"通信恢复", "从站掉线", "芯片过温", "温度恢复", "振动越限", "振动恢复"};
    return (unsigned)type <= BB_EVT_VIB_OK ? names[type] : "未知事件";
}
static const char *event_code(bb_event_type_t type)
{
    static const char *codes[] = {"SLAVE_ON", "SLAVE_LOST", "CHIP_HOT", "CHIP_OK", "VIB_HI", "VIB_OK"};
    return (unsigned)type <= BB_EVT_VIB_OK ? codes[type] : "UNKNOWN";
}
static uint32_t event_color(bb_event_type_t type)
{
    return type == BB_EVT_SLAVE_LOST || type == BB_EVT_CHIP_HOT ? BAD : type == BB_EVT_VIB_HI ? WARN : ACCENT;
}
static void time_text(lv_obj_t *o, uint32_t ms)
{
    if (ms >= 3600000)
        lv_label_set_text_fmt(o, "开机后 %u小时%02u分", (unsigned)(ms / 3600000), (unsigned)(ms / 60000 % 60));
    else if (ms >= 60000)
        lv_label_set_text_fmt(o, "开机后 %u分%02u秒", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60));
    else lv_label_set_text_fmt(o, "开机后 %u.%u秒", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
}
static uint32_t sqrt_u64(uint64_t n)
{
    uint64_t root = 0, bit = (uint64_t)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return (uint32_t)root;
}
/* Vibration here is magnitude deviation from 1g, not an FFT/RMS result. */
static int metric_value(const bb_frame_t *f, int channel)
{
    if (channel == 3) return f->chip_temp == -999 || f->chip_temp == -998 ? LV_CHART_POINT_NONE : f->chip_temp;
    if (!(f->flags & BB_F_SLAVE_ON)) return LV_CHART_POINT_NONE;
    if (channel < 2 && (f->flags & BB_F_ENV_INVALID)) return LV_CHART_POINT_NONE;
    if (channel == 2 && (f->flags & BB_F_IMU_INVALID)) return LV_CHART_POINT_NONE;
    if (channel == 0) return f->slave_temp / 10;
    if (channel == 1) return f->slave_humi / 10;
    uint64_t sq = (int64_t)f->ax * f->ax + (int64_t)f->ay * f->ay + (int64_t)f->az * f->az;
    int deviation = (int)sqrt_u64(sq) - 16384;
    if (deviation < 0) deviation = -deviation;
    return deviation * 1000 / 16384;
}
static lv_obj_t *graph(lv_obj_t *parent, int x, int y, int w, int h, lv_chart_series_t **s)
{
    lv_obj_t *o = lv_chart_create(parent);
    lv_obj_remove_style_all(o); lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(BG), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 0, 0); lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_line_color(o, lv_color_hex(BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_width(o, 1, LV_PART_MAIN); lv_obj_set_style_line_width(o, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(o, 0, 0, LV_PART_INDICATOR); lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(o, LV_CHART_TYPE_SCATTER); lv_chart_set_div_line_count(o, 3, 5);
    *s = lv_chart_add_series(o, lv_color_hex(ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    return o;
}
static void plot(lv_obj_t *o, lv_chart_series_t *s, const bb_frame_t *data, int n, int limit, int channel,
                 lv_obj_t *hi_label, lv_obj_t *lo_label)
{
    int use = n < limit ? n : limit, lo = INT32_MAX, hi = INT32_MIN;
    for (int i = n - use; i < n; i++) {
        int v = metric_value(&data[i], channel);
        if (v == LV_CHART_POINT_NONE) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (lo == INT32_MAX) { lo = 0; hi = channel == 2 ? 300 : 1000; }
    else if (channel == 2) { lo = 0; hi = hi < 300 ? 300 : hi + hi / 10; }
    else {
        int pad = (hi - lo) / 5; if (pad < 10) pad = 10;
        lo -= pad; hi += pad;
        if (channel == 1) { if (lo < 0) lo = 0; if (hi > 1000) hi = 1000; }
    }
    uint32_t duration = use > 1 ? data[n - 1].ts_ms - data[n - use].ts_ms : 100;
    lv_chart_set_range(o, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    lv_chart_set_range(o, LV_CHART_AXIS_PRIMARY_X, 0, duration ? duration : 1);
    lv_chart_set_point_count(o, use > 1 ? use : 2);
    for (int i = 0; i < (use > 1 ? use : 2); i++) {
        int v = i < use ? metric_value(&data[n - use + i], channel) : LV_CHART_POINT_NONE;
        int x = i < use ? (uint32_t)(data[n - use + i].ts_ms - data[n - use].ts_ms) : 0;
        lv_chart_set_value_by_id2(o, s, i, x, v);
    }
    lv_chart_refresh(o);
    if (hi_label) {
        char buf[20]; decimal(buf, sizeof(buf), hi, channel == 2 ? 1000 : 10, channel == 2 ? 2 : 1); lv_label_set_text(hi_label, buf);
        decimal(buf, sizeof(buf), lo, channel == 2 ? 1000 : 10, channel == 2 ? 2 : 1); lv_label_set_text(lo_label, buf);
    }
}
static void metric_text(lv_obj_t *o, const bb_frame_t *f, int channel)
{
    static const char *names[] = {"温度", "湿度", "振动", "结温"};
    int v = f ? metric_value(f, channel) : LV_CHART_POINT_NONE; char value[24];
    if (v == LV_CHART_POINT_NONE) strcpy(value, "--");
    else decimal(value, sizeof(value), v, channel == 2 ? 1000 : 10, channel == 2 ? 2 : 1);
    lv_label_set_text_fmt(o, "%s  %s %s", names[channel], value, channel == 1 ? "%RH" : channel == 2 ? "g" : "°C");
}
static const char *system_event_name(int type)
{
    static const char *names[] = {"未知事件", "通信恢复", "从站掉线", "采集程序重启", "数据停止更新", "数据恢复更新"};
    return type >= 1 && type <= 5 ? names[type] : names[0];
}
static int system_value(const bb_system_frame_t *f, int channel)
{
    if (!f->query_ok || !(f->metrics.valid & (1 << channel))) return LV_CHART_POINT_NONE;
    if (channel == 0) return f->metrics.cpu_centi;
    if (channel == 2) return f->metrics.load_milli;
    if (!f->metrics.mem_total_kib || f->metrics.mem_available_kib > f->metrics.mem_total_kib) return LV_CHART_POINT_NONE;
    return (uint64_t)(f->metrics.mem_total_kib - f->metrics.mem_available_kib) * 10000 / f->metrics.mem_total_kib;
}
static void system_plot_data(lv_obj_t *o, lv_chart_series_t *line, const bb_system_frame_t *data, int n, int channel, int limit, lv_obj_t *hi_label, lv_obj_t *lo_label)
{
    int use = n < limit ? n : limit, hi = channel == 2 ? 1000 : 10000;
    if (channel == 2) for (int i = n - use; i < n; i++) {
        int v = system_value(&data[i], channel); if (v != LV_CHART_POINT_NONE && v + 100 > hi) hi = v + 100;
    }
    uint32_t duration = use > 1 ? data[n - 1].ts_ms - data[n - use].ts_ms : 1000;
    lv_chart_set_range(o, LV_CHART_AXIS_PRIMARY_Y, 0, hi);
    lv_chart_set_range(o, LV_CHART_AXIS_PRIMARY_X, 0, duration ? duration : 1);
    lv_chart_set_point_count(o, use > 1 ? use : 2);
    for (int i = 0; i < (use > 1 ? use : 2); i++) {
        int v = i < use ? system_value(&data[n - use + i], channel) : LV_CHART_POINT_NONE;
        int x = i < use ? data[n - use + i].ts_ms - data[n - use].ts_ms : 0;
        lv_chart_set_value_by_id2(o, line, i, x, v);
    }
    lv_chart_refresh(o);
    if (hi_label) {
        char buf[24]; decimal(buf, sizeof(buf), hi, channel == 2 ? 1000 : 100, 1);
        lv_label_set_text(hi_label, buf); lv_label_set_text(lo_label, "0");
    }
}
static void system_plot(lv_obj_t *o, lv_chart_series_t *line, int channel, int limit, lv_obj_t *hi_label, lv_obj_t *lo_label)
{ system_plot_data(o, line, system_frames, system_count, channel, limit, hi_label, lo_label); }
static void select_node(int target)
{
    if (history_busy || history_ready) return;
    node2 = target; paused = metric = event_page = history_mode = detail_open = history_error = 0;
    frozen_count = detail_index = detail_has_window = detail_disk = 0;
    lv_slider_set_range(detail_slider, 0, 1);
    lv_slider_set_value(detail_slider, 0, LV_ANIM_OFF);
    lv_obj_add_state(detail_slider, LV_STATE_DISABLED);
    lv_obj_add_flag(event_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(event_list, LV_OBJ_FLAG_HIDDEN);
    button_text(node_button, node2 ? "龙芯 02 · 切换" : "星火 01 · 切换");
    for (int i = 0; i < 2; i++) {
        lv_label_set_text(card_titles[i], node2 ? i ? "内存使用率" : "CPU 使用率" : i ? "相对湿度" : "环境温度");
        lv_label_set_text(units[i], node2 ? "%" : i ? "%RH" : "°C");
    }
    static const char *sys[] = {"CPU", "内存", "负载"}, *star[] = {"温度", "湿度", "振动"};
    for (int i = 0; i < 3; i++) button_text(channels[i], node2 ? sys[i] : star[i]);
    lv_label_set_text(spark_title, node2 ? "CPU / 最近内存窗口 (%)" : "温度 / 最近内存窗口 (°C)");
    lv_label_set_text(system_hint, node2 ? "负载表示任务忙碌程度 · 不是百分比" : "振动显示加速度幅值与1g的偏差");
    refresh(NULL);
}
static void node_cb(lv_event_t *e) { (void)e; select_node(!node2); }
static void system_detail_refresh(void)
{
    if (frozen_count <= 0 || frozen_count > BB_SYSTEM_RING_LEN) detail_has_window = 0;
    if (detail_has_window) {
        if (detail_index < 0) detail_index = 0;
        if (detail_index >= frozen_count) detail_index = frozen_count - 1;
    }
    const bb_system_frame_t *f = detail_has_window ? &system_frozen[detail_index] : &selected_system_event.frame;
    int v = system_value(f, metric); char value[24];
    if (v == LV_CHART_POINT_NONE) strcpy(value, "--"); else decimal(value, sizeof(value), v, metric == 2 ? 1000 : 100, metric == 2 ? 2 : 1);
    lv_label_set_text_fmt(detail_value, "%s %s%s", metric == 0 ? "CPU" : metric == 1 ? "内存" : "负载", value, metric == 2 ? "" : "%");
    if (detail_has_window) {
        int32_t delta = f->ts_ms - selected_system_event.frame.ts_ms; unsigned a = delta < 0 ? -(int64_t)delta : delta;
        if (!delta) lv_label_set_text(detail_offset, "正在看：事件发生时刻");
        else lv_label_set_text_fmt(detail_offset, "正在看：事件发生%s%u.%u秒", delta < 0 ? "前" : "后", a / 1000, a % 1000 / 100);
        lv_chart_set_cursor_point(detail_chart, detail_cursor, detail_series, detail_index);
    } else lv_label_set_text(detail_offset, detail_disk ? "TF波形不可读或记录已淘汰" : "波形已离开内存窗口");
}
static void present_system_window(void)
{
    detail_index = 0; uint32_t nearest = UINT32_MAX;
    uint32_t ts = selected_system_event.frame.ts_ms;
    for (int i = 0; i < frozen_count; i++) {
        int32_t delta = system_frozen[i].ts_ms - ts; uint32_t a = delta < 0 ? -(int64_t)delta : delta;
        if (a < nearest) { nearest = a; detail_index = i; }
    }
    detail_open = 1; lv_obj_add_flag(event_list, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(event_detail, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_name, system_event_name(selected_system_event.type));
    lv_obj_set_style_text_color(detail_name, lv_color_hex(selected_system_event.type == 2 || selected_system_event.type == 4 ? BAD : ACCENT), 0);
    lv_label_set_text_fmt(detail_time, "龙芯事件 · 玄铁%s%u分%02u.%u秒", detail_disk ? "那次开机后" : "开机后",
        (unsigned)(ts / 60000), (unsigned)(ts / 1000 % 60), (unsigned)(ts % 1000 / 100));
    int complete = detail_has_window && (int32_t)(ts - system_frozen[0].ts_ms) >= 29000 &&
                   (int32_t)(system_frozen[frozen_count - 1].ts_ms - ts) >= 29000;
    lv_label_set_text(detail_mode, detail_has_window ? detail_disk ? complete ? "TF回放" : "TF回放(部分)" : "内存回放" : "事件上下文");
    lv_point_t hidden = {-100, -100}; lv_chart_set_cursor_pos(detail_chart, detail_cursor, &hidden);
    system_plot_data(detail_chart, detail_series, system_frozen, detail_has_window ? frozen_count : 0, metric, BB_SYSTEM_RING_LEN, detail_hi, detail_lo);
    lv_slider_set_range(detail_slider, 0, detail_has_window && frozen_count > 1 ? frozen_count - 1 : 1);
    lv_slider_set_value(detail_slider, detail_index, LV_ANIM_OFF);
    if (detail_has_window) {
        lv_obj_remove_state(detail_slider, LV_STATE_DISABLED);
        int32_t before = ts - system_frozen[0].ts_ms, after = system_frozen[frozen_count - 1].ts_ms - ts;
        if (before < 0) before = 0;
        if (after < 0) after = 0;
        if (before) lv_label_set_text_fmt(detail_left, "从事件前%u.%u秒开始", (unsigned)(before / 1000), (unsigned)(before % 1000 / 100));
        else lv_label_set_text(detail_left, "暂无事前记录");
        if (after) lv_label_set_text_fmt(detail_right, "到事件后%u.%u秒", (unsigned)(after / 1000), (unsigned)(after % 1000 / 100));
        else lv_label_set_text(detail_right, "暂无事后记录");
    } else { lv_obj_add_state(detail_slider, LV_STATE_DISABLED); lv_label_set_text(detail_left, "当前仅能查看事件上下文"); lv_label_set_text(detail_right, ""); }
    system_detail_refresh();
}
static void open_system_event(int index)
{
    if (busy || history_busy || history_ready || index < 0 || index >= system_event_count) return;
    selected_system_event = system_events[system_event_count - 1 - index]; detail_disk = history_mode;
    frozen_count = detail_disk ? 0 : bb_system_recent(system_frozen, BB_SYSTEM_RING_LEN);
    int kept = 0;
    for (int i = 0; i < frozen_count; i++) {
        int32_t d = system_frozen[i].ts_ms - selected_system_event.frame.ts_ms;
        if (d >= -30000 && d <= 30000) system_frozen[kept++] = system_frozen[i];
    }
    frozen_count = kept; detail_has_window = kept > 0;
    if (!detail_has_window && selected_system_event.disk_source == 1) detail_disk = 1;
    present_system_window();
    if (detail_disk) { lv_label_set_text(detail_mode, "读取TF中"); lv_obj_add_state(detail_slider, LV_STATE_DISABLED); begin_history(2); }
}
static void detail_refresh(void)
{
    if (frozen_count <= 0 || frozen_count > (node2 ? BB_SYSTEM_RING_LEN : BB_RING_LEN)) detail_has_window = 0;
    if (detail_has_window) {
        if (detail_index < 0) detail_index = 0;
        if (detail_index >= frozen_count) detail_index = frozen_count - 1;
    }
    if (node2) { system_detail_refresh(); return; }
    const bb_frame_t *f = detail_has_window ? &frozen[detail_index] : &selected_event.frame;
    metric_text(detail_value, f, metric);
    if (detail_has_window) {
        int32_t delta = (int32_t)(f->ts_ms - selected_event.ts_ms); int a = delta < 0 ? -delta : delta;
        if (!delta) lv_label_set_text(detail_offset, "正在看：事件发生时刻");
        else lv_label_set_text_fmt(detail_offset, "正在看：事件发生%s%u.%u秒", delta < 0 ? "前" : "后", (unsigned)(a / 1000), (unsigned)(a % 1000 / 100));
        lv_chart_set_cursor_point(detail_chart, detail_cursor, detail_series, detail_index);
    } else lv_label_set_text(detail_offset, detail_disk ? "TF波形不可读或记录已淘汰" : "波形已离开内存窗口");
}
static void open_event(int index)
{
    index += event_page;
    if (busy || history_busy || history_ready || index < 0 || index >= event_count) return;
    selected_event = events[event_count - 1 - index];
    detail_disk = history_mode;
    frozen_count = detail_disk ? 0 : bb_ring_recent(frozen, BB_RING_LEN);
    /* Timestamps restart with a collection run, so disk mode never uses the live ring. */
    detail_has_window = frozen_count && (int32_t)(selected_event.ts_ms - frozen[0].ts_ms) >= 0 &&
                        (int32_t)(frozen[frozen_count - 1].ts_ms - selected_event.ts_ms) >= 0;
    if (!detail_has_window && selected_event.disk_source == 1) detail_disk = 1;
    metric = selected_event.type == BB_EVT_VIB_HI || selected_event.type == BB_EVT_VIB_OK ? 2 :
             selected_event.type == BB_EVT_CHIP_HOT || selected_event.type == BB_EVT_CHIP_OK ? 3 : 0;
    detail_open = 1;
    present_event_window();
    if (detail_disk) {
        lv_label_set_text(detail_mode, "读取TF中");
        lv_obj_add_state(detail_slider, LV_STATE_DISABLED);
        begin_history(2);
    }
}
static void present_event_window(void)
{
    if (node2) { present_system_window(); return; }
    detail_index = 0; uint32_t nearest = UINT32_MAX;
    if (detail_has_window) for (int i = 0; i < frozen_count; i++) {
        int32_t d = (int32_t)(frozen[i].ts_ms - selected_event.ts_ms); uint32_t a = d < 0 ? -(int64_t)d : d;
        if (a < nearest) { nearest = a; detail_index = i; }
    }
    detail_open = 1; lv_obj_add_flag(event_list, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(event_detail, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_name, event_name(selected_event.type));
    lv_obj_set_style_text_color(detail_name, lv_color_hex(event_color(selected_event.type)), 0);
    uint32_t ts = selected_event.ts_ms;
    const char *origin = detail_disk ? "那次开机后" : "开机后";
    if (ts >= 3600000)
        lv_label_set_text_fmt(detail_time, "发生时间：%s%u小时%02u分%02u秒", origin, (unsigned)(ts / 3600000), (unsigned)(ts / 60000 % 60), (unsigned)(ts / 1000 % 60));
    else if (ts >= 60000)
        lv_label_set_text_fmt(detail_time, "发生时间：%s%u分%02u.%u秒", origin, (unsigned)(ts / 60000), (unsigned)(ts / 1000 % 60), (unsigned)(ts % 1000 / 100));
    else lv_label_set_text_fmt(detail_time, "发生时间：%s%u.%u秒", origin, (unsigned)(ts / 1000), (unsigned)(ts % 1000 / 100));
    int complete = detail_has_window && (int32_t)(selected_event.ts_ms - frozen[0].ts_ms) >= 29900 &&
                   (int32_t)(frozen[frozen_count - 1].ts_ms - selected_event.ts_ms) >= 29900;
    lv_label_set_text(detail_mode, detail_has_window ? detail_disk ? complete ? "TF回放" : "TF回放(部分)" : "内存回放" : "事件上下文");
    lv_point_t hidden_cursor = {-100, -100};
    lv_chart_set_cursor_pos(detail_chart, detail_cursor, &hidden_cursor);
    plot(detail_chart, detail_series, frozen, detail_has_window ? frozen_count : 0, BB_RING_LEN, metric, detail_hi, detail_lo);
    lv_slider_set_range(detail_slider, 0, detail_has_window && frozen_count > 1 ? frozen_count - 1 : 1);
    lv_slider_set_value(detail_slider, detail_index, LV_ANIM_OFF);
    if (detail_has_window) {
        lv_obj_remove_state(detail_slider, LV_STATE_DISABLED);
        int32_t before = (int32_t)(selected_event.ts_ms - frozen[0].ts_ms), after = (int32_t)(frozen[frozen_count - 1].ts_ms - selected_event.ts_ms);
        if (before < 0) before = 0;
        if (after < 0) after = 0;
        if (before) lv_label_set_text_fmt(detail_left, "从事件前%u.%u秒开始", (unsigned)(before / 1000), (unsigned)(before % 1000 / 100));
        else lv_label_set_text(detail_left, "暂无事前记录");
        if (after) lv_label_set_text_fmt(detail_right, "到事件后%u.%u秒", (unsigned)(after / 1000), (unsigned)(after % 1000 / 100));
        else lv_label_set_text(detail_right, "暂无事后记录");
    } else {
        lv_obj_add_state(detail_slider, LV_STATE_DISABLED); lv_label_set_text(detail_left, "当前仅能查看事件上下文"); lv_label_set_text(detail_right, "");
    }
    detail_refresh();
}
static void nav_cb(lv_event_t *e) { show_page((int)(intptr_t)lv_event_get_user_data(e)); }
static void event_cb(lv_event_t *e) {
    if (node2) { open_system_event(event_page + (int)(intptr_t)lv_event_get_user_data(e)); return; } open_event((int)(intptr_t)lv_event_get_user_data(e)); }
static void back_cb(lv_event_t *e)
{
    (void)e; detail_open = 0; lv_obj_add_flag(event_detail, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(event_list, LV_OBJ_FLAG_HIDDEN); refresh(NULL);
}
static void slider_cb(lv_event_t *e)
{
    (void)e; if (!history_busy && detail_has_window) { detail_index = lv_slider_get_value(detail_slider); detail_refresh(); }
}
static void channel_cb(lv_event_t *e) { metric = (int)(intptr_t)lv_event_get_user_data(e); refresh(NULL); }
static void range_cb(lv_event_t *e) { (void)e; points = points == 300 ? BB_RING_LEN : 300; refresh(NULL); }
static void pause_cb(lv_event_t *e) { (void)e; paused = !paused; refresh(NULL); }
static void control_worker(void *arg)
{
    (void)arg; control_result = start_requested ? bb_start_addr(status.addr ? status.addr : 0x01) : bb_stop();
#ifndef MONITOR_HOST_RTTHREAD_H
    rt_thread_t self = rt_thread_self();
    unsigned unused = 0;
    while (unused < self->stack_size && ((unsigned char *)self->stack_addr)[unused] == '#') unused++;
    unsigned used = self->stack_size - unused;
    if (used > control_stack_used) control_stack_used = used;
#endif
    busy = 0;
}
static void control_cb(lv_event_t *e)
{
    (void)e; if (busy || history_busy || history_ready) return; start_requested = !bb_is_running(); busy = 1;
    /* TF session enumeration enters FatFs/MMC; the old 2KiB stack overflowed on board. */
    rt_thread_t worker = rt_thread_create("mon_ctl", control_worker, RT_NULL, 16384, 21, 10);
    if (!worker) { control_result = -RT_ENOMEM; busy = 0; }
    else if (rt_thread_startup(worker) != RT_EOK) { rt_thread_delete(worker); control_result = -RT_ERROR; busy = 0; }
    refresh(NULL);
}
static void show_page(int target)
{
    page = target;
    for (int i = 0; i < 4; i++) {
        if (i == page) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(nav[i], lv_color_hex(i == page ? SOFT : BG), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(nav[i], 0), lv_color_hex(i == page ? ACCENT : MUTED), 0);
    }
    refresh(NULL);
}
static void refresh(lv_timer_t *t)
{
    if (!scr) return;
    if (requested_node >= 0 && !history_busy && !history_ready) { int target = requested_node; requested_node = -1; select_node(target); }
    if (requested_page >= 0) { int target = requested_page; requested_page = -1; show_page(target); }
    if (requested_action && !history_busy && !history_ready) {
        int action = requested_action; requested_action = 0;
        if (action == 1) lv_obj_send_event(history_toggle, LV_EVENT_CLICKED, NULL);
        else if (action == 2) lv_obj_send_event(older_btn, LV_EVENT_CLICKED, NULL);
        else if (action == 3) lv_obj_send_event(newer_btn, LV_EVENT_CLICKED, NULL);
        else if (action == 4) lv_obj_send_event(event_rows[0], LV_EVENT_CLICKED, NULL);
        else if (action == 5) back_cb(NULL);
        else if (action == 6 && detail_has_window) {
            lv_slider_set_value(detail_slider, frozen_count - 1, LV_ANIM_OFF);
            lv_obj_send_event(detail_slider, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }
    bb_gui_heartbeat();
    /* Execute shell test requests on LVGL's thread through the actual button callback. */
    if (t && control_test_left && !busy) {
        if (control_result < 0) { control_test_failed = 1; control_test_left = 0; }
        else { control_test_left--; control_cb(NULL); }
    }
    if (history_ready) {
        int action = history_ready; history_ready = 0;
        if (action == 1) {
            history_error = history_result < 0;
            event_count = history_result > 0 ? history_result : 0;
            if (node2) {
                system_event_count = event_count;
                if (event_count) memcpy(system_events, system_history_cache, event_count * sizeof(*system_events));
            } else if (event_count) memcpy(events, history_cache, event_count * sizeof(*events));
        } else if (detail_open) {
            frozen_count = history_result > 0 ? history_result : 0;
            detail_has_window = frozen_count > 0;
            present_event_window();
        }
    }
    if (bb_get_status(&status)) return;
    uint32_t now = bb_now_ms();
    int fresh = status.running && status.has_last && (uint32_t)(now - status.last.ts_ms) < 2000;
    int online = fresh && status.slave_online, valid = fresh && (status.last.flags & BB_F_SLAVE_ON) && !(status.last.flags & BB_F_ENV_INVALID);
    int disk = status.backend && strcmp(status.backend, "ram") != 0;
    const char *backend = !disk ? "内存" : !strcmp(status.backend, "sdcard") ? "TF" : "NOR";
    uint32_t color = status.write_errors ? BAD : online ? ACCENT : status.running ? BAD : MUTED;
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0); lv_obj_set_style_text_color(head, lv_color_hex(color), 0);
    if (!status.running) lv_label_set_text(head, "采集已停止");
    else if (status.write_errors) lv_label_set_text(head, online ? "在线 · 写入异常" : "离线 · 写入异常");
    else if (!online) lv_label_set_text(head, "从站离线 · 等待恢复");
    else lv_label_set_text_fmt(head, "在线 · %s%s", backend, disk ? "记录中" : "缓存中");
    int head_width = lv_text_get_width(lv_label_get_text(head), strlen(lv_label_get_text(head)), &mon_zh_12, 0);
    lv_obj_set_x(dot, 470 - (head_width < 190 ? head_width : 190) - 12);
    if (!paused && !node2) {
        char value[24];
        for (int i = 0; i < 2; i++) {
            if (valid) decimal(value, sizeof(value), i ? status.last.slave_humi : status.last.slave_temp, 100, 1); else strcpy(value, "--");
            lv_label_set_text(values[i], value);
            int x = 8 + lv_text_get_width(value, strlen(value), &lv_font_montserrat_28, 0); if (x > 101) x = 101;
            lv_obj_set_x(units[i], x + 2);
        }
        if (valid) {
            lv_label_set_text_fmt(sources[0], "AHT21 · 星火 %02X", status.addr ? status.addr : 1);
            lv_label_set_text_fmt(sources[1], "总线更新 %u.%us 前", (unsigned)((now - status.last.ts_ms) / 1000), (unsigned)((now - status.last.ts_ms) % 1000 / 100));
        } else {
            lv_label_set_text(sources[0], "数据暂不可用"); lv_label_set_text(sources[1], online ? "本次查询未成功" : "等待通信恢复");
        }
        lv_label_set_text_fmt(record_badge, "%s · %s", backend, status.write_errors ? "写入异常" : !status.running ? "已停止" : disk ? "采集中" : "仅缓存");
        lv_obj_set_style_text_color(record_badge, lv_color_hex(status.write_errors ? BAD : ACCENT), 0);
        lv_label_set_text_fmt(record_info, "%u 帧 / %u 事件", (unsigned)status.frames, (unsigned)status.events);
        if (fresh && (status.last.flags & BB_F_SLAVE_ON)) lv_label_set_text_fmt(record_link, "链路 %u ms · 10 Hz目标", (unsigned)status.last.rtt_ms); else lv_label_set_text(record_link, "链路 -- · 10 Hz目标");
        count = bb_ring_recent(frames, BB_RING_LEN);
        int live_count = bb_events_recent(live_events, BB_EVENT_RING_LEN);
        if (!history_mode) { event_count = live_count; memcpy(events, live_events, live_count * sizeof(*events)); }
        plot(spark, spark_series, frames, count, 300, 0, NULL, NULL);
        int use = count < 300 ? count : 300;
        uint32_t span = use > 1 ? frames[count - 1].ts_ms - frames[count - use].ts_ms : 0;
        lv_label_set_text_fmt(spark_left, "-%u.%us", (unsigned)(span / 1000), (unsigned)(span % 1000 / 100));
        lv_label_set_text_fmt(spark_mid, "-%u.%us", (unsigned)(span / 2000), (unsigned)(span % 2000 / 200));
        if (status.write_errors) { lv_label_set_text(recent_name, "存储写入异常"); lv_obj_set_style_text_color(recent_name, lv_color_hex(BAD), 0); lv_label_set_text(recent_time, "请检查存储诊断"); }
        else if (live_count) {
            bb_event_t *last = &live_events[live_count - 1]; lv_label_set_text(recent_name, event_name(last->type));
            lv_obj_set_style_text_color(recent_name, lv_color_hex(event_color(last->type)), 0); time_text(recent_time, last->ts_ms);
        } else { lv_label_set_text(recent_name, "暂无事件"); lv_label_set_text(recent_time, "等待状态变化"); }
    }
    if (!node2) {
        if (event_page >= event_count) event_page = event_count ? (event_count - 1) / 3 * 3 : 0;
        for (int i = 0; i < 3; i++) {
            if (i + event_page < event_count) {
                bb_event_t *evt = &events[event_count - 1 - event_page - i]; lv_obj_remove_flag(event_rows[i], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(event_names[i], event_name(evt->type)); lv_obj_set_style_text_color(event_names[i], lv_color_hex(event_color(evt->type)), 0);
                if (history_mode) lv_label_set_text_fmt(event_codes[i], "%s · S%u / #%u", event_code(evt->type), (unsigned)evt->session_id, (unsigned)evt->id);
                else lv_label_set_text_fmt(event_codes[i], "%s · #%u", event_code(evt->type), (unsigned)evt->id);
                time_text(event_times[i], evt->ts_ms);
            } else lv_obj_add_flag(event_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(event_note, history_busy ? "正在读取TF记录" : history_error ? "TF不可读 · 请检查存储" :
            event_count ? "右侧为玄铁开机后的发生时间" : history_mode ? "暂无新格式TF事件" : "暂无事件 · 等待状态变化");
        lv_label_set_text_fmt(event_tag, "%s · %d/%d页", history_mode ? "TF" : "内存", event_count ? event_page / 3 + 1 : 0, (event_count + 2) / 3);
        button_text(history_toggle, history_mode ? "返回内存" : "TF历史");
        if (history_busy) lv_obj_add_state(history_toggle, LV_STATE_DISABLED); else lv_obj_remove_state(history_toggle, LV_STATE_DISABLED);
        if (!event_page || history_busy) lv_obj_add_state(newer_btn, LV_STATE_DISABLED); else lv_obj_remove_state(newer_btn, LV_STATE_DISABLED);
        if (event_page + 3 >= event_count || history_busy) lv_obj_add_state(older_btn, LV_STATE_DISABLED); else lv_obj_remove_state(older_btn, LV_STATE_DISABLED);
    }
    if (page == 1 && !node2) {
        metric_text(trend_value, count ? &frames[count - 1] : NULL, metric); plot(chart, series, frames, count, points, metric, axis_hi, axis_lo);
        int use = count < points ? count : points;
        uint32_t span = use > 1 ? frames[count - 1].ts_ms - frames[count - use].ts_ms : 0;
        lv_label_set_text_fmt(trend_left, "-%u.%us", (unsigned)(span / 1000), (unsigned)(span % 1000 / 100));
        lv_label_set_text_fmt(trend_span, "%d 帧 · %u.%us", use, (unsigned)(span / 1000), (unsigned)(span % 1000 / 100));
        lv_label_set_text(trend_mode, paused ? "画面已暂停" : status.running ? "实时内存窗口" : "采集停止 · 内存保留");
        for (int i = 0; i < 3; i++) lv_obj_set_style_bg_color(channels[i], lv_color_hex(metric == i ? SOFT : BG), 0);
        button_text(range_btn, points == 300 ? "30秒" : "全部"); button_text(pause_btn, paused ? "继续" : "暂停");
    }
    button_text(collector_btn, busy ? start_requested ? "正在启动" : "正在停止" : status.running ? "停止" : "启动");
    if (busy) lv_obj_add_state(collector_btn, LV_STATE_DISABLED); else lv_obj_remove_state(collector_btn, LV_STATE_DISABLED);
    lv_label_set_text_fmt(storage_info, "%s · %s", backend, status.write_errors ? "写入异常" : disk ? "日志后端" : "未落盘");
    lv_obj_set_style_text_color(storage_info, lv_color_hex(status.write_errors ? BAD : MUTED), 0);
    lv_label_set_text_fmt(storage_note, "漏写 %u · 错误 %u · 降级 %u", (unsigned)status.dropped_frames, (unsigned)status.write_errors, (unsigned)status.backend_switches);
    if (status.has_last && metric_value(&status.last, 3) != LV_CHART_POINT_NONE) {
        char chip[20]; decimal(chip, sizeof(chip), status.last.chip_temp, 10, 1);
        lv_label_set_text_fmt(terminal_info, "D121 / 从站 %02X\n芯片结温 %s°C", status.addr ? status.addr : 1, chip);
    } else lv_label_set_text_fmt(terminal_info, "D121 / 从站 %02X\n芯片结温 --", status.addr ? status.addr : 1);
    if (control_result < 0) lv_label_set_text_fmt(control_note, "采集操作失败 (%d) · 请重试", control_result);
    else lv_label_set_text(control_note, "TF历史支持跨重启 · 最多16组文件");
    refresh_system();
}
static void refresh_system(void)
{
    bb_system_status(&system_status);
    uint32_t now = bb_now_ms();
    int fresh = status.running && system_status.has_last && (uint32_t)(now - system_status.last.ts_ms) < 2500;
    int on2 = fresh && system_status.online;
    int on1 = status.running && status.has_last && status.slave_online && (uint32_t)(now - status.last.ts_ms) < 2000;
    uint32_t color = status.write_errors || system_status.write_errors ? BAD : on1 && on2 ? ACCENT : status.running ? WARN : MUTED;
    lv_label_set_text_fmt(head, status.running ? "01 %s · 02 %s" : "采集已停止", on1 ? "在线" : "离线", on2 ? "在线" : "离线");
    lv_obj_set_style_text_color(head, lv_color_hex(color), 0); lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    int width = lv_text_get_width(lv_label_get_text(head), strlen(lv_label_get_text(head)), &mon_zh_12, 0);
    lv_obj_set_x(dot, 470 - (width < 190 ? width : 190) - 12);
    if (!node2) return;
    if (!paused) {
        system_count = bb_system_recent(system_frames, BB_SYSTEM_RING_LEN);
        int live_n = bb_system_events(system_live_events, BB_SYSTEM_EVENTS);
        if (!history_mode) { system_event_count = live_n; memcpy(system_events, system_live_events, live_n * sizeof(*system_events)); }
        for (int i = 0; i < 2; i++) {
            char buf[24]; int v = fresh ? system_value(&system_status.last, i) : LV_CHART_POINT_NONE;
            if (v == LV_CHART_POINT_NONE) strcpy(buf, "--"); else decimal(buf, sizeof(buf), v, 100, 1);
            lv_label_set_text(values[i], buf);
            int x = 8 + lv_text_get_width(buf, strlen(buf), &lv_font_montserrat_28, 0); if (x > 101) x = 101;
            lv_obj_set_x(units[i], x + 2);
        }
        lv_label_set_text(sources[0], !status.running ? "采集已停止" : system_status.stale ? "数据停止更新" : on2 ? "LoongOS · 龙芯 02" : "等待通信恢复");
        if (fresh && system_status.last.query_ok && (system_status.last.metrics.valid & RS485_SYS_MEMORY))
            lv_label_set_text_fmt(sources[1], "已用 %u / %u MiB", ((unsigned)system_status.last.metrics.mem_total_kib - (unsigned)system_status.last.metrics.mem_available_kib) / 1024,
                (unsigned)system_status.last.metrics.mem_total_kib / 1024);
        else lv_label_set_text(sources[1], "数据暂不可用");
        const char *backend = !strcmp(status.backend, "sdcard") ? "TF" : !strcmp(status.backend, "nor") ? "NOR" : "内存";
        lv_label_set_text_fmt(record_badge, "%s · %s", backend, (unsigned)system_status.write_errors ? "写入异常" : !status.running ? "已停止" : "采集中");
        lv_obj_set_style_text_color(record_badge, lv_color_hex(system_status.write_errors ? BAD : ACCENT), 0);
        lv_label_set_text_fmt(record_info, "%u 帧 / %u 事件", (unsigned)system_status.frames, (unsigned)system_status.events);
        if (fresh && system_status.last.query_ok) lv_label_set_text_fmt(record_link, "链路 %u ms · 1 Hz目标", (unsigned)system_status.last.rtt_ms);
        else lv_label_set_text(record_link, "链路 -- · 1 Hz目标");
        system_plot(spark, spark_series, 0, 30, NULL, NULL);
        int use = system_count < 30 ? system_count : 30;
        uint32_t span = use > 1 ? system_frames[system_count - 1].ts_ms - system_frames[system_count - use].ts_ms : 0;
        lv_label_set_text_fmt(spark_left, "-%u.%us", (unsigned)(span / 1000), (unsigned)(span % 1000 / 100));
        lv_label_set_text_fmt(spark_mid, "-%u.%us", (unsigned)(span / 2000), (unsigned)(span % 2000 / 200));
        if (live_n) {
            bb_system_event_t *e = &system_live_events[live_n - 1];
            lv_label_set_text(recent_name, system_event_name(e->type)); time_text(recent_time, e->frame.ts_ms);
            lv_obj_set_style_text_color(recent_name, lv_color_hex(e->type == 2 || e->type == 4 ? BAD : ACCENT), 0);
        } else { lv_label_set_text(recent_name, "暂无事件"); lv_label_set_text(recent_time, "等待状态变化"); }
    }
    event_count = system_event_count;
    if (event_page >= event_count) event_page = event_count ? (event_count - 1) / 3 * 3 : 0;
    for (int i = 0; i < 3; i++) {
        int index = system_event_count - 1 - event_page - i;
        if (index >= 0) {
            bb_system_event_t *e = &system_events[index]; lv_obj_remove_flag(event_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(event_names[i], system_event_name(e->type));
            lv_obj_set_style_text_color(event_names[i], lv_color_hex(e->type == 2 || e->type == 4 ? BAD : ACCENT), 0);
            if (history_mode) lv_label_set_text_fmt(event_codes[i], "龙芯 02 · S%u / #%u", (unsigned)e->session_id, (unsigned)e->id);
            else lv_label_set_text_fmt(event_codes[i], "龙芯 02 · #%u", (unsigned)e->id);
            time_text(event_times[i], e->frame.ts_ms);
        } else lv_obj_add_flag(event_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text_fmt(event_tag, "%s · %d/%d页", history_mode ? "TF" : "内存", event_count ? event_page / 3 + 1 : 0, (event_count + 2) / 3);
    button_text(history_toggle, history_mode ? "返回内存" : "TF历史");
    if (history_busy) lv_obj_add_state(history_toggle, LV_STATE_DISABLED); else lv_obj_remove_state(history_toggle, LV_STATE_DISABLED);
    lv_label_set_text(event_note, history_busy ? "正在读取TF记录" : history_error ? "TF不可读 · 请检查存储" :
        event_count ? "右侧为玄铁开机后的发生时间" : history_mode ? "暂无龙芯TF事件" : "暂无事件 · 等待状态变化");
    if (!event_page) lv_obj_add_state(newer_btn, LV_STATE_DISABLED); else lv_obj_remove_state(newer_btn, LV_STATE_DISABLED);
    if (event_page + 3 >= event_count) lv_obj_add_state(older_btn, LV_STATE_DISABLED); else lv_obj_remove_state(older_btn, LV_STATE_DISABLED);
    if (page == 1) {
        const char *names[] = {"CPU", "内存", "负载"}; char buf[24];
        for (int i = 0; i < 3; i++) lv_obj_set_style_bg_color(channels[i], lv_color_hex(metric == i ? SOFT : BG), 0);
        button_text(range_btn, points == 300 ? "30秒" : "全部"); button_text(pause_btn, paused ? "继续" : "暂停");
        int v = system_count ? system_value(&system_frames[system_count - 1], metric) : LV_CHART_POINT_NONE;
        if (v == LV_CHART_POINT_NONE) strcpy(buf, "--"); else decimal(buf, sizeof(buf), v, metric == 2 ? 1000 : 100, metric == 2 ? 2 : 1);
        lv_label_set_text_fmt(trend_value, "%s %s%s", names[metric], buf, metric == 2 ? "" : "%");
        int limit = points == 300 ? 30 : BB_SYSTEM_RING_LEN;
        system_plot(chart, series, metric, limit, axis_hi, axis_lo);
        int use = system_count < limit ? system_count : limit;
        uint32_t span = use > 1 ? system_frames[system_count - 1].ts_ms - system_frames[system_count - use].ts_ms : 0;
        lv_label_set_text_fmt(trend_left, "-%u.%us", (unsigned)(span / 1000), (unsigned)(span % 1000 / 100));
        lv_label_set_text_fmt(trend_span, "%d 帧 · %u.%us", use, (unsigned)(span / 1000), (unsigned)(span % 1000 / 100));
        lv_label_set_text(trend_mode, paused ? "画面已暂停" : status.running ? "龙芯 · 实时内存窗口" : "采集停止 · 内存保留");
    }
    const rs485_system_t *m = &system_status.last.metrics;
    if (fresh && system_status.last.query_ok && (m->valid & RS485_SYS_UPTIME))
        lv_label_set_text_fmt(terminal_info, "龙芯 02 · 运行 %u秒\n负载 %u.%03u · 超时 %u", (unsigned)m->uptime_s, m->load_milli / 1000, m->load_milli % 1000, (unsigned)system_status.timeouts);
    else lv_label_set_text(terminal_info, "D121 / 龙芯 02\n等待有效系统数据");
    lv_label_set_text_fmt(storage_note, "漏写 %u · 错误 %u", (unsigned)system_status.dropped, (unsigned)system_status.write_errors);
    lv_label_set_text(storage_info, system_status.write_errors ? "龙芯日志写入异常" : !strcmp(status.backend, "ram") ? "龙芯仅内存缓存" : "龙芯独立CSV记录");
}
static void delete_cb(lv_event_t *e)
{
    (void)e; if (timer) { lv_timer_delete(timer); timer = NULL; } scr = NULL; bb_gui_heartbeat_clear();
}
#ifndef MONITOR_HOST_RTTHREAD_H
static int cmd_monctl(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "page")) {
        char *end; unsigned n = strtoul(argv[2], &end, 10);
        if (*end || n > 3) return -RT_EINVAL;
        requested_page = n;
    }
    if (argc == 3 && !strcmp(argv[1], "action")) {
        const char *names[] = {"tf", "older", "newer", "open", "back", "slide"}; int found = 0;
        for (int i = 0; i < 6; i++) if (!strcmp(argv[2], names[i])) found = i + 1;
        if (!found || requested_action) return -RT_EINVAL;
        requested_action = found;
    }
    if (argc == 3 && !strcmp(argv[1], "node")) {
        if (!strcmp(argv[2], "1")) requested_node = 0;
        else if (!strcmp(argv[2], "2")) requested_node = 1;
        else return -RT_EINVAL;
    }
    if (argc == 3 && !strcmp(argv[1], "cycle")) {
        char *end; unsigned long pairs = strtoul(argv[2], &end, 10);
        if (*end || !pairs || pairs > 50 || !scr || busy || history_busy || control_test_left) return -RT_EINVAL;
        control_result = control_test_failed = 0; control_test_left = pairs * 2;
    }
    rt_kprintf("monctl remaining=%d failed=%d busy=%d rc=%d max_stack=%u stack=16384\r\n",
               control_test_left, control_test_failed, busy, control_result, control_stack_used);
    rt_kprintf("view node=%d page=%d history=%d offset=%d events=%d\r\n",
        node2 ? 2 : 1, page, history_mode, event_page, event_count);
    rt_kprintf("detail=%d window=%d points=%d index=%d hist_busy=%d hist_error=%d hist_stack=%u\r\n",
        detail_open, detail_has_window, frozen_count, detail_index,
        history_busy, history_error, history_stack_used);
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(cmd_monctl, monctl, monitor control callback regression test);
#endif
lv_obj_t *monitor_ui_init(void)
{
    if (scr) return scr;
    node2 = 0; requested_node = -1;
    page = metric = paused = detail_open = count = event_count = history_mode = event_page = history_error = 0; points = 300;
    scr = box(NULL, 0, 0, 480, 272, BG, 0); lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *h = box(scr, 0, 0, 480, 32, BG, 0);
    node_button = button(h, 10, 3, 170, 26, "星火 01 · 切换", node_cb, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(node_button, 0), &mon_zh_14, 0); lv_obj_center(lv_obj_get_child(node_button, 0));
    dot = box(h, 268, 14, 5, 5, ACCENT, 0); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    head = text(h, 280, 9, 190, "等待采集", ACCENT); lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < 4; i++) { pages[i] = box(scr, 0, 32, 480, 200, BG, 0); if (i) lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN); }
    lv_obj_t *p = pages[0]; const char *titles[] = {"环境温度", "相对湿度"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = box(p, 10 + i * 144, 8, 137, 88, PANEL, 1); card_titles[i] = text(c, 8, 8, 120, titles[i], MUTED);
        values[i] = text(c, 8, 28, 120, "--", FG); lv_obj_set_style_text_font(values[i], &lv_font_montserrat_28, 0);
        units[i] = text(c, 85, 46, 34, i ? "%RH" : "°C", MUTED); sources[i] = text(c, 8, 68, 120, "等待采集", MUTED);
    }
    lv_obj_t *c = box(p, 298, 8, 172, 88, PANEL, 1);
    text(c, 8, 8, 154, "记录状态", MUTED); record_badge = text(c, 8, 29, 154, "等待采集", ACCENT);
    lv_obj_set_style_bg_color(record_badge, lv_color_hex(SOFT), 0);
    lv_obj_set_style_bg_opa(record_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(record_badge, 4, 0);
    lv_obj_set_style_pad_ver(record_badge, 2, 0);
    lv_obj_set_style_radius(record_badge, 3, 0);
    record_info = text(c, 8, 50, 154, "0 帧 / 0 事件", FG); record_link = text(c, 8, 68, 154, "链路 --", MUTED);
    c = box(p, 10, 103, 275, 87, PANEL, 1); spark_title = text(c, 8, 7, 260, "温度 / 最近内存窗口", MUTED);
    spark = graph(c, 8, 29, 258, 37, &spark_series);
    spark_left = text(c, 8, 70, 75, "-0.0s", MUTED); spark_mid = text(c, 98, 70, 75, "-0.0s", MUTED); text(c, 238, 70, 30, "现在", MUTED);
    c = box(p, 292, 103, 178, 87, PANEL, 1); text(c, 8, 7, 162, "最近事件", MUTED);
    recent_name = text(c, 8, 27, 160, "暂无事件", WARN); recent_time = text(c, 8, 46, 160, "等待状态变化", MUTED);
    button(c, 8, 61, 118, 24, "查看记录 >", nav_cb, 2);
    p = pages[1]; const char *ch[] = {"温度", "湿度", "振动"};
    for (int i = 0; i < 3; i++) channels[i] = button(p, 10 + i * 77, 8, 71, 32, ch[i], channel_cb, i);
    range_btn = button(p, 314, 8, 70, 32, "30秒", range_cb, 0); pause_btn = button(p, 390, 8, 80, 32, "暂停", pause_cb, 0);
    trend_value = text(p, 10, 48, 200, "温度 -- °C", FG); lv_obj_set_style_text_font(trend_value, &mon_zh_14, 0);
    trend_mode = text(p, 215, 49, 255, "实时内存窗口", MUTED); lv_obj_set_style_text_align(trend_mode, LV_TEXT_ALIGN_RIGHT, 0);
    axis_hi = text(p, 6, 74, 33, "--", MUTED); axis_lo = text(p, 6, 147, 33, "--", MUTED);
    chart = graph(p, 42, 77, 426, 86, &series); trend_left = text(p, 42, 169, 100, "-0.0s", MUTED);
    trend_span = text(p, 176, 169, 180, "0 帧", MUTED); text(p, 440, 169, 30, "现在", MUTED); system_hint = text(p, 10, 186, 460, "振动显示加速度幅值与1g的偏差", MUTED);
    p = pages[2]; event_list = box(p, 0, 0, 480, 200, BG, 0);
    lv_obj_t *title = text(event_list, 10, 8, 160, "事件记录", FG); lv_obj_set_style_text_font(title, &mon_zh_14, 0);
    event_tag = text(event_list, 173, 10, 169, "内存", MUTED); lv_obj_set_style_text_align(event_tag, LV_TEXT_ALIGN_RIGHT, 0);
    history_toggle = button(event_list, 350, 5, 120, 26, "TF历史", history_cb, 0);
    for (int i = 0; i < 3; i++) {
        event_rows[i] = button(event_list, 10, 33 + i * 47, 460, 45, "", event_cb, i); lv_obj_set_style_bg_color(event_rows[i], lv_color_hex(PANEL), 0);
        event_names[i] = text(event_rows[i], 12, 6, 200, "", FG); event_codes[i] = text(event_rows[i], 12, 25, 320, "", MUTED);
        event_times[i] = text(event_rows[i], 333, 16, 116, "", MUTED); lv_obj_set_style_text_align(event_times[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    event_note = text(event_list, 10, 181, 330, "暂无事件", MUTED);
    newer_btn = button(event_list, 350, 175, 54, 24, "较新", event_page_cb, -3);
    older_btn = button(event_list, 410, 175, 60, 24, "较早", event_page_cb, 3);
    event_detail = box(p, 0, 0, 480, 200, BG, 0); lv_obj_add_flag(event_detail, LV_OBJ_FLAG_HIDDEN);
    button(event_detail, 10, 7, 95, 30, "< 事件列表", back_cb, 0);
    detail_name = text(event_detail, 116, 14, 180, "事件", FG); lv_obj_set_style_text_font(detail_name, &mon_zh_14, 0);
    detail_mode = text(event_detail, 362, 15, 108, "内存回放", ACCENT); lv_obj_set_style_text_align(detail_mode, LV_TEXT_ALIGN_RIGHT, 0);
    detail_time = text(event_detail, 10, 44, 460, "", MUTED); detail_hi = text(event_detail, 6, 67, 33, "--", MUTED); detail_lo = text(event_detail, 6, 127, 33, "--", MUTED);
    detail_chart = graph(event_detail, 42, 68, 426, 73, &detail_series);
    detail_cursor = lv_chart_add_cursor(detail_chart, lv_color_hex(WARN), LV_DIR_TOP | LV_DIR_BOTTOM);
    lv_obj_set_style_line_width(detail_chart, 1, LV_PART_CURSOR);
    lv_obj_set_style_line_color(detail_chart, lv_color_hex(WARN), LV_PART_CURSOR);
    lv_obj_set_style_line_dash_width(detail_chart, 3, LV_PART_CURSOR);
    lv_obj_set_style_line_dash_gap(detail_chart, 3, LV_PART_CURSOR);
    detail_left = text(event_detail, 10, 147, 220, "", MUTED); detail_right = text(event_detail, 330, 147, 140, "", MUTED); lv_obj_set_style_text_align(detail_right, LV_TEXT_ALIGN_RIGHT, 0);
    detail_slider = lv_slider_create(event_detail); lv_obj_remove_style_all(detail_slider); lv_obj_set_pos(detail_slider, 20, 168); lv_obj_set_size(detail_slider, 440, 6);
    lv_obj_set_style_bg_color(detail_slider, lv_color_hex(BORDER), LV_PART_MAIN); lv_obj_set_style_bg_opa(detail_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(detail_slider, lv_color_hex(ACCENT), LV_PART_INDICATOR); lv_obj_set_style_bg_opa(detail_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(detail_slider, lv_color_hex(ACCENT), LV_PART_KNOB); lv_obj_set_style_bg_opa(detail_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(detail_slider, 5, LV_PART_KNOB); lv_obj_set_style_radius(detail_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(detail_slider, 10); lv_obj_add_event_cb(detail_slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    detail_value = text(event_detail, 10, 183, 235, "", FG); detail_offset = text(event_detail, 246, 183, 224, "", MUTED); lv_obj_set_style_text_align(detail_offset, LV_TEXT_ALIGN_RIGHT, 0);
    p = pages[3]; c = box(p, 10, 8, 223, 79, PANEL, 1); text(c, 8, 9, 120, "采集控制", FG);
    collector_btn = button(c, 147, 5, 68, 36, "启动", control_cb, 0); text(c, 8, 51, 207, "启停采集 · 保留已有记录", MUTED);
    c = box(p, 240, 8, 230, 79, PANEL, 1); text(c, 8, 9, 210, "存储诊断", FG);
    storage_info = text(c, 8, 32, 212, "等待采集", MUTED); storage_note = text(c, 8, 54, 212, "", MUTED);
    c = box(p, 10, 94, 223, 78, PANEL, 1); text(c, 8, 9, 207, "告警阈值", FG);
    text(c, 8, 31, 207, "振动幅值 1.28g / 1.13g\n结温 85°C / 80°C · 固定配置", MUTED);
    c = box(p, 240, 94, 230, 78, PANEL, 1); text(c, 8, 9, 210, "终端信息", FG); terminal_info = text(c, 8, 31, 212, "D121 / 从站 01", MUTED);
    control_note = text(p, 10, 181, 460, "", MUTED);
    lv_obj_t *bottom = box(scr, 0, 232, 480, 40, BG, 0); const char *names[] = {"总览", "趋势", "黑匣子", "设置"};
    for (int i = 0; i < 4; i++) {
        nav[i] = button(bottom, 6 + i * 118, 3, 114, 34, names[i], nav_cb, i);
        lv_obj_set_style_text_font(lv_obj_get_child(nav[i], 0), &mon_zh_14, 0); lv_obj_center(lv_obj_get_child(nav[i], 0));
    }
    timer = lv_timer_create(refresh, 250, NULL); show_page(0); return scr;
}
