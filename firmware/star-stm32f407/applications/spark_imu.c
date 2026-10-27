/*
 * spark_imu.c —— 双 IMU 自适应 + 板载 AHT21
 *
 *   芯片      总线            引脚            WHO_AM_I(0x75)
 *   ICM20608  i2c2(soft)      SCL=PF1 SDA=PF0 0xEA/0xAF   (板载, 本批板实测无应答)
 *   ICM42605  i2c4(soft)      SCL=PB7 SDA=PB6 0x42        (外接模块)
 *   AHT21     i2c3(soft)      SCL=PE0 SDA=PE1 0x38        (板载, 官方 aht10-v2.1.0 协议)
 *
 * 初始化时先扫 i2c2 再扫 i2c4：哪个芯片 ACK 且 WHO_AM_I 匹配就绑定哪个，
 * 寄存器表两套都带 —— 换板/回退都不用重烧。
 *
 * 寄存器编码权威来源：
 *   ICM20608: 官方 icm20608-v1.0.0 包 (= 星火手册第11章)，数据 0x3B.. / 0x43..
 *   ICM42605: Linux mainline drivers/iio/imu/inv_icm42600 (2026-09-15 拉取核对)
 *             PWR_MGMT0=0x4E(TEMP_EN|A_LN<<2|G_LN) GYRO_CONFIG0=0x4F
 *             ACCEL_CONFIG0=0x50(FS[7:5]|ODR[3:0]) 数据 accel@0x1F x6, gyro@0x25 x6
 *             大端; ±2g=16384LSB/g, ±250dps=131LSB/dps (与20608量程刻度一致)
 *
 * Copyright (c) 2026   SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "spark_imu.h"

#define I2C_BUS_20608          "i2c2"
#define I2C_BUS_42605          "i2c4"   /* SCL=PB7 SDA=PB6  */
#define I2C_BUS_42605_ALT      "i2c1"   /* SCL=PB6 SDA=PB7 (RPi silkscreen order) */
#define IMU_ADDR_CAND0         0x68
#define IMU_ADDR_CAND1         0x69
#define REG_WHO_AM_I           0x75
#define WHO_ICM20608_G         0xEA
#define WHO_ICM20608_D         0xAF
#define WHO_ICM42605           0x42
/* 实测(2026-09-15): 龙邱模块实际返回 0x47 = ICM-42688-P(ArduPilot/Betaflight
 * 权威定义)。同族同 BANK0 寄存器映射、同 FS 编码，走同一初始化与读取路径。 */
#define WHO_ICM42688P          0x47

/* ---- ICM-20608 register map (official pkg sequence) ---- */
#define R20_GYRO_CONFIG        0x1B
#define R20_ACCEL_CONFIG1      0x1C
#define R20_ACCEL_CONFIG2      0x1D
#define R20_ACCEL_MEAS         0x3B   /* x6, then temp 0x41, gyro @0x43 x6 */
#define R20_GYRO_MEAS          0x43
#define R20_TEMP_MEAS          0x41
#define R20_PWR_MGMT1          0x6B
#define R20_PWR_MGMT2          0x6C

/* ---- ICM-42605/42688-P register map (inv_icm42600 kernel driver, 两芯片
 * BANK0 映射与 FS 编码一致: accel FS[7:5] 3=±2g, gyro FS[7:5] 3=±250dps;
 * ODR[3:0] 9=50Hz —— 编码表逐行核对自内核 enum, 勿凭印象改) ---- */
#define R42_DEVICE_CONFIG      0x11   /* bit0 SOFT_RESET */
#define R42_TEMP_DATA          0x1D   /* x2, T=raw/132.48+25 (需 TEMP_DIS=0) */
#define R42_ACCEL_DATA         0x1F   /* x6 big-endian */
#define R42_GYRO_DATA          0x25   /* x6 big-endian */
#define R42_PWR_MGMT0          0x4E   /* 0x0F = IDLE=0|ACCEL_LN|GYRO_LN, TEMP_DIS=0 */
#define R42_GYRO_CONFIG0       0x4F   /* 0x69 = FS 0b011(±250dps) | ODR 9(50Hz) */
#define R42_ACCEL_CONFIG0      0x50   /* 0x69 = FS 0b011(±2g)     | ODR 9(50Hz) */

enum imu_chip { CHIP_NONE = 0, CHIP_ICM20608, CHIP_ICM42605 };

static struct rt_i2c_bus_device *g_bus = RT_NULL;
static rt_uint8_t g_addr = IMU_ADDR_CAND0;
static enum imu_chip g_chip = CHIP_NONE;
static rt_int16_t g_accel_off[3] = {0, 0, 0};
static rt_int16_t g_gyro_off[3]  = {0, 0, 0};
static struct rt_mutex g_imu_mutex, g_env_mutex;
static int g_sensor_ipc_ready;
static rt_tick_t g_uptime_origin;
static rt_tick_t g_probe_after;
static int g_probe_scheduled;
static unsigned g_imu_failures;
static spark_imu_t g_imu_sample;
static rt_tick_t g_imu_sample_tick;
static int g_imu_sample_valid;
static rt_uint32_t g_imu_sample_sequence;
static int sensor_ipc_init(void)
{
    int result = RT_EOK;
    rt_enter_critical();
    if (!g_sensor_ipc_ready) {
        result = rt_mutex_init(&g_imu_mutex, "imu", RT_IPC_FLAG_PRIO);
        if (result == RT_EOK) {
            result = rt_mutex_init(&g_env_mutex, "env", RT_IPC_FLAG_PRIO);
            if (result != RT_EOK) rt_mutex_detach(&g_imu_mutex);
            else { g_sensor_ipc_ready = 1; g_uptime_origin = rt_tick_get(); }
        }
    }
    rt_exit_critical(); return result;
}
INIT_COMPONENT_EXPORT(sensor_ipc_init);
rt_uint32_t spark_uptime_ms(void)
{
    if (sensor_ipc_init() != RT_EOK) return 0;
    return (rt_uint32_t)((rt_uint64_t)(rt_tick_get() - g_uptime_origin) * 1000 / RT_TICK_PER_SECOND);
}
static int sensor_lock(struct rt_mutex *mutex)
{
    if (sensor_ipc_init() != RT_EOK) return -RT_ERROR;
    return rt_mutex_take(mutex, rt_tick_from_millisecond(20));
}
static rt_int16_t bounded_axis(int raw, int offset)
{
    int value = raw - offset;
    return value > 32767 ? 32767 : value < -32768 ? -32768 : value;
}

/* ---------------- 低层事务 ---------------- */

static rt_err_t reg_write(rt_uint8_t reg, rt_uint8_t val)
{
    rt_uint8_t buf[2] = { reg, val };
    struct rt_i2c_msg msg;

    msg.addr  = g_addr;
    msg.flags = RT_I2C_WR;
    msg.buf   = buf;
    msg.len   = 2;
    return (rt_i2c_transfer(g_bus, &msg, 1) == 1) ? RT_EOK : -RT_ERROR;
}

/* 寄存器读：拆成"单写指针 + 单读数据"两次独立事务，不用 RESTART。
 * 依据：AHT21 在 i2c3 上用同款两段式已验证可通，而合并式 RESTART 读在本板
 * 位敲驱动上从未跑通过 —— 复用被验证过的形态，别赌没验过的。 */
static rt_err_t reg_read(rt_uint8_t reg, rt_uint8_t *buf, rt_uint8_t len)
{
    if (rt_i2c_master_send(g_bus, g_addr, 0, &reg, 1) != 1)
        return -RT_ERROR;
    if (rt_i2c_master_recv(g_bus, g_addr, 0, buf, len) != (int)len)
        return -RT_ERROR;
    return RT_EOK;
}

/* 总线被从机拉死时的解锁：SCL 补 9 个脉冲（沿用 drv_soft_i2c 的做法）。
 * pin 号: PB6=22 PB7=23 PF0=80 PF1=81 */
static void bus_unlock(const char *bus_name)
{
    rt_base_t scl, sda;
    if (rt_strcmp(bus_name, I2C_BUS_42605) == 0)        { scl = 23; sda = 22; }
    else if (rt_strcmp(bus_name, I2C_BUS_42605_ALT) == 0) { scl = 22; sda = 23; }
    else                                                 { scl = 81; sda = 80; }

    rt_pin_mode(scl, PIN_MODE_OUTPUT_OD);
    rt_pin_mode(sda, PIN_MODE_OUTPUT_OD);
    rt_pin_write(scl, PIN_HIGH);
    rt_pin_write(sda, PIN_HIGH);
    for (int i = 0; i < 9; i++)
    {
        rt_pin_write(scl, PIN_LOW);
        rt_thread_mdelay(1);
        rt_pin_write(scl, PIN_HIGH);
        rt_thread_mdelay(1);
    }
}

static int be16(const rt_uint8_t *b) { return (rt_int16_t)((b[0] << 8) | b[1]); }

/* ---------------- 各芯片初始化 ---------------- */

static rt_err_t init_20608(void)
{
    if (reg_write(R20_PWR_MGMT1, 0x00) != RT_EOK) return -RT_ERROR;  /* 软复位 */
    rt_thread_mdelay(50);
    if (reg_write(R20_PWR_MGMT2, 0x00) != RT_EOK) return -RT_ERROR;  /* 放行全通道 */
    if (reg_write(R20_GYRO_CONFIG, 0x00) != RT_EOK ||
        reg_write(R20_ACCEL_CONFIG1, 0x00) != RT_EOK ||
        reg_write(R20_ACCEL_CONFIG2, 0x01) != RT_EOK) return -RT_ERROR;
    rt_thread_mdelay(20);
    {
        rt_uint8_t pm = 0xFF;
        if (reg_read(R20_PWR_MGMT2, &pm, 1) != RT_EOK || pm != 0x00) return -RT_ERROR;
    }
    return RT_EOK;
}

/* SOFT_RESET 后 I2C 接口要过一会才回来, 独立事务易 NACK:
 * 每笔写都带重试, 复位后给足 100ms */
static rt_err_t reg_write_retry(const char *step, rt_uint8_t reg, rt_uint8_t val, int tries)
{
    for (int i = 0; i < tries; i++)
    {
        if (reg_write(reg, val) == RT_EOK)
            return RT_EOK;
        rt_thread_mdelay(20);
    }
    rt_kprintf("[imu] init step '%s' (reg 0x%02X<=%02X) NACK after %d tries\n",
               step, reg, val, tries);
    return -RT_ERROR;
}

static rt_err_t init_42605_once(void)
{
    if (reg_write_retry("SOFT_RESET", R42_DEVICE_CONFIG, 0x01, 3) != RT_EOK) return -RT_ERROR;
    rt_thread_mdelay(100);   /* 复位后等接口回来 */
    if (reg_write_retry("GYRO_CFG",  R42_GYRO_CONFIG0,   0x69, 5) != RT_EOK) return -RT_ERROR;
    if (reg_write_retry("ACCEL_CFG", R42_ACCEL_CONFIG0,  0x69, 5) != RT_EOK) return -RT_ERROR;
    if (reg_write_retry("PWR",       R42_PWR_MGMT0,      0x0F, 5) != RT_EOK) return -RT_ERROR;
    rt_thread_mdelay(70); /* Give accel/gyro time to publish their first 50Hz samples. */
    {
        /* 回读也带重试: 刚写完芯片可能仍忙 */
        rt_uint8_t pm = 0xFF;
        for (int i = 0; i < 5 && reg_read(R42_PWR_MGMT0, &pm, 1) != RT_EOK; i++)
            rt_thread_mdelay(20);
        if ((pm & 0x3F) != 0x0F) return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t init_42605(void)
{
    rt_err_t r = -RT_ERROR;
    for (int i = 0; i < 3 && r != RT_EOK; i++)
    {
        r = init_42605_once();
        if (r != RT_EOK)
        {
            rt_kprintf("[imu] 42605 init retry %d\n", i + 1);
            rt_thread_mdelay(50);
        }
    }
    if (r == RT_EOK)
    {
        rt_uint8_t pm = 0xFF;
        reg_read(R42_PWR_MGMT0, &pm, 1);
        if ((pm & 0x3F) != 0x0F)
            rt_kprintf("[imu] PWR_MGMT0 readback=0x%02X (expect 0x0F)\n", pm);
    }
    return r;
}

/* ---------------- 总线/芯片探测 (懒执行一次) ---------------- */

static rt_bool_t probe_on_bus(const char *bus_name)
{
    struct rt_i2c_bus_device *bus =
        (struct rt_i2c_bus_device *)rt_device_find(bus_name);
    if (bus == RT_NULL)
    {
        rt_kprintf("[imu] %s not found\n", bus_name);
        return RT_FALSE;
    }

    const rt_uint8_t addrs[2] = { IMU_ADDR_CAND0, IMU_ADDR_CAND1 };
    for (int a = 0; a < 2; a++)
    {
        struct rt_i2c_msg msg;
        rt_uint8_t reg = REG_WHO_AM_I, who = 0;

        msg.addr  = addrs[a];
        msg.flags = RT_I2C_WR;
        msg.buf   = &reg;
        msg.len   = 1;
        if (rt_i2c_transfer(bus, &msg, 1) != 1)
        {
            rt_kprintf("[imu] %s 0x%02X NACK\n", bus_name, addrs[a]);
            continue;
        }

        g_bus  = bus;
        g_addr = addrs[a];
        if (reg_read(REG_WHO_AM_I, &who, 1) != RT_EOK)
        {
            rt_kprintf("[imu] %s 0x%02X ACK but WHO read fail\n", bus_name, addrs[a]);
            g_bus = RT_NULL;
            return RT_FALSE;
        }

        if (who == WHO_ICM20608_G || who == WHO_ICM20608_D)
        {
            if (init_20608() == RT_EOK)
            {
                g_chip = CHIP_ICM20608;
                rt_kprintf("[imu] ICM20608 @0x%02X on %s ready\n", g_addr, bus_name);
                return RT_TRUE;
            }
        }
        else if (who == WHO_ICM42605 || who == WHO_ICM42688P)
        {
            if (init_42605() == RT_EOK)
            {
                g_chip = CHIP_ICM42605;   /* 42688-P 同映射同编码, 共用路径 */
                rt_kprintf("[imu] ICM426xx (id 0x%02X) @0x%02X on %s ready\n",
                           who, g_addr, bus_name);
                return RT_TRUE;
            }
        }
        else
            rt_kprintf("[imu] %s 0x%02X WHO_AM_I=0x%02X unknown\n", bus_name, addrs[a], who);

        g_bus = RT_NULL;
    }

    bus_unlock(bus_name);   /* 全 NACK 且总线可能被钳: 解锁后下一轮再试 */
    return RT_FALSE;
}

static rt_err_t imu_init_once(void)
{
    if (g_chip != CHIP_NONE)
        return RT_EOK;
    if (g_probe_scheduled && (rt_int32_t)(rt_tick_get() - g_probe_after) < 0) return -RT_ERROR;
    g_probe_after = rt_tick_get() + rt_tick_from_millisecond(1000);
    g_probe_scheduled = 1;
    /* 实测(2026-09-15): 用户按丝印 SCL->PB6/SDA->PB7 接线, i2c1 先探 */
    if (probe_on_bus(I2C_BUS_42605_ALT))   /* SCL=PB6 SDA=PB7 (丝印序) */
        return RT_EOK;
    if (probe_on_bus(I2C_BUS_42605))       /* SCL=PB7 SDA=PB6 (反接兜底) */
        return RT_EOK;
    if (probe_on_bus(I2C_BUS_20608))       /* 板载 20608 */
        return RT_EOK;
    return -RT_ERROR;
}

/* ---------------- 对外接口 ---------------- */

int spark_imu_read(spark_imu_t *out)
{
    rt_uint8_t a[6], g[6];

    if (out == RT_NULL)
        return -RT_EINVAL;
    rt_memset(out, 0, sizeof(*out));
    if (sensor_lock(&g_imu_mutex) != RT_EOK) return -RT_ERROR;
    if (imu_init_once() != RT_EOK)
    {
        goto failed;
    }

    if (g_chip == CHIP_ICM42605)
    {
        rt_uint8_t raw[12];
        if (reg_read(R42_ACCEL_DATA, raw, sizeof(raw)) != RT_EOK)
        {
            goto failed;
        }
        for (int i = 0; i < 6; i++) if (be16(raw + 2 * i) == -32768) goto failed;
        memcpy(a, raw, 6); memcpy(g, raw + 6, 6);
        out->ax = bounded_axis(be16(a), g_accel_off[0]);
        out->ay = bounded_axis(be16(a + 2), g_accel_off[1]);
        out->az = bounded_axis(be16(a + 4), g_accel_off[2]);
        out->gx = bounded_axis(be16(g), g_gyro_off[0]);
        out->gy = bounded_axis(be16(g + 2), g_gyro_off[1]);
        out->gz = bounded_axis(be16(g + 4), g_gyro_off[2]);
    }
    else
    {
        rt_uint8_t raw[14];
        if (reg_read(R20_ACCEL_MEAS, raw, sizeof(raw)) != RT_EOK)
        {
            goto failed;
        }
        memcpy(a, raw, 6); memcpy(g, raw + 8, 6);
        out->ax = bounded_axis(be16(a), g_accel_off[0]);
        out->ay = bounded_axis(be16(a + 2), g_accel_off[1]);
        out->az = bounded_axis(be16(a + 4), g_accel_off[2]);
        out->gx = bounded_axis(be16(g), g_gyro_off[0]);
        out->gy = bounded_axis(be16(g + 2), g_gyro_off[1]);
        out->gz = bounded_axis(be16(g + 4), g_gyro_off[2]);
    }

    /* A complete transfer is valid even when every axis happens to be zero. */
    out->valid = RT_TRUE;
    g_imu_failures = 0; rt_mutex_release(&g_imu_mutex); return RT_EOK;
failed:
    if (++g_imu_failures >= 3) { g_chip = CHIP_NONE; g_bus = RT_NULL; g_imu_failures = 0; }
    rt_mutex_release(&g_imu_mutex); return -RT_ERROR;
}

static void imu_sample_once(void)
{
    spark_imu_t sample;
    int ok = spark_imu_read(&sample) == RT_EOK;
    rt_enter_critical();
    g_imu_sample_valid = ok;
    if (ok) { g_imu_sample = sample; g_imu_sample_tick = rt_tick_get(); g_imu_sample_sequence++; }
    rt_exit_critical();
}
int spark_imu_cached(spark_imu_t *out)
{
    return spark_imu_snapshot(out, RT_NULL, RT_NULL);
}
int spark_imu_snapshot(spark_imu_t *out, rt_uint32_t *sequence, rt_uint32_t *age_ms)
{
    if (!out) return -RT_EINVAL;
    rt_enter_critical();
    int valid = g_imu_sample_valid &&
        (rt_tick_t)(rt_tick_get() - g_imu_sample_tick) <= rt_tick_from_millisecond(200);
    if (valid) *out = g_imu_sample;
    else rt_memset(out, 0, sizeof(*out));
    if (sequence) *sequence = g_imu_sample_sequence;
    if (age_ms) *age_ms = g_imu_sample_sequence ?
        (rt_uint32_t)((rt_uint64_t)(rt_tick_get() - g_imu_sample_tick) * 1000 / RT_TICK_PER_SECOND) : UINT32_MAX;
    rt_exit_critical();
    return valid ? RT_EOK : -RT_ERROR;
}
static void imu_thread_entry(void *arg)
{
    (void)arg;
    for (;;) { imu_sample_once(); rt_thread_mdelay(20); }
}
static int imu_thread_start(void)
{
    rt_thread_t th = rt_thread_create("sparkimu", imu_thread_entry, RT_NULL, 4096, 24, 10);
    if (!th) return -RT_ENOMEM;
    if (rt_thread_startup(th) != RT_EOK) { rt_thread_delete(th); return -RT_ERROR; }
    return RT_EOK;
}
INIT_APP_EXPORT(imu_thread_start);

/* 芯片结温(0.01C)：20608 raw/326.8+25, 42605 raw/132.48+25 —— 与 AHT21 交叉验证 */
static int imu_temp_locked(rt_int16_t *out)
{
    rt_uint8_t b[2];
    if (imu_init_once() != RT_EOK)
        return -RT_ERROR;
    {
        rt_uint8_t reg = (g_chip == CHIP_ICM42605) ? R42_TEMP_DATA : R20_TEMP_MEAS;
        if (reg_read(reg, b, 2) != RT_EOK)
            return -RT_ERROR;
        {
            rt_int32_t raw = be16(b);
            if (g_chip == CHIP_ICM42605 && raw == -32768) return -RT_ERROR;
            /* 2026-09-16 修 BUG：换算系数写小了 100 倍。T = raw/132.48 + 25(℃)
             *  ⇒ 0.01℃ 应为 raw*10000/13248（20608 是 raw*10000/32680）。
             *  原来写 raw*100/xxxxx，整数量化后几乎恒定输出 25.0x℃，看起来
             *  "结温不动"，实际上是缩放错了 100 倍。*/
            rt_int32_t centi = 2500 + (rt_int32_t)((rt_int64_t)raw * 10000 /
                                 (g_chip == CHIP_ICM42605 ? 13248 : 32680));
            if (centi > 15000) centi = 15000;      /* 合理上限 150.00C, 防 int16 溢出 */
            if (centi < -4000) centi = -4000;
            *out = (rt_int16_t)centi;
        }
    }
    return RT_EOK;
}
int spark_imu_temp_centi(rt_int16_t *out)
{
    if (!out) return -RT_EINVAL;
    if (sensor_lock(&g_imu_mutex) != RT_EOK) return -RT_ERROR;
    int result = imu_temp_locked(out); rt_mutex_release(&g_imu_mutex); return result;
}

/* 校准：两套芯片刻度一致(±2g=16384/g, ±250dps=131/dps)，共用同一偏移 */
int spark_imu_calib(int times)
{
    rt_int32_t a[3] = {0, 0, 0}, g[3] = {0, 0, 0};
    spark_imu_t s;
    int i, ok = 0;

    if (times < 1)
        times = 10;
    if (times > 512) return -RT_EINVAL;
    if (sensor_lock(&g_imu_mutex) != RT_EOK) return -RT_ERROR;
    rt_int16_t old_a[3], old_g[3];
    memcpy(old_a, g_accel_off, sizeof(old_a)); memcpy(old_g, g_gyro_off, sizeof(old_g));
    memset(g_accel_off, 0, sizeof(g_accel_off)); memset(g_gyro_off, 0, sizeof(g_gyro_off));
    for (i = 0; i < times; i++)
    {
        if (spark_imu_read(&s) == RT_EOK)
        {
            a[0] += s.ax; a[1] += s.ay; a[2] += s.az;
            g[0] += s.gx; g[1] += s.gy; g[2] += s.gz;
            ok++;
        }
        rt_thread_mdelay(5);
    }
    if (ok == 0)
    {
        memcpy(g_accel_off, old_a, sizeof(old_a)); memcpy(g_gyro_off, old_g, sizeof(old_g));
        rt_mutex_release(&g_imu_mutex); return -RT_ERROR;
    }

    if (abs(a[2] / ok) < 14000 || abs(a[0] / ok) > 4000 || abs(a[1] / ok) > 4000) {
        memcpy(g_accel_off, old_a, sizeof(old_a)); memcpy(g_gyro_off, old_g, sizeof(old_g));
        rt_mutex_release(&g_imu_mutex); return -RT_EINVAL;
    }
    g_accel_off[0] = (rt_int16_t)(a[0] / ok);
    g_accel_off[1] = (rt_int16_t)(a[1] / ok);
    g_accel_off[2] = bounded_axis(a[2] / ok, a[2] < 0 ? -16384 : 16384);
    g_gyro_off[0]  = (rt_int16_t)(g[0] / ok);
    g_gyro_off[1]  = (rt_int16_t)(g[1] / ok);
    g_gyro_off[2]  = (rt_int16_t)(g[2] / ok);
    rt_kprintf("[imu] calib done n=%d accel off %d %d %d gyro off %d %d %d\n",
               ok, g_accel_off[0], g_accel_off[1], g_accel_off[2],
               g_gyro_off[0], g_gyro_off[1], g_gyro_off[2]);
    rt_mutex_release(&g_imu_mutex); return RT_EOK;
}

static void cmd_imu_read(int argc, char **argv)
{
    spark_imu_t s;
    rt_int16_t t = 0;

    for (int i = 0; i < 5; i++)
    {
        if (spark_imu_read(&s) == RT_EOK)
        {
            int temp_ok = spark_imu_temp_centi(&t) == RT_EOK;
            rt_kprintf("%s accel %6d %6d %6d   gyro %6d %6d %6d",
                       g_chip == CHIP_ICM42605 ? "42605" : "20608",
                       s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
            if (temp_ok) rt_kprintf("   die %s%d.%02d C\n", t < 0 ? "-" : "", abs(t) / 100, abs(t) % 100);
            else rt_kprintf("   die N/A\n");
        }
        else
            rt_kprintf("[imu] read fail\n");
        rt_thread_mdelay(50);
    }
}
MSH_CMD_EXPORT_ALIAS(cmd_imu_read, imu_read, read onboard/module ICM 5 frames);

static void cmd_imu_calib(int argc, char **argv)
{
    rt_kprintf("[imu] keep board FLAT and STILL, calibrating...\n");
    rt_thread_mdelay(500);
    spark_imu_calib(20);
}
MSH_CMD_EXPORT_ALIAS(cmd_imu_calib, imu_calib, ICM bias calibration keep still);

/* ============================ AHT21 温湿度 (软 I2C3 @ PE0/PE1, 0x38) ======
 * 协议逐字节对齐官方 aht10-v2.1.0 包（手册第 9 章）。AC 测量本身要 ~80ms，
 * 不进 100ms 查询应答路径 —— 用 1Hz 线程缓存，从站线程只读缓存。
 * ========================================================================== */

#define AHT21_ADDR        0x38
#define AHT21_CMD_CAL     0xBE
#define AHT21_CMD_GET     0xAC
#define AHT_ENV_BUS       "i2c3"

static struct rt_i2c_bus_device *g_env_bus = RT_NULL;
static rt_bool_t g_env_inited = RT_FALSE;
static rt_int16_t g_env_temp_centi = 0;
static rt_uint16_t g_env_humi_centi = 0;
static rt_bool_t g_env_valid;
static rt_tick_t g_env_sample_tick;
static rt_uint32_t g_env_errors;
/* Aosong AHT21 manual: CRC8 init 0xff, polynomial 0x31, status BUSY bit7. */
static rt_uint8_t env_crc8(const rt_uint8_t *data, int count)
{
    rt_uint8_t crc = 0xff;
    for (int i = 0; i < count; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = crc & 0x80 ? (crc << 1) ^ 0x31 : crc << 1;
    }
    return crc;
}
static int env_status(rt_uint8_t *status)
{
    rt_uint8_t command = 0x71;
    return rt_i2c_master_send(g_env_bus, AHT21_ADDR, 0, &command, 1) == 1 &&
           rt_i2c_master_recv(g_env_bus, AHT21_ADDR, 0, status, 1) == 1 ? RT_EOK : -RT_ERROR;
}

static rt_err_t aht21_cmd(struct rt_i2c_bus_device *bus, rt_uint8_t cmd,
                          rt_uint8_t a, rt_uint8_t b)
{
    rt_uint8_t buf[3] = { cmd, a, b };
    return (rt_i2c_master_send(bus, AHT21_ADDR, 0, buf, 3) == 3) ? RT_EOK : -RT_ERROR;
}

static rt_err_t aht_env_init(void)
{
    if (g_env_inited)
        return RT_EOK;

    g_env_bus = (struct rt_i2c_bus_device *)rt_device_find(AHT_ENV_BUS);
    if (g_env_bus == RT_NULL)
    {
        rt_kprintf("[env] %s not found, check BSP_USING_I2C3\n", AHT_ENV_BUS);
        return -RT_ERROR;
    }

    rt_uint8_t status;
    if (env_status(&status) != RT_EOK) return -RT_ERROR;
    if (!(status & 0x08)) {
        if (aht21_cmd(g_env_bus, AHT21_CMD_CAL, 0x08, 0x00) != RT_EOK) return -RT_ERROR;
        rt_thread_mdelay(10);
        if (env_status(&status) != RT_EOK || !(status & 0x08)) return -RT_ERROR;
    }

    g_env_inited = RT_TRUE;
    rt_kprintf("[env] AHT21 ready on %s (0x%02X)\n", AHT_ENV_BUS, AHT21_ADDR);
    return RT_EOK;
}

static void aht_env_sample_locked(void)
{
    rt_uint8_t d[7];
    struct rt_i2c_msg msg;

    if (aht_env_init() != RT_EOK)
        return;
    if (aht21_cmd(g_env_bus, AHT21_CMD_GET, 0x33, 0) != RT_EOK)
        return;
    rt_thread_mdelay(80);                        /* 测量时间 */

    msg.addr  = AHT21_ADDR;
    msg.flags = RT_I2C_RD;
    msg.buf   = d;
    msg.len   = sizeof(d);
    if (rt_i2c_transfer(g_env_bus, &msg, 1) != 1)
        return;
    if ((d[0] & 0x88) != 0x08 || env_crc8(d, 6) != d[6])
        return;

    /* ---- 2026-09-16 修 BUG（两条通道同时恒定偏低 40.96，就是这条）----
     * 20 位原始值最大 1048575，公式里的 *10000u / *20000 在 32 位里会溢出：
     *   RH: rh_raw*10000 > 2^32 ⇔ rh_raw > 429497 ⇔ RH > 40.96%
     *   T : t_raw *20000 > 2^32 ⇔ t_raw  > 214748 ⇔ T  > -9.03℃（几乎永远成立）
     * 溢出后被 2^32 取模，再 >>20 恰好少 (2^32>>20)=4096 = 40.96，于是真实
     * 25.0℃ 显示成 -15.96℃、真实 46.2%RH 显示成 5.28%RH（实测吻合）。
     * 用 64 位中间量即可，换算系数本身没错。 */
    {
        rt_uint32_t rh_raw = (rt_uint32_t)d[1] << 12 | (rt_uint32_t)d[2] << 4
                             | (d[3] & 0xF0) >> 4;
        rt_uint32_t t_raw  = (rt_uint32_t)(d[3] & 0x0F) << 16 | (rt_uint32_t)d[4] << 8
                             | d[5];

        rt_enter_critical();
        g_env_humi_centi = (rt_uint16_t)((rt_uint32_t)(((rt_uint64_t)rh_raw * 10000u) >> 20));
        g_env_temp_centi = (rt_int16_t)((rt_int32_t)(((rt_uint64_t)t_raw * 20000u) >> 20) - 5000);
        g_env_sample_tick = rt_tick_get(); g_env_valid = RT_TRUE;
        rt_exit_critical();
    }
}
static void aht_env_sample(void)
{
    if (sensor_lock(&g_env_mutex) != RT_EOK) return;
    rt_tick_t previous = g_env_sample_tick;
    aht_env_sample_locked();
    if (g_env_sample_tick == previous) g_env_errors++;
    rt_mutex_release(&g_env_mutex);
}

static void env_thread_entry(void *arg)
{
    while (1)
    {
        aht_env_sample();
        rt_thread_mdelay(1000);
    }
}

static int env_thread_start(void)
{
    rt_thread_t th = rt_thread_create("sparkenv", env_thread_entry, RT_NULL,
                                      4096, 24, 10);
    if (!th) return -RT_ENOMEM;
    if (rt_thread_startup(th) != RT_EOK) { rt_thread_delete(th); return -RT_ERROR; }
    return RT_EOK;
}
INIT_APP_EXPORT(env_thread_start);

rt_bool_t spark_env_read(rt_int16_t *temp_centi, rt_uint16_t *humi_centi)
{
    rt_enter_critical();
    int valid = g_env_valid && (rt_tick_t)(rt_tick_get() - g_env_sample_tick) <= rt_tick_from_millisecond(3000);
    if (temp_centi) *temp_centi = valid ? g_env_temp_centi : 0;
    if (humi_centi) *humi_centi = valid ? g_env_humi_centi : 0;
    rt_exit_critical(); return valid;
}

static void cmd_env_read(int argc, char **argv)
{
    rt_int16_t t; rt_uint16_t h;
    if (spark_env_read(&t, &h))
        rt_kprintf("[env] %s%d.%02d C  %u.%02u %%RH\n", t < 0 ? "-" : "", abs(t) / 100, abs(t) % 100,
                   h / 100, h % 100);
    else
        rt_kprintf("[env] no fresh sample (errors=%u)\n", g_env_errors);
}
MSH_CMD_EXPORT_ALIAS(cmd_env_read, env_read, read onboard AHT21 cache);

/* env_raw —— 现场验伤：读一帧原始 7 字节 + 状态字，同时打印 64 位换算与 32 位
 * 溢出复现值（解释上面那条 BUG）。判链路/判换算用，不参与遥测。 */
static void cmd_env_raw_locked(int argc, char **argv)
{
    rt_uint8_t d[7];
    struct rt_i2c_msg msg;
    rt_uint32_t rh_raw, t_raw;
    rt_int32_t rh_c, t_c;

    if (aht_env_init() != RT_EOK)
        return;
    if (aht21_cmd(g_env_bus, AHT21_CMD_GET, 0x33, 0) != RT_EOK)
    {
        rt_kprintf("[raw] trigger NACK\n");
        return;
    }
    rt_thread_mdelay(80);

    msg.addr = AHT21_ADDR; msg.flags = RT_I2C_RD; msg.buf = d; msg.len = sizeof(d);
    if (rt_i2c_transfer(g_env_bus, &msg, 1) != 1)
    {
        rt_kprintf("[raw] read failed\n");
        return;
    }

    rt_kprintf("[raw] bytes: %02X %02X %02X %02X %02X %02X %02X\n",
               d[0], d[1], d[2], d[3], d[4], d[5], d[6]);
    rt_kprintf("[raw] status 0x%02X  CAL=%d  BUSY=%d\n", d[0],
               (d[0] & 0x08) ? 1 : 0, (d[0] & 0x80) ? 1 : 0);
    rt_kprintf("[raw] CRC %s\n", env_crc8(d, 6) == d[6] ? "OK" : "ERROR");

    rh_raw = (rt_uint32_t)d[1] << 12 | (rt_uint32_t)d[2] << 4 | (d[3] & 0xF0) >> 4;
    t_raw  = (rt_uint32_t)(d[3] & 0x0F) << 16 | (rt_uint32_t)d[4] << 8 | d[5];
    rt_kprintf("[raw] rh_raw=%u  t_raw=%u\n", (unsigned)rh_raw, (unsigned)t_raw);

    rh_c = (rt_int32_t)(((rt_uint64_t)rh_raw * 10000u) >> 20);          /* 0.01 %RH */
    t_c  = (rt_int32_t)(((rt_uint64_t)t_raw  * 20000u) >> 20) - 5000;   /* 0.01 C   */
    rt_kprintf("[raw] 64位换算   : RH %d.%02d %%   T %d.%02d C\n",
               rh_c / 100, rh_c % 100, t_c / 100, (t_c < 0 ? -t_c : t_c) % 100);

    rh_c = (rt_int32_t)((rh_raw * 10000u) >> 20);                        /* 溢出复现 */
    t_c  = (rt_int32_t)((t_raw * 20000u) >> 20) - 5000;
    rt_kprintf("[raw] 32位溢出复现: RH %d.%02d %%   T %d.%02d C\n",
               rh_c / 100, rh_c % 100, t_c / 100, (t_c < 0 ? -t_c : t_c) % 100);
    rt_kprintf("[raw] 缓存(遥测实际值): %d.%02d C  %u.%02u %%RH\n",
               g_env_temp_centi / 100,
               (g_env_temp_centi < 0 ? -g_env_temp_centi : g_env_temp_centi) % 100,
               g_env_humi_centi / 100, g_env_humi_centi % 100);
}
static void cmd_env_raw(int argc, char **argv)
{
    if (sensor_lock(&g_env_mutex) != RT_EOK) return;
    cmd_env_raw_locked(argc, argv); rt_mutex_release(&g_env_mutex);
}
MSH_CMD_EXPORT_ALIAS(cmd_env_raw, env_raw, dump AHT21 raw bytes (bug forensics));
