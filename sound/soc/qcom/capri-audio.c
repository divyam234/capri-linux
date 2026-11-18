// SPDX-License-Identifier: GPL-2.0-only
/* Capri mono primary MI2S speaker link. Q6 setup follows upstream sm8250.c. */
#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "common.h"
#include "sdw.h"
#include "qdsp6/q6afe.h"

struct capri_clock {
    struct mutex lock;
    unsigned int users;
    struct snd_soc_dai *owner;
    bool capture_prepared;
};

/* Backend channel selection takes effect on the next PCM open. */
static unsigned int be_channels = 1;
module_param(be_channels, uint, 0644);
MODULE_PARM_DESC(be_channels, "MI2S backend channels (1 or 2)");

static int capri_be_fixup(struct snd_soc_pcm_runtime *rtd, struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *format = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	rate->min = rate->max = 48000;
	if (be_channels != 1 && be_channels != 2)
		return -EINVAL;
	switch (snd_soc_rtd_to_cpu(rtd, 0)->id) {
	case PRIMARY_MI2S_TX:
		channels->min = channels->max = 2;
		break;
	case TX_CODEC_DMA_TX_3:
		channels->min = channels->max = 1;
		break;
	default:
		channels->min = channels->max = be_channels;
		break;
	}
	snd_mask_none(format);
	if (snd_soc_rtd_to_cpu(rtd, 0)->id == TX_CODEC_DMA_TX_3)
		snd_mask_set_format(format, SNDRV_PCM_FORMAT_S24_LE);
	else
		snd_mask_set_format(format, SNDRV_PCM_FORMAT_S16_LE);
	return 0;
}

static int capri_startup(struct snd_pcm_substream *substream)
{
    struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
    struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);
    struct capri_clock *clock = snd_soc_card_get_drvdata(rtd->card);
    int ret;

    if (cpu->id != PRIMARY_MI2S_RX && cpu->id != PRIMARY_MI2S_TX)
        return -EINVAL;
    ret = snd_soc_dai_set_fmt(cpu, SND_SOC_DAIFMT_BP_FP);
    ret = ret ?: snd_soc_dai_set_fmt(snd_soc_rtd_to_codec(rtd, 0),
        SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_BC_FC);
    if (ret)
        return ret;
    guard(mutex)(&clock->lock);
    if (!clock->users) {
        ret = snd_soc_dai_set_sysclk(cpu, Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
                                    1536000, SNDRV_PCM_STREAM_PLAYBACK);
        if (ret)
            return ret;
        clock->owner = cpu;
    }
    clock->users++;
    return 0;
}

static void capri_shutdown(struct snd_pcm_substream *substream)
{
    struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
    struct capri_clock *clock = snd_soc_card_get_drvdata(rtd->card);

    guard(mutex)(&clock->lock);
    if (WARN_ON(!clock->users))
        return;
    if (!--clock->users) {
        snd_soc_dai_set_sysclk(clock->owner, Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
                              0, SNDRV_PCM_STREAM_PLAYBACK);
        clock->owner = NULL;
    }
}
static const struct snd_soc_ops capri_be_ops = { .startup = capri_startup, .shutdown = capri_shutdown };

static int capri_sdw_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct capri_clock *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->capture_prepared);
}

static int capri_sdw_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct capri_clock *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_hw_free(substream, &data->capture_prepared);
}

static const struct snd_soc_ops capri_sdw_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.prepare = capri_sdw_prepare,
	.hw_free = capri_sdw_hw_free,
};

static int capri_audio_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct capri_clock *clock;
	struct snd_soc_dai_link *link;
	int ret, i;
	card = devm_kzalloc(&pdev->dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	clock = devm_kzalloc(&pdev->dev, sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return -ENOMEM;
	mutex_init(&clock->lock);
	snd_soc_card_set_drvdata(card, clock);
	card->owner = THIS_MODULE;
	card->dev = &pdev->dev;
	card->driver_name = "capri-mainline";
	dev_set_drvdata(&pdev->dev, card);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;
	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm) {
			if (link->id == TX_CODEC_DMA_TX_3)
				link->ops = &capri_sdw_ops;
			else
				link->ops = &capri_be_ops;
			link->be_hw_params_fixup = capri_be_fixup;
		}
	}
	return devm_snd_soc_register_card(&pdev->dev, card);
}
static const struct of_device_id capri_audio_of[] = { { .compatible = "motorola,capri-sndcard" }, {} };
MODULE_DEVICE_TABLE(of, capri_audio_of);
static struct platform_driver capri_audio_driver = {
	.probe = capri_audio_probe,
	.driver = { .name = "capri-audio", .of_match_table = capri_audio_of },
};
module_platform_driver(capri_audio_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Capri Q6 primary MI2S speaker card");
