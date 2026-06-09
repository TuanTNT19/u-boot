// SPDX-License-Identifier: GPL-2.0+
/*
 * Written by Tuan Truong Nho - tuantnt08@gmail.com
*/

#include "ssd1306_lib.h"

struct ssd1306_priv {
	struct ssd1306_i2c_module ssd;
};
static const struct udevice_id ssd1306_ids[] = {
	{.compatible = "tnt19,ssd1306"},
    {}
};

static int ssd1306_probe(struct udevice *dev)
{
	struct ssd1306_priv *priv = dev_get_priv(dev);
	
	priv->ssd.dev = dev;
	priv->ssd.line_num = 0;
	priv->ssd.cursor_position = 0;
	priv->ssd.font_size = SSD1306_DEF_FONT_SIZE;

	ssd1306_display_init(&priv->ssd);
	ssd1306_set_cursor(&priv->ssd, 2, 30);
	ssd1306_print_string(&priv->ssd, "RASP PI4");

	ssd1306_set_cursor(&priv->ssd, 5, 28);
	ssd1306_print_string(&priv->ssd, "Booting...");

	return 0;
}

U_BOOT_DRIVER(ssd1306) = {
	.name       = "ssd1306",
	.id         = UCLASS_I2C_GENERIC,
	.of_match   = ssd1306_ids,
	.probe      = ssd1306_probe,
	.priv_auto = sizeof(struct ssd1306_priv),
};
