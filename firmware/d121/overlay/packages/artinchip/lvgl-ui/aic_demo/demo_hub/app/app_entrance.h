/*
 * Copyright (c) 2023-2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  ArtInChip
 */

/* 2026-09-16 方案B 收敛后仅剩 4 个入口（被删 app 的枚举已移除，git 可找回） */
enum {
    APP_NAVIGATION = 0,
    APP_INPUT_TEST,
    APP_DASHBOARD,
    APP_MONITOR,      /* M3: 设备监控三页 */
    APP_NONE,
};
typedef int app_index_t;

void app_entrance(app_index_t index, int auto_del);
app_index_t app_running(void);
