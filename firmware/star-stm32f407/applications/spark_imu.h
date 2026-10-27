/*
 * 本地 ICM20608 + AHT21 驱动 —— 星火一号板载传感器
 *
 * 为什么不用官方包：官方 icm20608/aht10 包整体包在 #ifdef PKG_USING_xxx 里，
 * 而 Studio 的 Debug/makefile 只编译登记过的 .c，packages/ 目录不会被自动构建。
 * 这里做成自包含普通源文件（寄存器序列逐项对齐官方包），登记进 applications。
 *
 * 硬件事实（星火1号用户手册第 9/11 章 + BSP 03_driver_axis/03_driver_temp_humi，
 * 均本地核过）：
 *   ICM-20608-G: 软 I2C2, SCL=PF1, SDA=PF0, addr 0x68; INT=PG12(不用,轮询)
 *   AHT21:       软 I2C3, SCL=PE0, SDA=PE1, addr 0x38
 *
 * Copyright (c) 2026   SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SPARK_IMU_H__
#define __SPARK_IMU_H__

#include <rtthread.h>

typedef struct
{
    rt_int16_t ax, ay, az;    /* 原始 LSB，±2g：16384 LSB/g */
    rt_int16_t gx, gy, gz;    /* 原始 LSB，±250dps：131 LSB/dps */
    rt_bool_t  valid;
} spark_imu_t;

/* 懒初始化（首次读取时才碰总线）。返回 0 成功。 */
int spark_imu_read(spark_imu_t *out);

/* Nonblocking telemetry snapshot; no I2C, probing or sensor mutex waits. */
int spark_imu_cached(spark_imu_t *out);
int spark_imu_snapshot(spark_imu_t *out, rt_uint32_t *sequence, rt_uint32_t *age_ms);

/* 手动触发校准（板子静止时调用，N 次平均）。 */
int spark_imu_calib(int times);

/* 芯片结温(0.01C)，来自 ICM20608 内部温度寄存器 —— 与 AHT21 交叉验证用 */
int spark_imu_temp_centi(rt_int16_t *out);

/* 板载 AHT21 缓存读取（1Hz 线程采样）；返回 RT_FALSE 表示还没出过有效值 */
rt_bool_t spark_env_read(rt_int16_t *temp_centi, rt_uint16_t *humi_centi);
rt_uint32_t spark_uptime_ms(void);

#endif
