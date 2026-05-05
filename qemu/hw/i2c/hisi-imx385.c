/*
 * Sony IMX385 image sensor I2C stub.
 *
 * Sony IMX385 is the 1080p Starvis sensor on Hi3516AV200/3519V101-class
 * boards.  ipctool / vendor SDK detect IMX385 by reading a cluster of
 * registers in the 0x3000-0x33FF window:
 *   1. I2C addr 0x34 (8-bit) = 0x1A (7-bit slave addr)
 *   2. 0x3004 == 0x10
 *   3. 0x300C == 0x00 && 0x300E == 0x01
 *   4. 0x300D == 0x00 && 0x3010 == 0x01 && 0x3011 == 0x00 &&
 *      0x301E == 0x01 && 0x301F == 0x00
 *   5. 0x3338 != 0x00   (else IMX225 is reported)
 *
 * The reset stub answers exactly that pattern; all other registers
 * read back as 0xFF (uninitialized) so earlier branches in the probe
 * (IMX347, IMX334, IMX335, IMX415, IMX291, IMX138 etc.) all fall
 * through.
 *
 * Reference: ipctool/src/sensors.c detect_sony_sensor() lines 294-314
 *
 * Copyright (c) 2026 OpenIPC.
 * Written by Dmitry Ilyin
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TYPE_HISI_IMX385 "hisi-imx385"
OBJECT_DECLARE_SIMPLE_TYPE(HisiIMX385State, HISI_IMX385)

#define IMX385_REG_BASE  0x3000
#define IMX385_REG_COUNT 0x1000

struct HisiIMX385State {
    I2CSlave parent_obj;
    uint8_t  reg_buf[2];
    uint8_t  reg_ptr;
    uint16_t cur_reg;
    uint8_t  regs[IMX385_REG_COUNT];
};

static void hisi_imx385_reset_regs(HisiIMX385State *s)
{
    memset(s->regs, 0xFF, sizeof(s->regs));

    /* IMX385 detection cluster */
    s->regs[0x3004 - IMX385_REG_BASE] = 0x10;
    s->regs[0x300C - IMX385_REG_BASE] = 0x00;
    s->regs[0x300D - IMX385_REG_BASE] = 0x00;
    s->regs[0x300E - IMX385_REG_BASE] = 0x01;
    s->regs[0x3010 - IMX385_REG_BASE] = 0x01;
    s->regs[0x3011 - IMX385_REG_BASE] = 0x00;
    s->regs[0x301E - IMX385_REG_BASE] = 0x01;
    s->regs[0x301F - IMX385_REG_BASE] = 0x00;
    /* 0x3338 != 0 distinguishes IMX385 from IMX225 */
    s->regs[0x3338 - IMX385_REG_BASE] = 0x01;

    /* Avoid earlier-branch false matches:
     *   IMX138 cluster also keys off READ(0xC)==0 && READ(0xE)==1, but
     *   additionally requires READ(0xD)==0x20 — we hold 0x300D at 0,
     *   so the IMX138 / IMX123 branches fall through. */
}

static int hisi_imx385_event(I2CSlave *i2c, enum i2c_event event)
{
    HisiIMX385State *s = HISI_IMX385(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->reg_ptr = 0;
        break;
    case I2C_START_RECV:
        s->cur_reg = ((uint16_t)s->reg_buf[0] << 8) | s->reg_buf[1];
        break;
    default:
        break;
    }
    return 0;
}

static int hisi_imx385_send(I2CSlave *i2c, uint8_t data)
{
    HisiIMX385State *s = HISI_IMX385(i2c);

    if (s->reg_ptr < 2) {
        s->reg_buf[s->reg_ptr++] = data;
    } else {
        uint16_t addr = ((uint16_t)s->reg_buf[0] << 8) | s->reg_buf[1];
        if (addr >= IMX385_REG_BASE &&
            addr < IMX385_REG_BASE + IMX385_REG_COUNT) {
            s->regs[addr - IMX385_REG_BASE] = data;
        }
        s->reg_buf[1]++;
        if (s->reg_buf[1] == 0) {
            s->reg_buf[0]++;
        }
    }
    return 0;
}

static uint8_t hisi_imx385_recv(I2CSlave *i2c)
{
    HisiIMX385State *s = HISI_IMX385(i2c);
    uint8_t val = 0xFF;

    if (s->cur_reg >= IMX385_REG_BASE &&
        s->cur_reg < IMX385_REG_BASE + IMX385_REG_COUNT) {
        val = s->regs[s->cur_reg - IMX385_REG_BASE];
    }
    s->cur_reg++;
    return val;
}

static void hisi_imx385_reset(DeviceState *dev)
{
    HisiIMX385State *s = HISI_IMX385(dev);

    s->reg_ptr = 0;
    s->cur_reg = 0;
    memset(s->reg_buf, 0, sizeof(s->reg_buf));
    hisi_imx385_reset_regs(s);
}

static void hisi_imx385_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    device_class_set_legacy_reset(dc, hisi_imx385_reset);
    k->event = hisi_imx385_event;
    k->recv  = hisi_imx385_recv;
    k->send  = hisi_imx385_send;
}

static const TypeInfo hisi_imx385_info = {
    .name          = TYPE_HISI_IMX385,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(HisiIMX385State),
    .class_init    = hisi_imx385_class_init,
};

static void hisi_imx385_register_types(void)
{
    type_register_static(&hisi_imx385_info);
}

type_init(hisi_imx385_register_types)
