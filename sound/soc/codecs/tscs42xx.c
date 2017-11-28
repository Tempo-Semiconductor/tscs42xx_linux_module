/*
 * tscs42xx.c -- TSCS42xx ALSA SoC Audio driver
 *
 * Copyright 2017 Tempo Semiconductor, Inc.
 *
 * Author: Steven Eckhoff <steven.eckhoff.opensource@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/of_device.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <linux/firmware.h>
#include <linux/sysfs.h>

#include "tscs42xx.h"

enum {
	PLL_SRC_CLK_XTAL,
	PLL_SRC_CLK_MCLK2,
};

/*
 * Functions that receive a pointer to data
 * should assume that the data is locked.
 */
struct tscs42xx_priv {
	struct regmap *regmap;
	struct device *dev;
	struct clk *mclk;
	int mclk_src_freq;
	int pll_src_clk;
	int bclk_ratio;
	int samplerate;
	int pll_users;
	struct snd_soc_codec *codec;
	struct mutex data_lock;
};

static bool tscs42xx_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case R_DACCRWRL:
	case R_DACCRWRM:
	case R_DACCRWRH:
	case R_DACCRRDL:
	case R_DACCRRDM:
	case R_DACCRRDH:
	case R_DACCRSTAT:
	case R_DACCRADDR:
	case R_PLLCTL0:
		return true;
	default:
		return false;
	};
}

static bool tscs42xx_precious(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case R_DACCRWRL:
	case R_DACCRWRM:
	case R_DACCRWRH:
	case R_DACCRRDL:
	case R_DACCRRDM:
	case R_DACCRRDH:
		return true;
	default:
		return false;
	};
}

static const struct regmap_config tscs42xx_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.volatile_reg = tscs42xx_volatile,
	.precious_reg = tscs42xx_precious,
	.max_register = R_DACMBCREL3H,

	.cache_type = REGCACHE_RBTREE,
	.can_multi_write = true,
};

static struct reg_default r_inits[] = {
	{ .reg = R_ADCSR,   .def = RV_ADCSR_ABCM_64 },
	{ .reg = R_DACSR,   .def = RV_DACSR_DBCM_64 },
	{ .reg = R_AIC2,    .def = RV_AIC2_BLRCM_DAC_BCLK_LRCLK_SHARED },
};

#define COEFF_SIZE 3
#define COEFF_MAX_ADDR 0xCD
#define COEFF_COUNT (COEFF_MAX_ADDR + 1)
#define COEFF_RAM_SIZE (COEFF_COUNT * COEFF_SIZE)
#define COEFF_RAM_TLV_SIZE (COEFF_RAM_SIZE + 2 * sizeof(unsigned int))
static int load_dac_coefficient_ram(struct snd_soc_codec *codec)
{
	struct tscs42xx_priv *tscs42xx = snd_soc_codec_get_drvdata(codec);
	const struct firmware *fw = NULL;
	int ret;
	int i;
	int addr;

	ret = request_firmware_direct(&fw, "tscs42xx_daccram.dfw", codec->dev);
	if (ret) {
		dev_dbg(codec->dev,
			"No tscs42xx_daccram.dfw file found (%d)\n", ret);
		return 0;
	}

	if (fw->size % COEFF_SIZE != 0) {
		ret = -EINVAL;
		dev_err(codec->dev, "Malformed daccram file (%d)\n", ret);
		return ret;
	}

	for (i = 0, addr = 0; i < fw->size; i += COEFF_SIZE, addr++) {

		do {
			ret = snd_soc_read(codec, R_DACCRSTAT);
			if (ret < 0) {
				dev_err(codec->dev,
					"Failed to read daccrstat (%d)\n", ret);
				return ret;
			}
		} while (ret);

		ret = snd_soc_write(codec, R_DACCRADDR, addr);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to write DACCRADDR (%d)\n", ret);
			return ret;
		}

		ret = regmap_bulk_write(tscs42xx->regmap,
			R_DACCRWRL, &fw->data[i], COEFF_SIZE);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to write coefficient (%d)\n",
				ret);
			return ret;
		}
	}

	dev_dbg(codec->dev, "Loaded tscs42xx_daccram.dfw\n");

	return 0;
}

#define MAX_PLL_LOCK_20MS_WAITS 1
static bool plls_locked(struct snd_soc_codec *codec)
{
	int ret;
	int count = MAX_PLL_LOCK_20MS_WAITS;

	do {
		ret = snd_soc_read(codec, R_PLLCTL0);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to read PLL lock status (%d)\n", ret);
			return false;
		} else if (ret > 0) {
			return true;
		}
		msleep(20);
	} while (count--);

	return false;
}

static int sample_rate_to_pll_freq_out(int sample_rate)
{
	switch (sample_rate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		return 112896000;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 96000:
		return 122880000;
	default:
		return -EINVAL;
	}
}

static int power_down_audio_plls(struct snd_soc_codec *codec,
	struct tscs42xx_priv *tscs42xx)
{
	int ret;

	tscs42xx->pll_users--;
	if (tscs42xx->pll_users > 0)
		return 0;

	ret = snd_soc_update_bits(codec, R_PLLCTL1C,
			RM_PLLCTL1C_PDB_PLL1,
			RV_PLLCTL1C_PDB_PLL1_DISABLE);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to turn PLL off (%d)\n", ret);
		return ret;
	}
	ret = snd_soc_update_bits(codec, R_PLLCTL1C,
			RM_PLLCTL1C_PDB_PLL2,
			RV_PLLCTL1C_PDB_PLL2_DISABLE);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to turn PLL off (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int power_up_audio_plls(struct snd_soc_codec *codec,
	struct tscs42xx_priv *tscs42xx)
{
	int freq_out;
	int ret;
	unsigned int mask;
	unsigned int val;

	freq_out = sample_rate_to_pll_freq_out(tscs42xx->samplerate);
	switch (freq_out) {
	case 122880000: /* 48k */
		mask = RM_PLLCTL1C_PDB_PLL1;
		val = RV_PLLCTL1C_PDB_PLL1_ENABLE;
		break;
	case 112896000: /* 44.1k */
		mask = RM_PLLCTL1C_PDB_PLL2;
		val = RV_PLLCTL1C_PDB_PLL2_ENABLE;
		break;
	default:
		ret = -EINVAL;
		dev_err(codec->dev, "Unrecognized PLL output freq (%d)\n", ret);
		return ret;
	}

	ret = snd_soc_update_bits(codec, R_PLLCTL1C, mask, val);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to turn PLL on (%d)\n", ret);
		return ret;
	}

	if (!plls_locked(codec)) {
		dev_err(codec->dev, "Failed to lock plls\n");
		return -ENOMSG;
	}

	tscs42xx->pll_users++;

	return 0;
}

static int enable_daccram_access(struct tscs42xx_priv *tscs42xx)
{
	struct snd_soc_codec *codec = tscs42xx->codec;
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(codec);
	int ret;

	/* DAC needs to be powered */
	ret = snd_soc_dapm_force_enable_pin(dapm, "DAC L");
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to enable DAC for DACCRAM access (%d)\n", ret);
		return ret;
	}
	ret = snd_soc_dapm_sync(dapm);
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to sync dapm context (%d)\n", ret);
		return ret;
	}

	/* If no one is using the PLL make sure there is a valid rate */
	if (tscs42xx->pll_users == 0)
		tscs42xx->samplerate = 48000;

	return power_up_audio_plls(tscs42xx->codec, tscs42xx);
}

static int disable_daccram_access(struct tscs42xx_priv *tscs42xx)
{
	struct snd_soc_codec *codec = tscs42xx->codec;
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(codec);
	int ret;

	/* DAC needs to be powered */
	ret = snd_soc_dapm_disable_pin(dapm, "DAC L");
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to disable DAC after DACCRAM access (%d)\n",
			ret);
		return ret;
	}
	ret = snd_soc_dapm_sync(dapm);
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to sync dapm context (%d)\n", ret);
		return ret;
	}

	return power_down_audio_plls(tscs42xx->codec, tscs42xx);
}

static int coefficient_get(struct snd_kcontrol *kcontrol,
	unsigned int __user *bytes, unsigned int size)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tscs42xx_priv *tscs42xx = dev_get_drvdata(codec->dev);
	u8 *buffer;
	unsigned int *type;
	unsigned int *len;
	u8 *value;
	u8 addr;
	int i;
	int ret;

	/* We must dump all the coefficients */
	if (size != COEFF_RAM_TLV_SIZE) {
		ret = -EINVAL;
		dev_err(codec->dev,
			"Cannot read %u bytes. Read must be %u bytes (%d)\n",
			size, COEFF_RAM_SIZE, ret);
		goto early_exit_1;
	}

	buffer = kzalloc(size, GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		dev_err(codec->dev,
			"Failed to allocate memory (%d)\n", ret);
		goto early_exit_1;
	}
	type = (unsigned int *)buffer;
	*type = SNDRV_CTL_ELEM_TYPE_BYTES;
	len = (unsigned int *)(buffer + sizeof(unsigned int));
	*len = COEFF_RAM_SIZE;
	value = buffer + 2 * sizeof(unsigned int);

	mutex_lock(&tscs42xx->data_lock);

	ret = enable_daccram_access(tscs42xx);
	if (ret < 0)
		goto early_exit_2;

	for (i = 0, addr = 0; i < COEFF_RAM_SIZE; i += COEFF_SIZE, addr++) {
		ret = regmap_write(tscs42xx->regmap, R_DACCRADDR, addr);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to set daccram address (%d)\n", ret);
			goto exit;
		}

		ret = regmap_bulk_read(tscs42xx->regmap, R_DACCRRDL,
			&value[i], COEFF_SIZE);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to read coefficient (%d)\n", ret);
				goto exit;
		}
	}

	ret = copy_to_user(bytes, buffer, size);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to copy %d bytes to user\n", ret);
		ret = -EFAULT;
		goto exit;
	}

	ret = 0;
exit:
	disable_daccram_access(tscs42xx);

early_exit_2:
	mutex_unlock(&tscs42xx->data_lock);
	kfree(buffer);

early_exit_1:
	return ret;
}

static int coefficient_put(struct snd_kcontrol *kcontrol,
	const unsigned int __user *bytes, unsigned int size)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tscs42xx_priv *tscs42xx = dev_get_drvdata(codec->dev);
	unsigned int stat;
	unsigned int tl_size = sizeof(unsigned int) * 2;
	unsigned int c_size = size - tl_size - 1; /* Byte 0 is the address */
	unsigned int c_count = c_size / COEFF_SIZE;
	unsigned int i;
	unsigned int cnt;
	u8 *buffer;
	u8 *c_bytes;
	u8 addr;
	int ret;

	mutex_lock(&tscs42xx->data_lock);

	ret = enable_daccram_access(tscs42xx);
	if (ret < 0)
		goto early_exit_1;

	buffer = kzalloc(size, GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto early_exit_2;
	}

	ret = copy_from_user(buffer, bytes, size);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to copy %d bytes from user\n", ret);
		ret = -EFAULT;
		goto exit;
	}

	c_bytes = buffer + tl_size + 1;

	addr = buffer[tl_size];
	if (addr + c_size / COEFF_SIZE > COEFF_MAX_ADDR) {
		ret = -EINVAL;
		dev_err(codec->dev,
			"DACCRM Address is out of range (%d)\n", ret);
		goto exit;
	}

	for (i = 0, cnt = 0; cnt < c_count; i += COEFF_SIZE, cnt++, addr++) {

		ret = regmap_write(tscs42xx->regmap, R_DACCRADDR, addr);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to set daccram address (%d)\n", ret);
			goto exit;
		}

		ret = regmap_bulk_write(tscs42xx->regmap,
			R_DACCRWRL, &c_bytes[i], COEFF_SIZE);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to write daccram (%d)\n", ret);
			goto exit;
		}

		do {
			ret = regmap_read(tscs42xx->regmap, R_DACCRSTAT, &stat);
			if (ret < 0) {
				dev_err(codec->dev,
					"Failed to read daccrstat (%d)\n", ret);
				goto exit;
			}
		} while (stat);
	}

	ret = 0;
exit:
	kfree(buffer);

early_exit_2:
	disable_daccram_access(tscs42xx);

early_exit_1:
	mutex_unlock(&tscs42xx->data_lock);

	return ret;
}

static int compressor_attack_time_get(struct snd_kcontrol *kcontrol,
	unsigned int __user *bytes, unsigned int size)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tscs42xx_priv *tscs42xx = dev_get_drvdata(codec->dev);
	unsigned int low;
	unsigned int hi;
	u16 buffer;
	int ret;

	if (size != sizeof(u16)) {
		dev_err(codec->dev,
			"Compressor attack is %u bytes (%d)\n",
			sizeof(u16), -EINVAL);
		return -EINVAL;
	}

	ret = regmap_read(tscs42xx->regmap, R_CATKTCL, &low);
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to read compressor attack (%d)\n", ret);
		return ret;
	}
	ret = regmap_read(tscs42xx->regmap, R_CATKTCH, &hi);
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to read compressor attack (%d)\n", ret);
		return ret;
	}
	buffer = hi << 8 | low;

	ret = copy_to_user(bytes, &buffer, size);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to copy %d bytes to user\n", ret);
		return -EFAULT;
	}

	return 0;
}

static int compressor_attack_time_put(struct snd_kcontrol *kcontrol,
	const unsigned int __user *bytes, unsigned int size)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct tscs42xx_priv *tscs42xx = dev_get_drvdata(codec->dev);
	u16 buffer;
	unsigned int byte;
	int ret;

	if (size != sizeof(u16)) {
		dev_err(codec->dev,
			"Compressor attack is %u bytes (%d)\n",
			sizeof(u16), -EINVAL);
		return -EINVAL;
	}

	ret = copy_from_user(&buffer, bytes, size);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to copy %d bytes from user\n", ret);
		return -EFAULT;
	}

	byte = buffer & 0xff;
	ret = regmap_write(tscs42xx->regmap, R_CATKTCL, byte);
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to write compressor attack (%d)\n", ret);
		return ret;
	}

	byte = buffer >> 8 & 0xff;
	ret = regmap_write(tscs42xx->regmap, R_CATKTCH, byte);
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to write compressor attack (%d)\n", ret);
		return ret;
	}

	return 0;
}

/* D2S Input Select */
static char const * const d2s_input_select_text[] = {
	"Line 1", "Line 2"
};

static const struct soc_enum d2s_input_select_enum =
SOC_ENUM_SINGLE(R_INMODE, FB_INMODE_DS, ARRAY_SIZE(d2s_input_select_text),
		d2s_input_select_text);

static const struct snd_kcontrol_new d2s_input_mux =
SOC_DAPM_ENUM("D2S_IN_MUX", d2s_input_select_enum);

/* Input L Capture Route */
static char const * const input_select_text[] = {
	"Line 1", "Line 2", "Line 3", "D2S"
};

static const struct soc_enum left_input_select_enum =
SOC_ENUM_SINGLE(R_INSELL, FB_INSELL, ARRAY_SIZE(input_select_text),
		input_select_text);

static const struct snd_kcontrol_new left_input_select =
SOC_DAPM_ENUM("LEFT_INPUT_SELECT_ENUM", left_input_select_enum);

/* Input R Capture Route */
static const struct soc_enum right_input_select_enum =
SOC_ENUM_SINGLE(R_INSELR, FB_INSELR, ARRAY_SIZE(input_select_text),
		input_select_text);

static const struct snd_kcontrol_new right_input_select =
SOC_DAPM_ENUM("RIGHT_INPUT_SELECT_ENUM", right_input_select_enum);

/* Input Channel Mapping */
static char const * const ch_map_select_text[] = {
	"Normal", "Left to Right", "Right to Left", "Swap"
};

static const struct soc_enum ch_map_select_enum =
SOC_ENUM_SINGLE(R_AIC2, FB_AIC2_ADCDSEL, ARRAY_SIZE(ch_map_select_text),
		ch_map_select_text);

static int dapm_vref_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol, int event)
{
	msleep(20);
	return 0;
}

static int dapm_micb_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol, int event)
{
	msleep(20);
	return 0;
}

static const struct snd_soc_dapm_widget tscs42xx_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY_S("Vref", 1, R_PWRM2, FB_PWRM2_VREF, 0,
		dapm_vref_event, SND_SOC_DAPM_POST_PMU|SND_SOC_DAPM_PRE_PMD),

	/* Headphone */
	SND_SOC_DAPM_DAC("DAC L", "HiFi Playback", R_PWRM2, FB_PWRM2_HPL, 0),
	SND_SOC_DAPM_DAC("DAC R", "HiFi Playback", R_PWRM2, FB_PWRM2_HPR, 0),
	SND_SOC_DAPM_OUTPUT("Headphone L"),
	SND_SOC_DAPM_OUTPUT("Headphone R"),

	/* Speaker */
	SND_SOC_DAPM_DAC("ClassD L", "HiFi Playback",
		R_PWRM2, FB_PWRM2_SPKL, 0),
	SND_SOC_DAPM_DAC("ClassD R", "HiFi Playback",
		R_PWRM2, FB_PWRM2_SPKR, 0),
	SND_SOC_DAPM_OUTPUT("Speaker L"),
	SND_SOC_DAPM_OUTPUT("Speaker R"),

	/* Capture */
	SND_SOC_DAPM_PGA("Analog In PGA L", R_PWRM1, FB_PWRM1_PGAL, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Analog In PGA R", R_PWRM1, FB_PWRM1_PGAR, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Analog Boost L", R_PWRM1, FB_PWRM1_BSTL, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Analog Boost R", R_PWRM1, FB_PWRM1_BSTR, 0, NULL, 0),
	SND_SOC_DAPM_PGA("ADC Mute", R_CNVRTR0, FB_CNVRTR0_HPOR, true, NULL, 0),
	SND_SOC_DAPM_ADC("ADC L", "HiFi Capture", R_PWRM1, FB_PWRM1_ADCL, 0),
	SND_SOC_DAPM_ADC("ADC R", "HiFi Capture", R_PWRM1, FB_PWRM1_ADCR, 0),

	/* Capture Input */
	SND_SOC_DAPM_MUX("Input L Capture Route", R_PWRM2,
			FB_PWRM2_INSELL, 0, &left_input_select),
	SND_SOC_DAPM_MUX("Input R Capture Route", R_PWRM2,
			FB_PWRM2_INSELR, 0, &right_input_select),

	/* Digital Mic */
	SND_SOC_DAPM_SUPPLY_S("Digital Mic Enable", 2, R_DMICCTL,
		FB_DMICCTL_DMICEN, 0, NULL,
		SND_SOC_DAPM_POST_PMU|SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_INPUT("Digital Mic L"),
	SND_SOC_DAPM_INPUT("Digital Mic R"),

	/* Analog Mic */
	SND_SOC_DAPM_SUPPLY_S("Mic Bias", 2, R_PWRM1, FB_PWRM1_MICB,
		0, dapm_micb_event, SND_SOC_DAPM_POST_PMU|SND_SOC_DAPM_PRE_PMD),

	/* Line In */
	SND_SOC_DAPM_INPUT("Line In 1 L"),
	SND_SOC_DAPM_INPUT("Line In 1 R"),
	SND_SOC_DAPM_INPUT("Line In 2 L"),
	SND_SOC_DAPM_INPUT("Line In 2 R"),
	SND_SOC_DAPM_INPUT("Line In 3 L"),
	SND_SOC_DAPM_INPUT("Line In 3 R"),
};

static const struct snd_soc_dapm_route tscs42xx_intercon[] = {
	{"DAC L", NULL, "Vref"},
	{"DAC R", NULL, "Vref"},
	{"Headphone L", NULL, "DAC L"},
	{"Headphone R", NULL, "DAC R"},

	{"ClassD L", NULL, "Vref"},
	{"ClassD R", NULL, "Vref"},
	{"Speaker L", NULL, "ClassD L"},
	{"Speaker R", NULL, "ClassD R"},

	{"Input L Capture Route", NULL, "Vref"},
	{"Input R Capture Route", NULL, "Vref"},

	{"Mic Bias", NULL, "Vref"},

	{"Input L Capture Route", "Line 1", "Line In 1 L"},
	{"Input R Capture Route", "Line 1", "Line In 1 R"},
	{"Input L Capture Route", "Line 2", "Line In 2 L"},
	{"Input R Capture Route", "Line 2", "Line In 2 R"},
	{"Input L Capture Route", "Line 3", "Line In 3 L"},
	{"Input R Capture Route", "Line 3", "Line In 3 R"},

	{"Analog In PGA L", NULL, "Input L Capture Route"},
	{"Analog In PGA R", NULL, "Input R Capture Route"},
	{"Analog Boost L", NULL, "Analog In PGA L"},
	{"Analog Boost R", NULL, "Analog In PGA R"},
	{"ADC Mute", NULL, "Analog Boost L"},
	{"ADC Mute", NULL, "Analog Boost R"},
	{"ADC L", NULL, "ADC Mute"},
	{"ADC R", NULL, "ADC Mute"},
};

/************
 * CONTROLS *
 ************/

static char const * const eq_band_enable_text[] = {
	"Prescale only",
	"Band1",
	"Band1:2",
	"Band1:3",
	"Band1:4",
	"Band1:5",
	"Band1:6",
};

static char const * const level_detection_text[] = {
	"Average",
	"Peak",
};

static char const * const level_detection_window_text[] = {
	"512 Samples",
	"64 Samples",
};

static char const * const compressor_ratio_text[] = {
	"Reserved", "1.5:1", "2:1", "3:1", "4:1", "5:1", "6:1",
	"7:1", "8:1", "9:1", "10:1", "11:1", "12:1", "13:1", "14:1",
	"15:1", "16:1", "17:1", "18:1", "19:1", "20:1",
};

static DECLARE_TLV_DB_SCALE(hpvol_scale, -8850, 75, 0);
static DECLARE_TLV_DB_SCALE(spkvol_scale, -7725, 75, 0);
static DECLARE_TLV_DB_SCALE(dacvol_scale, -9563, 38, 0);
static DECLARE_TLV_DB_SCALE(adcvol_scale, -7125, 38, 0);
static DECLARE_TLV_DB_SCALE(invol_scale, -1725, 75, 0);
static DECLARE_TLV_DB_SCALE(mic_boost_scale, 0, 1000, 0);
static DECLARE_TLV_DB_MINMAX(mugain_scale, 0, 4650);
static DECLARE_TLV_DB_MINMAX(compth_scale, -9562, 0);

static const struct soc_enum eq1_band_enable_enum =
	SOC_ENUM_SINGLE(R_CONFIG1, FB_CONFIG1_EQ1_BE,
		ARRAY_SIZE(eq_band_enable_text), eq_band_enable_text);

static const struct soc_enum eq2_band_enable_enum =
	SOC_ENUM_SINGLE(R_CONFIG1, FB_CONFIG1_EQ2_BE,
		ARRAY_SIZE(eq_band_enable_text), eq_band_enable_text);

static const struct soc_enum cle_level_detection_enum =
	SOC_ENUM_SINGLE(R_CLECTL, FB_CLECTL_LVL_MODE,
		ARRAY_SIZE(level_detection_text),
		level_detection_text);

static const struct soc_enum cle_level_detection_window_enum =
	SOC_ENUM_SINGLE(R_CLECTL, FB_CLECTL_WINDOWSEL,
		ARRAY_SIZE(level_detection_window_text),
		level_detection_window_text);

static const struct soc_enum mbc_level_detection_enums[] = {
	SOC_ENUM_SINGLE(R_DACMBCCTL, FB_DACMBCCTL_LVLMODE1,
		ARRAY_SIZE(level_detection_text),
			level_detection_text),
	SOC_ENUM_SINGLE(R_DACMBCCTL, FB_DACMBCCTL_LVLMODE2,
		ARRAY_SIZE(level_detection_text),
			level_detection_text),
	SOC_ENUM_SINGLE(R_DACMBCCTL, FB_DACMBCCTL_LVLMODE3,
		ARRAY_SIZE(level_detection_text),
			level_detection_text),
};

static const struct soc_enum mbc_level_detection_window_enums[] = {
	SOC_ENUM_SINGLE(R_DACMBCCTL, FB_DACMBCCTL_WINSEL1,
		ARRAY_SIZE(level_detection_window_text),
			level_detection_window_text),
	SOC_ENUM_SINGLE(R_DACMBCCTL, FB_DACMBCCTL_WINSEL2,
		ARRAY_SIZE(level_detection_window_text),
			level_detection_window_text),
	SOC_ENUM_SINGLE(R_DACMBCCTL, FB_DACMBCCTL_WINSEL3,
		ARRAY_SIZE(level_detection_window_text),
			level_detection_window_text),
};

static const struct soc_enum compressor_ratio_enum =
	SOC_ENUM_SINGLE(R_CMPRAT, FB_CMPRAT,
		ARRAY_SIZE(compressor_ratio_text), compressor_ratio_text);

static const struct snd_kcontrol_new tscs42xx_snd_controls[] = {
	/* Volumes */
	SOC_DOUBLE_R_TLV("Headphone Playback Volume", R_HPVOLL, R_HPVOLR,
			FB_HPVOLL, 0x7F, 0, hpvol_scale),
	SOC_DOUBLE_R_TLV("Speaker Playback Volume", R_SPKVOLL, R_SPKVOLR,
			FB_SPKVOLL, 0x7F, 0, spkvol_scale),
	SOC_DOUBLE_R_TLV("Master Playback Volume", R_DACVOLL, R_DACVOLR,
			FB_DACVOLL, 0xFF, 0, dacvol_scale),
	SOC_DOUBLE_R_TLV("PCM Capture Volume", R_ADCVOLL, R_ADCVOLR,
			FB_ADCVOLL, 0xFF, 0, adcvol_scale),
	SOC_DOUBLE_R_TLV("Master Capture Volume", R_INVOLL, R_INVOLR,
			FB_INVOLL, 0x3F, 0, invol_scale),

	/* INSEL */
	SOC_DOUBLE_R_TLV("Mic Boost Capture Volume", R_INSELL, R_INSELR,
			FB_INSELL_MICBSTL, FV_INSELL_MICBSTL_30DB,
			0, mic_boost_scale),

	/* Input Channel Map */
	SOC_ENUM("Input Channel Map Switch", ch_map_select_enum),

	/* DSP */
	SND_SOC_BYTES_TLV("Coefficients", COEFF_RAM_TLV_SIZE,
		coefficient_get, coefficient_put),

	/* EQ */
	SOC_SINGLE("EQ1 Switch", R_CONFIG1, FB_CONFIG1_EQ1_EN, 1, 0),
	SOC_SINGLE("EQ2 Switch", R_CONFIG1, FB_CONFIG1_EQ2_EN, 1, 0),
	SOC_ENUM("EQ1 Band Enable Switch", eq1_band_enable_enum),
	SOC_ENUM("EQ2 Band Enable Switch", eq2_band_enable_enum),

	/* CLE */
	SOC_ENUM("CLE Level Detection Switch",
		cle_level_detection_enum),
	SOC_ENUM("CLE Level Detection Window Switch",
		cle_level_detection_window_enum),
	SOC_SINGLE("Expander Switch",
		R_CLECTL, FB_CLECTL_EXP_EN, 1, 0),
	SOC_SINGLE("Limiter Switch",
		R_CLECTL, FB_CLECTL_LIMIT_EN, 1, 0),
	SOC_SINGLE("Compressor Switch",
		R_CLECTL, FB_CLECTL_COMP_EN, 1, 0),
	SOC_SINGLE_TLV("CLE Make-Up Gain Playback Volume",
		R_MUGAIN, FB_MUGAIN_CLEMUG, 0x1f, 0, mugain_scale),
	SOC_SINGLE_TLV("Compressor Threshold Playback Volume",
		R_COMPTH, FB_COMPTH, 0xff, 0, compth_scale),
	SOC_ENUM("Compressor Ratio", compressor_ratio_enum),
	SND_SOC_BYTES_TLV("Compressor Attack Time", sizeof(u16),
		compressor_attack_time_get, compressor_attack_time_put),

	/* Effects */
	SOC_SINGLE("3D Switch", R_FXCTL, FB_FXCTL_3DEN, 1, 0),
	SOC_SINGLE("Treble Switch", R_FXCTL, FB_FXCTL_TEEN, 1, 0),
	SOC_SINGLE("Treble Bypass Switch", R_FXCTL, FB_FXCTL_TNLFBYPASS, 1, 0),
	SOC_SINGLE("Bass Switch", R_FXCTL, FB_FXCTL_BEEN, 1, 0),
	SOC_SINGLE("Bass Bypass Switch", R_FXCTL, FB_FXCTL_BNLFBYPASS, 1, 0),

	/* MBC */
	SOC_SINGLE("MBC Band1 Switch", R_DACMBCEN, FB_DACMBCEN_MBCEN1, 1, 0),
	SOC_SINGLE("MBC Band2 Switch", R_DACMBCEN, FB_DACMBCEN_MBCEN2, 1, 0),
	SOC_SINGLE("MBC Band3 Switch", R_DACMBCEN, FB_DACMBCEN_MBCEN3, 1, 0),
	SOC_ENUM("MBC Band1 Level Detection Switch",
		mbc_level_detection_enums[0]),
	SOC_ENUM("MBC Band2 Level Detection Switch",
		mbc_level_detection_enums[1]),
	SOC_ENUM("MBC Band3 Level Detection Switch",
		mbc_level_detection_enums[2]),
	SOC_ENUM("MBC Band1 Level Detection Window Switch",
		mbc_level_detection_window_enums[0]),
	SOC_ENUM("MBC Band2 Level Detection Window Switch",
		mbc_level_detection_window_enums[1]),
	SOC_ENUM("MBC Band3 Level Detection Window Switch",
		mbc_level_detection_window_enums[2]),
};

#define TSCS42XX_RATES SNDRV_PCM_RATE_8000_96000

#define TSCS42XX_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE \
	| SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

static int setup_sample_format(struct snd_soc_codec *codec,
		snd_pcm_format_t format)
{
	unsigned int width;
	int ret;

	switch (format) {
	case SNDRV_PCM_FORMAT_S16_LE:
		width = RV_AIC1_WL_16;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		width = RV_AIC1_WL_20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		width = RV_AIC1_WL_24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		width = RV_AIC1_WL_32;
		break;
	default:
		ret = -EINVAL;
		dev_err(codec->dev, "Unsupported format width (%d)\n", ret);
		return ret;
	}
	ret = snd_soc_update_bits(codec, R_AIC1, RM_AIC1_WL, width);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set sample width (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int setup_sample_rate(struct snd_soc_codec *codec,
		unsigned int rate,
		struct tscs42xx_priv *tscs42xx)
{
	unsigned int br, bm;
	int ret;

	switch (rate) {
	case 8000:
		br = RV_DACSR_DBR_32;
		bm = RV_DACSR_DBM_PT25;
		break;
	case 16000:
		br = RV_DACSR_DBR_32;
		bm = RV_DACSR_DBM_PT5;
		break;
	case 24000:
		br = RV_DACSR_DBR_48;
		bm = RV_DACSR_DBM_PT5;
		break;
	case 32000:
		br = RV_DACSR_DBR_32;
		bm = RV_DACSR_DBM_1;
		break;
	case 48000:
		br = RV_DACSR_DBR_48;
		bm = RV_DACSR_DBM_1;
		break;
	case 96000:
		br = RV_DACSR_DBR_48;
		bm = RV_DACSR_DBM_2;
		break;
	case 11025:
		br = RV_DACSR_DBR_44_1;
		bm = RV_DACSR_DBM_PT25;
		break;
	case 22050:
		br = RV_DACSR_DBR_44_1;
		bm = RV_DACSR_DBM_PT5;
		break;
	case 44100:
		br = RV_DACSR_DBR_44_1;
		bm = RV_DACSR_DBM_1;
		break;
	case 88200:
		br = RV_DACSR_DBR_44_1;
		bm = RV_DACSR_DBM_2;
		break;
	default:
		dev_err(codec->dev, "Unsupported sample rate %d\n", rate);
		return -EINVAL;
	}

	/* DAC and ADC share bit and frame clock */
	ret = snd_soc_update_bits(codec, R_DACSR, RM_DACSR_DBR, br);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to update register (%d)\n", ret);
		return ret;
	}
	ret = snd_soc_update_bits(codec, R_DACSR, RM_DACSR_DBM, bm);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to update register (%d)\n", ret);
		return ret;
	}
	ret = snd_soc_update_bits(codec, R_ADCSR, RM_DACSR_DBR, br);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to update register (%d)\n", ret);
		return ret;
	}
	ret = snd_soc_update_bits(codec, R_ADCSR, RM_DACSR_DBM, bm);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to update register (%d)\n", ret);
		return ret;
	}

	tscs42xx->samplerate = rate;

	return 0;
}

struct reg_setting {
	unsigned int addr;
	unsigned int val;
	unsigned int mask;
};

#define PLL_REG_SETTINGS_COUNT 13
struct pll_ctl {
	int input_freq;
	struct reg_setting settings[PLL_REG_SETTINGS_COUNT];
};

#define PLL_CTL(f, rt, rd, r1b_l, r9, ra, rb,				\
		rc, r12, r1b_h, re, rf, r10, r11)			\
	{								\
		.input_freq = f,					\
		.settings = {						\
			{R_TIMEBASE,  rt,   0xFF},			\
			{R_PLLCTLD,   rd,   0xFF},			\
			{R_PLLCTL1B, r1b_l, 0x0F},			\
			{R_PLLCTL9,   r9,   0xFF},			\
			{R_PLLCTLA,   ra,   0xFF},			\
			{R_PLLCTLB,   rb,   0xFF},			\
			{R_PLLCTLC,   rc,   0xFF},			\
			{R_PLLCTL12, r12,   0xFF},			\
			{R_PLLCTL1B, r1b_h, 0xF0},			\
			{R_PLLCTLE,   re,   0xFF},			\
			{R_PLLCTLF,   rf,   0xFF},			\
			{R_PLLCTL10, r10,   0xFF},			\
			{R_PLLCTL11, r11,   0xFF},			\
		},							\
	}

static const struct pll_ctl pll_ctls[] = {
	PLL_CTL(1411200, 0x05,
		0x39, 0x04, 0x07, 0x02, 0xC3, 0x04,
		0x1B, 0x10, 0x03, 0x03, 0xD0, 0x02),
	PLL_CTL(1536000, 0x05,
		0x1A, 0x04, 0x02, 0x03, 0xE0, 0x01,
		0x1A, 0x10, 0x02, 0x03, 0xB9, 0x01),
	PLL_CTL(2822400, 0x0A,
		0x23, 0x04, 0x07, 0x04, 0xC3, 0x04,
		0x22, 0x10, 0x05, 0x03, 0x58, 0x02),
	PLL_CTL(3072000, 0x0B,
		0x22, 0x04, 0x07, 0x03, 0x48, 0x03,
		0x1A, 0x10, 0x04, 0x03, 0xB9, 0x01),
	PLL_CTL(5644800, 0x15,
		0x23, 0x04, 0x0E, 0x04, 0xC3, 0x04,
		0x1A, 0x10, 0x08, 0x03, 0xE0, 0x01),
	PLL_CTL(6144000, 0x17,
		0x1A, 0x04, 0x08, 0x03, 0xE0, 0x01,
		0x1A, 0x10, 0x08, 0x03, 0xB9, 0x01),
	PLL_CTL(12000000, 0x2E,
		0x1B, 0x04, 0x19, 0x03, 0x00, 0x03,
		0x2A, 0x10, 0x19, 0x05, 0x98, 0x04),
	PLL_CTL(19200000, 0x4A,
		0x13, 0x04, 0x14, 0x03, 0x80, 0x01,
		0x1A, 0x10, 0x19, 0x03, 0xB9, 0x01),
	PLL_CTL(22000000, 0x55,
		0x2A, 0x04, 0x37, 0x05, 0x00, 0x06,
		0x22, 0x10, 0x26, 0x03, 0x49, 0x02),
	PLL_CTL(22579200, 0x57,
		0x22, 0x04, 0x31, 0x03, 0x20, 0x03,
		0x1A, 0x10, 0x1D, 0x03, 0xB3, 0x01),
	PLL_CTL(24000000, 0x5D,
		0x13, 0x04, 0x19, 0x03, 0x80, 0x01,
		0x1B, 0x10, 0x19, 0x05, 0x4C, 0x02),
	PLL_CTL(24576000, 0x5F,
		0x13, 0x04, 0x1D, 0x03, 0xB3, 0x01,
		0x22, 0x10, 0x40, 0x03, 0x72, 0x03),
	PLL_CTL(27000000, 0x68,
		0x22, 0x04, 0x4B, 0x03, 0x00, 0x04,
		0x2A, 0x10, 0x7D, 0x03, 0x20, 0x06),
	PLL_CTL(36000000, 0x8C,
		0x1B, 0x04, 0x4B, 0x03, 0x00, 0x03,
		0x2A, 0x10, 0x7D, 0x03, 0x98, 0x04),
	PLL_CTL(25000000, 0x61,
		0x1B, 0x04, 0x37, 0x03, 0x2B, 0x03,
		0x1A, 0x10, 0x2A, 0x03, 0x39, 0x02),
	PLL_CTL(26000000, 0x65,
		0x23, 0x04, 0x41, 0x05, 0x00, 0x06,
		0x1A, 0x10, 0x26, 0x03, 0xEF, 0x01),
	PLL_CTL(12288000, 0x2F,
		0x1A, 0x04, 0x12, 0x03, 0x1C, 0x02,
		0x22, 0x10, 0x20, 0x03, 0x72, 0x03),
	PLL_CTL(40000000, 0x9B,
		0x22, 0x08, 0x7D, 0x03, 0x80, 0x04,
		0x23, 0x10, 0x7D, 0x05, 0xE4, 0x06),
	PLL_CTL(512000, 0x01,
		0x22, 0x04, 0x01, 0x03, 0xD0, 0x02,
		0x1B, 0x10, 0x01, 0x04, 0x72, 0x03),
	PLL_CTL(705600, 0x02,
		0x22, 0x04, 0x02, 0x03, 0x15, 0x04,
		0x22, 0x10, 0x01, 0x04, 0x80, 0x02),
	PLL_CTL(1024000, 0x03,
		0x22, 0x04, 0x02, 0x03, 0xD0, 0x02,
		0x1B, 0x10, 0x02, 0x04, 0x72, 0x03),
	PLL_CTL(2048000, 0x07,
		0x22, 0x04, 0x04, 0x03, 0xD0, 0x02,
		0x1B, 0x10, 0x04, 0x04, 0x72, 0x03),
	PLL_CTL(2400000, 0x08,
		0x22, 0x04, 0x05, 0x03, 0x00, 0x03,
		0x23, 0x10, 0x05, 0x05, 0x98, 0x04),
};

static const struct pll_ctl *get_pll_ctl(int input_freq)
{
	int i;
	const struct pll_ctl *pll_ctl = NULL;

	for (i = 0; i < ARRAY_SIZE(pll_ctls); ++i)
		if (input_freq == pll_ctls[i].input_freq) {
			pll_ctl = &pll_ctls[i];
			break;
		}

	return pll_ctl;
}


static int set_pll_ctl_from_input_freq(struct snd_soc_codec *codec,
		const int input_freq)
{
	int ret;
	int i;
	const struct pll_ctl *pll_ctl;

	pll_ctl = get_pll_ctl(input_freq);
	if (!pll_ctl) {
		ret = -EINVAL;
		dev_err(codec->dev, "No PLL input entry for %d (%d)\n",
			input_freq, ret);
		return ret;
	}

	for (i = 0; i < PLL_REG_SETTINGS_COUNT; ++i) {
		ret = snd_soc_update_bits(codec,
			pll_ctl->settings[i].addr,
			pll_ctl->settings[i].mask,
			pll_ctl->settings[i].val);
		if (ret < 0) {
			dev_err(codec->dev, "Failed to set pll ctl (%d)\n",
				ret);
			return ret;
		}
	}

	return 0;
}

static int configure_clocks(struct snd_soc_codec *codec,
		struct tscs42xx_priv *tscs42xx)
{
	int ret;

	ret = set_pll_ctl_from_input_freq(codec, tscs42xx->mclk_src_freq);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to setup PLL input (%d)\n", ret);
		return ret;
	}

	switch (tscs42xx->pll_src_clk) {
	case PLL_SRC_CLK_XTAL:
		ret = snd_soc_write(codec, R_PLLREFSEL,
				RV_PLLREFSEL_PLL1_REF_SEL_XTAL_MCLK1 |
				RV_PLLREFSEL_PLL2_REF_SEL_XTAL_MCLK1);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to set pll reference input (%d)\n",
				ret);
			return ret;
		}
		break;
	case PLL_SRC_CLK_MCLK2:
		ret = clk_set_rate(tscs42xx->mclk, tscs42xx->mclk_src_freq);
		if (ret < 0) {
			dev_err(codec->dev,
				"Could not set mclk rate %d (%d)\n",
				tscs42xx->mclk_src_freq, ret);
			return ret;
		}

		ret = clk_prepare_enable(tscs42xx->mclk);
		if (ret < 0) {
			dev_err(codec->dev, "Failed to enable mclk: (%d)\n",
				ret);
			return ret;
		}

		ret = snd_soc_write(codec, R_PLLREFSEL,
				RV_PLLREFSEL_PLL1_REF_SEL_MCLK2 |
				RV_PLLREFSEL_PLL2_REF_SEL_MCLK2);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to set PLL reference (%d)\n", ret);
			return ret;
		}
		break;
	}

	return 0;
}

static int tscs42xx_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *codec_dai)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct tscs42xx_priv *tscs42xx = snd_soc_codec_get_drvdata(codec);
	int ret;

	mutex_lock(&tscs42xx->data_lock);

	ret = setup_sample_format(codec, params_format(params));
	if (ret < 0) {
		dev_err(codec->dev, "Failed to setup sample format (%d)\n",
			ret);
		goto exit;
	}

	ret = setup_sample_rate(codec, params_rate(params), tscs42xx);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to setup sample rate (%d)\n", ret);
		goto exit;
	}

	ret = 0;
exit:
	mutex_unlock(&tscs42xx->data_lock);

	return ret;
}

static int dac_mute(struct snd_soc_codec *codec,
	struct tscs42xx_priv *tscs42xx)
{
	int ret;

	ret = snd_soc_update_bits(codec, R_CNVRTR1, RM_CNVRTR1_DACMU,
		RV_CNVRTR1_DACMU_ENABLE);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to mute DAC (%d)\n",
				ret);
		return ret;
	}

	ret = power_down_audio_plls(codec, tscs42xx);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to power down plls (%d)\n",
			ret);
		return ret;
	}

	return 0;
}

static int dac_unmute(struct snd_soc_codec *codec,
	struct tscs42xx_priv *tscs42xx)
{
	int ret;

	ret = power_up_audio_plls(codec, tscs42xx);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to power up plls (%d)\n",
			ret);
		return ret;
	}

	ret = snd_soc_update_bits(codec, R_CNVRTR1, RM_CNVRTR1_DACMU,
		RV_CNVRTR1_DACMU_DISABLE);
	if (ret < 0) {
		power_down_audio_plls(codec, tscs42xx);
		dev_err(codec->dev, "Failed to unmute DAC (%d)\n",
				ret);
		return ret;
	}

	return 0;
}

static int adc_mute(struct snd_soc_codec *codec,
	struct tscs42xx_priv *tscs42xx)
{
	int ret;

	ret = snd_soc_update_bits(codec, R_CNVRTR0, RM_CNVRTR0_ADCMU,
		RV_CNVRTR0_ADCMU_ENABLE);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to mute ADC (%d)\n",
				ret);
		return ret;
	}

	ret = power_down_audio_plls(codec, tscs42xx);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to power down plls (%d)\n",
			ret);
		return ret;
	}

	return 0;
}

static int adc_unmute(struct snd_soc_codec *codec,
	struct tscs42xx_priv *tscs42xx)
{
	int ret;

	ret = power_up_audio_plls(codec, tscs42xx);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to power up plls (%d)\n",
			ret);
		return ret;
	}

	ret = snd_soc_update_bits(codec, R_CNVRTR0, RM_CNVRTR0_ADCMU,
		RV_CNVRTR0_ADCMU_DISABLE);
	if (ret < 0) {
		power_down_audio_plls(codec, tscs42xx);
		dev_err(codec->dev, "Failed to unmute ADC (%d)\n",
				ret);
		return ret;
	}

	return 0;
}

static int tscs42xx_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_soc_codec *codec = dai->codec;
	struct tscs42xx_priv *tscs42xx = snd_soc_codec_get_drvdata(codec);
	int ret;

	mutex_lock(&tscs42xx->data_lock);

	if (mute)
		if (stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = dac_mute(codec, tscs42xx);
		else
			ret = adc_mute(codec, tscs42xx);
	else
		if (stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = dac_unmute(codec, tscs42xx);
		else
			ret = adc_unmute(codec, tscs42xx);

	mutex_unlock(&tscs42xx->data_lock);

	return ret;
}

static int tscs42xx_set_dai_fmt(struct snd_soc_dai *codec_dai,
		unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	int ret;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		ret = snd_soc_update_bits(codec, R_AIC1, RM_AIC1_MS,
				RV_AIC1_MS_MASTER);
		if (ret < 0)
			dev_err(codec->dev,
				"Failed to set codec DAI master (%d)\n", ret);
		else
			ret = 0;
		break;
	default:
		ret = -EINVAL;
		dev_err(codec->dev, "Unsupported format (%d)\n", ret);
		break;
	}

	return ret;
}

static int tscs42xx_set_bclk_ratio(struct snd_soc_dai *codec_dai,
		unsigned int ratio)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct tscs42xx_priv *tscs42xx = snd_soc_codec_get_drvdata(codec);
	unsigned int value;
	int ret = 0;

	mutex_lock(&tscs42xx->data_lock);

	switch (ratio) {
	case 32:
		value = RV_DACSR_DBCM_32;
		break;
	case 40:
		value = RV_DACSR_DBCM_40;
		break;
	case 64:
		value = RV_DACSR_DBCM_64;
		break;
	default:
		ret = -EINVAL;
		dev_err(codec->dev, "Unsupported bclk ratio (%d)\n", ret);
		goto exit;
	}

	ret = snd_soc_update_bits(codec, R_DACSR, RM_DACSR_DBCM, value);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set DAC BCLK ratio (%d)\n", ret);
		goto exit;
	}
	ret = snd_soc_update_bits(codec, R_ADCSR, RM_ADCSR_ABCM, value);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set ADC BCLK ratio (%d)\n", ret);
		goto exit;
	}

	tscs42xx->bclk_ratio = ratio;

	ret = 0;
exit:
	mutex_unlock(&tscs42xx->data_lock);

	return ret;
}

static const struct snd_soc_dai_ops tscs42xx_dai_ops = {
	.hw_params	= tscs42xx_hw_params,
	.mute_stream	= tscs42xx_mute_stream,
	.set_fmt	= tscs42xx_set_dai_fmt,
	.set_bclk_ratio = tscs42xx_set_bclk_ratio,
};

static struct snd_soc_dai_driver tscs42xx_dai = {
	.name = "tscs42xx-HiFi",
	.playback = {
		.stream_name = "HiFi Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = TSCS42XX_RATES,
		.formats = TSCS42XX_FORMATS,},
	.capture = {
		.stream_name = "HiFi Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = TSCS42XX_RATES,
		.formats = TSCS42XX_FORMATS,},
	.ops = &tscs42xx_dai_ops,
	.symmetric_rates = 1,
};

static int part_is_valid(struct tscs42xx_priv *tscs42xx)
{
	int val;
	int ret;
	unsigned int reg;

	ret = regmap_read(tscs42xx->regmap, R_DEVIDH, &reg);
	if (ret < 0)
		return ret;

	val = reg << 8;
	ret = regmap_read(tscs42xx->regmap, R_DEVIDL, &reg);
	if (ret < 0)
		return ret;

	val |= reg;

	switch (val) {
	case 0x4A74:
	case 0x4A73:
		ret = true;
		break;
	default:
		ret = false;
		break;
	};

	if (ret)
		dev_dbg(tscs42xx->dev, "Found part 0x%04x\n", val);
	else
		dev_err(tscs42xx->dev, "0x%04x is not a valid part\n", val);

	return ret;
}

static int set_data_from_of(struct i2c_client *i2c,
		struct tscs42xx_priv *tscs42xx)
{
	struct device_node *np = i2c->dev.of_node;
	char const *mclk_src = NULL;
	int ret;

	ret = of_property_read_string(np, "mclk-src", &mclk_src);
	if (ret) {
		dev_err(&i2c->dev, "mclk-src is needed (%d)\n", ret);
		return ret;
	}

	if (!strncmp(mclk_src, "mclk", 4)) {
		tscs42xx->mclk = devm_clk_get(&i2c->dev, NULL);
		if (IS_ERR(tscs42xx->mclk)) {
			dev_dbg(&i2c->dev, "mclk not present trying again\n");
			return -EPROBE_DEFER;
		}
		tscs42xx->pll_src_clk = PLL_SRC_CLK_MCLK2;
	} else if (!strncmp(mclk_src, "xtal", 4)) {
		tscs42xx->pll_src_clk = PLL_SRC_CLK_XTAL;
	} else {
		dev_err(&i2c->dev, "mclk-src %s is unsupported\n", mclk_src);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "mclk-src-freq",
			&tscs42xx->mclk_src_freq);
	if (ret) {
		dev_err(&i2c->dev, "mclk-src-freq not provided (%d)\n", ret);
		return ret;
	}
	if (!get_pll_ctl(tscs42xx->mclk_src_freq)) {
		dev_err(&i2c->dev, "mclk frequency unsupported\n");
		return ret;
	}

	return 0;
}

//static struct tempo_control_reg control_regs[] = {
//	TEMPO_CONTROL_REG(config0, R_CONFIG0),		/* 0x1F */
//	TEMPO_CONTROL_REG(config1, R_CONFIG1),		/* 0x20 */
//	TEMPO_CONTROL_REG(clectl, R_CLECTL),		/* 0x25 */
//	TEMPO_CONTROL_REG(mugain, R_MUGAIN),		/* 0x26 */
//	TEMPO_CONTROL_REG(compth, R_COMPTH),		/* 0x27 */
//	TEMPO_CONTROL_REG(cmprat, R_CMPRAT),		/* 0x28 */
//	TEMPO_CONTROL_REG(catktcl, R_CATKTCL),		/* 0x29 */
//	TEMPO_CONTROL_REG(catktch, R_CATKTCH),		/* 0x2A */
//	TEMPO_CONTROL_REG(creltcl, R_CRELTCL),		/* 0x2B */
//	TEMPO_CONTROL_REG(creltch, R_CRELTCH),		/* 0x2C */
//	TEMPO_CONTROL_REG(limth, R_LIMTH),		/* 0x2D */
//	TEMPO_CONTROL_REG(limtgt, R_LIMTGT),		/* 0x2E */
//	TEMPO_CONTROL_REG(latktcl, R_LATKTCL),		/* 0x2F */
//	TEMPO_CONTROL_REG(latktch, R_LATKTCH),		/* 0x30 */
//	TEMPO_CONTROL_REG(lreltcl, R_LRELTCL),		/* 0x31 */
//	TEMPO_CONTROL_REG(lreltch, R_LRELTCH),		/* 0x32 */
//	TEMPO_CONTROL_REG(expth, R_EXPTH),		/* 0x33 */
//	TEMPO_CONTROL_REG(exprat, R_EXPRAT),		/* 0x34 */
//	TEMPO_CONTROL_REG(xatktcl, R_XATKTCL),		/* 0x35 */
//	TEMPO_CONTROL_REG(xatktch, R_XATKTCH),		/* 0x36 */
//	TEMPO_CONTROL_REG(xreltcl, R_XRELTCL),		/* 0x37 */
//	TEMPO_CONTROL_REG(xreltch, R_XRELTCH),		/* 0x38 */
//	TEMPO_CONTROL_REG(fxctl, R_FXCTL),		/* 0x39 */
//	TEMPO_CONTROL_REG(daccrwrl, R_DACCRWRL),	/* 0x3A */
//	TEMPO_CONTROL_REG(daccrwrm, R_DACCRWRM),	/* 0x3B */
//	TEMPO_CONTROL_REG(daccrwrh, R_DACCRWRH),	/* 0x3C */
//	TEMPO_CONTROL_REG(daccrrdl, R_DACCRRDL),	/* 0x3D */
//	TEMPO_CONTROL_REG(daccrrdm, R_DACCRRDM),	/* 0x3E */
//	TEMPO_CONTROL_REG(daccrrdh, R_DACCRRDH),	/* 0x3F */
//	TEMPO_CONTROL_REG(daccraddr, R_DACCRADDR),	/* 0x40 */
//	TEMPO_CONTROL_REG(dcofsel, R_DCOFSEL),		/* 0x41 */
//	TEMPO_CONTROL_REG(daccrstat, R_DACCRSTAT),	/* 0x8A */
//	TEMPO_CONTROL_REG(dacmbcen, R_DACMBCEN),	/* 0xC7 */
//	TEMPO_CONTROL_REG(dacmbcctl, R_DACMBCCTL),	/* 0xC8 */
//	TEMPO_CONTROL_REG(dacmbcmug1, R_DACMBCMUG1),	/* 0xC9 */
//	TEMPO_CONTROL_REG(dacmbcthr1, R_DACMBCTHR1),	/* 0xCA */
//	TEMPO_CONTROL_REG(dacmbcrat1, R_DACMBCRAT1),	/* 0xCB */
//	TEMPO_CONTROL_REG(dacmbcatk1l, R_DACMBCATK1L),	/* 0xCC */
//	TEMPO_CONTROL_REG(dacmbcatk1h, R_DACMBCATK1H),	/* 0xCD */
//	TEMPO_CONTROL_REG(dacmbcrel1l, R_DACMBCREL1L),	/* 0xCE */
//	TEMPO_CONTROL_REG(dacmbcrel1h, R_DACMBCREL1H),	/* 0xCF */
//	TEMPO_CONTROL_REG(dacmbcmug2, R_DACMBCMUG2),	/* 0xD0 */
//	TEMPO_CONTROL_REG(dacmbcthr2, R_DACMBCTHR2),	/* 0xD1 */
//	TEMPO_CONTROL_REG(dacmbcrat2, R_DACMBCRAT2),	/* 0xD2 */
//	TEMPO_CONTROL_REG(dacmbcatk2l, R_DACMBCATK2L),	/* 0xD3 */
//	TEMPO_CONTROL_REG(dacmbcatk2h, R_DACMBCATK2H),	/* 0xD4 */
//	TEMPO_CONTROL_REG(dacmbcrel2l, R_DACMBCREL2L),	/* 0xD5 */
//	TEMPO_CONTROL_REG(dacmbcrel2h, R_DACMBCREL2H),	/* 0xD6 */
//	TEMPO_CONTROL_REG(dacmbcmug3, R_DACMBCMUG3),	/* 0xD7 */
//	TEMPO_CONTROL_REG(dacmbcthr3, R_DACMBCTHR3),	/* 0xD8 */
//	TEMPO_CONTROL_REG(dacmbcrat3, R_DACMBCRAT3),	/* 0xD9 */
//	TEMPO_CONTROL_REG(dacmbcatk3l, R_DACMBCATK3L),	/* 0xDA */
//	TEMPO_CONTROL_REG(dacmbcatk3h, R_DACMBCATK3H),	/* 0xDB */
//	TEMPO_CONTROL_REG(dacmbcrel3l, R_DACMBCREL3L),	/* 0xDC */
//	TEMPO_CONTROL_REG(dacmbcrel3h, R_DACMBCREL3H),	/* 0xDD */
//};

static int tscs42xx_probe(struct snd_soc_codec *codec)
{
	struct tscs42xx_priv *tscs42xx = snd_soc_codec_get_drvdata(codec);
	int i;
	int ret;

	mutex_lock(&tscs42xx->data_lock);

	tscs42xx->codec = codec;

	ret = configure_clocks(codec, tscs42xx);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to configure clocks (%d)\n", ret);
		goto exit;
	}

	for (i = 0; i < ARRAY_SIZE(r_inits); ++i) {
		ret = snd_soc_write(codec, r_inits[i].reg, r_inits[i].def);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to write codec defaults (%d)\n", ret);
			goto exit;
		}
	}

	/* Power up an interface so the daccram can be accessed */
	ret = snd_soc_update_bits(codec, R_PWRM2, RM_PWRM2_HPL,
		RV_PWRM2_HPL_ENABLE);
	if (ret < 0) {
		dev_err(codec->dev,
			"Failed to power up interface (%d)\n", ret);
		goto exit;
	}

	/* PLLs also needed to be powered */
	tscs42xx->samplerate = 48000; /* No valid rate exist yet */
	ret = power_up_audio_plls(codec, tscs42xx);
	if (ret < 0) {
		goto exit;
		snd_soc_update_bits(codec, R_PWRM2,
					RM_PWRM2_HPL, RV_PWRM2_HPL_DISABLE);
	}

	ret = load_dac_coefficient_ram(codec);
	if (ret < 0) {
		dev_dbg(codec->dev, "Failed to load DAC Coefficients (%d)\n",
			ret);
		power_down_audio_plls(codec, tscs42xx);
		snd_soc_update_bits(codec, R_PWRM2,
					RM_PWRM2_HPL, RV_PWRM2_HPL_DISABLE);
		goto exit;
	}

	power_down_audio_plls(codec, tscs42xx);
	snd_soc_update_bits(codec, R_PWRM2, RM_PWRM2_HPL, RV_PWRM2_HPL_DISABLE);

	ret = 0;
exit:
	mutex_unlock(&tscs42xx->data_lock);

	return ret;
}

static int tscs42xx_remove(struct snd_soc_codec *codec)
{
	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_tscs42xx = {
	.probe =	tscs42xx_probe,
	.remove =	tscs42xx_remove,
	.component_driver = {
		.dapm_widgets = tscs42xx_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(tscs42xx_dapm_widgets),
		.dapm_routes = tscs42xx_intercon,
		.num_dapm_routes = ARRAY_SIZE(tscs42xx_intercon),
		.controls =	tscs42xx_snd_controls,
		.num_controls = ARRAY_SIZE(tscs42xx_snd_controls),
	},
};

static int tscs42xx_i2c_probe(struct i2c_client *i2c,
		const struct i2c_device_id *id)
{
	struct tscs42xx_priv *tscs42xx;
	int ret = 0;

	tscs42xx = devm_kzalloc(&i2c->dev, sizeof(*tscs42xx), GFP_KERNEL);
	if (!tscs42xx)
		return -ENOMEM;
	tscs42xx->dev = &i2c->dev;

	mutex_init(&tscs42xx->data_lock);
	mutex_lock(&tscs42xx->data_lock);

	tscs42xx->pll_users = 0;

	ret = set_data_from_of(i2c, tscs42xx);
	if (ret < 0) {
		dev_err(&i2c->dev, "Error parsing device tree info (%d)", ret);
		goto exit;
	}

	tscs42xx->regmap = devm_regmap_init_i2c(i2c, &tscs42xx_regmap);
	if (IS_ERR(tscs42xx->regmap)) {
		ret = PTR_ERR(tscs42xx->regmap);
		dev_err(&i2c->dev, "Failed to allocat regmap (%d)\n", ret);
		goto exit;
	}

	ret = part_is_valid(tscs42xx);
	if (ret <= 0) {
		dev_err(&i2c->dev, "No valid part (%d)\n", ret);
		ret = -ENODEV;
		goto exit;
	}

	ret = regmap_write(tscs42xx->regmap, R_RESET, RV_RESET_ENABLE);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to reset device (%d)\n", ret);
		goto exit;
	}

	i2c_set_clientdata(i2c, tscs42xx);

	ret = snd_soc_register_codec(&i2c->dev, &soc_codec_dev_tscs42xx,
			&tscs42xx_dai, 1);
	if (ret) {
		dev_err(&i2c->dev, "Failed to register codec (%d)\n", ret);
		goto exit;
	}

	ret = 0;
exit:
	mutex_unlock(&tscs42xx->data_lock);

	return ret;
}

static int tscs42xx_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);

	return 0;
}

static const struct i2c_device_id tscs42xx_i2c_id[] = {
	{ "tscs42xx", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tscs42xx_i2c_id);

static const struct of_device_id tscs42xx_of_match[] = {
	{ .compatible = "tscs,tscs42xx", },
	{ }
};
MODULE_DEVICE_TABLE(of, tscs42xx_of_match);

static struct i2c_driver tscs42xx_i2c_driver = {
	.driver = {
		.name = "tscs42xx",
		.owner = THIS_MODULE,
		.of_match_table = tscs42xx_of_match,
	},
	.probe =    tscs42xx_i2c_probe,
	.remove =   tscs42xx_i2c_remove,
	.id_table = tscs42xx_i2c_id,
};

module_i2c_driver(tscs42xx_i2c_driver);

MODULE_AUTHOR("Tempo Semiconductor <steven.eckhoff.opensource@gmail.com");
MODULE_DESCRIPTION("ASoC TSCS42xx driver");
MODULE_LICENSE("GPL");
