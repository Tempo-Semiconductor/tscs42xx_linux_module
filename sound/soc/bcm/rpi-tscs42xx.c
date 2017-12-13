/*
 * ASoC Driver for TSCS42xx  
 *
 * Author:	Steven W. Eckhoff <steven.w.eckhoff.kernel@gmail.com>	
 * 		based on code by Daniel Matuschek, 
 * 		Stuart MacLean <stuart@hifiberry.com>,
 * 		Florian Meier <florian.meier@koalo.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include "../codecs/tscs42xx.h"

#define DEBUG_TSCS
#ifdef DEBUG_TSCS
#define tempo_debug(str, ...) printk(KERN_DEBUG "%s(): " str "\n", __func__, ##__VA_ARGS__)
#else
#define tempo_debug(str, ...) 
#endif

struct tscs_priv {
	int gpio_hp;
	int gpio_hp_active_low;
	struct snd_kcontrol *headphone_kctl;
	int gpio_mic;
	int gpio_mic_active_low;
	struct snd_kcontrol *analog_mic_kctl;
	//struct snd_kcontrol *line_in_kctl;
	struct snd_soc_codec *codec;
	int pll_src_id;
	unsigned int pll_src_freq;
};

static struct snd_soc_jack hp_jack;
static struct snd_soc_jack_pin hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
		.invert = false,
	},
};
static struct snd_soc_jack_gpio hp_jack_gpio = {
	.name = "Headphone Detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
	.invert = 0,
};

static struct snd_soc_jack mic_jack;
static struct snd_soc_jack_pin mic_jack_pins[] = {
	{
		.pin = "Analog Mic",
		.mask = SND_JACK_MICROPHONE,
		.invert = false,
	},
};
static struct snd_soc_jack_gpio mic_jack_gpio = {
	.name = "Mic Detect",
	.report = SND_JACK_MICROPHONE,
	.debounce_time = 150,
	.invert = 0,
};

static int hp_jack_status_check(void *data)
{
	int ret, hp_status;
	struct snd_soc_card *card = (struct snd_soc_card *)data;
	struct tscs_priv *priv = (struct tscs_priv *)snd_soc_card_get_drvdata(card);
	struct snd_soc_dapm_context *dapm = &card->dapm;
	
	hp_status = gpio_get_value(priv->gpio_hp) ? 1 : 0;
	hp_status = (hp_status << 1) | priv->gpio_hp_active_low;
	switch (hp_status) {
	case 0:
		ret = 0;
		break;
	case 1: /* Enable */
		ret = hp_jack_gpio.report;
		break;
	case 2: /* Enable */
		ret = hp_jack_gpio.report;
		break;
	case 3:
		ret = 0;	
		break;
	}

	if (ret == hp_jack_gpio.report) {
		if (snd_soc_dapm_disable_pin(dapm, 
				"Speaker") < 0)
			dev_warn(card->dev, 
					"Failed to disable Speaker");
	} else {
		if (snd_soc_dapm_enable_pin(dapm, 
				"Speaker") < 0)
			dev_warn(card->dev, 
					"Failed to enable Speaker");
	}

	return ret;
}

static int mic_jack_status_check(void *data)
{
	int ret, mic_status;
	struct snd_soc_card *card = (struct snd_soc_card *)data;
	struct tscs_priv *priv = (struct tscs_priv *)snd_soc_card_get_drvdata(card);

	mic_status = gpio_get_value(priv->gpio_mic) ? 1 : 0;
	mic_status = (mic_status << 1) | priv->gpio_mic_active_low;
	switch (mic_status) {
	case 0:
		ret = 0;
		break;
	case 1: /* Enable */
		ret = mic_jack_gpio.report;
		break;
	case 2: /* Enable */
		ret = mic_jack_gpio.report;
		break;
	case 3:
		ret = 0;	
		break;
	}

	if (ret == mic_jack_gpio.report) {
		/* Analog Mic */
		if (snd_soc_update_bits(priv->codec, R_INSELL, 
				RM_INSELL, RV_INSELL_IN1) < 0)
			dev_err(priv->codec->dev, "Failed to select analog mic");
		if (snd_soc_update_bits(priv->codec, R_INSELR, 
				RM_INSELR, RV_INSELR_IN1) < 0)
			dev_err(priv->codec->dev, "Failed to select analog mic");
	} else {
		/* Digital Mic */
		if (snd_soc_update_bits(priv->codec, R_INSELL, 
				RM_INSELL, RV_INSELL_IN3) < 0)
			dev_err(priv->codec->dev, "Failed to select digital mic");
		if (snd_soc_update_bits(priv->codec, R_INSELR, 
				RM_INSELR, RV_INSELR_IN3) < 0)
			dev_err(priv->codec->dev, "Failed to select digital mic");
	}

	return ret;
}
	
/* codec/machine specific init */
static int snd_rpi_tscs42xx_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;
	struct tscs_priv *tscs42xx = snd_soc_card_get_drvdata(rtd->card);

	tempo_debug("");

	ret = snd_soc_update_bits(rtd->codec, R_AIC2, RM_AIC2_BLRCM,
		RV_AIC2_BLRCM_DAC_BCLK_LRCLK_SHARED);
	if (ret < 0) {
		dev_err(rtd->codec->dev,
			"Failed to setup audio interface (%d)\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_bclk_ratio(rtd->codec_dai, 64);
	if (ret < 0) {
		dev_err(rtd->codec->dev, "Failed to set codec bclk ratio");
		return ret;
	}

	ret = snd_soc_dai_set_bclk_ratio(rtd->cpu_dai, 64);
	if (ret < 0) {
		dev_err(rtd->codec->dev, "Failed to set the cpu dai bclk ratio");
		return ret;
	}

	dev_info(rtd->codec->dev, "Setting sysclk\n");
	ret = snd_soc_dai_set_sysclk(rtd->codec_dai, tscs42xx->pll_src_id,
		tscs42xx->pll_src_freq, 0);
	if (ret < 0) {
		dev_err(rtd->codec->dev, "Failed to set sysclk (%d)\n", ret);
		return ret;
	}

	return 0;
}

static struct snd_soc_dai_link snd_rpi_tscs42xx_dai[1] = {
	{
		.name		= "RPi TSCS42XX",
		.stream_name	= "RPi TSCS42XX HiFi",
		.cpu_dai_name	= "bcm2708-i2s.0",
		.codec_dai_name	= "tscs42xx-HiFi",
		.platform_name	= "bcm2708-i2s.0",
		.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBM_CFM,
		.init		= snd_rpi_tscs42xx_init,
	},
};

/* Widgets */
static const struct snd_soc_dapm_widget rpi_tscs42xx_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Analog Mic", NULL),
	SND_SOC_DAPM_MIC("Digital Mic", NULL),
	SND_SOC_DAPM_LINE("Line In", NULL),
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_tscs42xx = {
	.name         = "snd_rpi_tscs42xx",
	.dai_link     = snd_rpi_tscs42xx_dai,
	.num_links    = ARRAY_SIZE(snd_rpi_tscs42xx_dai),

	.dapm_widgets = rpi_tscs42xx_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(rpi_tscs42xx_dapm_widgets),
        .fully_routed = true,
};


static int snd_rpi_tscs42xx_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct tscs_priv *data;
	struct device_node *i2s_node;
	struct snd_soc_dai_link *dai;
	struct device_node *codec_of_node;
	struct snd_soc_pcm_runtime *rtd;
	char const *mclk_src = NULL;

	tempo_debug("");

	if (NULL == pdev->dev.of_node)
		return -ENODEV;

	data = (struct tscs_priv *) devm_kzalloc(&pdev->dev, 
			sizeof(*data), GFP_KERNEL);
	if (NULL == data)
		return -ENOMEM;

	snd_soc_card_set_drvdata(&snd_rpi_tscs42xx, data);

	snd_rpi_tscs42xx.dev = &pdev->dev;

	dai = &snd_rpi_tscs42xx_dai[0];
	i2s_node = of_parse_phandle(pdev->dev.of_node,
			"i2s-controller", 0);

	if (i2s_node) {
		dai->cpu_dai_name = NULL;
		dai->cpu_of_node = i2s_node;
		dai->platform_name = NULL;
		dai->platform_of_node = i2s_node;
	}

	codec_of_node = of_parse_phandle(pdev->dev.of_node,
			"audio-codec", 0);
	if (codec_of_node)
		dai->codec_of_node = codec_of_node;
	else
		dev_err(&pdev->dev,
				"Failed to get codec_of_node");

	/* Get Clocking Info */

	ret = of_property_read_string(pdev->dev.of_node, "mclk-src", &mclk_src);
	if (ret) {
		dev_err(&pdev->dev, "mclk-src is needed (%d)\n", ret);
		return ret;
	}

	if (!strncmp(mclk_src, "mclk", 4)) {
		data->pll_src_id = TSCS42XX_PLL_SRC_MCLK2;
	} else if (!strncmp(mclk_src, "xtal", 4)) {
		data->pll_src_id = TSCS42XX_PLL_SRC_XTAL;
	} else {
		dev_err(&pdev->dev, "mclk-src %s is unsupported\n", mclk_src);
		return -EINVAL;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "mclk-src-freq",
			&data->pll_src_freq);
	if (ret) {
		dev_err(&pdev->dev, "mclk-src-freq not provided (%d)\n", ret);
		return ret;
	}

	/* End Clocking Info */
	
	ret = snd_soc_of_parse_audio_routing(&snd_rpi_tscs42xx, "audio-routing");
	if (ret) {
		dev_err(&pdev->dev, 
			"Failed to parse audio routing from device tree");
		return -EINVAL;
	}

	ret = snd_soc_register_card(&snd_rpi_tscs42xx);
	if (ret) {
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);
		return -EPROBE_DEFER;
	}
	rtd = (struct snd_soc_pcm_runtime *) 
		list_first_entry_or_null(&snd_rpi_tscs42xx.rtd_list,
						struct snd_soc_card,
						rtd_list);
	if (NULL == rtd) {
		dev_err(&pdev->dev, "Failed to get runtime device");
		return -EPROBE_DEFER;
	}
	data->codec = rtd->codec;

	/* Headphone Jack */
	data->gpio_hp = of_get_named_gpio_flags(pdev->dev.of_node,
			"hp-gpios", 0, 
			(enum of_gpio_flags *)&data->gpio_hp_active_low);	

	if (!gpio_is_valid(data->gpio_hp)) {
		dev_info(&pdev->dev, "hp-gpios not found in dt. "
			"Defaulting to Headphone. "
			"See device tree binding for more info.");

		/* Now we should check for digital mic or pick a default config */
	} else {
		tempo_debug("hp gpio active low = %d", data->gpio_hp_active_low);

		ret = snd_soc_card_jack_new(&snd_rpi_tscs42xx, "Headphone Jack",
				SND_JACK_HEADPHONE, &hp_jack,
				hp_jack_pins, 
				ARRAY_SIZE(hp_jack_pins));	
		if (ret < 0) {
			dev_err(&pdev->dev, 
					"Failed to create Headphone Jack (%d)", 
					ret);
			return ret;
		}
		hp_jack_gpio.gpio = data->gpio_hp;
		hp_jack_gpio.data = &snd_rpi_tscs42xx;
		hp_jack_gpio.jack_status_check = hp_jack_status_check;
		ret = snd_soc_jack_add_gpios(&hp_jack, 1, &hp_jack_gpio);
		if (ret < 0) {
			dev_err(&pdev->dev, 
					"Failed set GPIOs for Headphone Jack (%d)", 
					ret);
			return ret;
		}
	}

	/* Mic Jack */
	data->gpio_mic = of_get_named_gpio_flags(pdev->dev.of_node,
			"mic-gpios", 0, 
			(enum of_gpio_flags *)&data->gpio_mic_active_low);	

	if (!gpio_is_valid(data->gpio_mic)) {
		dev_info(&pdev->dev, "mic-gpios not found in dt. "
			"Defaulting to Analog Mic. "
			"See device tree binding for more info.");
	} else {
		tempo_debug("mic gpio active low = %d", data->gpio_mic_active_low);

		ret = snd_soc_card_jack_new(&snd_rpi_tscs42xx, "Mic Jack",
				SND_JACK_HEADPHONE, &mic_jack,
				mic_jack_pins, 
				ARRAY_SIZE(mic_jack_pins));	
		if (ret < 0) {
			dev_err(&pdev->dev, 
					"Failed to create Mic Jack (%d)", 
					ret);
			return ret;
		}
		mic_jack_gpio.gpio = data->gpio_mic;
		mic_jack_gpio.data = &snd_rpi_tscs42xx;
		mic_jack_gpio.jack_status_check = mic_jack_status_check;
		ret = snd_soc_jack_add_gpios(&mic_jack, 1, &mic_jack_gpio);
		if (ret < 0) {
			dev_err(&pdev->dev, 
					"Failed set GPIOs for Mic Jack (%d)", 
					ret);
			return ret;
		}
	}

	return ret;
}

static int snd_rpi_tscs42xx_remove(struct platform_device *pdev)
{
	tempo_debug("");

	return snd_soc_unregister_card(&snd_rpi_tscs42xx);
}

static const struct of_device_id snd_rpi_tscs42xx_of_match[] = {
	{ .compatible = "tempo,rpi-wookie", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_rpi_tscs42xx_of_match);

static struct platform_driver snd_rpi_tscs42xx_driver = {
	.driver = {
		.name   = "snd-rpi-wookie",
		.owner  = THIS_MODULE,
		.of_match_table = snd_rpi_tscs42xx_of_match,
	},
	.probe          = snd_rpi_tscs42xx_probe,
	.remove         = snd_rpi_tscs42xx_remove,
};

module_platform_driver(snd_rpi_tscs42xx_driver);

MODULE_AUTHOR("Tempo Semiconductor: Steven W. Eckhoff <steven.eckhoff.opensource@gmail.com>");
MODULE_DESCRIPTION("ASoC Driver for TSCS42xx");
MODULE_LICENSE("GPL v2");
