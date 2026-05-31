// SPDX-License-Identifier: GPL-2.0+
/*
 * Written by Tuan Truong Nho - tuantnt08@gmail.com
*/

#include "ssd1306_lib.h"

struct ssd1306_priv {
	ssd1306_i2c_module ssd;
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
	ssd1306_set_cursor(&priv->ssd, 3, 2);
	ssd1306_print_string(&priv->ssd, "Welcome to SSD1306");
	printf("Probe ssd 1306 function done \n");

	return 0;
}

U_BOOT_DRIVER(ssd1036) = {
	.name       = "ssd1306",
	.id         = UCLASS_MISC,
	.of_match   = ssd1306_ids,
	.probe      = ssd1306_probe,
	.priv_auto = sizeof(struct ssd1306_priv),
};
