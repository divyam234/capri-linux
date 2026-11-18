// SPDX-License-Identifier: GPL-2.0-only
/* Samsung S5KGM1ST, four-lane 24 MHz configuration. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/unaligned.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5KGM1ST_FIRMWARE "samsung/s5kgm1st.bin"

#define S5KGM1ST_WIDTH 4000
#define S5KGM1ST_HEIGHT 3000
#define S5KGM1ST_LINE_LENGTH 5024
#define S5KGM1ST_FRAME_LENGTH 3181
#define S5KGM1ST_PIXEL_RATE 482000000

struct s5kgm1st_mode {
	u32 width;
	u32 height;
	u32 line_length;
	u32 frame_length;
	struct v4l2_rect crop;
	const struct cci_reg_sequence *regs;
	u32 num_regs;
};

struct s5kgm1st {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	const struct s5kgm1st_mode *mode;
	struct device *dev;
	struct regmap *regmap;
	struct clk *clock;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[3];
};

static const s64 s5kgm1st_link_freqs[] = { 576000000 };
static const char * const s5kgm1st_supply_names[] = { "dvdd", "dovdd", "avdd" };

/* Register values decoded from this handset's Android sensor configuration. */
static const struct cci_reg_sequence s5kgm1st_4000x3000_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0344), 0x0008 },
	{ CCI_REG16(0x0346), 0x0008 },
	{ CCI_REG16(0x0348), 0x0fa7 },
	{ CCI_REG16(0x034a), 0x0bbf },
	{ CCI_REG16(0x034c), 0x0fa0 },
	{ CCI_REG16(0x034e), 0x0bb8 },
	{ CCI_REG16(0x0350), 0x0000 },
	{ CCI_REG16(0x0352), 0x0000 },
	{ CCI_REG16(0x0340), 0x0c6d },
	{ CCI_REG16(0x0342), 0x13a0 },
	{ CCI_REG16(0x0900), 0x0111 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0001 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0001 },
	{ CCI_REG16(0x0404), 0x1000 },
	{ CCI_REG16(0x0402), 0x1010 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x0304), 0x0006 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x0306), 0x00f1 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0300), 0x0008 },
	{ CCI_REG16(0x030e), 0x0003 },
	{ CCI_REG16(0x0312), 0x0001 },
	{ CCI_REG16(0x0310), 0x0090 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x1492 },
	{ CCI_REG16(0x6f12), 0x0078 },
	{ CCI_REG16(0x602a), 0x0e4e },
	{ CCI_REG16(0x6f12), 0x007a },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0118), 0x0004 },
	{ CCI_REG16(0x021e), 0x0000 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x2126 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x1168 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x2db6 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1668 },
	{ CCI_REG16(0x6f12), 0xf0f0 },
	{ CCI_REG16(0x602a), 0x166a },
	{ CCI_REG16(0x6f12), 0xf0f0 },
	{ CCI_REG16(0x602a), 0x118a },
	{ CCI_REG16(0x6f12), 0x0802 },
	{ CCI_REG16(0x602a), 0x151e },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x217e },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1520 },
	{ CCI_REG16(0x6f12), 0x0008 },
	{ CCI_REG16(0x602a), 0x2522 },
	{ CCI_REG16(0x6f12), 0x0804 },
	{ CCI_REG16(0x602a), 0x2524 },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x602a), 0x2568 },
	{ CCI_REG16(0x6f12), 0x5500 },
	{ CCI_REG16(0x602a), 0x2588 },
	{ CCI_REG16(0x6f12), 0x1111 },
	{ CCI_REG16(0x602a), 0x258c },
	{ CCI_REG16(0x6f12), 0x1111 },
	{ CCI_REG16(0x602a), 0x25a6 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x252c },
	{ CCI_REG16(0x6f12), 0x0601 },
	{ CCI_REG16(0x602a), 0x252e },
	{ CCI_REG16(0x6f12), 0x0605 },
	{ CCI_REG16(0x602a), 0x25a8 },
	{ CCI_REG16(0x6f12), 0x1100 },
	{ CCI_REG16(0x602a), 0x25ac },
	{ CCI_REG16(0x6f12), 0x0011 },
	{ CCI_REG16(0x602a), 0x25b0 },
	{ CCI_REG16(0x6f12), 0x1100 },
	{ CCI_REG16(0x602a), 0x25b4 },
	{ CCI_REG16(0x6f12), 0x0011 },
	{ CCI_REG16(0x602a), 0x15a4 },
	{ CCI_REG16(0x6f12), 0x0141 },
	{ CCI_REG16(0x602a), 0x15a6 },
	{ CCI_REG16(0x6f12), 0x0545 },
	{ CCI_REG16(0x602a), 0x15a8 },
	{ CCI_REG16(0x6f12), 0x0649 },
	{ CCI_REG16(0x602a), 0x15aa },
	{ CCI_REG16(0x6f12), 0x024d },
	{ CCI_REG16(0x602a), 0x15ac },
	{ CCI_REG16(0x6f12), 0x0151 },
	{ CCI_REG16(0x602a), 0x15ae },
	{ CCI_REG16(0x6f12), 0x0555 },
	{ CCI_REG16(0x602a), 0x15b0 },
	{ CCI_REG16(0x6f12), 0x0659 },
	{ CCI_REG16(0x602a), 0x15b2 },
	{ CCI_REG16(0x6f12), 0x025d },
	{ CCI_REG16(0x602a), 0x15b4 },
	{ CCI_REG16(0x6f12), 0x0161 },
	{ CCI_REG16(0x602a), 0x15b6 },
	{ CCI_REG16(0x6f12), 0x0565 },
	{ CCI_REG16(0x602a), 0x15b8 },
	{ CCI_REG16(0x6f12), 0x0669 },
	{ CCI_REG16(0x602a), 0x15ba },
	{ CCI_REG16(0x6f12), 0x026d },
	{ CCI_REG16(0x602a), 0x15bc },
	{ CCI_REG16(0x6f12), 0x0171 },
	{ CCI_REG16(0x602a), 0x15be },
	{ CCI_REG16(0x6f12), 0x0575 },
	{ CCI_REG16(0x602a), 0x15c0 },
	{ CCI_REG16(0x6f12), 0x0679 },
	{ CCI_REG16(0x602a), 0x15c2 },
	{ CCI_REG16(0x6f12), 0x027d },
	{ CCI_REG16(0x602a), 0x15c4 },
	{ CCI_REG16(0x6f12), 0x0141 },
	{ CCI_REG16(0x602a), 0x15c6 },
	{ CCI_REG16(0x6f12), 0x0545 },
	{ CCI_REG16(0x602a), 0x15c8 },
	{ CCI_REG16(0x6f12), 0x0649 },
	{ CCI_REG16(0x602a), 0x15ca },
	{ CCI_REG16(0x6f12), 0x024d },
	{ CCI_REG16(0x602a), 0x15cc },
	{ CCI_REG16(0x6f12), 0x0151 },
	{ CCI_REG16(0x602a), 0x15ce },
	{ CCI_REG16(0x6f12), 0x0555 },
	{ CCI_REG16(0x602a), 0x15d0 },
	{ CCI_REG16(0x6f12), 0x0659 },
	{ CCI_REG16(0x602a), 0x15d2 },
	{ CCI_REG16(0x6f12), 0x025d },
	{ CCI_REG16(0x602a), 0x15d4 },
	{ CCI_REG16(0x6f12), 0x0161 },
	{ CCI_REG16(0x602a), 0x15d6 },
	{ CCI_REG16(0x6f12), 0x0565 },
	{ CCI_REG16(0x602a), 0x15d8 },
	{ CCI_REG16(0x6f12), 0x0669 },
	{ CCI_REG16(0x602a), 0x15da },
	{ CCI_REG16(0x6f12), 0x026d },
	{ CCI_REG16(0x602a), 0x15dc },
	{ CCI_REG16(0x6f12), 0x0171 },
	{ CCI_REG16(0x602a), 0x15de },
	{ CCI_REG16(0x6f12), 0x0575 },
	{ CCI_REG16(0x602a), 0x15e0 },
	{ CCI_REG16(0x6f12), 0x0679 },
	{ CCI_REG16(0x602a), 0x15e2 },
	{ CCI_REG16(0x6f12), 0x027d },
	{ CCI_REG16(0x602a), 0x1a50 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1a54 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0d00), 0x0101 },
	{ CCI_REG16(0x0d02), 0x0101 },
	{ CCI_REG16(0x0114), 0x0301 },
	{ CCI_REG16(0xf486), 0x0000 },
	{ CCI_REG16(0xf488), 0x0000 },
	{ CCI_REG16(0xf48a), 0x0000 },
	{ CCI_REG16(0xf48c), 0x0000 },
	{ CCI_REG16(0xf48e), 0x0000 },
	{ CCI_REG16(0xf490), 0x0000 },
	{ CCI_REG16(0xf492), 0x0000 },
	{ CCI_REG16(0xf494), 0x0000 },
	{ CCI_REG16(0xf496), 0x0000 },
	{ CCI_REG16(0xf498), 0x0000 },
	{ CCI_REG16(0xf49a), 0x0000 },
	{ CCI_REG16(0xf49c), 0x0000 },
	{ CCI_REG16(0xf49e), 0x0000 },
	{ CCI_REG16(0xf4a0), 0x0000 },
	{ CCI_REG16(0xf4a2), 0x0000 },
	{ CCI_REG16(0xf4a4), 0x0000 },
	{ CCI_REG16(0xf4a6), 0x0000 },
	{ CCI_REG16(0xf4a8), 0x0000 },
	{ CCI_REG16(0xf4aa), 0x0000 },
	{ CCI_REG16(0xf4ac), 0x0000 },
	{ CCI_REG16(0xf4ae), 0x0000 },
	{ CCI_REG16(0xf4b0), 0x0000 },
	{ CCI_REG16(0xf4b2), 0x0000 },
	{ CCI_REG16(0xf4b4), 0x0000 },
	{ CCI_REG16(0xf4b6), 0x0000 },
	{ CCI_REG16(0xf4b8), 0x0000 },
	{ CCI_REG16(0xf4ba), 0x0000 },
	{ CCI_REG16(0xf4bc), 0x0000 },
	{ CCI_REG16(0xf4be), 0x0000 },
	{ CCI_REG16(0xf4c0), 0x0000 },
	{ CCI_REG16(0xf4c2), 0x0000 },
	{ CCI_REG16(0xf4c4), 0x0000 },
	{ CCI_REG16(0x0202), 0x0010 },
	{ CCI_REG16(0x0226), 0x0010 },
	{ CCI_REG16(0x0204), 0x0020 },
	{ CCI_REG16(0x0b06), 0x0101 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x107a },
	{ CCI_REG16(0x6f12), 0x1d00 },
	{ CCI_REG16(0x602a), 0x1074 },
	{ CCI_REG16(0x6f12), 0x1d00 },
	{ CCI_REG16(0x602a), 0x0e7c },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1120 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x1122 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x602a), 0x1128 },
	{ CCI_REG16(0x6f12), 0x0604 },
	{ CCI_REG16(0x602a), 0x1ac0 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x1ac2 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x602a), 0x1494 },
	{ CCI_REG16(0x6f12), 0x3d68 },
	{ CCI_REG16(0x602a), 0x1498 },
	{ CCI_REG16(0x6f12), 0xf10d },
	{ CCI_REG16(0x602a), 0x1488 },
	{ CCI_REG16(0x6f12), 0x0f04 },
	{ CCI_REG16(0x602a), 0x148a },
	{ CCI_REG16(0x6f12), 0x170b },
	{ CCI_REG16(0x602a), 0x150e },
	{ CCI_REG16(0x6f12), 0x40c2 },
	{ CCI_REG16(0x602a), 0x1510 },
	{ CCI_REG16(0x6f12), 0x80af },
	{ CCI_REG16(0x602a), 0x1512 },
	{ CCI_REG16(0x6f12), 0x00a0 },
	{ CCI_REG16(0x602a), 0x1486 },
	{ CCI_REG16(0x6f12), 0x1430 },
	{ CCI_REG16(0x602a), 0x1490 },
	{ CCI_REG16(0x6f12), 0x5009 },
	{ CCI_REG16(0x602a), 0x149e },
	{ CCI_REG16(0x6f12), 0x01c4 },
	{ CCI_REG16(0x602a), 0x11cc },
	{ CCI_REG16(0x6f12), 0x0008 },
	{ CCI_REG16(0x602a), 0x11ce },
	{ CCI_REG16(0x6f12), 0x000b },
	{ CCI_REG16(0x602a), 0x11d0 },
	{ CCI_REG16(0x6f12), 0x0006 },
	{ CCI_REG16(0x602a), 0x11da },
	{ CCI_REG16(0x6f12), 0x0012 },
	{ CCI_REG16(0x602a), 0x11e6 },
	{ CCI_REG16(0x6f12), 0x002a },
	{ CCI_REG16(0x602a), 0x125e },
	{ CCI_REG16(0x6f12), 0x0048 },
	{ CCI_REG16(0x602a), 0x11f4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x11f8 },
	{ CCI_REG16(0x6f12), 0x0016 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0xf444), 0x05bf },
	{ CCI_REG16(0xf44a), 0x0016 },
	{ CCI_REG16(0xf44c), 0x1414 },
	{ CCI_REG16(0xf44e), 0x0014 },
	{ CCI_REG16(0xf458), 0x0008 },
	{ CCI_REG16(0xf46e), 0xc040 },
	{ CCI_REG16(0xf470), 0x0008 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x1caa },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cac },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cae },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb2 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb6 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb8 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cba },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cbc },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cbe },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc2 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc6 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc8 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x6000 },
	{ CCI_REG16(0x6f12), 0x000f },
	{ CCI_REG16(0x602a), 0x6002 },
	{ CCI_REG16(0x6f12), 0xffff },
	{ CCI_REG16(0x602a), 0x6004 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x6006 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6008 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x600a },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x600c },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x600e },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6010 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6012 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6014 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6016 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6018 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x601a },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x601c },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x601e },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6020 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6022 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6024 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6026 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6028 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x602a },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x602c },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x1144 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x1146 },
	{ CCI_REG16(0x6f12), 0x1b00 },
	{ CCI_REG16(0x602a), 0x1080 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x1084 },
	{ CCI_REG16(0x6f12), 0x00c0 },
	{ CCI_REG16(0x602a), 0x108a },
	{ CCI_REG16(0x6f12), 0x00c0 },
	{ CCI_REG16(0x602a), 0x1090 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1092 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1094 },
	{ CCI_REG16(0x6f12), 0xa32e },
	{ CCI_REG16(0x602a), 0x602e },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x6038 },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x602a), 0x603a },
	{ CCI_REG16(0x6f12), 0x005f },
	{ CCI_REG16(0x602a), 0x603c },
	{ CCI_REG16(0x6f12), 0x0060 },
	{ CCI_REG16(0x602a), 0x603e },
	{ CCI_REG16(0x6f12), 0x0061 },
	{ CCI_REG16(0x602a), 0x25d4 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x25d6 },
	{ CCI_REG16(0x6f12), 0x0020 },
};

static const struct cci_reg_sequence s5kgm1st_1920x1080_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0344), 0x0058 },
	{ CCI_REG16(0x0346), 0x01ac },
	{ CCI_REG16(0x0348), 0x0f57 },
	{ CCI_REG16(0x034a), 0x0a1b },
	{ CCI_REG16(0x034c), 0x0780 },
	{ CCI_REG16(0x034e), 0x0438 },
	{ CCI_REG16(0x0350), 0x0000 },
	{ CCI_REG16(0x0352), 0x0000 },
	{ CCI_REG16(0x0340), 0x0c7d },
	{ CCI_REG16(0x0342), 0x09d0 },
	{ CCI_REG16(0x0900), 0x0121 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0003 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0001 },
	{ CCI_REG16(0x0404), 0x1000 },
	{ CCI_REG16(0x0402), 0x1020 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x0304), 0x0006 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x0306), 0x00f1 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0300), 0x0008 },
	{ CCI_REG16(0x030e), 0x0003 },
	{ CCI_REG16(0x0312), 0x0001 },
	{ CCI_REG16(0x0310), 0x0090 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x1492 },
	{ CCI_REG16(0x6f12), 0x0078 },
	{ CCI_REG16(0x602a), 0x0e4e },
	{ CCI_REG16(0x6f12), 0x0074 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0118), 0x0004 },
	{ CCI_REG16(0x021e), 0x0000 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x2126 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1168 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x2db6 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1668 },
	{ CCI_REG16(0x6f12), 0xff00 },
	{ CCI_REG16(0x602a), 0x166a },
	{ CCI_REG16(0x6f12), 0xff00 },
	{ CCI_REG16(0x602a), 0x118a },
	{ CCI_REG16(0x6f12), 0x0402 },
	{ CCI_REG16(0x602a), 0x151e },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x217e },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1520 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x2522 },
	{ CCI_REG16(0x6f12), 0x0804 },
	{ CCI_REG16(0x602a), 0x2524 },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x602a), 0x2568 },
	{ CCI_REG16(0x6f12), 0x5500 },
	{ CCI_REG16(0x602a), 0x2588 },
	{ CCI_REG16(0x6f12), 0x1111 },
	{ CCI_REG16(0x602a), 0x258c },
	{ CCI_REG16(0x6f12), 0x1111 },
	{ CCI_REG16(0x602a), 0x25a6 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x252c },
	{ CCI_REG16(0x6f12), 0x0601 },
	{ CCI_REG16(0x602a), 0x252e },
	{ CCI_REG16(0x6f12), 0x0605 },
	{ CCI_REG16(0x602a), 0x25a8 },
	{ CCI_REG16(0x6f12), 0x1100 },
	{ CCI_REG16(0x602a), 0x25ac },
	{ CCI_REG16(0x6f12), 0x0011 },
	{ CCI_REG16(0x602a), 0x25b0 },
	{ CCI_REG16(0x6f12), 0x1100 },
	{ CCI_REG16(0x602a), 0x25b4 },
	{ CCI_REG16(0x6f12), 0x0011 },
	{ CCI_REG16(0x602a), 0x15a4 },
	{ CCI_REG16(0x6f12), 0x0141 },
	{ CCI_REG16(0x602a), 0x15a6 },
	{ CCI_REG16(0x6f12), 0x0545 },
	{ CCI_REG16(0x602a), 0x15a8 },
	{ CCI_REG16(0x6f12), 0x0649 },
	{ CCI_REG16(0x602a), 0x15aa },
	{ CCI_REG16(0x6f12), 0x024d },
	{ CCI_REG16(0x602a), 0x15ac },
	{ CCI_REG16(0x6f12), 0x0151 },
	{ CCI_REG16(0x602a), 0x15ae },
	{ CCI_REG16(0x6f12), 0x0555 },
	{ CCI_REG16(0x602a), 0x15b0 },
	{ CCI_REG16(0x6f12), 0x0659 },
	{ CCI_REG16(0x602a), 0x15b2 },
	{ CCI_REG16(0x6f12), 0x025d },
	{ CCI_REG16(0x602a), 0x15b4 },
	{ CCI_REG16(0x6f12), 0x0161 },
	{ CCI_REG16(0x602a), 0x15b6 },
	{ CCI_REG16(0x6f12), 0x0565 },
	{ CCI_REG16(0x602a), 0x15b8 },
	{ CCI_REG16(0x6f12), 0x0669 },
	{ CCI_REG16(0x602a), 0x15ba },
	{ CCI_REG16(0x6f12), 0x026d },
	{ CCI_REG16(0x602a), 0x15bc },
	{ CCI_REG16(0x6f12), 0x0171 },
	{ CCI_REG16(0x602a), 0x15be },
	{ CCI_REG16(0x6f12), 0x0575 },
	{ CCI_REG16(0x602a), 0x15c0 },
	{ CCI_REG16(0x6f12), 0x0679 },
	{ CCI_REG16(0x602a), 0x15c2 },
	{ CCI_REG16(0x6f12), 0x027d },
	{ CCI_REG16(0x602a), 0x15c4 },
	{ CCI_REG16(0x6f12), 0x0141 },
	{ CCI_REG16(0x602a), 0x15c6 },
	{ CCI_REG16(0x6f12), 0x0545 },
	{ CCI_REG16(0x602a), 0x15c8 },
	{ CCI_REG16(0x6f12), 0x0649 },
	{ CCI_REG16(0x602a), 0x15ca },
	{ CCI_REG16(0x6f12), 0x024d },
	{ CCI_REG16(0x602a), 0x15cc },
	{ CCI_REG16(0x6f12), 0x0151 },
	{ CCI_REG16(0x602a), 0x15ce },
	{ CCI_REG16(0x6f12), 0x0555 },
	{ CCI_REG16(0x602a), 0x15d0 },
	{ CCI_REG16(0x6f12), 0x0659 },
	{ CCI_REG16(0x602a), 0x15d2 },
	{ CCI_REG16(0x6f12), 0x025d },
	{ CCI_REG16(0x602a), 0x15d4 },
	{ CCI_REG16(0x6f12), 0x0161 },
	{ CCI_REG16(0x602a), 0x15d6 },
	{ CCI_REG16(0x6f12), 0x0565 },
	{ CCI_REG16(0x602a), 0x15d8 },
	{ CCI_REG16(0x6f12), 0x0669 },
	{ CCI_REG16(0x602a), 0x15da },
	{ CCI_REG16(0x6f12), 0x026d },
	{ CCI_REG16(0x602a), 0x15dc },
	{ CCI_REG16(0x6f12), 0x0171 },
	{ CCI_REG16(0x602a), 0x15de },
	{ CCI_REG16(0x6f12), 0x0575 },
	{ CCI_REG16(0x602a), 0x15e0 },
	{ CCI_REG16(0x6f12), 0x0679 },
	{ CCI_REG16(0x602a), 0x15e2 },
	{ CCI_REG16(0x6f12), 0x027d },
	{ CCI_REG16(0x602a), 0x1a50 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1a54 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0d00), 0x0101 },
	{ CCI_REG16(0x0d02), 0x0101 },
	{ CCI_REG16(0x0114), 0x0301 },
	{ CCI_REG16(0xf486), 0x0641 },
	{ CCI_REG16(0xf488), 0x0a45 },
	{ CCI_REG16(0xf48a), 0x0a49 },
	{ CCI_REG16(0xf48c), 0x064d },
	{ CCI_REG16(0xf48e), 0x0651 },
	{ CCI_REG16(0xf490), 0x0a55 },
	{ CCI_REG16(0xf492), 0x0a59 },
	{ CCI_REG16(0xf494), 0x065d },
	{ CCI_REG16(0xf496), 0x0661 },
	{ CCI_REG16(0xf498), 0x0a65 },
	{ CCI_REG16(0xf49a), 0x0a69 },
	{ CCI_REG16(0xf49c), 0x066d },
	{ CCI_REG16(0xf49e), 0x0671 },
	{ CCI_REG16(0xf4a0), 0x0a75 },
	{ CCI_REG16(0xf4a2), 0x0a79 },
	{ CCI_REG16(0xf4a4), 0x067d },
	{ CCI_REG16(0xf4a6), 0x0641 },
	{ CCI_REG16(0xf4a8), 0x0a45 },
	{ CCI_REG16(0xf4aa), 0x0a49 },
	{ CCI_REG16(0xf4ac), 0x064d },
	{ CCI_REG16(0xf4ae), 0x0651 },
	{ CCI_REG16(0xf4b0), 0x0a55 },
	{ CCI_REG16(0xf4b2), 0x0a59 },
	{ CCI_REG16(0xf4b4), 0x065d },
	{ CCI_REG16(0xf4b6), 0x0661 },
	{ CCI_REG16(0xf4b8), 0x0a65 },
	{ CCI_REG16(0xf4ba), 0x0a69 },
	{ CCI_REG16(0xf4bc), 0x066d },
	{ CCI_REG16(0xf4be), 0x0671 },
	{ CCI_REG16(0xf4c0), 0x0a75 },
	{ CCI_REG16(0xf4c2), 0x0a79 },
	{ CCI_REG16(0xf4c4), 0x067d },
	{ CCI_REG16(0x0202), 0x0010 },
	{ CCI_REG16(0x0226), 0x0010 },
	{ CCI_REG16(0x0204), 0x0020 },
	{ CCI_REG16(0x0b06), 0x0101 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x107a },
	{ CCI_REG16(0x6f12), 0x1d00 },
	{ CCI_REG16(0x602a), 0x1074 },
	{ CCI_REG16(0x6f12), 0x1d00 },
	{ CCI_REG16(0x602a), 0x0e7c },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1120 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1122 },
	{ CCI_REG16(0x6f12), 0x0078 },
	{ CCI_REG16(0x602a), 0x1128 },
	{ CCI_REG16(0x6f12), 0x0601 },
	{ CCI_REG16(0x602a), 0x1ac0 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x1ac2 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x602a), 0x1494 },
	{ CCI_REG16(0x6f12), 0x3d68 },
	{ CCI_REG16(0x602a), 0x1498 },
	{ CCI_REG16(0x6f12), 0xf10d },
	{ CCI_REG16(0x602a), 0x1488 },
	{ CCI_REG16(0x6f12), 0x0f04 },
	{ CCI_REG16(0x602a), 0x148a },
	{ CCI_REG16(0x6f12), 0x170b },
	{ CCI_REG16(0x602a), 0x150e },
	{ CCI_REG16(0x6f12), 0x40c2 },
	{ CCI_REG16(0x602a), 0x1510 },
	{ CCI_REG16(0x6f12), 0x80af },
	{ CCI_REG16(0x602a), 0x1512 },
	{ CCI_REG16(0x6f12), 0x00a0 },
	{ CCI_REG16(0x602a), 0x1486 },
	{ CCI_REG16(0x6f12), 0x1430 },
	{ CCI_REG16(0x602a), 0x1490 },
	{ CCI_REG16(0x6f12), 0x5009 },
	{ CCI_REG16(0x602a), 0x149e },
	{ CCI_REG16(0x6f12), 0x01c4 },
	{ CCI_REG16(0x602a), 0x11cc },
	{ CCI_REG16(0x6f12), 0x0008 },
	{ CCI_REG16(0x602a), 0x11ce },
	{ CCI_REG16(0x6f12), 0x000b },
	{ CCI_REG16(0x602a), 0x11d0 },
	{ CCI_REG16(0x6f12), 0x0006 },
	{ CCI_REG16(0x602a), 0x11da },
	{ CCI_REG16(0x6f12), 0x0012 },
	{ CCI_REG16(0x602a), 0x11e6 },
	{ CCI_REG16(0x6f12), 0x002a },
	{ CCI_REG16(0x602a), 0x125e },
	{ CCI_REG16(0x6f12), 0x0048 },
	{ CCI_REG16(0x602a), 0x11f4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x11f8 },
	{ CCI_REG16(0x6f12), 0x0016 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0xf444), 0x05bf },
	{ CCI_REG16(0xf44a), 0x0016 },
	{ CCI_REG16(0xf44c), 0x1414 },
	{ CCI_REG16(0xf44e), 0x0014 },
	{ CCI_REG16(0xf458), 0x0008 },
	{ CCI_REG16(0xf46e), 0xd040 },
	{ CCI_REG16(0xf470), 0x0008 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x1caa },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cac },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cae },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb2 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb6 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cb8 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cba },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cbc },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cbe },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc2 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc6 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1cc8 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x6000 },
	{ CCI_REG16(0x6f12), 0x000f },
	{ CCI_REG16(0x602a), 0x6002 },
	{ CCI_REG16(0x6f12), 0xffff },
	{ CCI_REG16(0x602a), 0x6004 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x6006 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6008 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x600a },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x600c },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x600e },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6010 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6012 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6014 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6016 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6018 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x601a },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x601c },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x601e },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6020 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6022 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6024 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6026 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x6028 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x602a },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x602c },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x1144 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x1146 },
	{ CCI_REG16(0x6f12), 0x1b00 },
	{ CCI_REG16(0x602a), 0x1080 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x1084 },
	{ CCI_REG16(0x6f12), 0x00c0 },
	{ CCI_REG16(0x602a), 0x108a },
	{ CCI_REG16(0x6f12), 0x00c0 },
	{ CCI_REG16(0x602a), 0x1090 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x1092 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1094 },
	{ CCI_REG16(0x6f12), 0xa32e },
	{ CCI_REG16(0x602a), 0x602e },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x6038 },
	{ CCI_REG16(0x6f12), 0x0003 },
	{ CCI_REG16(0x602a), 0x603a },
	{ CCI_REG16(0x6f12), 0x005f },
	{ CCI_REG16(0x602a), 0x603c },
	{ CCI_REG16(0x6f12), 0x0060 },
	{ CCI_REG16(0x602a), 0x603e },
	{ CCI_REG16(0x6f12), 0x0061 },
	{ CCI_REG16(0x602a), 0x25d4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x25d6 },
	{ CCI_REG16(0x6f12), 0x0000 },
};

static const struct s5kgm1st_mode s5kgm1st_modes[] = {
	{
		.width = 1920, .height = 1080,
		.line_length = 2512, .frame_length = 6394,
		.crop = { .left = 88, .top = 428, .width = 3840, .height = 2160 },
		.regs = s5kgm1st_1920x1080_regs,
		.num_regs = ARRAY_SIZE(s5kgm1st_1920x1080_regs),
	}, {
		.width = S5KGM1ST_WIDTH, .height = S5KGM1ST_HEIGHT,
		.line_length = S5KGM1ST_LINE_LENGTH,
		.frame_length = S5KGM1ST_FRAME_LENGTH,
		.crop = { .left = 8, .top = 8,
			  .width = S5KGM1ST_WIDTH, .height = S5KGM1ST_HEIGHT },
		.regs = s5kgm1st_4000x3000_regs,
		.num_regs = ARRAY_SIZE(s5kgm1st_4000x3000_regs),
	},
};

static const char * const s5kgm1st_test_patterns[] = {
	"Disabled",
	"Color bars",
};

static struct s5kgm1st *to_s5kgm1st(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5kgm1st, sd);
}

static int s5kgm1st_power_on(struct device *dev)
{
	struct s5kgm1st *sensor = to_s5kgm1st(dev_get_drvdata(dev));
	int ret, i;

	ret = clk_prepare_enable(sensor->clock);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	/* Digital, I/O, then analogue power, with reset held asserted. */
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret)
			goto disable_supplies;
	}
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(sensor->reset, 0);
	usleep_range(4000, 5000);
	return 0;

disable_supplies:
	while (i-- > 0)
		regulator_disable(sensor->supplies[i].consumer);
	clk_disable_unprepare(sensor->clock);
	return ret;
}

static int s5kgm1st_power_off(struct device *dev)
{
	struct s5kgm1st *sensor = to_s5kgm1st(dev_get_drvdata(dev));
	int i;

	gpiod_set_value_cansleep(sensor->reset, 1);
	usleep_range(1000, 2000);
	for (i = ARRAY_SIZE(sensor->supplies) - 1; i >= 0; i--)
		regulator_disable(sensor->supplies[i].consumer);
	usleep_range(1000, 2000);
	clk_disable_unprepare(sensor->clock);
	return 0;
}

static int s5kgm1st_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5kgm1st *sensor = container_of(ctrl->handler, struct s5kgm1st, ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK)
		__v4l2_ctrl_modify_range(sensor->exposure, 2,
			sensor->mode->height + ctrl->val - 64, 1, 1000);
	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	ret = cci_write(sensor->regmap, CCI_REG16(0x6028), 0x4000, NULL);
	if (ret)
		goto put_pm;
	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(sensor->regmap, CCI_REG16(0x0600),
				ctrl->val ? 2 : 0, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(sensor->regmap, CCI_REG16(0x0202), ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(sensor->regmap, CCI_REG16(0x0204), ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(sensor->regmap, CCI_REG16(0x0340),
				sensor->mode->height + ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
	}
put_pm:
	pm_runtime_put(sensor->dev);
	return ret;
}

static const struct v4l2_ctrl_ops s5kgm1st_ctrl_ops = {
	.s_ctrl = s5kgm1st_set_ctrl,
};

static int s5kgm1st_init_controls(struct s5kgm1st *sensor)
{
	struct v4l2_ctrl_handler *h = &sensor->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		return ret;
	ret = v4l2_ctrl_handler_init(h, 9);
	if (ret)
		return ret;
	ctrl = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ,
				    0, 0, s5kgm1st_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE,
		S5KGM1ST_PIXEL_RATE, S5KGM1ST_PIXEL_RATE, 1, S5KGM1ST_PIXEL_RATE);
	sensor->hblank = v4l2_ctrl_new_std(h, NULL, V4L2_CID_HBLANK,
		S5KGM1ST_LINE_LENGTH - S5KGM1ST_WIDTH,
		S5KGM1ST_LINE_LENGTH - S5KGM1ST_WIDTH, 1,
		S5KGM1ST_LINE_LENGTH - S5KGM1ST_WIDTH);
	sensor->exposure = v4l2_ctrl_new_std(h, &s5kgm1st_ctrl_ops,
		V4L2_CID_EXPOSURE, 2, S5KGM1ST_FRAME_LENGTH - 64, 1, 1000);
	sensor->vblank = v4l2_ctrl_new_std(h, &s5kgm1st_ctrl_ops, V4L2_CID_VBLANK,
		S5KGM1ST_FRAME_LENGTH - S5KGM1ST_HEIGHT, 65535 - S5KGM1ST_HEIGHT,
		1, S5KGM1ST_FRAME_LENGTH - S5KGM1ST_HEIGHT);
	v4l2_ctrl_new_std(h, &s5kgm1st_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
		32, 512, 1, 32);
	v4l2_ctrl_new_std_menu_items(h, &s5kgm1st_ctrl_ops,
		V4L2_CID_TEST_PATTERN, ARRAY_SIZE(s5kgm1st_test_patterns) - 1,
		0, 0, s5kgm1st_test_patterns);
	v4l2_ctrl_new_fwnode_properties(h, &s5kgm1st_ctrl_ops, &props);
	if (h->error) {
		ret = h->error;
		v4l2_ctrl_handler_free(h);
		return ret;
	}
	sensor->sd.ctrl_handler = h;
	sensor->sd.state_lock = h->lock;
	return 0;
}

static int s5kgm1st_load_firmware(struct s5kgm1st *sensor,
				 const struct firmware *fw)
{
	unsigned int i;
	int ret = 0;

	/* Two firmware blocks, written through the sensor's indirect data port. */
	if (fw->size != (916 + 28) * 2)
		return -EINVAL;

	cci_write(sensor->regmap, CCI_REG16(0x6028), 0x4000, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x0000), 0x0000, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x0000), 0xf8d1, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x6010), 0x0001, &ret);
	if (ret)
		return ret;
	usleep_range(6000, 7000);
	cci_write(sensor->regmap, CCI_REG16(0x6214), 0x7971, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x6218), 0x7150, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x0a02), 0x0074, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x6028), 0x2000, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x602a), 0x3f5c, &ret);
	for (i = 0; i < 916; i++)
		cci_write(sensor->regmap, CCI_REG16(0x6f12),
			  get_unaligned_be16(fw->data + i * 2), &ret);
	cci_write(sensor->regmap, CCI_REG16(0x602a), 0x46a0, &ret);
	for (; i < 916 + 28; i++)
		cci_write(sensor->regmap, CCI_REG16(0x6f12),
			  get_unaligned_be16(fw->data + i * 2), &ret);
	return ret;
}

static int s5kgm1st_enable_streams(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *state, u32 pad, u64 streams_mask)
{
	struct s5kgm1st *sensor = to_s5kgm1st(sd);
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, S5KGM1ST_FIRMWARE, sensor->dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret)
		goto release_firmware;
	ret = s5kgm1st_load_firmware(sensor, fw);
	cci_multi_reg_write(sensor->regmap, sensor->mode->regs,
			    sensor->mode->num_regs, &ret);
	if (ret)
		goto power_off;
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	cci_write(sensor->regmap, CCI_REG16(0x6028), 0x4000, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x0100), 0x0100, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x0bcc), 0, &ret);
	if (!ret)
		goto release_firmware;
power_off:
	cci_write(sensor->regmap, CCI_REG16(0x6028), 0x4000, NULL);
	cci_write(sensor->regmap, CCI_REG8(0x0100), 0, NULL);
	pm_runtime_put_autosuspend(sensor->dev);
release_firmware:
	release_firmware(fw);
	return ret;
}

static int s5kgm1st_disable_streams(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *state, u32 pad, u64 streams_mask)
{
	struct s5kgm1st *sensor = to_s5kgm1st(sd);
	int ret;

	ret = cci_write(sensor->regmap, CCI_REG16(0x6028), 0x4000, NULL);
	cci_write(sensor->regmap, CCI_REG8(0x0100), 0, &ret);
	pm_runtime_put_autosuspend(sensor->dev);
	return ret;
}

static void s5kgm1st_fill_format(struct v4l2_mbus_framefmt *fmt,
			       const struct s5kgm1st_mode *mode)
{
	*fmt = (struct v4l2_mbus_framefmt) {
		.width = mode->width,
		.height = mode->height,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
		.xfer_func = V4L2_XFER_FUNC_NONE,
	};
}

static int s5kgm1st_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	s5kgm1st_fill_format(v4l2_subdev_state_get_format(state, 0), &s5kgm1st_modes[1]);
	return 0;
}

static int s5kgm1st_set_fmt(struct v4l2_subdev *sd,
			const struct v4l2_subdev_client_info *client,
			struct v4l2_subdev_state *state,
			struct v4l2_subdev_format *fmt)
{
	struct s5kgm1st *sensor = to_s5kgm1st(sd);
	const struct s5kgm1st_mode *mode;
	u32 hblank, vblank;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && v4l2_subdev_is_streaming(sd))
		return -EBUSY;
	mode = v4l2_find_nearest_size(s5kgm1st_modes, ARRAY_SIZE(s5kgm1st_modes),
				    width, height, fmt->format.width, fmt->format.height);
	s5kgm1st_fill_format(&fmt->format, mode);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		sensor->mode = mode;
		hblank = mode->line_length - mode->width;
		vblank = mode->frame_length - mode->height;
		__v4l2_ctrl_modify_range(sensor->hblank, hblank, hblank, 1, hblank);
		__v4l2_ctrl_modify_range(sensor->vblank, vblank,
					65535 - mode->height, 1, vblank);
		__v4l2_ctrl_s_ctrl(sensor->vblank, vblank);
	}
	return 0;
}

static int s5kgm1st_enum_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int s5kgm1st_enum_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_frame_size_enum *size)
{
	if (size->index >= ARRAY_SIZE(s5kgm1st_modes) ||
	    size->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;
	size->min_width = size->max_width = s5kgm1st_modes[size->index].width;
	size->min_height = size->max_height = s5kgm1st_modes[size->index].height;
	return 0;
}

static int s5kgm1st_get_selection(struct v4l2_subdev *sd,
		const struct v4l2_subdev_client_info *client,
		struct v4l2_subdev_state *state, struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect) {
			.width = S5KGM1ST_WIDTH + 16,
			.height = S5KGM1ST_HEIGHT + 16,
		};
		return 0;
	case V4L2_SEL_TGT_CROP:
		if (v4l2_subdev_state_get_format(state, 0)->width == 1920)
			sel->r = s5kgm1st_modes[0].crop;
		else
			sel->r = s5kgm1st_modes[1].crop;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/* Eight border pixels on each side, as in the mode's crop registers. */
		sel->r = (struct v4l2_rect) {
			.left = 8,
			.top = 8,
			.width = S5KGM1ST_WIDTH,
			.height = S5KGM1ST_HEIGHT,
		};
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_video_ops s5kgm1st_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};
static const struct v4l2_subdev_pad_ops s5kgm1st_pad_ops = {
	.enum_mbus_code = s5kgm1st_enum_code,
	.enum_frame_size = s5kgm1st_enum_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5kgm1st_set_fmt,
	.get_selection = s5kgm1st_get_selection,
	.enable_streams = s5kgm1st_enable_streams,
	.disable_streams = s5kgm1st_disable_streams,
};
static const struct v4l2_subdev_ops s5kgm1st_ops = {
	.video = &s5kgm1st_video_ops,
	.pad = &s5kgm1st_pad_ops,
};
static const struct v4l2_subdev_internal_ops s5kgm1st_internal_ops = {
	.init_state = s5kgm1st_init_state,
};
static const struct media_entity_operations s5kgm1st_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5kgm1st_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_endpoint bus = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	struct s5kgm1st *sensor;
	u64 id;
	int ret, i;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->dev = dev;
	sensor->mode = &s5kgm1st_modes[1];
	v4l2_i2c_subdev_init(&sensor->sd, client, &s5kgm1st_ops);
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return PTR_ERR(sensor->regmap);
	sensor->clock = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->clock))
		return dev_err_probe(dev, PTR_ERR(sensor->clock), "master clock\n");
	if (clk_get_rate(sensor->clock) != 24000000)
		return dev_err_probe(dev, -EINVAL, "24 MHz clock required\n");
	ep = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!ep)
		return -ENXIO;
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus);
	fwnode_handle_put(ep);
	if (ret)
		return ret;
	if (bus.bus.mipi_csi2.num_data_lanes != 4 || bus.nr_of_link_frequencies != 1 ||
	    bus.link_frequencies[0] != s5kgm1st_link_freqs[0])
		ret = -EINVAL;
	v4l2_fwnode_endpoint_free(&bus);
	if (ret)
		return dev_err_probe(dev, ret, "unsupported CSI-2 configuration\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset), "reset GPIO\n");
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++)
		sensor->supplies[i].supply = s5kgm1st_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies), sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "sensor supplies\n");
	ret = s5kgm1st_power_on(dev);
	if (ret)
		return ret;
	ret = cci_read(sensor->regmap, CCI_REG16(0x0000), &id, NULL);
	if (ret)
		goto power_off;
	if (id != 0xf8d1) {
		ret = dev_err_probe(dev, -ENODEV, "unexpected chip ID %#llx\n", id);
		goto power_off;
	}
	dev_info(dev, "S5KGM1ST identified, 4000x3000 RAW10\n");
	ret = s5kgm1st_init_controls(sensor);
	if (ret)
		goto power_off;
	sensor->sd.internal_ops = &s5kgm1st_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->sd.entity.ops = &s5kgm1st_entity_ops;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto free_ctrls;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto clean_entity;
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto disable_pm;
	pm_runtime_put_autosuspend(dev);
	return 0;

disable_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&sensor->sd);
clean_entity:
	media_entity_cleanup(&sensor->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
power_off:
	s5kgm1st_power_off(dev);
	return ret;
}

static void s5kgm1st_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5kgm1st *sensor = to_s5kgm1st(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev))
		s5kgm1st_power_off(sensor->dev);
	pm_runtime_set_suspended(sensor->dev);
}

static const struct dev_pm_ops s5kgm1st_pm_ops = {
	RUNTIME_PM_OPS(s5kgm1st_power_off, s5kgm1st_power_on, NULL)
};
static const struct of_device_id s5kgm1st_match[] = {
	{ .compatible = "samsung,s5kgm1st" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5kgm1st_match);

static struct i2c_driver s5kgm1st_driver = {
	.driver = {
		.name = "s5kgm1st",
		.of_match_table = s5kgm1st_match,
		.pm = pm_ptr(&s5kgm1st_pm_ops),
	},
	.probe = s5kgm1st_probe,
	.remove = s5kgm1st_remove,
};
module_i2c_driver(s5kgm1st_driver);
MODULE_DESCRIPTION("Samsung S5KGM1ST camera sensor");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(S5KGM1ST_FIRMWARE);
