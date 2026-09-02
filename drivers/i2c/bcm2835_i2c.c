
// SPDX-License-Identifier: GPL-2.0+
/*
 * Written by Tuan Truong Nho - tuantnt08@gmail.com
*/

#include <log.h>
#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <i2c.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/types.h>

#define BCM2835_I2C_C		0x0
#define BCM2835_I2C_S		0x4
#define BCM2835_I2C_DLEN	0x8
#define BCM2835_I2C_A		0xc
#define BCM2835_I2C_FIFO	0x10
#define BCM2835_I2C_DIV		0x14
#define BCM2835_I2C_DEL		0x18
/*
 * 16-bit field for the number of SCL cycles to wait after rising SCL
 * before deciding the slave is not responding. 0 disables the
 * timeout detection.
 */
#define BCM2835_I2C_CLKT	0x1c

#define BCM2835_I2C_C_READ	BIT(0)
#define BCM2835_I2C_C_CLEAR	BIT(4) /* bits 4 and 5 both clear */
#define BCM2835_I2C_C_ST	BIT(7)
#define BCM2835_I2C_C_INTD	BIT(8)
#define BCM2835_I2C_C_INTT	BIT(9)
#define BCM2835_I2C_C_INTR	BIT(10)
#define BCM2835_I2C_C_I2CEN	BIT(15)

#define BCM2835_I2C_S_TA	BIT(0)
#define BCM2835_I2C_S_DONE	BIT(1)
#define BCM2835_I2C_S_TXW	BIT(2)
#define BCM2835_I2C_S_RXR	BIT(3)
#define BCM2835_I2C_S_TXD	BIT(4)
#define BCM2835_I2C_S_RXD	BIT(5)
#define BCM2835_I2C_S_TXE	BIT(6)
#define BCM2835_I2C_S_RXF	BIT(7)
#define BCM2835_I2C_S_ERR	BIT(8)
#define BCM2835_I2C_S_CLKT	BIT(9)
#define BCM2835_I2C_S_LEN	BIT(10) /* Fake bit for SW error reporting */

#define BCM2835_I2C_FEDL_SHIFT	16
#define BCM2835_I2C_REDL_SHIFT	0

#define BCM2835_I2C_CDIV_MIN	0x0002
#define BCM2835_I2C_CDIV_MAX	0xFFFE

#define BCM2835_I2C_CLKT_TOUT_MS 35

struct bcm2835_i2c_priv {
    void __iomem *regs;
    size_t msg_buf_remaining;
    u8 *msg_buf;
    u32 bus_clk_rate;
	struct i2c_msg *curr_msg;
	int msg_err;
	int num_msgs;
};

static inline void bcm2835_i2c_writel(struct bcm2835_i2c_priv *i2c_dev,
				      u32 reg, u32 val)
{
	writel(val, i2c_dev->regs + reg);
}

static inline u32 bcm2835_i2c_readl(struct bcm2835_i2c_priv *i2c_dev, u32 reg)
{
	return readl(i2c_dev->regs + reg);
}

static void bcm2835_fill_txfifo(struct bcm2835_i2c_priv *i2c_dev)
{
	u32 val;

	while (i2c_dev->msg_buf_remaining) {
		val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);
		if (!(val & BCM2835_I2C_S_TXD))
			break;
		bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_FIFO,
				   *i2c_dev->msg_buf);
		i2c_dev->msg_buf++;
		i2c_dev->msg_buf_remaining--;
	}
}

static void bcm2835_drain_rxfifo(struct bcm2835_i2c_priv *i2c_dev)
{
	u32 val;

	while (i2c_dev->msg_buf_remaining) {
		val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);
		if (!(val & BCM2835_I2C_S_RXD))
			break;
		*i2c_dev->msg_buf = bcm2835_i2c_readl(i2c_dev,
						      BCM2835_I2C_FIFO);
		i2c_dev->msg_buf++;
		i2c_dev->msg_buf_remaining--;
	}
}

static int bcm2835_i2c_calc_divider(unsigned long rate,
				unsigned long parent_rate)
{
	int divider = DIV_ROUND_UP(parent_rate, rate);

	/*
	 * Per the datasheet, the register is always interpreted as an even
	 * number, by rounding down. In other words, the LSB is ignored. So,
	 * if the LSB is set, increment the divider to avoid any issue.
	 */
	if (divider & 1)
		divider++;
	if ((divider < BCM2835_I2C_CDIV_MIN) ||
	    (divider > BCM2835_I2C_CDIV_MAX))
		return -EINVAL;

	return divider;
}

static int bcm2835_i2c_set_rate (struct udevice *bus, unsigned int rate) {
	struct bcm2835_i2c_priv *i2c_dev = dev_get_priv(bus);
	u32 parent_rate = 150000000;
	u32 redl, fedl;
	u32 clk_tout;
	u32 divider = bcm2835_i2c_calc_divider(rate, parent_rate);

	if (divider == -EINVAL)
		return -EINVAL;

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_DIV, divider);

	/*
	 * Number of core clocks to wait after falling edge before
	 * outputting the next data bit.  Note that both FEDL and REDL
	 * can't be greater than CDIV/2.
	 */
	fedl = max(divider / 16, 1u);

	/*
	 * Number of core clocks to wait after rising edge before
	 * sampling the next incoming data bit.
	 */
	redl = max(divider / 4, 1u);

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_DEL,
			   (fedl << BCM2835_I2C_FEDL_SHIFT) |
			   (redl << BCM2835_I2C_REDL_SHIFT));

	/*
	 * Set the clock stretch timeout.
	 */
	if (rate > 0xffff*1000/BCM2835_I2C_CLKT_TOUT_MS)
	    clk_tout = 0xffff;
	else
	    clk_tout = BCM2835_I2C_CLKT_TOUT_MS*rate/1000;

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_CLKT, clk_tout);

	return 0;
}

static void bcm2835_i2c_start_transfer(struct bcm2835_i2c_priv *i2c_dev)
{
	u32 c = BCM2835_I2C_C_ST | BCM2835_I2C_C_I2CEN;
	struct i2c_msg *msg = i2c_dev->curr_msg;
	bool last_msg = (i2c_dev->num_msgs == 1);

	if (!i2c_dev->num_msgs)
    	return;

	i2c_dev->num_msgs--;
	i2c_dev->msg_buf = msg->buf;
	i2c_dev->msg_buf_remaining = msg->len;

	if (msg->flags & I2C_M_RD)
		c |= BCM2835_I2C_C_READ ;
	else
		c |= BCM2835_I2C_C_INTT;

	if (last_msg)
		c |= BCM2835_I2C_C_INTD;

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_A, msg->addr);
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_DLEN, msg->len);
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, c);
}

static void bcm2835_i2c_finish_transfer(struct bcm2835_i2c_priv *i2c_dev)
{
	i2c_dev->curr_msg = NULL;
	i2c_dev->num_msgs = 0;

	i2c_dev->msg_buf = NULL;
	i2c_dev->msg_buf_remaining = 0;
}

static int bcm2835_i2c_xfer(struct udevice *dev, struct i2c_msg *msgs, int count)
{
	struct bcm2835_i2c_priv *i2c_dev = dev_get_priv(dev);
	bool ignore_nak = false;
	int i;
	u32 val, err;
	int timeout = 100000;

	for (i = 0; i < (count - 1); i++) {
		if (msgs[i].flags & I2C_M_RD) {
			printf ("bcm2835_i2c: READ message must be last\n");
			return -EOPNOTSUPP;
		}
		if (msgs[i].flags & I2C_M_IGNORE_NAK)
			ignore_nak = true;
	}
	i2c_dev->curr_msg = msgs;
	i2c_dev->num_msgs = count;
	i2c_dev->msg_err = 0;

	bcm2835_i2c_start_transfer(i2c_dev);
	// polling
	while(timeout--) {
		val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);

		err = val & (BCM2835_I2C_S_CLKT | BCM2835_I2C_S_ERR);
		if (err && !(val & BCM2835_I2C_S_TA))
			i2c_dev->msg_err = err;
		
		if (val & BCM2835_I2C_S_DONE) {
			if (!i2c_dev->curr_msg) {
			printf("Got unexpected interrupt (from firmware?)\n");
			} else if (i2c_dev->curr_msg->flags & I2C_M_RD) {
				bcm2835_drain_rxfifo(i2c_dev);
				val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);
			}

			if ((val & BCM2835_I2C_S_RXD) || i2c_dev->msg_buf_remaining)
				i2c_dev->msg_err = BCM2835_I2C_S_LEN;		
			break;
		}

		if (val & BCM2835_I2C_S_TXW) {
			if (!i2c_dev->msg_buf_remaining) {
				i2c_dev->msg_err = val | BCM2835_I2C_S_LEN;
				break;
			}

			bcm2835_fill_txfifo(i2c_dev);

			if (i2c_dev->num_msgs > 0 && !i2c_dev->msg_buf_remaining) {
				i2c_dev->curr_msg++;
				bcm2835_i2c_start_transfer(i2c_dev);
			}
		}

		if (val & BCM2835_I2C_S_RXR) {
			if (!i2c_dev->msg_buf_remaining) {
				i2c_dev->msg_err = val | BCM2835_I2C_S_LEN;
				break;
			}

			bcm2835_drain_rxfifo(i2c_dev);
		}
		udelay(1);
	}

	if (timeout <= 0) {

		bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, BCM2835_I2C_C_CLEAR);
		printf("bcm2835_i2c: transfer timeout\n");

		return -ETIMEDOUT;
	}

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, BCM2835_I2C_C_CLEAR);
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_S, BCM2835_I2C_S_CLKT |
			   	BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE);

	bcm2835_i2c_finish_transfer(i2c_dev);
	if (ignore_nak)
		i2c_dev->msg_err &= ~BCM2835_I2C_S_ERR;

	if (!i2c_dev->msg_err)
		return 0;

	if (i2c_dev->msg_err & BCM2835_I2C_S_ERR)
		return -EREMOTEIO;

	return -EIO;
}

static const struct dm_i2c_ops bcm2835_i2c_ops = {
	.xfer          = bcm2835_i2c_xfer,
	.set_bus_speed = bcm2835_i2c_set_rate,
};

static const struct udevice_id bcm2835_i2c_ids[] = {
	{.compatible = "brcm,bcm2711-i2c"},
	{.compatible = "brcm,bcm2835-i2c"},
	{}
};

static int bcm2835_i2c_probe(struct udevice *bus)
{
	struct bcm2835_i2c_priv *priv = dev_get_priv(bus);

	priv->regs = dev_read_addr_ptr(bus);
	if (!priv->regs) {
		printf("bcm2835_i2c: cannot get base address\n");
		return -EINVAL;
	}

	priv->bus_clk_rate = 100000;

	/* setup controller clock divider */
	bcm2835_i2c_set_rate(bus, priv->bus_clk_rate);

	/* clear FIFO + status */
	bcm2835_i2c_writel(priv,
			   BCM2835_I2C_C,
			   BCM2835_I2C_C_CLEAR);

	bcm2835_i2c_writel(priv,
			   BCM2835_I2C_S,
			   BCM2835_I2C_S_CLKT |
			   BCM2835_I2C_S_ERR |
			   BCM2835_I2C_S_DONE);
	
	return 0;
}

U_BOOT_DRIVER(bcm2835_i2c) = {
	.name       = "bcm2835_i2c",
	.id         = UCLASS_I2C,
	.of_match   = bcm2835_i2c_ids,
	.probe      = bcm2835_i2c_probe,
	.priv_auto  = sizeof(struct bcm2835_i2c_priv),
	.ops        = &bcm2835_i2c_ops,
};