/*
 * Copyright (c) 2023-2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  ArtInChip
 */

#include "aic_ui.h"
#include "lvgl.h"
#include "app_entrance.h"
#include "../demo_hub.h"

extern lv_obj_t* navigation_ui_init(void);
extern lv_obj_t *dashboard_ui_init(void);
extern lv_obj_t *input_test_ui_init(void);
extern lv_obj_t *monitor_ui_init(void);   /* app/monitor/monitor_ui.c */

static char cur_screen[64];

static void demo_hub_default_theme_set(lv_obj_t *obj, bool dark)
{
    lv_disp_t *disp = lv_disp_get_default();

    lv_theme_default_init(disp, lv_color_hex(0x1A1D26), lv_color_hex(0x272729), dark, LV_FONT_DEFAULT);

    lv_theme_t *theme = lv_theme_default_get();
    lv_theme_apply(obj);

    lv_disp_set_theme(disp, theme);
}

void app_entrance(app_index_t index, int auto_del)
{
    lv_obj_t *scr = NULL;

    switch(index) {
        case APP_NAVIGATION:
            layer_sys_ui_invisible(0);
            scr = navigation_ui_init();
            lv_theme_apply(scr);
            strncpy(cur_screen, "navigation", sizeof(cur_screen));
            break;
        case APP_DASHBOARD:
            layer_sys_ui_visible(0);
            scr = dashboard_ui_init();
            demo_hub_default_theme_set(scr, true);
            strncpy(cur_screen, "dashboard", sizeof(cur_screen));
            break;
        case APP_INPUT_TEST:
            scr = input_test_ui_init();
            layer_sys_ui_visible(0);
            demo_hub_default_theme_set(scr, false);
            strncpy(cur_screen, "input_test", sizeof(cur_screen));
            break;
        case APP_MONITOR:
            layer_sys_ui_visible(0);
            scr = monitor_ui_init();
            demo_hub_default_theme_set(scr, true);
            strncpy(cur_screen, "monitor", sizeof(cur_screen));
            break;
        default:
            break;
    }

    if (scr && auto_del)
        lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 5, true);
    if (scr && auto_del == 0)
        lv_scr_load(scr);
}

app_index_t app_running(void)
{
    if (strncmp(cur_screen, "navigation", strlen("navigation")) == 0)
        return APP_NAVIGATION;
    return APP_NONE;
}
