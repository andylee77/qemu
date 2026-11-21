/*
 * QTest testcase for when a T31's ISP and VIC raise their interrupts
 *
 * As a T31X (gc2053, 1080p at 30 fps) raises them, timed from user space
 * against /proc/interrupts over 120 frames: the VIC's interrupt (isp-w02)
 * once a frame, as it takes the frame in, and the core's frame start 0.53
 * to 0.79 ms after it; the frame's statistics, channel dones and frame
 * end as 6 to 9 interrupts of the core (isp-m0) within about 165 us at
 * its end, 1.8 to 2.1 ms before the next frame's VIC interrupt. tx-isp
 * serves every one of its blocks from either line, so a VIC bit raised
 * with the core's is cleared before the VIC's line is taken. The model
 * raises the end's bits together: their order and spacing inside the
 * 165 us were not seen.
 *
 * Copyright (C) 2026 Andy <andylee77@users.noreply.github.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define ISP             0x13300000
#define CPM_CLKGR0      0x10000020
#define INT_STA         0x0b4
#define INT_CLR         0x0b8
#define CH_EN           0x9804
#define CH0             0x9900
#define VIC_CTRL        0xe0000
#define VIC_STA         0xe01e0
#define VIC_CLR         0xe01f0
#define FRAME_NS        40000000        /* fps=25 */
#define END_NS          (FRAME_NS / 16 * 15)
#define US              1000
#define MS              1000000
#define BUF             0x00400000
#define AE_BUF          0x01030000
#define AWB_BUF         0x01080000      /* 4 buffers of 4 KiB */

#define FRAME_START     (1u << 12)
#define FRAME_END       (1u << 13)
#define CH0_DONE        (1u << 0)
#define AE_ZONES        (1u << 26)
#define AE_HIST         (1u << 27)
#define AWB             (1u << 30)

static QTestState *start(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t31 -device image-sensor,"
                                 "model=jxq03,bus=i2c0,fps=25");

    qtest_writel(qts, CPM_CLKGR0, 0);
    qtest_writel(qts, ISP + 0x4, 1920 << 16 | 1080);
    /* AE: its zones and histogram */
    qtest_writel(qts, ISP + 0xa004, 0xf001f001);
    for (int i = 0; i < 4; i++) {
        qtest_writel(qts, ISP + 0xa008 + i * 4, 0x40404040);
        qtest_writel(qts, ISP + 0xa018 + i * 4, 0x24242424);
    }
    qtest_writel(qts, ISP + 0xa02c, AE_BUF);
    qtest_writel(qts, ISP + 0xa03c, AE_BUF + 0x1000);
    qtest_writel(qts, ISP + 0xa000, 1);
    /* AWB */
    qtest_writel(qts, ISP + 0xb004, 15u << 28 | 1 << 16 | 15 << 12 | 1);
    for (int i = 0; i < 4; i++) {
        qtest_writel(qts, ISP + 0xb008 + i * 4, 0x40404040);
        qtest_writel(qts, ISP + 0xb018 + i * 4, 0x24242424);
        qtest_writel(qts, ISP + 0xb03c + i * 4, AWB_BUF + i * 0x1000);
    }
    qtest_writel(qts, ISP + 0xb038, 1 << 16 | 240 << 8 | 9);
    qtest_writel(qts, ISP + 0xb04c, 3);
    qtest_writel(qts, ISP + 0xb000, 1);
    /* channel 0, 64x64, one buffer */
    qtest_writel(qts, ISP + CH0 + 0x00, 64 << 16 | 64);
    qtest_writel(qts, ISP + CH0 + 0x80, 64);
    qtest_writel(qts, ISP + CH0 + 0x98, 64);
    qtest_writel(qts, ISP + CH0 + 0x6c, BUF);
    qtest_writel(qts, ISP + CH0 + 0x84, BUF + 0x1000);
    qtest_writel(qts, ISP + CH_EN, 1);
    return qts;
}

static uint32_t sta(QTestState *qts)
{
    return qtest_readl(qts, ISP + INT_STA);
}

/*
 * The VIC's bit as it takes the frame in, the core's frame start a few
 * lines on, the statistics not before the frame's end
 */
static void test_start(void)
{
    QTestState *qts = start();

    qtest_writel(qts, ISP + VIC_CTRL, 1);
    g_assert_cmphex(qtest_readl(qts, ISP + VIC_STA) & 1, ==, 1);
    g_assert_cmphex(sta(qts), ==, 0);
    qtest_clock_step(qts, 2 * MS);
    g_assert_cmphex(sta(qts), ==, FRAME_START);
    qtest_clock_step(qts, END_NS - 2 * MS - US);
    g_assert_cmphex(sta(qts), ==, FRAME_START);
    qtest_quit(qts);
}

/*
 * The frame's end: the statistics, the channel's done and the frame end
 * together, none of them before it
 */
static void test_end(void)
{
    QTestState *qts = start();

    qtest_writel(qts, ISP + VIC_CTRL, 1);
    qtest_clock_step(qts, 2 * MS);
    qtest_writel(qts, ISP + INT_CLR, sta(qts));
    qtest_writel(qts, ISP + VIC_CLR, 1);
    qtest_clock_step(qts, END_NS - 2 * MS - US);
    g_assert_cmphex(sta(qts), ==, 0);
    qtest_clock_step(qts, US);
    g_assert_cmphex(sta(qts), ==, AE_ZONES | AE_HIST | AWB | CH0_DONE |
                                  FRAME_END);
    qtest_writel(qts, ISP + INT_CLR, sta(qts));
    /* the next frame: the VIC's bit first, again */
    qtest_clock_step(qts, FRAME_NS - END_NS);
    g_assert_cmphex(qtest_readl(qts, ISP + VIC_STA) & 1, ==, 1);
    g_assert_cmphex(sta(qts), ==, 0);
    qtest_quit(qts);
}

static void wait_status(QTestState *qts, const char *key, const char *busy)
{
    for (int i = 0; i < 6000; i++) {
        QDict *rsp = qtest_qmp(qts, "{ 'execute': %s }", key);
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *st = qdict_get_try_str(ret, "status");
        bool done = st && strcmp(st, busy) && strcmp(st, "setup") &&
                    strcmp(st, "device");

        if (done) {
            g_assert_cmpstr(st, !=, "failed");
            qobject_unref(rsp);
            return;
        }
        qobject_unref(rsp);
        g_usleep(10000);
    }
    g_assert_not_reached();
}

/*
 * Saved while the frame start waits: the loading QEMU raises it, and not
 * at once
 */
static void test_migrate(void)
{
    g_autofree char *path = g_strdup_printf("%s/ingenic-isp-irq-%d.state",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    QTestState *src = start(), *dst;

    qtest_writel(src, ISP + VIC_CTRL, 1);
    qtest_clock_step(src, 100 * US);
    g_assert_cmphex(sta(src), ==, 0);
    qtest_qmp_assert_success(src, "{ 'execute': 'stop' }");
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate', 'arguments': "
                             "{ 'uri': %s } }", uri);
    wait_status(src, "query-migrate", "active");

    dst = qtest_initf("-machine ingenic-t31 -device image-sensor,"
                      "model=jxq03,bus=i2c0,fps=25 -incoming %s", uri);
    wait_status(dst, "query-status", "inmigrate");
    qtest_qmp_assert_success(dst, "{ 'execute': 'cont' }");
    g_assert_cmphex(sta(dst), ==, 0);
    qtest_clock_step(dst, 2 * MS);
    g_assert_cmphex(sta(dst), ==, FRAME_START);

    qtest_quit(dst);
    qtest_quit(src);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ingenic-isp-irq/t31/start", test_start);
    qtest_add_func("/ingenic-isp-irq/t31/end", test_end);
    qtest_add_func("/ingenic-isp-irq/t31/migrate", test_migrate);
    return g_test_run();
}
