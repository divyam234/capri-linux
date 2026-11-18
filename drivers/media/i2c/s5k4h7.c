// SPDX-License-Identifier: GPL-2.0-only
/* Samsung S5K4H7, four-lane 24 MHz configuration. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5K4H7_WIDTH 3264
#define S5K4H7_HEIGHT 2448
#define S5K4H7_LINE_LENGTH 3580
#define S5K4H7_FRAME_LENGTH 2530
#define S5K4H7_PIXEL_RATE 272000000

struct s5k4h7 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct device *dev;
	struct regmap *regmap;
	struct clk *clock;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[3];
};

static const s64 s5k4h7_link_freqs[] = { 362000000 };
static const char * const s5k4h7_supply_names[] = { "dovdd", "avdd", "dvdd" };

/* Register values decoded from this handset's Android sensor configuration. */
static const struct cci_reg_sequence s5k4h7_init_regs[] = {
	{ CCI_REG8(0x0100), 0x00 },
	{ CCI_REG8(0x0b05), 0x01 },
	{ CCI_REG8(0x3074), 0x06 },
	{ CCI_REG8(0x3075), 0x2f },
	{ CCI_REG8(0x308a), 0x20 },
	{ CCI_REG8(0x308b), 0x08 },
	{ CCI_REG8(0x308c), 0x0b },
	{ CCI_REG8(0x3081), 0x07 },
	{ CCI_REG8(0x307b), 0x85 },
	{ CCI_REG8(0x307a), 0x0a },
	{ CCI_REG8(0x3079), 0x0a },
	{ CCI_REG8(0x306e), 0x71 },
	{ CCI_REG8(0x306f), 0x28 },
	{ CCI_REG8(0x301f), 0x20 },
	{ CCI_REG8(0x306b), 0x9a },
	{ CCI_REG8(0x3091), 0x1f },
	{ CCI_REG8(0x30c4), 0x06 },
	{ CCI_REG8(0x3200), 0x09 },
	{ CCI_REG8(0x306a), 0x79 },
	{ CCI_REG8(0x30b0), 0xff },
	{ CCI_REG8(0x306d), 0x08 },
	{ CCI_REG8(0x3080), 0x00 },
	{ CCI_REG8(0x3929), 0x3f },
	{ CCI_REG8(0x3084), 0x16 },
	{ CCI_REG8(0x3070), 0x0f },
	{ CCI_REG8(0x3b45), 0x01 },
	{ CCI_REG8(0x30c2), 0x05 },
	{ CCI_REG8(0x3069), 0x87 },
	{ CCI_REG8(0x3924), 0x7f },
	{ CCI_REG8(0x3925), 0xfd },
	{ CCI_REG8(0x3c08), 0xff },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c31), 0xff },
	{ CCI_REG8(0x3c32), 0xff },
	{ CCI_REG8(0x300a), 0x52 },
	{ CCI_REG8(0x3012), 0x52 },
	{ CCI_REG8(0x3013), 0x36 },
	{ CCI_REG8(0x3019), 0x5f },
	{ CCI_REG8(0x301a), 0x57 },
	{ CCI_REG8(0x3024), 0x10 },
	{ CCI_REG8(0x3025), 0x4e },
	{ CCI_REG8(0x3026), 0x9a },
	{ CCI_REG8(0x302d), 0x0b },
	{ CCI_REG8(0x302e), 0x09 },
	{ CCI_REG8(0x3930), 0x80 },
	{ CCI_REG8(0x392f), 0x01 },
};

static const struct cci_reg_sequence s5k4h7_3264x2448_regs[] = {
	{ CCI_REG8(0x0100), 0x00 },
	{ CCI_REG8(0x0136), 0x18 },
	{ CCI_REG8(0x0137), 0x00 },
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x88 },
	{ CCI_REG8(0x030d), 0x06 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xb5 },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x3c17), 0x00 },
	{ CCI_REG8(0x3c1c), 0x05 },
	{ CCI_REG8(0x3c1d), 0x15 },
	{ CCI_REG8(0x0301), 0x04 },
	{ CCI_REG8(0x0820), 0x02 },
	{ CCI_REG8(0x0821), 0xdc },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x3906), 0x00 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0c },
	{ CCI_REG8(0x0349), 0xc7 },
	{ CCI_REG8(0x034a), 0x09 },
	{ CCI_REG8(0x034b), 0x97 },
	{ CCI_REG8(0x034c), 0x0c },
	{ CCI_REG8(0x034d), 0xc0 },
	{ CCI_REG8(0x034e), 0x09 },
	{ CCI_REG8(0x034f), 0x90 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x00 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x0340), 0x09 },
	{ CCI_REG8(0x0341), 0xe2 },
	{ CCI_REG8(0x0342), 0x0d },
	{ CCI_REG8(0x0343), 0xfc },
	{ CCI_REG8(0x0200), 0x0d },
	{ CCI_REG8(0x0201), 0x6c },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0x02 },
	{ CCI_REG8(0x3400), 0x01 },
};

static struct s5k4h7 *to_s5k4h7(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k4h7, sd);
}

static int s5k4h7_power_on(struct device *dev)
{
	struct s5k4h7 *sensor = to_s5k4h7(dev_get_drvdata(dev));
	int ret, i;

	ret = clk_prepare_enable(sensor->clock);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	/* I/O, analogue, then digital power, with reset held asserted. */
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

static int s5k4h7_power_off(struct device *dev)
{
	struct s5k4h7 *sensor = to_s5k4h7(dev_get_drvdata(dev));
	int i;

	clk_disable_unprepare(sensor->clock);
	gpiod_set_value_cansleep(sensor->reset, 1);
	for (i = ARRAY_SIZE(sensor->supplies) - 1; i >= 0; i--)
		regulator_disable(sensor->supplies[i].consumer);
	return 0;
}

static int s5k4h7_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k4h7 *sensor = container_of(ctrl->handler, struct s5k4h7, ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK)
		__v4l2_ctrl_modify_range(sensor->exposure, 2,
			S5K4H7_HEIGHT + ctrl->val - 8, 1, 1000);
	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(sensor->regmap, CCI_REG16(0x0202), ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(sensor->regmap, CCI_REG16(0x0204), ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(sensor->regmap, CCI_REG16(0x0340),
				S5K4H7_HEIGHT + ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
	}
	pm_runtime_put(sensor->dev);
	return ret;
}

static const struct v4l2_ctrl_ops s5k4h7_ctrl_ops = {
	.s_ctrl = s5k4h7_set_ctrl,
};

static int s5k4h7_init_controls(struct s5k4h7 *sensor)
{
	struct v4l2_ctrl_handler *h = &sensor->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		return ret;
	ret = v4l2_ctrl_handler_init(h, 8);
	if (ret)
		return ret;
	ctrl = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ,
				    0, 0, s5k4h7_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE,
		S5K4H7_PIXEL_RATE, S5K4H7_PIXEL_RATE, 1, S5K4H7_PIXEL_RATE);
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_HBLANK,
		S5K4H7_LINE_LENGTH - S5K4H7_WIDTH,
		S5K4H7_LINE_LENGTH - S5K4H7_WIDTH, 1,
		S5K4H7_LINE_LENGTH - S5K4H7_WIDTH);
	sensor->exposure = v4l2_ctrl_new_std(h, &s5k4h7_ctrl_ops,
		V4L2_CID_EXPOSURE, 2, S5K4H7_FRAME_LENGTH - 8, 1, 1000);
	v4l2_ctrl_new_std(h, &s5k4h7_ctrl_ops, V4L2_CID_VBLANK,
		S5K4H7_FRAME_LENGTH - S5K4H7_HEIGHT, 65535 - S5K4H7_HEIGHT,
		1, S5K4H7_FRAME_LENGTH - S5K4H7_HEIGHT);
	v4l2_ctrl_new_std(h, &s5k4h7_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
		32, 512, 1, 32);
	v4l2_ctrl_new_fwnode_properties(h, &s5k4h7_ctrl_ops, &props);
	if (h->error) {
		ret = h->error;
		v4l2_ctrl_handler_free(h);
		return ret;
	}
	sensor->sd.ctrl_handler = h;
	sensor->sd.state_lock = h->lock;
	return 0;
}

static int s5k4h7_enable_streams(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *state, u32 pad, u64 streams_mask)
{
	struct s5k4h7 *sensor = to_s5k4h7(sd);
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret)
		return ret;
	cci_multi_reg_write(sensor->regmap, s5k4h7_init_regs,
			    ARRAY_SIZE(s5k4h7_init_regs), &ret);
	cci_multi_reg_write(sensor->regmap, s5k4h7_3264x2448_regs,
			    ARRAY_SIZE(s5k4h7_3264x2448_regs), &ret);
	if (ret)
		goto power_off;
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	cci_write(sensor->regmap, CCI_REG8(0x0100), 1, &ret);
	cci_write(sensor->regmap, CCI_REG8(0x0106), 1, &ret);
	if (!ret)
		return 0;
power_off:
	cci_write(sensor->regmap, CCI_REG8(0x0100), 0, NULL);
	pm_runtime_put_autosuspend(sensor->dev);
	return ret;
}

static int s5k4h7_disable_streams(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *state, u32 pad, u64 streams_mask)
{
	struct s5k4h7 *sensor = to_s5k4h7(sd);
	int ret;

	ret = cci_write(sensor->regmap, CCI_REG8(0x0100), 0, NULL);
	pm_runtime_put_autosuspend(sensor->dev);
	return ret;
}

static void s5k4h7_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	*fmt = (struct v4l2_mbus_framefmt) {
		.width = S5K4H7_WIDTH,
		.height = S5K4H7_HEIGHT,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
		.xfer_func = V4L2_XFER_FUNC_NONE,
	};
}

static int s5k4h7_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	s5k4h7_fill_format(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static int s5k4h7_set_fmt(struct v4l2_subdev *sd,
			const struct v4l2_subdev_client_info *client,
			struct v4l2_subdev_state *state,
			struct v4l2_subdev_format *fmt)
{
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && v4l2_subdev_is_streaming(sd))
		return -EBUSY;
	s5k4h7_fill_format(&fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int s5k4h7_enum_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int s5k4h7_enum_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_frame_size_enum *size)
{
	if (size->index || size->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;
	size->min_width = size->max_width = S5K4H7_WIDTH;
	size->min_height = size->max_height = S5K4H7_HEIGHT;
	return 0;
}

static int s5k4h7_get_selection(struct v4l2_subdev *sd,
		const struct v4l2_subdev_client_info *client,
		struct v4l2_subdev_state *state, struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect) {
			.width = S5K4H7_WIDTH + 16,
			.height = S5K4H7_HEIGHT + 16,
		};
		return 0;
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/* Eight border pixels on each side, as in the mode's crop registers. */
		sel->r = (struct v4l2_rect) {
			.left = 8,
			.top = 8,
			.width = S5K4H7_WIDTH,
			.height = S5K4H7_HEIGHT,
		};
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_video_ops s5k4h7_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};
static const struct v4l2_subdev_pad_ops s5k4h7_pad_ops = {
	.enum_mbus_code = s5k4h7_enum_code,
	.enum_frame_size = s5k4h7_enum_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5k4h7_set_fmt,
	.get_selection = s5k4h7_get_selection,
	.enable_streams = s5k4h7_enable_streams,
	.disable_streams = s5k4h7_disable_streams,
};
static const struct v4l2_subdev_ops s5k4h7_ops = {
	.video = &s5k4h7_video_ops,
	.pad = &s5k4h7_pad_ops,
};
static const struct v4l2_subdev_internal_ops s5k4h7_internal_ops = {
	.init_state = s5k4h7_init_state,
};
static const struct media_entity_operations s5k4h7_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5k4h7_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_endpoint bus = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	struct s5k4h7 *sensor;
	u64 id;
	int ret, i;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->dev = dev;
	v4l2_i2c_subdev_init(&sensor->sd, client, &s5k4h7_ops);
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
	    bus.link_frequencies[0] != s5k4h7_link_freqs[0])
		ret = -EINVAL;
	v4l2_fwnode_endpoint_free(&bus);
	if (ret)
		return dev_err_probe(dev, ret, "unsupported CSI-2 configuration\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset), "reset GPIO\n");
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++)
		sensor->supplies[i].supply = s5k4h7_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies), sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "sensor supplies\n");
	ret = s5k4h7_power_on(dev);
	if (ret)
		return ret;
	ret = cci_read(sensor->regmap, CCI_REG16(0x0000), &id, NULL);
	if (ret)
		goto power_off;
	if (id != 0x487b) {
		ret = dev_err_probe(dev, -ENODEV, "unexpected chip ID %#llx\n", id);
		goto power_off;
	}
	dev_info(dev, "S5K4H7 identified, 3264x2448 RAW10\n");
	ret = s5k4h7_init_controls(sensor);
	if (ret)
		goto power_off;
	sensor->sd.internal_ops = &s5k4h7_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->sd.entity.ops = &s5k4h7_entity_ops;
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
	s5k4h7_power_off(dev);
	return ret;
}

static void s5k4h7_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k4h7 *sensor = to_s5k4h7(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev))
		s5k4h7_power_off(sensor->dev);
	pm_runtime_set_suspended(sensor->dev);
}

static const struct dev_pm_ops s5k4h7_pm_ops = {
	RUNTIME_PM_OPS(s5k4h7_power_off, s5k4h7_power_on, NULL)
};
static const struct of_device_id s5k4h7_match[] = {
	{ .compatible = "samsung,s5k4h7" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k4h7_match);

static struct i2c_driver s5k4h7_driver = {
	.driver = {
		.name = "s5k4h7",
		.of_match_table = s5k4h7_match,
		.pm = pm_ptr(&s5k4h7_pm_ops),
	},
	.probe = s5k4h7_probe,
	.remove = s5k4h7_remove,
};
module_i2c_driver(s5k4h7_driver);
MODULE_DESCRIPTION("Samsung S5K4H7 camera sensor");
MODULE_LICENSE("GPL");
