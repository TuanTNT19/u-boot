// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Tuan Truong Nho <tuantnt08@gmail.com>
 */

#include <dm.h>
#include <clk.h>
#include <dma.h>
#include <log.h>
#include <malloc.h>
#include <miiphy.h>
#include <net.h>
#include <reset.h>
#include <wait_bit.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <linux/printk.h>
/* Register definitions derived from Linux source */
#define SYS_REV_CTRL			0x00

#define SYS_PORT_CTRL			0x04
#define PORT_MODE_EXT_GPHY		3

#define GENET_SYS_OFF			0x0000
#define SYS_RBUF_FLUSH_CTRL		(GENET_SYS_OFF  + 0x08)
#define SYS_TBUF_FLUSH_CTRL		(GENET_SYS_OFF  + 0x0c)

#define GENET_EXT_OFF			0x0080
#define EXT_RGMII_OOB_CTRL		(GENET_EXT_OFF + 0x0c)
#define RGMII_LINK			BIT(4)
#define OOB_DISABLE			BIT(5)
#define RGMII_MODE_EN			BIT(6)
#define ID_MODE_DIS			BIT(16)

#define GENET_RBUF_OFF			0x0300
#define RBUF_TBUF_SIZE_CTRL		(GENET_RBUF_OFF + 0xb4)
#define RBUF_CTRL			(GENET_RBUF_OFF + 0x00)
#define RBUF_ALIGN_2B			BIT(1)

#define GENET_UMAC_OFF			0x0800
#define UMAC_MIB_CTRL			(GENET_UMAC_OFF + 0x580)
#define UMAC_MAX_FRAME_LEN		(GENET_UMAC_OFF + 0x014)
#define UMAC_MAC0			(GENET_UMAC_OFF + 0x00c)
#define UMAC_MAC1			(GENET_UMAC_OFF + 0x010)
#define UMAC_CMD			(GENET_UMAC_OFF + 0x008)   // control register used t on/off TX/RX, MAC speed, reset MAC, loopback,...
#define MDIO_CMD			(GENET_UMAC_OFF + 0x614)
#define UMAC_TX_FLUSH			(GENET_UMAC_OFF + 0x334)
#define MDIO_START_BUSY			BIT(29)
#define MDIO_READ_FAIL			BIT(28)
#define MDIO_RD				(2 << 26)
#define MDIO_WR				BIT(26)
#define MDIO_PMD_SHIFT			21
#define MDIO_PMD_MASK			0x1f
#define MDIO_REG_SHIFT			16
#define MDIO_REG_MASK			0x1f

#define CMD_TX_EN			BIT(0)
#define CMD_RX_EN			BIT(1)
#define UMAC_SPEED_10			0
#define UMAC_SPEED_100			1
#define UMAC_SPEED_1000			2
#define UMAC_SPEED_2500			3
#define CMD_SPEED_SHIFT			2
#define CMD_SPEED_MASK			3
#define CMD_SW_RESET			BIT(13)
#define CMD_LCL_LOOP_EN			BIT(15)
#define CMD_TX_EN			BIT(0)
#define CMD_RX_EN			BIT(1)

#define MIB_RESET_RX			BIT(0)
#define MIB_RESET_RUNT			BIT(1)
#define MIB_RESET_TX			BIT(2)

/* total number of Buffer Descriptors, same for Rx/Tx */
#define TOTAL_DESCS			256
#define RX_DESCS			TOTAL_DESCS
#define TX_DESCS			TOTAL_DESCS

#define DEFAULT_Q			0x10

/* Body(1500) + EH_SIZE(14) + VLANTAG(4) + BRCMTAG(6) + FCS(4) = 1528.
 * 1536 is multiple of 256 bytes
 */
#define ENET_BRCM_TAG_LEN		6
#define ENET_PAD			8
#define ENET_MAX_MTU_SIZE		(ETH_DATA_LEN + ETH_HLEN +	 \
					 VLAN_HLEN + ENET_BRCM_TAG_LEN + \
					 ETH_FCS_LEN + ENET_PAD)

/* Tx/Rx Dma Descriptor common bits */
#define DMA_EN				BIT(0)
#define DMA_RING_BUF_EN_SHIFT		0x01
#define DMA_RING_BUF_EN_MASK		0xffff
#define DMA_BUFLENGTH_MASK		0x0fff
#define DMA_BUFLENGTH_SHIFT		16
#define DMA_RING_SIZE_SHIFT		16
#define DMA_OWN				0x8000
#define DMA_EOP				0x4000
#define DMA_SOP				0x2000
#define DMA_WRAP			0x1000
#define DMA_MAX_BURST_LENGTH		0x8
/* Tx specific DMA descriptor bits */
#define DMA_TX_UNDERRUN			0x0200
#define DMA_TX_APPEND_CRC		0x0040
#define DMA_TX_OW_CRC			0x0020
#define DMA_TX_DO_CSUM			0x0010
#define DMA_TX_QTAG_SHIFT		7

/* DMA rings size */
#define DMA_RING_SIZE			0x40
#define DMA_RINGS_SIZE			(DMA_RING_SIZE * (DEFAULT_Q + 1))

/* DMA descriptor */
#define DMA_DESC_LENGTH_STATUS		0x00
#define DMA_DESC_ADDRESS_LO		0x04
#define DMA_DESC_ADDRESS_HI		0x08
#define DMA_DESC_SIZE			12

#define GENET_RX_OFF			0x2000
#define GENET_RDMA_REG_OFF					\
	(GENET_RX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)
#define GENET_TX_OFF			0x4000
#define GENET_TDMA_REG_OFF					\
	(GENET_TX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)

#define DMA_FC_THRESH_HI		(RX_DESCS >> 4)
#define DMA_FC_THRESH_LO		5
#define DMA_FC_THRESH_VALUE		((DMA_FC_THRESH_LO << 16) |	\
					  DMA_FC_THRESH_HI)

#define DMA_XOFF_THRESHOLD_SHIFT	16

#define TDMA_RING_REG_BASE					\
	(GENET_TDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define TDMA_READ_PTR			(TDMA_RING_REG_BASE + 0x00)
#define TDMA_CONS_INDEX			(TDMA_RING_REG_BASE + 0x08)
#define TDMA_PROD_INDEX			(TDMA_RING_REG_BASE + 0x0c)
#define DMA_RING_BUF_SIZE		0x10
#define DMA_START_ADDR			0x14
#define DMA_END_ADDR			0x1c
#define DMA_MBUF_DONE_THRESH		0x24
#define TDMA_FLOW_PERIOD		(TDMA_RING_REG_BASE + 0x28)
#define TDMA_WRITE_PTR			(TDMA_RING_REG_BASE + 0x2c)

#define RDMA_RING_REG_BASE					\
	(GENET_RDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define RDMA_WRITE_PTR			(RDMA_RING_REG_BASE + 0x00)
#define RDMA_PROD_INDEX			(RDMA_RING_REG_BASE + 0x08)
#define RDMA_CONS_INDEX			(RDMA_RING_REG_BASE + 0x0c)
#define RDMA_XON_XOFF_THRESH		(RDMA_RING_REG_BASE + 0x28)
#define RDMA_READ_PTR			(RDMA_RING_REG_BASE + 0x2c)

#define TDMA_REG_BASE			(GENET_TDMA_REG_OFF + DMA_RINGS_SIZE)
#define RDMA_REG_BASE			(GENET_RDMA_REG_OFF + DMA_RINGS_SIZE)
#define DMA_RING_CFG			0x00
#define DMA_CTRL			0x04
#define DMA_SCB_BURST_SIZE		0x0c

#define RX_BUF_LENGTH			2048
#define RX_TOTAL_BUFSIZE		(RX_BUF_LENGTH * RX_DESCS)
#define RX_BUF_OFFSET			2

/* =========================================================
 * 1. Private data
 * =========================================================
 */

struct bcmgenet_eth_priv {
	char rxbuffer[RX_TOTAL_BUFSIZE] __aligned(ARCH_DMA_MINALIGN);
	void *mac_reg;
	void *tx_desc_base;
	void *rx_desc_base;
	int tx_index;
	int rx_index;
	int c_index;
	int phyaddr;
	u32 interface;
	u32 speed;
	struct phy_device *phydev;
	struct mii_dev *bus;
};

static int bcmgenet_interface_set(struct bcmgenet_eth_priv *priv) {
	switch (priv->interface)
	{
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
		writel(PORT_MODE_EXT_GPHY, priv->mac_reg + SYS_PORT_CTRL);
		break;
	default:
		printf("unknown phy mode: %d\n", priv->interface);
		break;
		return -EINVAL;
	}

	return 0;
}

// Start MDIO transaction
static void bcmgenet_mdio_start(struct bcmgenet_eth_priv *priv)
{
	u32 reg;

	reg = readl_relaxed (priv->mac_reg + MDIO_CMD);
	reg |= MDIO_START_BUSY;
	writel_relaxed (reg, priv->mac_reg + MDIO_CMD);
	// setbits_32(priv->mac_reg + MDIO_CMD, MDIO_START_BUSY);
}

// Read value in a PHY register via MDIO
static int bcmgenet_mdio_read (struct mii_dev *bus, int phy_id, int devad, int reg) {
	struct udevice *dev = bus->priv;
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	u32 val;
	int ret;

/*
Reading value in PHY register flow:
- create cmd for reading : MDIO_RD | (phy_id << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT);
- send cmd by writel_relaxed
- wait for reading
- read data in register MDIO_CMD
- take 16bit lower
*/

	val = MDIO_RD | (phy_id << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT);
	writel_relaxed (val, priv->mac_reg + MDIO_CMD);

	bcmgenet_mdio_start(priv);
	ret = wait_for_bit_32(priv->mac_reg + MDIO_CMD,
			      MDIO_START_BUSY, false, 20, true);
	if (ret)
		return ret;

	val = readl_relaxed (priv->mac_reg + MDIO_CMD);

	return (val & 0xffff);
}

static int bcmgenet_mdio_write (struct mii_dev *bus, int phy_id, int deval, int reg, u16 data) {
	struct udevice *dev = bus->priv;
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	u32 cmd;

	cmd = MDIO_WR | (phy_id << MDIO_PMD_SHIFT) |
		(reg << MDIO_REG_SHIFT) | (0xffff & data);

	writel_relaxed (cmd, priv->mac_reg + MDIO_CMD);

	bcmgenet_mdio_start(priv);

	return wait_for_bit_32(priv->mac_reg + MDIO_CMD,
			       MDIO_START_BUSY, false, 20, true);
}

static int bcmgenet_mdio_init(const char *name, struct udevice *priv) {
	struct mii_dev *bus;
	bus = mdio_alloc();
	if (!bus) {
		debug("Failed to allocate MDIO bus\n");
		return -ENOMEM;
	}

	snprintf(bus->name, sizeof(bus->name), name);
	bus->priv = (void *)priv;
	bus->write = bcmgenet_mdio_write;
	bus->read = bcmgenet_mdio_read;
	snprintf (bus->name, sizeof (bus->name), name);

	return mdio_register(bus);
}

static int bcmgenet_phy_init (struct bcmgenet_eth_priv *priv, void *dev) {
	struct phy_device *phydev;
	int ret;
	
	phydev = phy_connect(priv->bus, priv->phyaddr, dev, priv->interface);
	if (!phydev) {
		return -ENODEV;
	}

	phydev->supported &= PHY_GBIT_FEATURES; // Giữ lại trong phydev->supported chỉ những feature nằm trong tập PHY_GBIT_FEATURES.
	if (priv->speed) {
		ret = phy_set_supported(priv->phydev, priv->speed);
		if (ret)
			return ret;
	}
	phydev->advertising = phydev->supported;
	priv->phydev = phydev;

	phy_config(priv->phydev);

	return 0;
}

static int bcmgenet_eth_of_to_plat(struct udevice *dev) {
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args phy_node;
	int ret;

	pdata->iobase = dev_read_addr(dev); // đọc địa chỉ từ reg

	pdata->phy_interface = dev_read_phy_mode(dev); // đọc phy mode
	if (pdata->phy_interface == PHY_INTERFACE_MODE_NA)
		return -EINVAL;

	ret = dev_read_phandle_with_args(dev, "phy-handle", NULL, 0, 0,
					 &phy_node);  // tìm phy-handle --> phy_node = phy1
	if (!ret) {
		ofnode_read_s32(phy_node.node, "reg", &priv->phyaddr); // lấy ra reg của phy1
		ofnode_read_s32(phy_node.node, "max-speed", &pdata->max_speed);   // lấy ra max speed của phy1
	}	

	return 0;
}

/* =========================================================
 * 2. Probe
 * =========================================================
 */

static int bcmgenet_tnt_probe(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);

	ofnode mdio_node;
	const char *name;
	u32 reg;
	int ret;
	u8 major;
	
	priv->mac_reg = map_physmem (pdata->iobase, SZ_64K, MAP_NOCACHE);
	priv->interface = pdata->phy_interface;
	priv->speed = pdata->max_speed;

	/* Read GENET HW version */
	reg = readl_relaxed(priv->mac_reg + SYS_REV_CTRL);
	major = (reg >> 24 & 0x0f);
	if (major == 6)
		major = 5;
	else if (major == 5)
		major = 4;
	else if (major == 0)
		major = 1;
	else {
		printf("Unsupported GENETv%d.%d\n", major, (reg >> 16) & 0x0f);
		return -ENODEV;
	}

	ret = bcmgenet_interface_set (priv);
	if (ret) {
		return ret;
	}

	writel (0, priv->mac_reg + SYS_RBUF_FLUSH_CTRL); // clear Receiver buffer in GENET MAC
	udelay(10);

	/* disable MAC while updating its registers */
	writel(0, priv->mac_reg + UMAC_CMD); // Disable TX và RX của MAC.
	/* issue soft reset with (rg)mii loopback to ensure a stable rxclk */
	writel(CMD_SW_RESET | CMD_LCL_LOOP_EN, priv->mac_reg + UMAC_CMD);

	mdio_node = dev_read_first_subnode(dev);
	name = ofnode_get_name(mdio_node);

	ret = bcmgenet_mdio_init (name, dev);
	if (ret) {
		return ret;
	}

	priv->bus = miiphy_get_dev_by_name(name); // tìm một MDIO/MII bus đã được đăng ký trong U-Boot theo tên name (qua hàm bcmgenet_mdio_init), rồi lưu con trỏ tới bus đó vào priv->bus.
    // Bây h, priv->bus chính là mii_dev được đăng ký trong hàm bcmgenet_mdio_init
	
	return bcmgenet_phy_init(priv, dev);
}

/* =========================================================
 * 3. Remove
 * =========================================================
 */

static int my_remove(struct udevice *dev)
{
    printf("my_remove()\n");

    return 0;
}

/* =========================================================
 * 4. Operations (optional)
 * =========================================================
 */

// reset , set UMAC and Genet in default state
static void bcmgenet_umac_reset(struct bcmgenet_eth_priv *priv) {
	u32 reg;

	// Flush RX buffer
	reg = readl (priv->mac_reg + SYS_RBUF_FLUSH_CTRL);
	reg |= BIT(1); // reg |= ( 1UL << 1 )
	writel(reg, priv->mac_reg + SYS_RBUF_FLUSH_CTRL);
	udelay(10);

	// clear bits flush
	reg &= ~BIT(1);
	writel(reg, priv->mac_reg + SYS_RBUF_FLUSH_CTRL);
	udelay(10);

	// write 0 to full register
	writel(0, priv->mac_reg + SYS_RBUF_FLUSH_CTRL);
	udelay(10);

	// Disable MAC - TX/RX Disable 
	writel(0, priv->mac_reg + UMAC_CMD);

	// Soft reset UMAC
	writel(CMD_SW_RESET | CMD_LCL_LOOP_EN,
       priv->mac_reg + UMAC_CMD);
	udelay(2);
	writel(0, priv->mac_reg + UMAC_CMD);

	/* Clear MIB counters- - Management Information Base */
	writel(MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT,
       priv->mac_reg + UMAC_MIB_CTRL);
	writel(0, priv->mac_reg + UMAC_MIB_CTRL); // clear reset bit

	// Set maximum Ethernet frame length
	writel(ENET_MAX_MTU_SIZE, priv->mac_reg + UMAC_MAX_FRAME_LEN);

	/* init rx registers, enable ip header optimization */
	reg = readl(priv->mac_reg + RBUF_CTRL);
	reg |= RBUF_ALIGN_2B;
	writel(reg, (priv->mac_reg + RBUF_CTRL));

	writel(1, (priv->mac_reg + RBUF_TBUF_SIZE_CTRL));
}

// ghi MAC address của card Ethernet vào các thanh ghi phần cứng của GENET MAC.
static int bcmgenet_gmac_write_hwaddr(struct udevice *dev) {
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	struct eth_pdata *pdata = dev_get_plat(dev);
	uchar *addr = pdata->enetaddr; // get MAC address of ethernet card
	u32 reg;
/*
genet co 2 thanh ghi de chua MAC address la UMAC_MAC0 vs UMAC_MAC1
tong cong can chua 48bit = 6 byte

Nhung moi register co 32bit nen chia MAC thanh
MAC0 -> 4byte dau
MAc1-> 2 byte sau
*/

	reg = addr[0] << 24 | addr[1] << 16 | addr[2] << 8 | addr[3];
	writel_relaxed(reg, priv->mac_reg + UMAC_MAC0);

	reg = addr[4] << 8 | addr[5];
	writel_relaxed(reg, priv->mac_reg + UMAC_MAC1);

	return 0;
}

// Disable DMA of TX/RX and confirm all data in TZ queue will be flush clearly 
static void bcmgenet_disable_dma(struct bcmgenet_eth_priv *priv)
{
	clrbits_32(priv->mac_reg + TDMA_REG_BASE + DMA_CTRL, DMA_EN);
	clrbits_32(priv->mac_reg + RDMA_REG_BASE + DMA_CTRL, DMA_EN);

	writel(1, priv->mac_reg + UMAC_TX_FLUSH);
	udelay(10);
	writel(0, priv->mac_reg + UMAC_TX_FLUSH);
}

// Enable TX/RX DMA 
static void bcmgenet_enable_dma(struct bcmgenet_eth_priv *priv)
{
	u32 dma_ctrl = (1 << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)) | DMA_EN;

	writel(dma_ctrl, priv->mac_reg + TDMA_REG_BASE + DMA_CTRL);

	setbits_32(priv->mac_reg + RDMA_REG_BASE + DMA_CTRL, dma_ctrl);
}

// prepare DMA TX descriptor , thong bao den RAM
static int bcmgenet_gmac_eth_send(struct udevice *dev, void *packet, int length) {
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	// TX descriptor address
	void *desc_base = priv->tx_desc_base + priv->tx_index * DMA_DESC_SIZE;
	u32 len_stat = length << DMA_BUFLENGTH_SHIFT; // create length field
	ulong packet_aligned = rounddown((ulong)packet, ARCH_DMA_MINALIGN);
	u32 prod_index, cons;
	u32 tries = 100;

	prod_index = readl(priv->mac_reg + TDMA_PROD_INDEX); // get producer index

	flush_dcache_range(packet_aligned,
			   packet_aligned + roundup(length, ARCH_DMA_MINALIGN)); // đảm bảo dữ liệu packet từ cache được ghi xuống RAM trước khi DMA đọc.

	len_stat |= 0x3F << DMA_TX_QTAG_SHIFT;
	len_stat |= DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;

	/* Set-up packet for transmission */
	writel(lower_32_bits((ulong)packet), (desc_base + DMA_DESC_ADDRESS_LO)); // write 32 low bit packet to descriptor
	writel(upper_32_bits((ulong)packet), (desc_base + DMA_DESC_ADDRESS_HI)); // write 32 high bit packet to descriptor
	writel(len_stat, (desc_base + DMA_DESC_LENGTH_STATUS));	

// Finish setup TX descriptor
/*
TX Descriptor
┌─────────────────────────────┐
│ LENGTH                      │
│ SOP                         │
│ EOP                         │
│ APPEND_CRC                  │
│ QTAG                        │
├─────────────────────────────┤
│ ADDRESS_LO                  │
├─────────────────────────────┤
│ ADDRESS_HI                  │
└─────────────────────────────┘
*/
	/* Increment index and start transmission */
	if (++priv->tx_index >= TX_DESCS)
		priv->tx_index = 0;

	prod_index++;

	/* Start Transmisson */
	writel(prod_index, priv->mac_reg + TDMA_PROD_INDEX); // Notify to DMA

    // Wait DMA for finisj work
	do {
		cons = readl(priv->mac_reg + TDMA_CONS_INDEX);
	} while ((cons & 0xffff) < prod_index && --tries);
	if (!tries)
		return -ETIMEDOUT;

	return 0;
}


/* Check whether all cache lines affected by an invalidate are within
 * the buffer, to make sure we don't accidentally lose unrelated dirty
 * data stored nearby.
 * Alignment of the buffer start address will be checked in the implementation
 * of invalidate_dcache_range().
 */
static void invalidate_dcache_check(unsigned long addr, size_t size,
				    size_t buffer_size)
{
	size_t inval_size = roundup(size, ARCH_DMA_MINALIGN);

	if (unlikely(inval_size > buffer_size))
		printf("WARNING: Cache invalidate area exceeds buffer size\n");

	invalidate_dcache_range(addr, addr + inval_size);
}

static int bcmgenet_gmac_eth_recv(struct udevice *dev,
				  int flags, uchar **packetp)
{
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	void *desc_base = priv->rx_desc_base + priv->rx_index * DMA_DESC_SIZE;
	u32 prod_index = readl(priv->mac_reg + RDMA_PROD_INDEX);
	u32 length, addr;

	if (prod_index == priv->c_index) // check if have any new packet
		return -EAGAIN;

	length = readl(desc_base + DMA_DESC_LENGTH_STATUS);
	length = (length >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;
/*
for ex:
 DMA_DESC_LENGTH_STATUS reg
register
┌───────────────────────┬───────────────┐
│      LENGTH           │    STATUS     │
└───────────────────────┴───────────────┘
          ↑
       shift 16

*/	

	addr = readl(desc_base + DMA_DESC_ADDRESS_LO); // get packet address in RAM that DMA had written
	invalidate_dcache_check(addr, length, RX_BUF_LENGTH);

	*packetp = (uchar *)(ulong)addr + RX_BUF_OFFSET;
/*
RX buffer trong RAM

0x82000000
     │
     ├── n RX_BUF_OFFSET
     │
0x8200000n
     │
     ▼
┌─────────────────────────┐
│ Ethernet packet         │
│ Dest MAC                │
│ Src MAC                 │
│ EtherType               │
│ Payload                 │
│ FCS ...                 │
└─────────────────────────┘
*/

	return length;
}

// Đây là hàm hoàn tất việc xử lý một RX packet
static int bcmgenet_gmac_free_pkt(struct udevice *dev, uchar *packet,
				  int length)
{
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	void *desc_base = priv->rx_desc_base + priv->rx_index * DMA_DESC_SIZE;
	u32 addr = readl(desc_base + DMA_DESC_ADDRESS_LO); // // get packet address in RAM that DMA had written

	flush_dcache_range(addr, addr + RX_BUF_LENGTH);
/*
c_index and prod_index
- c_index = CPU đã tiêu thụ packet đến đâu. ( CPU xu ly xong descriptor nao r )
- prod_index = DMA đã sản xuất packet đến đâu.
VD: Ban dau, ca 2 index = 2
Sau do, DMA nhan them 3 package, --> prod_index = 2+3=5
tai day, c_index dang bang 2, co 3 packet dang cho xu ly, moi lan goi recv xong thi c_index + 1
*/

	/* Tell the MAC we have consumed that last receive buffer. */
	priv->c_index = (priv->c_index + 1) & 0xFFFF;
	writel(priv->c_index, priv->mac_reg + RDMA_CONS_INDEX); // Notify to DMA that this packet done

/* Forward our descriptor pointer, wrapping around if needed. */
	if (++priv->rx_index >= RX_DESCS)
		priv->rx_index = 0;

	return 0;
}


static void rx_descs_init(struct bcmgenet_eth_priv *priv)
{
	char *rxbuffs = &priv->rxbuffer[0];
	u32 len_stat, i;
	void *desc_base = priv->rx_desc_base;

	len_stat = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN;

	for (i = 0; i < RX_DESCS; i++) {
		writel(lower_32_bits((uintptr_t)&rxbuffs[i * RX_BUF_LENGTH]),
		       desc_base + i * DMA_DESC_SIZE + DMA_DESC_ADDRESS_LO);
		writel(upper_32_bits((uintptr_t)&rxbuffs[i * RX_BUF_LENGTH]),
		       desc_base + i * DMA_DESC_SIZE + DMA_DESC_ADDRESS_HI);
		writel(len_stat,
		       desc_base + i * DMA_DESC_SIZE + DMA_DESC_LENGTH_STATUS);
	}
}

static void rx_ring_init(struct bcmgenet_eth_priv *priv)
{
	writel(DMA_MAX_BURST_LENGTH,
	       priv->mac_reg + RDMA_REG_BASE + DMA_SCB_BURST_SIZE);

	writel(0x0, priv->mac_reg + RDMA_RING_REG_BASE + DMA_START_ADDR);
	writel(0x0, priv->mac_reg + RDMA_READ_PTR);
	writel(0x0, priv->mac_reg + RDMA_WRITE_PTR);
	writel(RX_DESCS * DMA_DESC_SIZE / 4 - 1,
	       priv->mac_reg + RDMA_RING_REG_BASE + DMA_END_ADDR);

	/* cannot init RDMA_PROD_INDEX to 0, so align RDMA_CONS_INDEX on it instead */
	priv->c_index = readl(priv->mac_reg + RDMA_PROD_INDEX);
	writel(priv->c_index, priv->mac_reg + RDMA_CONS_INDEX);
	priv->rx_index = priv->c_index;
	priv->rx_index &= 0xFF;
	writel((RX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH,
	       priv->mac_reg + RDMA_RING_REG_BASE + DMA_RING_BUF_SIZE);
	writel(DMA_FC_THRESH_VALUE, priv->mac_reg + RDMA_XON_XOFF_THRESH);
	writel(1 << DEFAULT_Q, priv->mac_reg + RDMA_REG_BASE + DMA_RING_CFG);
}

static void tx_ring_init(struct bcmgenet_eth_priv *priv)
{
	writel(DMA_MAX_BURST_LENGTH,
	       priv->mac_reg + TDMA_REG_BASE + DMA_SCB_BURST_SIZE);

	writel(0x0, priv->mac_reg + TDMA_RING_REG_BASE + DMA_START_ADDR);
	writel(0x0, priv->mac_reg + TDMA_READ_PTR);
	writel(0x0, priv->mac_reg + TDMA_WRITE_PTR);
	writel(TX_DESCS * DMA_DESC_SIZE / 4 - 1,
	       priv->mac_reg + TDMA_RING_REG_BASE + DMA_END_ADDR);
	/* cannot init TDMA_CONS_INDEX to 0, so align TDMA_PROD_INDEX on it instead */
	priv->tx_index = readl(priv->mac_reg + TDMA_CONS_INDEX);
	writel(priv->tx_index, priv->mac_reg + TDMA_PROD_INDEX);
	priv->tx_index &= 0xFF;
	writel(0x1, priv->mac_reg + TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH);
	writel(0x0, priv->mac_reg + TDMA_FLOW_PERIOD);
	writel((TX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH,
	       priv->mac_reg + TDMA_RING_REG_BASE + DMA_RING_BUF_SIZE);

	writel(1 << DEFAULT_Q, priv->mac_reg + TDMA_REG_BASE + DMA_RING_CFG);
}

/*
hàm đồng bộ cấu hình MAC GENET theo trạng thái thực tế của PHY.

PHY đã xác định được link đang chạy ở tốc độ nào + interface gì, 
 -> bcmgenet_adjust_link() lấy thông tin đó và cấu hình lại MAC GENET cho khớp.
*/
static int bcmgenet_adjust_link(struct bcmgenet_eth_priv *priv) {
	struct phy_device *phydev = priv->phydev;
	u32 speed;

	switch (phydev->speed) {
	case SPEED_1000:
		speed = UMAC_SPEED_1000;
		break;
	case SPEED_100:
		speed = UMAC_SPEED_100;
		break;
	case SPEED_10:
		speed = UMAC_SPEED_10;
		break;
	default:
		printf("bcmgenet: Unsupported PHY speed: %d\n", phydev->speed);
		return -EINVAL;
	}

	clrsetbits_32(priv->mac_reg + EXT_RGMII_OOB_CTRL, OOB_DISABLE, RGMII_LINK | RGMII_MODE_EN);


	if (phydev->interface == PHY_INTERFACE_MODE_RGMII ||
	    phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
		setbits_32(priv->mac_reg + EXT_RGMII_OOB_CTRL, ID_MODE_DIS); // set bit ID_MODE_DIS of register to 1
	
	writel(speed << CMD_SPEED_SHIFT, (priv->mac_reg + UMAC_CMD));
	
	return 0;
}


/* Start ethernet genet
*/
static int bcmgenet_gmac_eth_start(struct udevice *dev) {
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);
	int ret;
	
	priv->tx_desc_base = priv->mac_reg + GENET_TX_OFF;
	priv->rx_desc_base = priv->mac_reg + GENET_RX_OFF;

	bcmgenet_umac_reset(priv);
	bcmgenet_gmac_write_hwaddr(dev);


	/* Disable RX/TX DMA and flush TX queues */
	bcmgenet_disable_dma(priv);

	rx_ring_init(priv);
	rx_descs_init(priv);

	tx_ring_init(priv);

	/* Enable RX/TX DMA */
	bcmgenet_enable_dma(priv);


	/* read PHY properties over the wire from generic PHY set-up */
	ret = phy_startup(priv->phydev);
	if (ret) {
		printf("bcmgenet: PHY startup failed: %d\n", ret);
		return ret;
	}

    /* Update/Sync MAC followed PHY */
	ret = bcmgenet_adjust_link(priv);
	if (ret) {
		printf("bcmgenet: adjust PHY link failed: %d\n", ret);
		return ret;
	}

	/* Enable Rx/Tx */
	setbits_32(priv->mac_reg + UMAC_CMD, CMD_TX_EN | CMD_RX_EN);

	return 0;
}

static void bcmgenet_gmac_eth_stop(struct udevice *dev)
{
	struct bcmgenet_eth_priv *priv = dev_get_priv(dev);

	clrbits_32(priv->mac_reg + UMAC_CMD, CMD_TX_EN | CMD_RX_EN);

	bcmgenet_disable_dma(priv);
}

static const struct eth_ops bcmgenet_gmac_eth_ops_tnt = {
	.start                  = bcmgenet_gmac_eth_start,
	.write_hwaddr           = bcmgenet_gmac_write_hwaddr,
	.send                   = bcmgenet_gmac_eth_send,
	.recv                   = bcmgenet_gmac_eth_recv,
	.free_pkt               = bcmgenet_gmac_free_pkt,
	.stop                   = bcmgenet_gmac_eth_stop,
};

/* =========================================================
 * 5. Device Tree match table
 * =========================================================
 */

static const struct udevice_id bcmgenet_tnt_ids[] = {
    { .compatible = "brcm,bcm2711-genet-v5-tnt" },
    { }
};

/* =========================================================
 * 6. Register driver
 * =========================================================
 */

U_BOOT_DRIVER(bcmgenet_tnt) = { // tên symbol trong source C. Dat tuy y
    .name       = "bcmgenet_tnt", // tên driver trong Driver Model. Dat tuy y
    .id         = UCLASS_ETH, // list ID xem tai include/dm/uclass-id.h
    .of_match   = bcmgenet_tnt_ids,
	.of_to_plat = bcmgenet_eth_of_to_plat,
    .probe      = bcmgenet_tnt_probe,
    .ops        = &bcmgenet_gmac_eth_ops_tnt,
	.priv_auto	= sizeof(struct bcmgenet_eth_priv),
	.plat_auto	= sizeof(struct eth_pdata),
	.flags = DM_FLAG_ALLOC_PRIV_DMA,
};