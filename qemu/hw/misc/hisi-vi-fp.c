/*
 * HiSilicon Video Input Frame Producer (QEMU emulation helper).
 *
 * MVP scaffolding to wake the vendor VI / VPSS / VENC pipeline on QEMU.
 *
 * On real silicon the MIPI receiver delivers a CSI-2 frame to a VI capture
 * buffer in DDR, and the VI hardware fires the VI_CAP IRQ (followed by
 * VI_PROC then VPSS once the frame walks through the pipeline).  Without
 * any of that hardware on QEMU the vendor VENC blob blocks forever in
 * its "wait for next encoded frame" loop, hence Majestic's repeating
 *   "Timeout from venc channel 0"
 * messages.
 *
 * This device fires the VI_CAP / VI_PROC / VPSS IRQ lines on a periodic
 * timer matching the configured sensor framerate (default 5 fps).  No
 * MMIO contract — vendor VI / VPSS register state is whatever the RAM-
 * backed regbanks behind 0x11000000 / 0x11200000 / 0x11400000 carry,
 * which is "all zeros at reset, then whatever the vendor wrote".  The
 * IRQ alone is enough to make the vendor's IRQ-route routines run; how
 * far the pipeline gets is then a function of the regbank state.
 *
 * Wired only on hi3516ev300 as MVP (HisiSoCConfig.vi_fp_base != 0).
 *
 * Copyright (c) 2026 OpenIPC.
 * Written by Dmitry Ilyin
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"

#define TYPE_HISI_VI_FP "hisi-vi-fp"
OBJECT_DECLARE_SIMPLE_TYPE(HisiViFpState, HISI_VI_FP)

/* Tiny placeholder MMIO; sysbus requires at least one region but the
 * guest has no driver for this address. */
#define VI_FP_REGION_SIZE  0x100

/* Inter-stage delay so VI_CAP -> VI_PROC -> VPSS arrive in plausible
 * temporal order on the IRQ controller. */
#define VI_FP_STAGE_DELAY_NS    1000   /* 1 us */

struct HisiViFpState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    /* IRQ outputs, in pipeline order. */
    qemu_irq irq_vi_cap;
    qemu_irq irq_vi_proc;
    qemu_irq irq_vpss;

    /* Sensor framerate from QOM property (default 5 fps — see
     * hisi_vi_fp_properties for why TCG can't keep up at 25). */
    uint32_t fps;

    /* Periodic frame timer. */
    QEMUTimer *frame_timer;

    /* Deassert timer — fires shortly after frame_timer to drop the
     * IRQ lines back to 0 once the vendor handlers have had a chance
     * to run.  Without this delayed deassert the kernel sees the line
     * stay high and re-enters the handler in a tight loop, starving
     * userspace.  With qemu_irq_pulse the GIC misses the level entirely
     * (counter stays at 0 in /proc/interrupts).  This timer threads the
     * needle: assert in tick, deassert ~5 ms later. */
    QEMUTimer *deassert_timer;

    /* IRQ heartbeat enable.
     *   Guest writes 1 to MMIO offset 0x000 to start the heartbeat
     *   (typically from a userspace script after vendor MPP modules
     *   are fully initialised — firing earlier hits NULL pointers
     *   inside the vendor IRQ handlers).
     *   Writing 0 stops it. */
    uint32_t  enable;

    /* Pulse counters for /sysctl-style introspection during bring-up. */
    uint64_t  pulses;
};

/* MMIO map (16 bytes total):
 *   0x000  CTRL    RW   bit 0 = enable IRQ heartbeat (start/stop)
 *   0x004  STATUS  RO   pulse counter (low 32 bits)
 *   0x008  FPS     RW   frames per second (overrides QOM property)
 */
#define VI_FP_REG_CTRL    0x000
#define VI_FP_REG_STATUS  0x004
#define VI_FP_REG_FPS     0x008

static uint64_t hisi_vi_fp_read(void *opaque, hwaddr offset, unsigned size)
{
    HisiViFpState *s = HISI_VI_FP(opaque);

    switch (offset) {
    case VI_FP_REG_CTRL:
        return s->enable;
    case VI_FP_REG_STATUS:
        return (uint32_t)(s->pulses & 0xFFFFFFFF);
    case VI_FP_REG_FPS:
        return s->fps;
    }
    return 0;
}

static void hisi_vi_fp_arm_timer(HisiViFpState *s)
{
    uint32_t period_ms = s->fps ? (1000U / s->fps) : 200U;
    timer_mod(s->frame_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + period_ms);
}

static void hisi_vi_fp_write(void *opaque, hwaddr offset,
                             uint64_t val, unsigned size)
{
    HisiViFpState *s = HISI_VI_FP(opaque);

    switch (offset) {
    case VI_FP_REG_CTRL: {
        uint32_t prev = s->enable;
        s->enable = val & 1;
        if (s->enable && !prev) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "hisi-vi-fp: heartbeat ENABLED at %u fps\n",
                          s->fps);
            hisi_vi_fp_arm_timer(s);
        } else if (!s->enable && prev) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "hisi-vi-fp: heartbeat DISABLED (pulses=%llu)\n",
                          (unsigned long long)s->pulses);
            timer_del(s->frame_timer);
            /* Drop any held-high IRQ lines so masked vendor handlers
             * don't see a phantom level on rmmod. */
            qemu_set_irq(s->irq_vi_cap, 0);
            qemu_set_irq(s->irq_vi_proc, 0);
            qemu_set_irq(s->irq_vpss, 0);
        }
        return;
    }
    case VI_FP_REG_FPS:
        if (val) {
            s->fps = val;
        }
        return;
    }
}

static const MemoryRegionOps hisi_vi_fp_ops = {
    .read = hisi_vi_fp_read,
    .write = hisi_vi_fp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* "Frame-ready" status pokes — addresses that real silicon raises
 * before firing the corresponding IRQ.  Reverse-engineered from
 * /home/john-1/git/openhisilicon/kernel/vi/vi.o:
 *
 *   VI_HAL_GetProcIrqStatus(0) returns *(VI_PROC0_BASE + 0x310).
 *   VI_HAL_IsProcIrqValid(status) returns (status != 0).
 *   If invalid, VI_COMM_ProcIrqRoute panics at line 1993 — exactly
 *   what we observed when firing IRQs without populating this register.
 *
 * Real-cam value (devmem on openipc-hi3516ev300.dlab.doty.ru, while
 * Majestic is streaming): 0x06732001 — bit 0 = "frame done", bits 13-26
 * appear to be a hw timing counter.  The register auto-clears on read.
 *
 * Setting any non-zero value satisfies IsProcIrqValid; we use the real
 * captured value for fidelity. */
static const struct {
    hwaddr   addr;
    uint32_t value;
    const char *name;
} hisi_vi_fp_pokes[] = {
    { 0x11200310, 0x06732001, "VI_PROC0+0x310 (proc IRQ status)" },
    /* VI_HAL_GetCapIntStatus(0) returns *(VI_CAP0_BASE + 0xF0).
     * VI_DRV_IsCapValidInt(status) returns ((status & 0xF00) != 0).
     * If invalid, the kernel decides the IRQ is spurious and after a
     * few hits disables IRQ #42 — observed before this poke as
     *   "Disabling IRQ #42"
     * in dmesg.  Real silicon clears this register on read so it
     * sampled as 0 over SSH; any value with bits 8-11 set satisfies
     * the validity check. */
    { 0x110000F0, 0x00000F00, "VI_CAP0+0xF0   (cap IRQ status, IsCapValidInt mask)" },
};

static void hisi_vi_fp_deassert(void *opaque)
{
    HisiViFpState *s = HISI_VI_FP(opaque);
    qemu_set_irq(s->irq_vi_cap, 0);
    qemu_set_irq(s->irq_vi_proc, 0);
    qemu_set_irq(s->irq_vpss, 0);
}

static void hisi_vi_fp_tick(void *opaque)
{
    HisiViFpState *s = HISI_VI_FP(opaque);
    int i;

    if (!s->enable) {
        return;
    }

    /* Pre-populate hardware status registers the vendor IRQ handlers
     * read to validate the IRQ is real (otherwise they panic). */
    for (i = 0; i < ARRAY_SIZE(hisi_vi_fp_pokes); i++) {
        address_space_stl(&address_space_memory,
                          hisi_vi_fp_pokes[i].addr,
                          hisi_vi_fp_pokes[i].value,
                          MEMTXATTRS_UNSPECIFIED, NULL);
        if (s->pulses < 3) {
            uint32_t rb = address_space_ldl(&address_space_memory,
                                             hisi_vi_fp_pokes[i].addr,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "hisi-vi-fp: poke %s @0x%llx <- 0x%08x readback=0x%08x\n",
                          hisi_vi_fp_pokes[i].name,
                          (unsigned long long)hisi_vi_fp_pokes[i].addr,
                          hisi_vi_fp_pokes[i].value, rb);
        }
    }

    /* Assert the lines now; the deassert_timer (5 ms later) drops
     * them.  Real silicon clears the IRQ source on register read,
     * which we don't model — but giving the kernel a 5 ms window of
     * "line high" is enough for one handler invocation per pulse,
     * and the subsequent deassert keeps it from looping. */
    qemu_set_irq(s->irq_vi_cap, 1);
    qemu_set_irq(s->irq_vi_proc, 1);
    qemu_set_irq(s->irq_vpss, 1);
    timer_mod(s->deassert_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 5);

    s->pulses++;
    if (s->pulses <= 5 || (s->pulses % 25) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "hisi-vi-fp: pulse #%llu fired VI_CAP/VI_PROC/VPSS\n",
                      (unsigned long long)s->pulses);
    }

    /* Reschedule. */
    hisi_vi_fp_arm_timer(s);
}

static void hisi_vi_fp_realize(DeviceState *dev, Error **errp)
{
    HisiViFpState *s = HISI_VI_FP(dev);

    /* QEMU_CLOCK_REALTIME (wall-clock) — not VIRTUAL: a real CMOS sensor
     * fires its frame-ready strobe at a wall-clock rate fixed by the
     * pixel-clock crystal, regardless of how busy the SoC is.  And on
     * TCG, where each guest cycle costs ~10x its native budget, using
     * VIRTUAL would let the heartbeat outrun the guest's IRQ-handler
     * throughput so the kernel never gets back to userspace. */
    s->frame_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                  hisi_vi_fp_tick, s);
    s->deassert_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                     hisi_vi_fp_deassert, s);
    /* Don't start firing yet — guest must explicitly enable via
     * MMIO write to VI_FP_REG_CTRL once the vendor MPP modules and
     * Majestic are fully initialised.  Firing earlier hits NULL
     * pointers inside vendor IRQ handlers (verified empirically:
     * VI_DRV_IspIsr@open_vi.ko derefs NULL during insmod). */
}

static void hisi_vi_fp_init(Object *obj)
{
    HisiViFpState *s = HISI_VI_FP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &hisi_vi_fp_ops, s,
                          "hisi-vi-fp", VI_FP_REGION_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    sysbus_init_irq(sbd, &s->irq_vi_cap);
    sysbus_init_irq(sbd, &s->irq_vi_proc);
    sysbus_init_irq(sbd, &s->irq_vpss);
}

static void hisi_vi_fp_reset(DeviceState *dev)
{
    HisiViFpState *s = HISI_VI_FP(dev);
    s->pulses = 0;
    s->enable = 0;
    if (s->frame_timer) {
        timer_del(s->frame_timer);
    }
    if (s->deassert_timer) {
        timer_del(s->deassert_timer);
    }
    qemu_set_irq(s->irq_vi_cap, 0);
    qemu_set_irq(s->irq_vi_proc, 0);
    qemu_set_irq(s->irq_vpss, 0);
}

static const Property hisi_vi_fp_properties[] = {
    /* Default 5 fps — high enough that vendor "wait for next frame"
     * loops make forward progress, low enough that the IRQ-handler
     * cost on TCG (~10x native) leaves the guest scheduler some
     * wall-clock to dispatch userspace.  Override with
     * `-global hisi-vi-fp.fps=N`. */
    DEFINE_PROP_UINT32("fps", HisiViFpState, fps, 5),
};

static void hisi_vi_fp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = hisi_vi_fp_realize;
    device_class_set_legacy_reset(dc, hisi_vi_fp_reset);
    device_class_set_props(dc, hisi_vi_fp_properties);
}

static const TypeInfo hisi_vi_fp_info = {
    .name          = TYPE_HISI_VI_FP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HisiViFpState),
    .instance_init = hisi_vi_fp_init,
    .class_init    = hisi_vi_fp_class_init,
};

static void hisi_vi_fp_register_types(void)
{
    type_register_static(&hisi_vi_fp_info);
}

type_init(hisi_vi_fp_register_types)
