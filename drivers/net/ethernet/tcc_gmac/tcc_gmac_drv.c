/*
 * linux/driver/net/tcc_gmac/tcc_gmac_drv.c
 * 	
 * Based on : STMMAC of STLinux 2.4
 * Author : Telechips <linux@telechips.com>
 * Created : June 22, 2010
 * Description : This is the driver for the Telechips MAC 10/100/1000 on-chip Ethernet controllers.  
 *               Telechips Ethernet IPs are built around a Synopsys IP Core.
 *
 * Copyright (C) 2010 Telechips
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */ 

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/phy.h>
#include <linux/interrupt.h>

#include <mach/bsp.h>

#include <linux/fs.h>
#include <linux/module.h>

#include "tcc_gmac_ctrl.h"
#include "tcc_gmac_drv.h"

#include <mach/tca_gmac.h>

#ifdef CONFIG_TCC_GMAC_PTP
#include <linux/net_tstamp.h>
#include "tcc_gmac_ptp.h"
#endif

#undef TCC_GMAC_DEBUG
//#define TCC_GMAC_DEBUG
#undef pr_debug
#define pr_debug(fmt, args...)	printk(fmt, ## args)

#define ETHERNET_DEV_NAME			"eth0"
#define MAC_ADDRESS_LEN				6

#ifdef TCC_GMAC_DEBUG


#define DBG(nlevel, klevel, fmt, args...) \
        ((void)(netif_msg_##nlevel(priv) && \
        printk(KERN_##klevel fmt, ## args)))
#else
#define DBG(nlevel, klevel, fmt, args...) do { } while (0)
#endif

#undef TCC_GMAC_RX_DEBUG
#ifdef TCC_GMAC_RX_DEBUG
#define RX_DBG(fmt, args...)  printk(fmt, ## args)
#else
#define RX_DBG(fmt, args...)  do { } while (0)
#endif

#undef TCC_GMAC_XMIT_DEBUG

#ifdef TCC_GMAC_TX_DEBUG
#define TX_DBG(fmt, args...)  printk(fmt, ## args)
#else
#define TX_DBG(fmt, args...)  do { } while (0)
#endif

#define TCC_GMAC_ALIGN(x) L1_CACHE_ALIGN(x)
#define JUMBO_LEN   9000

/* *****************************************
 *
 *         GMAC Module Parameters
 *
 * ****************************************/

/* Module parameters */
#define TX_TIMEO 5000 /* default 5 seconds */
static int watchdog = TX_TIMEO;
module_param(watchdog, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(watchdog, "Transmit timeout in milliseconds");

static int debug = -1;      /* -1: default, 0: no output, 16:  all */
module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Message Level (0: no output, 16: all)");

//#define DMA_TX_SIZE 16
//#define DMA_TX_SIZE 256
#define DMA_TX_SIZE 1024
static int dma_txsize = DMA_TX_SIZE;
module_param(dma_txsize, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dma_txsize, "Number of descriptors in the TX list");

//#define DMA_RX_SIZE 16
//#define DMA_RX_SIZE 256
#define DMA_RX_SIZE 1024

static int dma_rxsize = DMA_RX_SIZE;
module_param(dma_rxsize, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dma_rxsize, "Number of descriptors in the RX list");

static int flow_ctrl = FLOW_OFF;
module_param(flow_ctrl, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(flow_ctrl, "Flow control ability [on/off]");

static int pause = PAUSE_TIME;
module_param(pause, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(pause, "Flow Control Pause Time");

#define TC_DEFAULT 64
static int tc = TC_DEFAULT;
module_param(tc, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tc, "DMA threshold control value");

#define RX_NO_COALESCE  1   /* Always interrupt on completion */
#define TX_NO_COALESCE  -1  /* No moderation by default */

#define DMA_BUFFER_SIZE BUF_SIZE_2KiB

static int buf_sz = DMA_BUFFER_SIZE;
module_param(buf_sz, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(buf_sz, "DMA buffer size");

/* In case of Giga ETH, we can enable/disable the COE for the
 *  * transmit HW checksum computation.
 *   * Note that, if tx csum is off in HW, SG will be still supported. */
static int tx_coe = HW_CSUM;

module_param(tx_coe, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tx_coe, "GMAC COE type 2 [on/off]");

static const u32 default_msg_level = (NETIF_MSG_DRV | NETIF_MSG_PROBE |
				                      NETIF_MSG_LINK | NETIF_MSG_IFUP |
               					      NETIF_MSG_IFDOWN | NETIF_MSG_TIMER);

static int tcc_gmac_start_xmit(struct sk_buff *skb, struct net_device *dev);
static int tcc_gmac_start_xmit_ch(struct sk_buff *skb, struct net_device *dev, unsigned int ch);

int tcc_gmac_set_adjust_bandwidth_reservation(struct net_device *dev);

/* 
 * tcc_gmac_verify_args - verify the driver parameters.
 * Description: it verifies if some wrong parameter is passed to the driver.
 * Note that wrong parameters are replaced with the default values.
 */
static void tcc_gmac_verify_args(void)
{
    if (unlikely(watchdog < 0))
        watchdog = TX_TIMEO;
    if (unlikely(dma_rxsize < 0))
        dma_rxsize = DMA_RX_SIZE;
    if (unlikely(dma_txsize < 0))
        dma_txsize = DMA_TX_SIZE;
    if (unlikely((buf_sz < DMA_BUFFER_SIZE) || (buf_sz > BUF_SIZE_16KiB)))
        buf_sz = DMA_BUFFER_SIZE;
    if (unlikely(flow_ctrl > 1))
        flow_ctrl = FLOW_AUTO;
    else if (likely(flow_ctrl < 0))
        flow_ctrl = FLOW_OFF;
    if (unlikely((pause < 0) || (pause > 0xffff)))
        pause = PAUSE_TIME;

    return;
}

#if defined(TCC_GMAC_XMIT_DEBUG) || defined(TCC_GMAC_RX_DEBUG)
static void print_pkt(unsigned char *buf, int len)
{
	int j;
	pr_info("len = %d byte, buf addr: 0x%p", len, buf);
	for (j = 0; j < len; j++) {
		if ((j % 16) == 0)
			pr_info("\n %03x:", j);
		pr_info(" %02x", buf[j]);
	}
	pr_info("\n");
	return;
}
#endif

/* minimum number of free TX descriptors required to wake up TX process */
#define TCC_GMAC_TX_THRESH(x)	(x->dma_tx_size/4)

static inline u32 tcc_gmac_tx_avail(struct tcc_gmac_priv *priv, unsigned int ch)
{
	struct tx_dma_ch_t *dma = &priv->tx_dma_ch[ch];

	return dma->dirty_tx + dma->dma_tx_size - dma->cur_tx - 1;
}

/*
 * To perform emulated hardware segmentation on skb.
 */
static int tcc_gmac_sw_tso(struct tcc_gmac_priv *priv, struct sk_buff *skb, unsigned int ch)
{
	struct sk_buff *segs, *curr_skb;
	int gso_segs = skb_shinfo(skb)->gso_segs;
	struct netdev_queue *txq = netdev_get_tx_queue(priv->dev, ch);

	/* Estimate the number of fragments in the worst case */
	if (unlikely(tcc_gmac_tx_avail(priv, ch) < gso_segs)) {
		//netif_stop_queue(priv->dev);
		netif_tx_stop_queue(txq);
		TX_DBG(KERN_ERR "%s: TSO BUG! Tx Ring full when queue awake\n", __func__);
		if (tcc_gmac_tx_avail(priv, ch) < gso_segs)
			return NETDEV_TX_BUSY;

		//netif_wake_queue(priv->dev);
		netif_tx_wake_queue(txq);
	}
	TX_DBG("\ttcc_gmac_sw_tso: segmenting: skb %p (len %d)\n",
	       skb, skb->len);

	segs = skb_gso_segment(skb, priv->dev->features & ~NETIF_F_TSO);
	if (unlikely(IS_ERR(segs)))
		goto sw_tso_end;

	do {
		curr_skb = segs;
		segs = segs->next;
		TX_DBG("\t\tcurrent skb->len: %d, *curr %p,"
		       "*next %p\n", curr_skb->len, curr_skb, segs);
		curr_skb->next = NULL;
		tcc_gmac_start_xmit_ch(curr_skb, priv->dev, ch);
	} while (segs);

sw_tso_end:
	dev_kfree_skb(skb);

	return NETDEV_TX_OK;
}

/* *****************************************
 *
 *           GMAC DMA Funcs
 *
 * ****************************************/

/**
 * display_ring
 * @p: pointer to the ring.
 * @size: size of the ring.
 * Description: display all the descriptors within the ring.
 */
static void display_ring(struct dma_desc *p, int size)
{
	struct tmp_s {
		u64 a;
		unsigned int b;
		unsigned int c;
	};
	int i;
	for (i = 0; i < size; i++) {
		struct tmp_s *x = (struct tmp_s *)(p + i);
		pr_info("\t%d [0x%x]: DES0=0x%x DES1=0x%x BUF1=0x%x BUF2=0x%x",
		       i, (unsigned int)virt_to_phys(&p[i]),
		       (unsigned int)(x->a), (unsigned int)((x->a) >> 32),
		       x->b, x->c);
		pr_info("\n");
	}
}

static void init_tx_dma_desc_rings(struct net_device *dev, unsigned int ch)
{
	int i; 
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	struct tx_dma_ch_t *dma = &priv->tx_dma_ch[ch];
	unsigned int txsize = dma->dma_tx_size;

	dma->tx_skbuff = kmalloc(sizeof(struct sk_buff *) * txsize, GFP_KERNEL);
	dma->dma_tx = (struct dma_desc *)dma_alloc_coherent(priv->device,
									txsize * sizeof(struct dma_desc),
									&dma->dma_tx_phy, 
									GFP_KERNEL);

	if ((dma->dma_tx == NULL)) {
		pr_err("%s:ERROR allocating the DMA Tx/Rx desc\n", __func__);
		return;
	}

	/* TX INITIALIZATION */
	for (i = 0; i < txsize; i++) {
		dma->tx_skbuff[i] = NULL;
		dma->dma_tx[i].des2 = 0;
	}
	dma->dirty_tx = 0;
	dma->cur_tx = 0;
	
	printk("%s : ch %d txsize %d \n",__func__, ch, txsize);
	
	priv->hw->desc->init_tx_desc(dma->dma_tx, txsize);

	if (netif_msg_hw(priv)) {
		pr_info("ch %d TX descriptor ring:\n", ch);
		display_ring(dma->dma_tx, txsize);
	}
}

static void init_rx_dma_desc_rings(struct net_device *dev, unsigned int ch)
{
	int i; 
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	struct sk_buff *skb;
	unsigned int rxsize = priv->rx_dma_ch[ch].dma_rx_size;
	unsigned int bfsize = priv->dma_buf_sz;
	int buff2_needed = 0, dis_ic = 0;

	printk("%s : dev->mtu %d \n",__func__,dev->mtu);
    /* Set the Buffer size according to the MTU;
 	 * indeed, in case of jumbo we need to bump-up the buffer sizes.
 	 */
    if (unlikely(dev->mtu >= BUF_SIZE_8KiB))
        bfsize = BUF_SIZE_16KiB;
    else if (unlikely(dev->mtu >= BUF_SIZE_4KiB))
        bfsize = BUF_SIZE_8KiB;
    else if (unlikely(dev->mtu >= BUF_SIZE_2KiB))
        bfsize = BUF_SIZE_4KiB;
    else if (unlikely(dev->mtu >= DMA_BUFFER_SIZE))
        bfsize = BUF_SIZE_2KiB;
    else
        bfsize = DMA_BUFFER_SIZE;


	/* If the MTU exceeds 8k so use the second buffer in the chain */
	if (bfsize >= BUF_SIZE_8KiB)
		buff2_needed = 1;

	DBG(probe, INFO, "tcc_gmac: rxsize %d, bfsize %d\n",
	    rxsize, bfsize);


	priv->rx_dma_ch[ch].rx_skbuff_dma = kmalloc(rxsize * sizeof(dma_addr_t), GFP_KERNEL);
	priv->rx_dma_ch[ch].rx_skbuff = kmalloc(sizeof(struct sk_buff *) * rxsize, GFP_KERNEL);
	priv->rx_dma_ch[ch].dma_rx = (struct dma_desc *)dma_alloc_coherent(priv->device,
									rxsize * sizeof(struct dma_desc),
									&priv->rx_dma_ch[ch].dma_rx_phy,
									GFP_KERNEL);

	if ((priv->rx_dma_ch[ch].dma_rx == NULL) ) {
		pr_err("%s:ERROR allocating the DMA Tx/Rx desc\n", __func__);
		return;
	}

	DBG(probe, INFO, "tcc_gmac: SKB addresses:\n"
			 "skb\t\tskb data\tdma data\n");

	/* RX INITIALIZATION */
	for (i = 0; i < rxsize; i++) {
		struct dma_desc *p = priv->rx_dma_ch[ch].dma_rx + i;

		//skb = netdev_alloc_skb_ip_align(dev, bfsize);
		skb = netdev_alloc_skb(dev, bfsize);
		if (unlikely(skb == NULL)) {
			pr_err("%s: Rx init fails; skb is NULL\n", __func__);
			break;
		}
		skb_reserve(skb, NET_IP_ALIGN);

		priv->rx_dma_ch[ch].rx_skbuff[i] = skb;
		priv->rx_dma_ch[ch].rx_skbuff_dma[i] = dma_map_single(priv->device, skb->data,
						bfsize, DMA_FROM_DEVICE);

		p->des2 = priv->rx_dma_ch[ch].rx_skbuff_dma[i];
		if (unlikely(buff2_needed))
			p->des3 = p->des2 + BUF_SIZE_8KiB;
		DBG(probe, INFO, "[%p]\t[%p]\t[%x]\n", priv->rx_skbuff[i],
			priv->rx_skbuff[i]->data, priv->rx_skbuff_dma[i]);
	}
	priv->rx_dma_ch[ch].cur_rx = 0;
	priv->rx_dma_ch[ch].dirty_rx = (unsigned int)(i - rxsize);
	priv->dma_buf_sz = bfsize;
	buf_sz = bfsize;
	
	printk("%s : dirty_rx %d dma_buf_sz %d \n",__func__,priv->rx_dma_ch[ch].dirty_rx,buf_sz);

	pr_debug(">>> init_rx_desc: : before \n");

	/* Clear the Rx/Tx descriptors */
	priv->hw->desc->init_rx_desc(priv->rx_dma_ch[ch].dma_rx, rxsize, dis_ic);
	pr_debug(">>> init_rx_desc: : done \n");

	if (netif_msg_hw(priv)) {
		pr_info("RX descriptor ring:\n");
		display_ring(priv->rx_dma_ch[ch].dma_rx, rxsize);
	}

}

void dump_ptp_regs(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;

	printk("--- IEEE 1588 Regs ---\n");
	printk("Time Stamp ctrl		: 0x%08x\n", readl(ioaddr+GMAC_TIMESTAMP_CONTROL));
	printk("Sub-Second Increment	: 0x%08x\n", readl(ioaddr+GMAC_SUB_SECOND_INCREMENT));

	printk("System Time - s		: 0x%08x\n", readl(ioaddr+GMAC_SYSTIME_SECONDS));
	printk("System Time - ns		: 0x%08x\n", readl(ioaddr+GMAC_SYSTIME_NANO_SECONDS));

	printk("System Time - higher s	: 0x%08x\n", readl(ioaddr+GMAC_SYSTIME_HIGHER_WORD_SECONDS));
	printk("System Time - s Up	: 0x%08x\n", readl(ioaddr+GMAC_SYSTIME_SECONDS_UPDATE));
	printk("System Time - ns Up	: 0x%08x\n", readl(ioaddr+GMAC_SYSTIME_NANO_SECONDS_UPDATE));

	printk("TimeStamp Addend 	: 0x%08x\n", readl(ioaddr+GMAC_TIMESTAMP_ADDEND));
	printk("Target Time - s		: 0x%08x\n", readl(ioaddr+GMAC_TARGET_TIME_SECONDS));
	printk("Target Time - ns		: 0x%08x\n", readl(ioaddr+GMAC_TARGET_TIME_NANO_SECONDS));
	printk("TimeStamp Status 	: 0x%08x\n", readl(ioaddr+GMAC_TIMESTAMP_STATUS));

	printk("Auxiliary - ns		: 0x%08x\n", readl(ioaddr+GMAC_AUXILIARY_TIMESTAMP_NANO_SECONDS));
	printk("Auxiliary - s		: 0x%08x\n", readl(ioaddr+GMAC_AUXILIARY_TIMESTAMP_SECONDS));
	printk("PPS Control		: 0x%08x\n", readl(ioaddr+GMAC_PPS_CONTROL));
	printk("PPS0 Interval		: 0x%08x\n", readl(ioaddr+GMAC_PPS0_INTERVAL));
	printk("PPS0 Width		: 0x%08x\n", readl(ioaddr+GMAC_PPS0_WIDTH));

	printk("PPS1 Target Time -s 	: 0x%08x\n", readl(ioaddr+GMAC_PPS1_TARGET_TIME_SECONDS));
	printk("PPS1 Target Time -ns	: 0x%08x\n", readl(ioaddr+GMAC_PPS1_TARGET_TIME_NANO_SECONDS));
	printk("PPS1 Interval		: 0x%08x\n", readl(ioaddr+GMAC_PPS1_INTERVAL));

	printk("PPS2 Target Time -s 	: 0x%08x\n", readl(ioaddr+GMAC_PPS2_TARGET_TIME_SECONDS));
	printk("PPS2 Target Time -ns	: 0x%08x\n", readl(ioaddr+GMAC_PPS2_TARGET_TIME_NANO_SECONDS));
	printk("PPS2 Interval		: 0x%08x\n", readl(ioaddr+GMAC_PPS2_INTERVAL));

	printk("PPS3 Target Time -s 	: 0x%08x\n", readl(ioaddr+GMAC_PPS3_TARGET_TIME_SECONDS));
	printk("PPS3 Target Time -ns	: 0x%08x\n", readl(ioaddr+GMAC_PPS3_TARGET_TIME_NANO_SECONDS));
	printk("PPS3 Interval		: 0x%08x\n", readl(ioaddr+GMAC_PPS3_INTERVAL));
}

/*
 * init_dma_desc_rings - init the RX/TX descriptor rings
 * @dev: net device structure
 * Description:  this function initializes the DMA RX/TX descriptors
 * and allocates the socket buffers.
 */
static void init_dma_desc_rings(struct net_device *dev)
{
	int i;

	for (i=0; i < NUMS_OF_DMA_CH; i++) {
		init_rx_dma_desc_rings(dev, i);
		init_tx_dma_desc_rings(dev, i);
	}
	return;
}

/**
 *  tcc_gmac_dma_operation_mode - HW DMA operation mode
 *  @priv : pointer to the private device structure.
 *  Description: it sets the DMA operation mode: tx/rx DMA thresholds
 *  or Store-And-Forward capability. It also verifies the COE for the
 *  transmission in case of Giga ETH.
 */
static void tcc_gmac_dma_operation_mode(struct tcc_gmac_priv *priv, unsigned int ch)
{
	if ((priv->dev->mtu <= ETH_DATA_LEN) && (tx_coe)) {
		priv->hw->dma[ch]->dma_mode(priv->dev->base_addr, SF_DMA_MODE, SF_DMA_MODE);
		tc = SF_DMA_MODE;
		priv->tx_coe = HW_CSUM;
	} else {
		/* Checksum computation is performed in software. */
		priv->hw->dma[ch]->dma_mode(priv->dev->base_addr, tc, SF_DMA_MODE);
		priv->tx_coe = NO_HW_CSUM;
	}
	tx_coe = priv->tx_coe;

	return;
}

/* *****************************************
 *
 *           GMAC MAC Ctrl Funcs
 *
 * ****************************************/

static inline void tcc_gmac_enable_rx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + MAC_CTRL_REG);
	value |= MAC_RNABLE_RX;
	/* Set the RE (receive enable bit into the MAC CTRL register).  */
	writel(value, ioaddr + MAC_CTRL_REG);
}

static inline void tcc_gmac_enable_tx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + MAC_CTRL_REG);
	value |= MAC_ENABLE_TX;
	/* Set the TE (transmit enable bit into the MAC CTRL register).  */
	writel(value, ioaddr + MAC_CTRL_REG);
}

static inline void tcc_gmac_disable_rx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + MAC_CTRL_REG);
	value &= ~MAC_RNABLE_RX;
	writel(value, ioaddr + MAC_CTRL_REG);
}

static inline void tcc_gmac_disable_tx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + MAC_CTRL_REG);
	value &= ~MAC_ENABLE_TX;
	writel(value, ioaddr + MAC_CTRL_REG);
}

static inline void tcc_gmac_enable_irq(struct tcc_gmac_priv *priv, unsigned int ch)
{
	priv->hw->dma[ch]->enable_dma_irq(priv->dev->base_addr);
}

static inline void tcc_gmac_disable_irq(struct tcc_gmac_priv *priv, unsigned int ch)
{
	priv->hw->dma[ch]->disable_dma_irq(priv->dev->base_addr);
}

static int tcc_gmac_has_work(struct tcc_gmac_priv *priv)
{
    unsigned int has_work = 0;
    int rxret, tx_work = 0, rx_work = 0;
	struct tx_dma_ch_t *tx_dma;
	struct rx_dma_ch_t *rx_dma;
	int i;
	

	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		rx_dma = &priv->rx_dma_ch[i];
		rxret = priv->hw->desc->get_rx_owner(rx_dma->dma_rx + 
								(rx_dma->cur_rx % rx_dma->dma_rx_size));

		if (likely(!rxret)) {
			rx_work = 1;
		}

		if (rx_work)
			break;
	}

	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		tx_dma = &priv->tx_dma_ch[i];
    	if (tx_dma->dirty_tx != tx_dma->cur_tx)
			tx_work = 1;

		if (tx_work)
			break;
	}

    if (likely(rx_work || tx_work))
        has_work = 1;

    return has_work;
}

static inline void _tcc_gmac_schedule(struct tcc_gmac_priv *priv)
{
	int i;

	if (likely(tcc_gmac_has_work(priv))) {
		for (i=0; i < NUMS_OF_DMA_CH; i++) {
			tcc_gmac_disable_irq(priv, i);
		}
		napi_schedule(&priv->napi);
	}
}

static void dma_free_rx_skbufs(struct tcc_gmac_priv *priv, unsigned int ch)
{
	int i;

	for (i = 0; i < priv->rx_dma_ch[ch].dma_rx_size; i++) {
		if (priv->rx_dma_ch[ch].rx_skbuff[i]) {
			dma_unmap_single(priv->device, priv->rx_dma_ch[ch].rx_skbuff_dma[i],
					priv->dma_buf_sz, DMA_FROM_DEVICE);
			dev_kfree_skb_any(priv->rx_dma_ch[ch].rx_skbuff[i]);
		}
		priv->rx_dma_ch[ch].rx_skbuff[i] = NULL;
	}
	return;
}

static void dma_free_tx_skbufs(struct tcc_gmac_priv *priv, unsigned int ch)
{
	int i;
	struct tx_dma_ch_t *dma = &priv->tx_dma_ch[ch];

	for (i = 0; i < dma->dma_tx_size; i++) {
		if (dma->tx_skbuff[i] != NULL) {
			struct dma_desc *p = dma->dma_tx + i;
			if (p->des2)
				dma_unmap_single(priv->device, p->des2,
						priv->hw->desc->get_tx_len(p),
						DMA_TO_DEVICE);
			dev_kfree_skb_any(dma->tx_skbuff[i]);
			dma->tx_skbuff[i] = NULL;
		}
	}
	return;
}

static void free_dma_desc_resources(struct tcc_gmac_priv *priv)
{
	int i;
	struct tx_dma_ch_t *tx_dma;
	struct rx_dma_ch_t *rx_dma;

	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		/* Release the DMA TX/RX socket buffers */
		dma_free_rx_skbufs(priv, i);
		dma_free_tx_skbufs(priv, i);

		/* Free the region of consistent memory previously allocated for the DMA */
		tx_dma = &priv->tx_dma_ch[i];
		rx_dma = &priv->rx_dma_ch[i];

		dma_free_coherent(priv->device,
					tx_dma->dma_tx_size * sizeof(struct dma_desc),
					tx_dma->dma_tx, tx_dma->dma_tx_phy);
		kfree(tx_dma->tx_skbuff);

		dma_free_coherent(priv->device,
						rx_dma->dma_rx_size * sizeof(struct dma_desc),
						rx_dma->dma_rx, rx_dma->dma_rx_phy);

		kfree(rx_dma->rx_skbuff_dma);
		kfree(rx_dma->rx_skbuff);
	}

	return;
}

static unsigned int tcc_gmac_handle_jumbo_frames(struct sk_buff *skb,
                           struct net_device *dev,
                           int csum_insertion,
						   unsigned int ch)
{
    struct tcc_gmac_priv *priv = netdev_priv(dev);
    unsigned int nopaged_len = skb_headlen(skb);
	struct tx_dma_ch_t *dma = &priv->tx_dma_ch[ch];
    unsigned int txsize = dma->dma_tx_size;
    unsigned int entry = dma->cur_tx % txsize;
    struct dma_desc *desc = dma->dma_tx + entry;

    if (nopaged_len > BUF_SIZE_8KiB) {

        int buf2_size = nopaged_len - BUF_SIZE_8KiB;

        desc->des2 = dma_map_single(priv->device, skb->data,
                        BUF_SIZE_8KiB, DMA_TO_DEVICE);
        desc->des3 = desc->des2 + BUF_SIZE_4KiB;
        priv->hw->desc->prepare_tx_desc(desc, 1, BUF_SIZE_8KiB,
                        csum_insertion);

        entry = (++dma->cur_tx) % txsize;
        desc = dma->dma_tx + entry;

        desc->des2 = dma_map_single(priv->device,
                    skb->data + BUF_SIZE_8KiB,
                    buf2_size, DMA_TO_DEVICE);
        desc->des3 = desc->des2 + BUF_SIZE_4KiB;
        priv->hw->desc->prepare_tx_desc(desc, 0, buf2_size,
                        csum_insertion);
        priv->hw->desc->set_tx_owner(desc);
        dma->tx_skbuff[entry] = NULL;
    } else {
        desc->des2 = dma_map_single(priv->device, skb->data,
                    nopaged_len, DMA_TO_DEVICE);
        desc->des3 = desc->des2 + BUF_SIZE_4KiB;
        priv->hw->desc->prepare_tx_desc(desc, 1, nopaged_len,
                        csum_insertion);
    }
    return entry;
}


/**
 * tcc_gmac_tx:
 * @priv: private driver structure
 * Description: it reclaims resources after transmission completes.
 */
static void tcc_gmac_tx(struct tcc_gmac_priv *priv, unsigned int ch)
{
	struct tx_dma_ch_t *tx_dma = &priv->tx_dma_ch[ch];
	struct rx_dma_ch_t *rx_dma = &priv->rx_dma_ch[ch];
    unsigned int txsize = tx_dma->dma_tx_size;
    unsigned long ioaddr = priv->dev->base_addr;
	struct netdev_queue *txq = netdev_get_tx_queue(priv->dev, ch);
	//unsigned long flags;

	//spin_lock_irqsave(&priv->lock, flags);

	while (tx_dma->dirty_tx != tx_dma->cur_tx) {
		int last;
		unsigned int entry = tx_dma->dirty_tx % txsize;
		struct sk_buff *skb = tx_dma->tx_skbuff[entry];
		struct dma_desc *p = tx_dma->dma_tx + entry;

		/* Check if the descriptor is owned by the DMA. */
		if (priv->hw->desc->get_tx_owner(p)) 
			break;
	

#ifdef CONFIG_TCC_GMAC_PTP
		if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS)) {
			struct skb_shared_hwtstamps shhwtstamps;

			memset(&shhwtstamps, 0, sizeof(shhwtstamps));
			shhwtstamps.hwtstamp.tv.sec = p->des7;
			shhwtstamps.hwtstamp.tv.nsec = p->des6;
			skb_tstamp_tx(skb, &shhwtstamps);
		}
#endif

		/* Verify tx error by looking at the last segment */
		last = priv->hw->desc->get_tx_ls(p);
		if (likely(last)) { 
			int tx_error = priv->hw->desc->tx_status(&priv->dev->stats, 
													&priv->xstats,
													p,
													ioaddr);
			if (likely(tx_error == 0)) {
				priv->dev->stats.tx_packets++;
				priv->xstats.tx_pkt_n++;
			} else
				priv->dev->stats.tx_errors++;
		}

		TX_DBG("ch : %d, %s: curr %d, dirty %d\n", __func__, ch, tx_dma->cur_tx, tx_dma->dirty_tx);

		if (likely(p->des2))
			dma_unmap_single(priv->device, p->des2,
							priv->hw->desc->get_tx_len(p),
							DMA_TO_DEVICE);
		if (unlikely(p->des3))
				p->des3 = 0;

		if (likely(skb != NULL)) {
			/*
			 * If there's room in the queue (limit it to size)
			 * we add this skb back into the pool,
			 * if it's the right size.
			 */
			if ((skb_queue_len(&priv->rx_recycle) < rx_dma->dma_rx_size) &&
				skb_recycle_check(skb, priv->dma_buf_sz))
				__skb_queue_head(&priv->rx_recycle, skb);
			else
				dev_kfree_skb(skb);

			tx_dma->tx_skbuff[entry] = NULL;
		}

		//priv->hw->desc->release_tx_desc(p);
		entry = (++tx_dma->dirty_tx) % txsize;
	}
	
	//if (unlikely(netif_queue_stopped(priv->dev) &&
	if (unlikely(netif_tx_queue_stopped(txq) &&
		tcc_gmac_tx_avail(priv, ch) > TCC_GMAC_TX_THRESH(tx_dma))) {
		netif_tx_lock(priv->dev);
		//if (netif_queue_stopped(priv->dev) &&
		if (netif_tx_queue_stopped(txq) &&
			tcc_gmac_tx_avail(priv, ch) > TCC_GMAC_TX_THRESH(tx_dma)) {
			TX_DBG("%s: restart transmit\n", __func__);
			//netif_wake_queue(priv->dev);
			netif_tx_wake_queue(txq);
		}
		netif_tx_unlock(priv->dev);
	}
	//spin_unlock_irqrestore(&priv->lock, flags);
}

/**
 * tcc_gmac_tx_err:
 * @priv: pointer to the private device structure
 * Description: it cleans the descriptors and restarts the transmission
 * in case of errors.
 */ 
static void tcc_gmac_tx_err(struct tcc_gmac_priv *priv)
{
	struct tx_dma_ch_t *dma;
	int i;

	netif_stop_queue(priv->dev);
	printk(" %s :  ",__func__);

	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		priv->hw->dma[i]->stop_tx(priv->dev->base_addr);

		dma  = &priv->tx_dma_ch[i];
		dma_free_tx_skbufs(priv, i);
		priv->hw->desc->init_tx_desc(dma->dma_tx, dma->dma_tx_size);
		dma->dirty_tx = 0;
		dma->cur_tx = 0;

		priv->hw->dma[i]->start_tx(priv->dev->base_addr);
	}

	priv->dev->stats.tx_errors++;
	netif_wake_queue(priv->dev);

	return;
}


static inline void tcc_gmac_rx_refill(struct tcc_gmac_priv *priv, unsigned int ch)
{
	struct rx_dma_ch_t *dma = &priv->rx_dma_ch[ch];
	unsigned int rxsize = dma->dma_rx_size;
	int bfsize = priv->dma_buf_sz;
	struct dma_desc *p = dma->dma_rx;

	for (; dma->cur_rx - dma->dirty_rx > 0; dma->dirty_rx++) {
		unsigned int entry = dma->dirty_rx % rxsize;
		if (likely(dma->rx_skbuff[entry] == NULL)) {
			struct sk_buff *skb;

			skb = __skb_dequeue(&priv->rx_recycle);
			if (skb == NULL) {
				//skb = netdev_alloc_skb_ip_align(priv->dev, bfsize);
				skb = netdev_alloc_skb(priv->dev, bfsize);
			if (unlikely(skb == NULL))
				break;

				skb_reserve(skb, NET_IP_ALIGN);
			}

			dma->rx_skbuff[entry] = skb;
			dma->rx_skbuff_dma[entry] =
				dma_map_single(priv->device, skb->data, bfsize,
						DMA_FROM_DEVICE);

			(p + entry)->des2 = dma->rx_skbuff_dma[entry];
			if (bfsize >= BUF_SIZE_8KiB)
				(p + entry)->des3 = (p + entry)->des2 + BUF_SIZE_8KiB;

			RX_DBG(KERN_INFO "\trefill entry #%d\n", entry);
		}
		wmb();
		priv->hw->desc->set_rx_owner(p + entry);
	}
	return;
}

static int tcc_gmac_rx(struct tcc_gmac_priv *priv, int limit, unsigned int ch)
{
	struct rx_dma_ch_t *dma = &priv->rx_dma_ch[ch];
	unsigned int rxsize = dma->dma_rx_size;
	unsigned int entry = dma->cur_rx % rxsize;
	unsigned int next_entry;
	unsigned int count = 0;
	struct dma_desc *p = dma->dma_rx + entry;
	struct dma_desc *p_next;

#ifdef TCC_GMAC_RX_DEBUG
	if (netif_msg_hw(priv)) {
		pr_debug(">>> tcc_gmac_rx: descriptor ring:\n");
		display_ring(priv->dma_rx, rxsize);
	}
#endif

	count = 0;
	while (!priv->hw->desc->get_rx_owner(p)) {
		int status;

		if (count >= limit)
			break;

		count++;

		next_entry = (++dma->cur_rx) % rxsize;
		p_next = dma->dma_rx + next_entry;
		prefetch(p_next);

		/* read the status of the incoming frame */
		status = (priv->hw->desc->rx_status(&priv->dev->stats, &priv->xstats, p));

		if (unlikely(status == discard_frame))
			priv->dev->stats.rx_errors++;
		else {
			struct sk_buff *skb;
			/* Length should omit the CRC */
			int frame_len = priv->hw->desc->get_rx_frame_len(p) - 4;

#ifdef TCC_GMAC_RX_DEBUG
			if (frame_len > ETH_FRAME_LEN)
				pr_debug("\tRX frame size %d, COE status: %d\n",
						frame_len, status);

			if (netif_msg_hw(priv))
				pr_debug("\tdesc: %p [entry %d] buff=0x%x\n",
						p, entry, p->des2);
#endif
			skb = dma->rx_skbuff[entry];
			if (unlikely(!skb)) {
				pr_err("%s: Inconsistent Rx descriptor chain\n",
						priv->dev->name);
				priv->dev->stats.rx_dropped++;
				break;
			}
			prefetch(skb->data - NET_IP_ALIGN);
			dma->rx_skbuff[entry] = NULL;

			skb_put(skb, frame_len);
			dma_unmap_single(priv->device,
					dma->rx_skbuff_dma[entry],
					priv->dma_buf_sz, DMA_FROM_DEVICE);

#ifdef TCC_GMAC_RX_DEBUG
			if (netif_msg_pktdata(priv)) {
				pr_info(" frame received (%dbytes)", frame_len);
					print_pkt(skb->data, frame_len);
			}
#endif
			skb->protocol = eth_type_trans(skb, priv->dev);

#ifdef CONFIG_TCC_GMAC_PTP
			{
				struct skb_shared_hwtstamps *shhwtstamps = skb_hwtstamps(skb);
				memset(shhwtstamps, 0, sizeof(*shhwtstamps));
				shhwtstamps->hwtstamp.tv.sec = p->des7;
				shhwtstamps->hwtstamp.tv.nsec = p->des6;
			}
#endif

			if (unlikely(status == csum_none)) {
				/* always for the old mac 10/100 */
				skb->ip_summed = CHECKSUM_NONE;
				netif_receive_skb(skb);
			} else {
				skb->ip_summed = CHECKSUM_UNNECESSARY;
				napi_gro_receive(&priv->napi, skb);
				//netif_receive_skb(skb);	
			}

			priv->dev->stats.rx_packets++;
			priv->dev->stats.rx_bytes += frame_len;
			priv->dev->last_rx = jiffies;
		}
		entry = next_entry;
		p = p_next; /* use prefetched values */
	}

	tcc_gmac_rx_refill(priv, ch);

	priv->xstats.rx_pkt_n += count;

	return count;
}

static void tcc_gmac_dma_interrupt(struct tcc_gmac_priv *priv)
{
	unsigned long ioaddr = priv->dev->base_addr;
	int status[NUMS_OF_DMA_CH];

#ifndef CONFIG_TCC_GMAC_FQTSS_SUPPORT
	status[0] = priv->hw->dma[0]->dma_interrupt(ioaddr, &priv->xstats);
	if (likely(status[0] == handle_tx_rx)) {
		_tcc_gmac_schedule(priv);
	}

	if (unlikely(status[0] == tx_hard_error_bump_tc)) {
		printk("tx_hard_error_bump_tc\n");
		/* Try to bump up the dma threshold on this failure */
		if (unlikely(tc != SF_DMA_MODE) && (tc <= 256)) {
			tc += 64;
			priv->hw->dma[0]->dma_mode(ioaddr, tc, SF_DMA_MODE);
			priv->xstats.threshold = tc;
		}
		tcc_gmac_tx_err(priv);
	} else if (unlikely(status[0] == tx_hard_error)) {
		printk("tx_hard_error\n");
		tcc_gmac_tx_err(priv);
	}
#else
	int i;

	for (i=0; i < NUMS_OF_DMA_CH; i++) {
		status[i] = priv->hw->dma[i]->dma_interrupt(ioaddr, &priv->xstats);
		if (likely(status[i] == handle_tx_rx)) {
			_tcc_gmac_schedule(priv);
			break;
		}
	}
#endif
	return;
}
/* *****************************************
 *
 *           GMAC MDIO BUS Funcs
 *
 * ****************************************/

/**
 * tcc_gmac_mdio_read
 * @bus: points to the mii_bus structure
 * @mii_id: MII addr reg bits 15-11
 * @regnum: MII addr reg bits 10-6
 * Description: it reads data from the MII register from within the phy device.
 * For the GMAC, we must set the bit 0 in the MII address register while
 * accessing the PHY registers.
 */
static int tcc_gmac_mdio_read(struct mii_bus *bus, int mii_id, int regnum)
{
	struct net_device *dev = bus->priv;
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	unsigned int mii_address = priv->hw->mii.addr;
	unsigned int mii_data = priv->hw->mii.data;
	unsigned int clk_rate = priv->hw->clk_rate;

	u32 addr;
	int data = 0;

	addr = ((mii_id<< GMII_ADDR_SHIFT) & GPHY_ADDR_MASK) |
		((regnum<< GMII_REG_SHIFT) & GMII_REG_MSK);

	writel((addr | clk_rate | GMAC_MII_ADDR_BUSY), ioaddr + mii_address);
	while(readl(ioaddr + mii_address) & GMAC_MII_ADDR_BUSY); // Wait for Busy Cleared
	data = (int)readl(ioaddr + mii_data); 

	//printk("%s - id : %d, regnum : 0x%02x, data : 0x%02x\n", __func__, mii_id, regnum, data);

	return data;
}

/**
 * tcc_gmac_mdio_write
 * @bus: points to the mii_bus structure
 * @mii_id: MII addr reg bits 15-11
 * @regnum: MII addr reg bits 10-6
 * @value: phy data
 * Description: it writes the data into the MII register from within the device.
 */
static int tcc_gmac_mdio_write(struct mii_bus *bus, int mii_id, int regnum, u16 value)
{
	struct net_device *dev = bus->priv;
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	unsigned int mii_address = priv->hw->mii.addr;
	unsigned int mii_data = priv->hw->mii.data;
	unsigned int clk_rate = priv->hw->clk_rate;
	unsigned long ioaddr = dev->base_addr;
	u32 addr;

	writel((unsigned int )value, ioaddr + mii_data);

	addr = ((mii_id<< GMII_ADDR_SHIFT) & GPHY_ADDR_MASK) |
		((regnum<< GMII_REG_SHIFT) & GMII_REG_MSK) | GMAC_MII_ADDR_WRITE;

	writel((addr | clk_rate | GMAC_MII_ADDR_BUSY), ioaddr + mii_address);

	while(readl(ioaddr + mii_address) & GMAC_MII_ADDR_BUSY); // Wait for Busy Cleared

	//printk("%s - id : %d, regnum : 0x%02x, value : 0x%02x\n", __func__, mii_id, regnum, value);

	return 0;
}

static int tcc_gmac_mdio_reset(struct mii_bus *bus)
{
//	tca_gmac_phy_reset();

	return 0;
}

static void tcc_gmac_adjust_link(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	struct phy_device *phydev = priv->phydev;
	unsigned long ioaddr = dev->base_addr;
	unsigned long flags;
	int new_state = 0;
	unsigned int fc = priv->flow_ctrl, pause_time = priv->pause;

	if (phydev == NULL)
		return;
	
	spin_lock_irqsave(&priv->lock, flags);
	if (phydev->link) {
		u32 ctrl = readl(ioaddr + MAC_CTRL_REG);

		/* Now we make sure that we can be in full duplex mode.
		 * If not, we operate in half-duplex mode. */
		if (phydev->duplex != priv->oldduplex) {
			new_state = 1;
			if (!(phydev->duplex))
				ctrl &= ~priv->hw->link.duplex;
			else
				ctrl |= priv->hw->link.duplex;
			priv->oldduplex = phydev->duplex;
		}

		/* Flow Control operation */
		if (phydev->pause)
			priv->hw->mac->flow_ctrl(ioaddr, phydev->duplex, fc, pause_time);

		if (phydev->speed != priv->speed) {
			
			new_state = 1;
			switch (phydev->speed) {
			case 1000:
				tcc_gmac_set_clk_rate(priv, 1250000);
				ctrl &= ~priv->hw->link.port;
				break;
			case 100:
			case 10:
				ctrl |= priv->hw->link.port;
				if (phydev->speed == SPEED_100) {
					tcc_gmac_set_clk_rate(priv, 250000);
					ctrl |= priv->hw->link.speed;
				} else {
					tcc_gmac_set_clk_rate(priv, 25000);
					ctrl &= ~(priv->hw->link.speed);
				}
				break;
			default:
				if (netif_msg_link(priv))
					pr_warning("%s: Speed (%d) is not 10"
				       " or 100!\n", dev->name, phydev->speed);
				break;
			}

#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
			tcc_gmac_set_adjust_bandwidth_reservation(dev);
#endif
			priv->speed = phydev->speed;
		}

		writel(ctrl, ioaddr + MAC_CTRL_REG);

		if (!priv->oldlink) {
			new_state = 1;
			priv->oldlink = 1;
		}

	} else if (priv->oldlink) {
		new_state = 1;
		priv->oldlink = 0;
		priv->speed = 0;
		priv->oldduplex = -1;
	}

	if (new_state && netif_msg_link(priv))
		phy_print_status(phydev);

	spin_unlock_irqrestore(&priv->lock, flags);

	DBG(probe, DEBUG, "tcc_gmac_adjust_link: exiting\n");
}


static int tcc_gmac_phy_probe(struct net_device *dev) 
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	struct mii_bus *bus = priv->mii;
	struct phy_device *phy = NULL;
	int phy_addr;

	
	pr_debug(">>> tcc_gmac_phy_probe: :\n");

	for (phy_addr=0; phy_addr < PHY_MAX_ADDR; phy_addr++) {
		if (bus->phy_map[phy_addr]) {
			phy = bus->phy_map[phy_addr];
			pr_info("Phy Addr : %d, Phy Chip ID : 0x%08x\n", phy_addr, phy->phy_id);
			break;
		} 
	}

	if (!phy) {
			pr_info("No Phy found\n");
			return -1;
	}

#if defined(CONFIG_TCC_GMAC_MII_MODE)
	if(phy->phy_id == 0x001cc912)
	{
		pr_info("No Phy macthed. Check Ethernet Configuration \n");
		return -1;
	}
#endif

	/* Attach the MAC to the Phy */
#if defined(CONFIG_TCC_GMAC_MII_MODE)
	phy = phy_connect(dev, dev_name(&phy->dev), &tcc_gmac_adjust_link, 0, PHY_INTERFACE_MODE_MII);
#elif defined(CONFIG_TCC_GMAC_RMII_MODE)
	phy = phy_connect(dev, dev_name(&phy->dev), &tcc_gmac_adjust_link, 0, PHY_INTERFACE_MODE_RMII);
#elif defined(CONFIG_TCC_GMAC_GMII_MODE)
	phy = phy_connect(dev, dev_name(&phy->dev), &tcc_gmac_adjust_link, 0, PHY_INTERFACE_MODE_GMII);
#elif defined(CONFIG_TCC_GMAC_RGMII_MODE)
	phy = phy_connect(dev, dev_name(&phy->dev), &tcc_gmac_adjust_link, 0, PHY_INTERFACE_MODE_RGMII);
#endif

	priv->phy_addr = phy_addr;
	priv->phydev = phy;
	pr_debug(">>> tcc_gmac_phy_probe:  phy_addr %x :\n",priv->phy_addr);

	return 0;
}

static int tcc_gmac_mdio_register(struct net_device *dev)
{
	int err = 0;
	struct mii_bus *bus;
	int *irq_list;
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	int i;

	bus = mdiobus_alloc();
	if (!bus) 
		return -ENOMEM;

	irq_list = kzalloc(sizeof(int) * PHY_MAX_ADDR, GFP_KERNEL);
	if (!irq_list) {
		err = -ENOMEM;
		goto irq_list_alloc_fail;
	}

	for (i=0; i<PHY_MAX_ADDR; i++) {
		irq_list[i] = PHY_POLL;
	}
	
	bus->name = "tcc-gmac-mdio";
	bus->read = tcc_gmac_mdio_read;
	bus->write = tcc_gmac_mdio_write;
	bus->reset= tcc_gmac_mdio_reset;
	snprintf(bus->id, MII_BUS_ID_SIZE, "0");
	bus->priv = dev;
	bus->irq = irq_list;
	bus->parent = priv->device;
	
	err = mdiobus_register(bus);
	if (err) {
		pr_err("%s: Cannot register as MDIO bus\n", bus->name); 
		goto bus_register_fail;
	}

	priv->mii = bus;

	return 0;

irq_list_alloc_fail:
	kfree(irq_list);
bus_register_fail:
	kfree(bus);
	
	return err;
}

static int tcc_gmac_mdio_unregister(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);

	mdiobus_unregister(priv->mii);
	priv->mii->priv = NULL;
	kfree(priv->mii);

	return 0;
}

/* *****************************************
 *
 *       Network Device Driver ops
 *
 * ****************************************/

static irqreturn_t tcc_gmac_irq_handler(int irq, void *dev_id)
{
	struct net_device *dev = (struct net_device*)dev_id;
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;

	if (unlikely(!dev)) {
		pr_err("%s: invalid dev pointer\n", __func__);
		return IRQ_NONE;
	}

	/* To handle GMAC own interrupts */
	priv->hw->mac->host_irq_status(ioaddr);
	tcc_gmac_dma_interrupt(priv);

	return IRQ_HANDLED;
}

static unsigned char char_to_num(char c)
{
	unsigned char ret;
	if( c >= '0' && c <= '9'){
		ret = c -'0';
	}
	else if( c >= 'a' && c <= 'f'){
		ret = c - 'a' + 0xa;
	}
	else if( c >= 'A' && c <= 'F'){
		ret = c - 'A' + 0xa;
	}
	else{
		ret = 0;
	}

	return ret;
}

#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
void tcc_gmac_set_default_avb_opt(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);

	priv->avb_opt.class_a_priority = SR_CLASS_A_DEFAULT_PRIORITY;
	priv->avb_opt.class_b_priority = SR_CLASS_B_DEFAULT_PRIORITY;
	priv->avb_opt.class_a_bandwidth = SR_CLASS_A_DEFAULT_BANDWIDTH;
	priv->avb_opt.class_b_bandwidth = SR_CLASS_B_DEFAULT_BANDWIDTH;
}

int tcc_gmac_set_adjust_bandwidth_reservation(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	struct tcc_gmac_avb_opt_t *avb_opt = &priv->avb_opt;
	struct phy_device *phydev = priv->phydev;
	unsigned long ioaddr = dev->base_addr;
	struct {
		int idle_slope;
		int send_slope; 
		int hi_credit;
		int lo_credit;
	} credit_arg_ch1, credit_arg_ch2;

	/*
	 * Refer to IEEE 802.1Q-2011 
	 * - 8.6.8.2(Credit-based shaper algorithm)
	 * - Annex L(Operation of the credit-shaper algorithm)
	 */
	switch(phydev->speed) {
		case 1000:
			if (avb_opt->class_a_bandwidth + avb_opt->class_b_bandwidth > 1000) {
				return -1;
			}
			credit_arg_ch2.idle_slope = ((8*avb_opt->class_a_bandwidth)*1024) / 1000;
			credit_arg_ch2.send_slope = (8*1024) - credit_arg_ch2.idle_slope;
			credit_arg_ch2.hi_credit = (MAX_INTERFERENCE_SIZE * credit_arg_ch2.idle_slope) / 8;
			credit_arg_ch2.lo_credit = -((MAX_FRAME_SIZE * credit_arg_ch2.send_slope) / 8);

			credit_arg_ch1.idle_slope = ((8*avb_opt->class_b_bandwidth)*1024) / 1000;
			credit_arg_ch1.send_slope = (8*1024) - credit_arg_ch1.idle_slope;
			credit_arg_ch1.hi_credit = (MAX_INTERFERENCE_SIZE * credit_arg_ch1.idle_slope) / 8;
			credit_arg_ch1.lo_credit = -((MAX_FRAME_SIZE * credit_arg_ch1.send_slope) / 8);
			break;
		case 100:
			if (avb_opt->class_a_bandwidth + avb_opt->class_b_bandwidth > 100) {
				return -1;
			}
			credit_arg_ch2.idle_slope = ((4*avb_opt->class_a_bandwidth)*1024) / 100;
			credit_arg_ch2.send_slope = (4*1024) - credit_arg_ch2.idle_slope;
			credit_arg_ch2.hi_credit = (MAX_INTERFERENCE_SIZE * credit_arg_ch2.idle_slope) / 4;
			credit_arg_ch2.lo_credit = -((MAX_FRAME_SIZE * credit_arg_ch2.send_slope) / 4);

			credit_arg_ch1.idle_slope = ((4*avb_opt->class_b_bandwidth)*1024) / 100;
			credit_arg_ch1.send_slope = (4*1024) - credit_arg_ch1.idle_slope;
			credit_arg_ch1.hi_credit = (MAX_INTERFERENCE_SIZE * credit_arg_ch1.idle_slope) / 4;
			credit_arg_ch1.lo_credit = -((MAX_FRAME_SIZE * credit_arg_ch1.send_slope) / 4);
			break;
		default:
			return -1;
			break;
	}

	// SR CLASS A
	priv->hw->dma[1]->idle_slope_credit(ioaddr, credit_arg_ch1.idle_slope);
	priv->hw->dma[1]->send_slope_credit(ioaddr, credit_arg_ch1.send_slope);
	priv->hw->dma[1]->hi_credit(ioaddr, credit_arg_ch1.hi_credit);
	priv->hw->dma[1]->lo_credit(ioaddr, credit_arg_ch1.lo_credit);

	// SR CLASS B
	priv->hw->dma[2]->idle_slope_credit(ioaddr, credit_arg_ch2.idle_slope);
	priv->hw->dma[2]->send_slope_credit(ioaddr, credit_arg_ch2.send_slope);
	priv->hw->dma[2]->hi_credit(ioaddr, credit_arg_ch2.hi_credit);
	priv->hw->dma[2]->lo_credit(ioaddr, credit_arg_ch2.lo_credit);

	return 0;
}
#endif


/*
 * tcc_eth_open : Makes the ethernet interface up
 *
 * @dev : Pointer to the net device interface set up.
 *
 * This function is alligned with the user space ifconfig
 * application to make the ethernet interface up.
 */
 extern char* atheros_wifi_mac;
static int tcc_gmac_open(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	int i;
	int ret;
#ifdef CONFIG_TCC_GMAC_PTP
	struct timespec cur_time;
#endif /*CONFIG_TCC_GMAC_PTP*/

	pr_debug(">>> tcc_gmac_open: :\n");
	printk("NUMS_OF_DMA_CH : %d\n", NUMS_OF_DMA_CH);

	tca_gmac_clk_init();
	tcc_gmac_verify_args();
	tca_gmac_clk_enable();
	tca_gmac_portinit();
	tca_gmac_phy_pwr_on();
	tca_gmac_phy_reset();

	priv->hw->clk_rate = calc_mdio_clk_rate(tca_gmac_get_hsio_clk());

	if (!priv->is_mdio_registered) {
		pr_debug("\tMDIO bus priv->pbl %d ",priv->pbl );
		ret = tcc_gmac_mdio_register(dev);
		if (ret < 0)
			return -1;
		priv->is_mdio_registered = 1;
		pr_debug("registered!\n");
	}
	if (tcc_gmac_phy_probe(dev)) {
		pr_warning("%s: No PHY found\n", dev->name);
		tca_gmac_phy_pwr_off();
		tca_gmac_clk_disable();
		return -1;
	}
	pr_debug(">>> tcc_gmac_phy_probe done:dev->name %s dev_addr %x \n",
				dev->name,(unsigned int)dev->dev_addr);

	tca_gmac_irq_init();

	ret = request_irq(dev->irq, tcc_gmac_irq_handler, IRQF_SHARED, dev->name, dev);
	if (ret) {
		pr_err("%s : ERROR: allocating IRQ %d (error : %d)\n", __func__, dev->irq, ret);
		
		tca_gmac_phy_pwr_off();
		tca_gmac_clk_disable();
		return ret;
	}

	/* Create and initialize the Tx/Rx descriptor chains */
	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		priv->tx_dma_ch[i].dma_tx_size = TCC_GMAC_ALIGN(dma_txsize);
		priv->rx_dma_ch[i].dma_rx_size = TCC_GMAC_ALIGN(dma_rxsize);
	}
	priv->dma_buf_sz = TCC_GMAC_ALIGN(buf_sz);
	init_dma_desc_rings(dev);

	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		/* DMA initialization and SW reset */
		if (priv->hw->dma[i]->init(ioaddr, priv->pbl, 
									priv->tx_dma_ch[i].dma_tx_phy, 
									priv->rx_dma_ch[i].dma_rx_phy)) {
				pr_err("%s : DMA Initialization failed\n", __func__);
				tca_gmac_clk_disable();
				tca_gmac_phy_pwr_off();
				return -1;
		}
	}
#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
	tcc_gmac_set_default_avb_opt(dev);

	priv->hw->dma[0]->set_bus_mode(ioaddr, 0, 0, 0, 0);
	priv->hw->dma[1]->set_bus_mode(ioaddr, 0, 0, 0, 0);
	priv->hw->dma[2]->set_bus_mode(ioaddr, 0, 0, 0, 0);

	priv->hw->dma[1]->slot_control(ioaddr, 0, 0);
	priv->hw->dma[2]->slot_control(ioaddr, 0, 0);

	priv->hw->mac->set_av_ethertype(ioaddr, ETH_P_AVTP);
	priv->hw->mac->set_av_priority(ioaddr, SR_CLASS_A_DEFAULT_PRIORITY);
	priv->hw->mac->set_av_avch(ioaddr, 0);
	priv->hw->mac->set_av_ptpch(ioaddr, 0);
#endif

#ifdef CONFIG_TCC_GMAC_PTP
	priv->hw->ptp->set_ssinc(ioaddr, 0x14); // ptp clk is 50Mhz
	priv->hw->ptp->digital_rollover_enable(ioaddr);
	priv->hw->ptp->enable(ioaddr);

	cur_time = current_kernel_time();
	priv->hw->ptp->init(ioaddr, cur_time.tv_sec, cur_time.tv_nsec);
	priv->tx_timestamp_on = false;
#endif

	pr_debug(">>> DMA initialization done: ");
	pr_debug("buf sz %d pbl %d \n", priv->dma_buf_sz,priv->pbl);
	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		pr_debug("\tch%d tx %d rx %d \n", 
				i, priv->tx_dma_ch[i].dma_tx_size, priv->rx_dma_ch[i].dma_rx_size);
	}

	priv->hw->mac->set_umac_addr(ioaddr, dev->dev_addr, 0); // Setup MAC Address
	priv->hw->mac->core_init(ioaddr); // Initialize the MAC Core

	priv->shutdown = 0;

	/* Initialize the MMC to disable all interrupts */
	writel(0xffffffff, ioaddr + MMC_RX_INTR_MASK);
	writel(0xffffffff, ioaddr + MMC_TX_INTR_MASK);

//	printk("Hw Feature : 0x%08x\n", readl(ioaddr + DMA_CH0_HW_FEATURE));

	tcc_gmac_enable_tx(ioaddr); // Enable MAC Tx
	tcc_gmac_enable_rx(ioaddr); // Enable MAC Rx
	
	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		tcc_gmac_dma_operation_mode(priv, i); // Set the HW DMA mode and the COE
										   // (Checksum Offload Engine)
	}

	/* Extra statistics */
	memset(&priv->xstats, 0, sizeof(struct tcc_gmac_extra_stats));
	priv->xstats.threshold = tc;

	DBG(probe, DEBUG, "%s: DMA RX/TX processes started...\n", dev->name);
	for (i=0; i<NUMS_OF_DMA_CH; i++) {
		priv->hw->dma[i]->start_tx(ioaddr); // Start Tx
		priv->hw->dma[i]->start_rx(ioaddr); // Start Rx
	}

	/* Dump DMA/MAC registers */
	if (netif_msg_hw(priv)) {
		priv->hw->mac->dump_regs(ioaddr);
		for (i=0; i<NUMS_OF_DMA_CH; i++) {
			priv->hw->dma[i]->dump_regs(ioaddr);
		}
	}

	/* Phy Start */
	if (priv->phydev) {
		pr_debug(">>> phy_start: :\n");
		netif_carrier_off(dev);
		phy_start(priv->phydev);
	}
	pr_debug(">>> tcc_gmac_open done: :ioaddr %x \n",(unsigned int)ioaddr);

#ifdef CONFIG_TCC_GMAC_PTP
	priv->ptp_clk = tcc_gmac_ptp_probe(dev);

	if (0){
		//for test
		priv->hw->ptp->pps0_set_time(ioaddr, cur_time.tv_sec + 10, 0x00);
		priv->hw->ptp->pps0_trig_enable(ioaddr);
		priv->hw->mac->timestamp_irq_enable(ioaddr);
		priv->hw->ptp->trig_irq_enable(ioaddr);
	}
	
#endif

	napi_enable(&priv->napi);
	skb_queue_head_init(&priv->rx_recycle);
	netif_start_queue(dev);

	return 0;
}

/*
 * tcc_gmac_stop : Makes the Ether interface down
 *
 * @dev :   Pointer to the net device interface set up.
 *
 * This function is alligned with the user space ifconfig
 * application to make the ethernet interface down.
 *
 */

static int tcc_gmac_stop(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	int i;

	pr_debug(">>> tcc_gmac_stop: :\n");

#ifdef CONFIG_TCC_GMAC_PTP
	tcc_gmac_ptp_remove(priv->ptp_clk);
#endif

	/* Stop and disconnect the PHY */
	if (priv->phydev) {
		phy_stop(priv->phydev);
		phy_disconnect(priv->phydev);
		priv->phydev = NULL;
		priv->oldlink = 0;
		priv->speed = 0;
		priv->oldduplex = -1;
		priv->phydev = NULL;
	}

	netif_stop_queue(dev);

	napi_disable(&priv->napi);
	skb_queue_purge(&priv->rx_recycle);

	/* Free the IRQ lines */
	free_irq(dev->irq, dev);


	/* Stop TX/RX DMA and clear the descriptors */
	for (i=0; i < NUMS_OF_DMA_CH; i++) {
		priv->hw->dma[i]->stop_tx(dev->base_addr);
		priv->hw->dma[i]->stop_rx(dev->base_addr);
	}

	/* Release and free the Rx/Tx resources */
	free_dma_desc_resources(priv);

	/* Disable the MAC core */
	tcc_gmac_disable_tx(dev->base_addr);
	tcc_gmac_disable_rx(dev->base_addr);

	netif_carrier_off(dev);

	tca_gmac_phy_pwr_off();
	tca_gmac_clk_disable();

	return 0;
}

/*
 * tcc_gmac_tx_timeout :Called on Tx timeout
 *
 * @ndev :  Pointer to net device interface structure.
 *
 * This function is called whenever a watch dog time out occurs while the stack
 * has queued up the data, but the transmission is not done within the
 * stipulated time for the timeout.
 */

static void tcc_gmac_tx_timeout(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);

	/* Clear Tx resources and restart transmitting again */
	tcc_gmac_tx_err(priv);
	return;
}


/*
 * tcc_gmac_start_xmit : MAC layer function for transmission handling.
 *
 * @skb     : SK Buffer Packet to be transmitted
 * @dev     : Pointer to net device structure
 *
 * This function is the MAC layer function to handle the transmission of the
 * packets that are send by the Etheret stack. This function configures the
 * DMA and initializes the proper descriptor to handle the same.1
 */
static int tcc_gmac_start_xmit_ch(struct sk_buff *skb, struct net_device *dev, unsigned int ch)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	unsigned int entry;
	int i, csum_insertion = 0;
	int nfrags = skb_shinfo(skb)->nr_frags;
	struct dma_desc *desc, *first;
	//unsigned long flags;
	struct tx_dma_ch_t *dma = &priv->tx_dma_ch[ch];
	unsigned int txsize = dma->dma_tx_size;
	struct netdev_queue *txq = netdev_get_tx_queue(dev, ch);

//	spin_lock_irqsave(&priv->lock, flags);

	if (unlikely(tcc_gmac_tx_avail(priv, ch) < nfrags+1)) {
		//if (!netif_queue_stopped(dev)) {
		//	netif_stop_queue(dev);
		if (!netif_tx_queue_stopped(txq)) {
			netif_tx_stop_queue(txq);
			/* This is hard error, log it. */
			pr_err("%s: BUG! Tx Ring Full when netif queue awake\n", __func__);
		}
		//spin_unlock_irqrestore(&priv->lock, flags);
		return NETDEV_TX_BUSY;
	}

	entry = dma->cur_tx % txsize;

#ifdef TCC_GMAC_XMIT_DEBUG
	if ((skb->len > ETH_FRAME_LEN) || nfrags)
		pr_info("tcc_gmac xmit:\n"
		       "\tskb addr %p - len: %d - nopaged_len: %d\n"
		       "\tn_frags: %d - ip_summed: %d - %s gso\n",
		       skb, skb->len, skb_headlen(skb), nfrags, skb->ip_summed,
		       !skb_is_gso(skb) ? "isn't" : "is");
#endif

	if (unlikely(skb_is_gso(skb))) {
		int ret_val = tcc_gmac_sw_tso(priv, skb, ch);
		//spin_unlock_irqrestore(&priv->lock, flags);
		return ret_val;
	}

	if (likely((skb->ip_summed == CHECKSUM_PARTIAL))) {
		if (likely(priv->tx_coe == NO_HW_CSUM))
			skb_checksum_help(skb);
		else
			csum_insertion = 1;
	}

	desc = dma->dma_tx + entry;
	first = desc;

#ifdef TCC_GMAC_XMIT_DEBUG
	if ((nfrags > 0) || (skb->len > ETH_FRAME_LEN))
		pr_debug("tcc_gmac xmit: skb len: %d, nopaged_len: %d,\n"
		       "\t\tn_frags: %d, ip_summed: %d\n",
		       skb->len, skb_headlen(skb), nfrags, skb->ip_summed);
#endif
	dma->tx_skbuff[entry] = skb;
	if (unlikely(skb->len >= BUF_SIZE_4KiB)) {
		entry = tcc_gmac_handle_jumbo_frames(skb, dev, csum_insertion, ch);
		desc = dma->dma_tx + entry;
	} else {
		unsigned int nopaged_len = skb_headlen(skb);
		desc->des2 = dma_map_single(priv->device, skb->data, nopaged_len, DMA_TO_DEVICE);
		priv->hw->desc->prepare_tx_desc(desc, 1, nopaged_len, csum_insertion);
	}


	for (i = 0; i < nfrags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		int len = frag->size;

		entry = (++dma->cur_tx) % txsize;
		desc = dma->dma_tx + entry;

		TX_DBG("\t[entry %d] segment len: %d\n", entry, len);
		#if 0
		desc->des2 = dma_map_page(priv->device, frag->page,
					  frag->page_offset,
					  len, DMA_TO_DEVICE);
		#else
		desc->des2 = skb_frag_dma_map(priv->device, frag, 0, len, DMA_TO_DEVICE);
		#endif
		dma->tx_skbuff[entry] = NULL;
		priv->hw->desc->prepare_tx_desc(desc, 0, len, csum_insertion);
		wmb();
		priv->hw->desc->set_tx_owner(desc);
		wmb();
	}

	/* Interrupt on completition only for the latest segment */
	priv->hw->desc->close_tx_desc(desc);
	wmb();

	/* To avoid raise condition */
	priv->hw->desc->set_tx_owner(first);
	dma->cur_tx++;
	
#ifdef TCC_GMAC_XMIT_DEBUG
	if (netif_msg_pktdata(priv)) {
		pr_info("tcc_gmac xmit: current=%d, dirty=%d, entry=%d, "
		       "first=%p, nfrags=%d\n",
		       (priv->cur_tx % txsize), (priv->dirty_tx % txsize),
		       entry, first, nfrags);
		display_ring(priv->dma_tx, txsize);
		pr_info(">>> frame to be transmitted: ");
		print_pkt(skb->data, skb->len);
	}
#endif
	if (unlikely(tcc_gmac_tx_avail(priv, ch) <= (MAX_SKB_FRAGS + 1))) {
		TX_DBG("%s: stop transmitted packets\n", __func__);
		//netif_stop_queue(dev);
		netif_tx_stop_queue(txq);
	}
#ifdef CONFIG_TCC_GMAC_PTP
	if ((skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) && priv->tx_timestamp_on)
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	else 
		skb_tx_timestamp(skb);//s/w timestamp 
#endif

	dev->stats.tx_bytes += skb->len;


	priv->hw->dma[ch]->enable_dma_transmission(dev->base_addr);
	//spin_unlock_irqrestore(&priv->lock, flags);
	return NETDEV_TX_OK;
}

u16 tcc_gmac_select_queue(struct net_device *dev, struct sk_buff *skb)
{
#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
	struct tcc_gmac_priv *priv = netdev_priv(dev);
#endif
	int ch = 0;

#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
	if (skb->priority == priv->avb_opt.class_a_priority) {
		ch = 2;
	} else if (skb->priority == priv->avb_opt.class_b_priority) {
		ch = 1;
	}
#endif

	return ch;
}

/*
 * tcc_gmac_start_xmit : MAC layer function for transmission handling.
 *
 * @skb     : SK Buffer Packet to be transmitted
 * @dev     : Pointer to net device structure
 *
 * This function is the MAC layer function to handle the transmission of the
 * packets that are send by the Etheret stack. This function configures the
 * DMA and initializes the proper descriptor to handle the same.1
 */
static int tcc_gmac_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	int ret;

	//dump_ptp_regs(dev);

#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
	int ch = 0;

	ch = skb_get_queue_mapping(skb);

	ret = tcc_gmac_start_xmit_ch(skb, dev, ch);
#else
	ret = tcc_gmac_start_xmit_ch(skb, dev, 0);
#endif

	return ret;
}



#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
int ioctl_avb_config(struct net_device *dev, struct ifreq *rq)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	struct phy_device *phydev = priv->phydev;
	struct ifr_avb_config_t *req;
	unsigned long ioaddr = priv->dev->base_addr;
	int ret = -EINVAL;

	req = (struct ifr_avb_config_t*)rq->ifr_data;

	switch(req->cmd) {
		case AVB_CMD_GET_CLASS_A_PRIORITY:
			req->data = priv->avb_opt.class_a_priority;
			ret = 0;
			break;
		case AVB_CMD_GET_CLASS_B_PRIORITY:
			req->data = priv->avb_opt.class_b_priority;
			ret = 0;
			break;
		case AVB_CMD_GET_CLASS_A_BANDWIDTH:
			req->data = priv->avb_opt.class_a_bandwidth;
			ret = 0;
			break;
		case AVB_CMD_GET_CLASS_B_BANDWIDTH:
			req->data = priv->avb_opt.class_b_bandwidth;
			ret = 0;
			break;
		case AVB_CMD_SET_CLASS_A_PRIORITY:
			if (req->data <= 7) {
				priv->avb_opt.class_a_priority = (u8)req->data;
				if (priv->avb_opt.class_a_priority >= priv->avb_opt.class_b_priority) {
					priv->hw->mac->set_av_priority(ioaddr, priv->avb_opt.class_a_priority);
				} else {
					priv->hw->mac->set_av_priority(ioaddr, priv->avb_opt.class_b_priority);
				}
				ret = 0;
			} else {
				pr_err("Request Priority is over 7\n"); 
			}
			break;
		case AVB_CMD_SET_CLASS_B_PRIORITY:
			if ((req->data <= 7)) {
				priv->avb_opt.class_b_priority = (u8)req->data;
				if (priv->avb_opt.class_a_priority >= priv->avb_opt.class_b_priority) {
					priv->hw->mac->set_av_priority(ioaddr, priv->avb_opt.class_a_priority);
				} else {
					priv->hw->mac->set_av_priority(ioaddr, priv->avb_opt.class_b_priority);
				}
				ret = 0;
			} else {
				pr_err("Request Priority is over 7\n"); 
			}
			break;
		case AVB_CMD_SET_CLASS_A_BANDWIDTH:
			if (req->data + priv->avb_opt.class_b_bandwidth < ((phydev->speed*3)/4)) {
				priv->avb_opt.class_a_bandwidth = (u32)req->data;
				ret = tcc_gmac_set_adjust_bandwidth_reservation(dev);
			} else {
				pr_err("Class A bandwidth + Class B bandwidth > portTransmitRate's 75%%\n"); 
			}
			break;
		case AVB_CMD_SET_CLASS_B_BANDWIDTH:
			if (req->data + priv->avb_opt.class_a_bandwidth < ((phydev->speed*3)/4)) {
				priv->avb_opt.class_b_bandwidth = (u32)req->data;
				ret = tcc_gmac_set_adjust_bandwidth_reservation(dev);
			} else {
				pr_err("Class B bandwidth + Class B bandwidth > portTransmitRate's 75%%\n"); 
			}
			break;
		default:
			break;
	}

	return ret;
}
#endif

#ifdef CONFIG_TCC_GMAC_PTP
static int ioctl_hwtstamp(struct net_device *dev, struct ifreq *rq)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	struct hwtstamp_config config;

	if (copy_from_user(&config, rq->ifr_data, sizeof(config)))
		return -EFAULT;

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
		case HWTSTAMP_TX_OFF:
			priv->tx_timestamp_on = false;
			break;
		case HWTSTAMP_TX_ON:
			priv->tx_timestamp_on = true;
			break;
		default:
			return -ERANGE;
	}

	if (priv->hw->ptp->snapshot_mode(dev->base_addr, config.rx_filter) < 0)
		return -EINVAL;

	return copy_to_user(rq->ifr_data, &config, sizeof(config)) ?  -EFAULT : 0;
}
#endif

static int tcc_gmac_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	int ret = -EOPNOTSUPP;

	if (!netif_running(dev))
		return -EINVAL;

	switch(cmd) {
		case SIOCGMIIPHY:
		case SIOCGMIIREG:
		case SIOCSMIIREG:
			if (!priv->phydev)
				return -EINVAL;
			
			spin_lock(&priv->lock);
			ret = phy_mii_ioctl(priv->phydev, rq, cmd);
			spin_unlock(&priv->lock);
			break;
#ifdef CONFIG_TCC_GMAC_FQTSS_SUPPORT
		case IOCTL_AVB_CONFIG:
			spin_lock(&priv->lock);
			ret = ioctl_avb_config(dev, rq);
			spin_unlock(&priv->lock);
			break;
#endif
#ifdef CONFIG_TCC_GMAC_PTP
		case SIOCSHWTSTAMP:
			ret = ioctl_hwtstamp(dev, rq);
			break;
#endif
		default:
			break;
	}

	return ret;
}

/**
 * tcc_gmac_set_rx_mode - entry point for multicast addressing
 * @dev : pointer to the device structure
 * Description:
 * This function is a driver entry point which gets called by the kernel
 * whenever multicast addresses must be enabled/disabled.
 * Return value:
 * void.
 */ 
static void tcc_gmac_set_rx_mode(struct net_device *dev)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);

	spin_lock(&priv->lock);
	priv->hw->mac->set_filter(dev);
	spin_unlock(&priv->lock);

	return;
}

#ifdef TCC_GMAC_VLAN_TAG_USED
static void tcc_gmac_vlan_rx_register(struct net_device *dev, struct vlan_group *grp)
{
	struct tcc_gmac_priv *priv = netdev_priv(dev);

	DBG(probe, INFO, "%s: Setting vlgrp to %p\n", dev->name, grp);

	spin_lock(&priv->lock);
	priv->vlgrp = grp;
	spin_unlock(&priv->lock);

	return;
}
#endif

static int tcc_gmac_poll(struct napi_struct *napi, int budget)
{
	struct tcc_gmac_priv *priv = container_of(napi, struct tcc_gmac_priv, napi);
	int work_done[NUMS_OF_DMA_CH] = {0};
	int total_work_done = 0;
	int i;

	priv->xstats.poll_n++;
	//for (i=0; i<NUMS_OF_DMA_CH; i++) {
	for (i=NUMS_OF_DMA_CH-1; i >= 0; i--) {
		tcc_gmac_tx(priv, i);
		work_done[i] = tcc_gmac_rx(priv, budget, i);
		total_work_done += work_done[i];
	}

	if (total_work_done < budget) {
		napi_complete(napi);
		for (i=0; i<NUMS_OF_DMA_CH; i++) {
			tcc_gmac_enable_irq(priv, i);
		}
	}
	return 0;
}

/*
 * tcc_gmac_change_mtu :Change the MTU size.
 *
 * @dev :   Pointer to net device struct.
 * @new_mtu:    New MTU size to be configured.
 *
 * This function is used to set up the MTU size, specially applicable
 * when the driver supports the Jumbo Frame.
 */

static int tcc_gmac_change_mtu(struct net_device *dev, int new_mtu)
{
	if (netif_running(dev)) {
		pr_err("%s: must be stopped to change its MTU\n", dev->name);
		return -EBUSY;
	}

	if ((new_mtu < 46) || (new_mtu > JUMBO_LEN)) {
		pr_err("%s: invalid MTU, max MTU is: %d\n", dev->name, JUMBO_LEN);
		return -EINVAL;
	}

	dev->mtu = new_mtu;
	printk("%s : new_mtu %d \n ",__func__,new_mtu);
	return 0;
}


/* *****************************************
 *
 *       Platform Device Driver ops
 *
 * ****************************************/

static int tcc_gmac_suspend(struct platform_device *pdev, pm_message_t state)
{
#if 1
	struct net_device *dev = platform_get_drvdata(pdev);
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	//int dis_ic = 0;

	if (!dev || !netif_running(dev))
		return 0;

	//spin_lock(&priv->lock);
	#if 0
	if (tcc_power_state == TCC_PM_STATE_SLEEP) {
		netif_device_detach(dev);
		netif_stop_queue(dev);
		if (priv->phydev)
			phy_stop(priv->phydev);

		napi_disable(&priv->napi);

		/* Stop TX/RX DMA */
		priv->hw->dma_ch0->stop_tx(dev->base_addr);
		priv->hw->dma_ch0->stop_rx(dev->base_addr);
		/* Clear the Rx/Tx descriptors */
		priv->hw->desc->init_rx_desc(priv->dma_rx, priv->dma_rx_size,
				dis_ic);
		priv->hw->desc->init_tx_desc(priv->dma_tx, priv->dma_tx_size);

		tcc_gmac_disable_tx(dev->base_addr);

		if (device_may_wakeup(&(pdev->dev))) {
			/* Enable Power down mode by programming the PMT regs */
			if (priv->wolenabled == PMT_SUPPORTED)
				priv->hw->mac->pmt(dev->base_addr,
						priv->wolopts);
		} else {
			tcc_gmac_disable_rx(dev->base_addr);
		}
		tca_gmac_clk_disable();
	} else 
	#endif
	{
		priv->shutdown = 1;
		/* Although this can appear slightly redundant it actually
		 * makes fast the standby operation and guarantees the driver
		 * working if hibernation is on media. */
		tcc_gmac_stop(dev);
	}

	//spin_unlock(&priv->lock);
#endif
	return 0;
}

static int tcc_gmac_resume(struct platform_device *pdev)
{
#if 1
	struct net_device *dev = platform_get_drvdata(pdev);
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	int i;

	if (!netif_running(dev))
		return 0;

	//spin_lock(&priv->lock);

	if (priv->shutdown) {
		/* Re-open the interface and re-init the MAC/DMA
		 *            and the rings. */
		tcc_gmac_open(dev);
		//tcc_gmac_adjust_link(dev);
		goto out_resume;
	}

	tca_gmac_clk_enable();
	/* Power Down bit, into the PM register, is cleared
	 * automatically as soon as a magic packet or a Wake-up frame
	 * is received. Anyway, it's better to manually clear
	 * this bit because it can generate problems while resuming
	 * from another devices (e.g. serial console). */
	if (device_may_wakeup(&(pdev->dev)))
		if (priv->wolenabled == PMT_SUPPORTED)
			priv->hw->mac->pmt(dev->base_addr, 0);

	netif_device_attach(dev);

	/* Enable the MAC and DMA */
	tcc_gmac_enable_rx(ioaddr);
	tcc_gmac_enable_tx(ioaddr);
	for (i=0; i < NUMS_OF_DMA_CH; i++) {
		priv->hw->dma[i]->start_tx(ioaddr);
		priv->hw->dma[i]->start_rx(ioaddr);
	}

	napi_enable(&priv->napi);

	if (priv->phydev) {
		phy_start(priv->phydev);
	}

	netif_start_queue(dev);

out_resume:
	//spin_unlock(&priv->lock);
#endif
	return 0;
}

static const struct net_device_ops tcc_gmac_netdev_ops = {
	.ndo_open = tcc_gmac_open,
	.ndo_stop = tcc_gmac_stop,
	.ndo_do_ioctl = tcc_gmac_ioctl,
	.ndo_tx_timeout = tcc_gmac_tx_timeout,
	.ndo_start_xmit = tcc_gmac_start_xmit,
	.ndo_set_rx_mode = tcc_gmac_set_rx_mode,
	.ndo_change_mtu = tcc_gmac_change_mtu,
	.ndo_select_queue = tcc_gmac_select_queue,
	
#ifdef TCC_GMAC_VLAN_TAG_USED
	.ndo_vlan_rx_register = tcc_gmac_vlan_rx_register,
#endif
	.ndo_set_mac_address = eth_mac_addr,
};

#ifdef CONFIG_TCC_GMAC_SET_MAC_FROM_CHIP
// ECID Code
// -------- 31 ------------- 15 ----------- 0 --------
// [0]	   |****************|****************|	  : '*' is valid
// [1]	   |0000000000000000|****************|	  : 
//
#define MODE	Hw31
#define CS		Hw30
#define FSET	Hw29
#define PRCHG	Hw27
#define PROG	Hw26
#define SCK 	Hw25
#define SDI 	Hw24
#define SIGDEV	Hw23
#define A2		Hw19
#define A1		Hw18
#define A0		Hw17
#define SEC 	Hw16
#define USER	(~Hw16)

static unsigned gECID[4] = {0,0,0,0};

static void
IO_UTIL_ReadECID (void)
{
	volatile PGPIO	pGPIO = (volatile PGPIO)tcc_p2v(HwGPIO_BASE);
	unsigned i;

	gECID[0] = gECID[1] = gECID[2] = gECID[3] = 0;

	pGPIO->ECID0 = MODE;
	pGPIO->ECID0 = MODE | CS | SEC;

	for (i=0;i<8;i++) {
		pGPIO->ECID0 = MODE | CS | (i<<17) | SEC;
		pGPIO->ECID0 = MODE | CS | (i<<17) | SIGDEV | SEC ;
		pGPIO->ECID0 = MODE | CS | (i<<17) | SIGDEV | PRCHG | SEC ;
		pGPIO->ECID0 = MODE | CS | (i<<17) | SIGDEV | PRCHG | FSET | SEC;
		pGPIO->ECID0 = MODE | CS | (i<<17) | PRCHG	| FSET	| SEC;
		pGPIO->ECID0 = MODE | CS | (i<<17) | FSET	| SEC;
		pGPIO->ECID0 = MODE | CS | (i<<17) | SEC;
	}

	gECID[0] = pGPIO->ECID2;	// Low	32 Bit
	gECID[1] = pGPIO->ECID3;	// High 16 Bit

	pGPIO->ECID0 = 0x00000000;	// ECID Closed

	pGPIO->ECID0 = MODE;
	pGPIO->ECID0 = MODE | CS;

	for (i=0;i<8;i++) {
		pGPIO->ECID0 = MODE | CS | (i<<17);
		pGPIO->ECID0 = MODE | CS | (i<<17) | SIGDEV  ;
		pGPIO->ECID0 = MODE | CS | (i<<17) | SIGDEV | PRCHG ;
		pGPIO->ECID0 = MODE | CS | (i<<17) | SIGDEV | PRCHG | FSET;
		pGPIO->ECID0 = MODE | CS | (i<<17) | PRCHG	| FSET;
		pGPIO->ECID0 = MODE | CS | (i<<17) | FSET;
		pGPIO->ECID0 = MODE | CS | (i<<17);
	}

	gECID[2] = pGPIO->ECID2;	// Low	32 Bit
	gECID[3] = pGPIO->ECID3;	// High 16 Bit

	pGPIO->ECID0 = 0x00000000;	// ECID Closed

	/*
	printk("ECID DATA[0] : 0x%x\n",gECID[0]);
	printk("ECID DATA[1] : 0x%x\n",gECID[1]);

	
	printk("ECID DATA[2] : 0x%x\n",gECID[2]);
	printk("ECID DATA[3] : 0x%x\n",gECID[3]);
*/
}
#endif

static int tcc_gmac_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct net_device *dev;
	struct tcc_gmac_priv *priv;
	int ret = 0;
	unsigned char MAC_addr[MAC_ADDRESS_LEN] = {0x00,0x00,0x00,0x00,0x00,0x00};
	int i;
	
	pr_debug(">>> tcc_gmac_probe: :\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	
	pr_debug(">>> platform_get_resource: :\n");

	//dev = alloc_etherdev(sizeof(struct tcc_gmac_priv));
	dev = alloc_etherdev_mqs(sizeof(struct tcc_gmac_priv), NUMS_OF_DMA_CH, NUMS_OF_DMA_CH);
	if (!dev) {
		return -ENOMEM;
	}
	SET_NETDEV_DEV(dev, &pdev->dev);

	priv = netdev_priv(dev);

	memset(priv, 0, sizeof(struct tcc_gmac_priv));
	priv->device = &(pdev->dev);
	priv->dev = dev;

	strcpy(dev->name, ETHERNET_DEV_NAME);

	platform_set_drvdata(pdev, dev);

	dev->base_addr = res->start;

	priv->hw = tcc_gmac_setup(dev->base_addr, tca_gmac_get_hsio_clk());
	if (!priv->hw) {
		return -ENOMEM;
	}

	pr_debug(">>> tcc_gmac_setup: :\n");
	
	priv->wolenabled = priv->hw->pmt;
	if (priv->wolenabled == PMT_SUPPORTED)
		priv->wolopts = WAKE_MAGIC;

	/* Register Driver's operations */
	dev->netdev_ops = &tcc_gmac_netdev_ops;

	dev->irq = platform_get_irq(pdev, 0);

	tcc_gmac_set_ethtool_ops(dev);

	//dev->features |= (NETIF_F_SG | NETIF_F_HW_CSUM);
	dev->features |= NETIF_F_HW_CSUM;
	dev->watchdog_timeo = msecs_to_jiffies(watchdog);
#ifdef TCC_GMAC_VLAN_TAG_USED
    /* gmac support receive VLAN tag detection */
    dev->features |= NETIF_F_HW_VLAN_RX;
#endif
    priv->msg_enable = netif_msg_init(debug, default_msg_level);
	priv->pbl = 8; //Programmable Burst Length
	priv->rx_csum = 1;
	priv->is_mdio_registered = 0;

	if (flow_ctrl) 
		priv->flow_ctrl = FLOW_AUTO;    /* RX/TX pause on */

	priv->pause = pause;
	netif_napi_add(dev, &priv->napi, tcc_gmac_poll, 64);

	ret = register_netdev(dev);
	if (ret) {
		pr_err("%s: ERROR %i registering the device\n", __func__, ret);
		return -ENODEV;
	}

	
#ifdef CONFIG_TCC_GMAC_SET_MAC_FROM_CHIP
	IO_UTIL_ReadECID();

	//F4-50-EB-XX-XX-XX
	if(gECID[2] !=0 || gECID[3] !=0){
		MAC_addr[0]=0xF4;
		MAC_addr[1]=0x50;
		MAC_addr[2]=0xEB;
		MAC_addr[3]= (gECID[3] >>8) & 0xFF;
		MAC_addr[4]= (gECID[3] & 0xFF);
		MAC_addr[5]= (gECID[2] >> 24) & 0xFF ;

		for(i=0;i<MAC_ADDRESS_LEN;i++)
		{
			dev->dev_addr[i] = MAC_addr[i];
			pr_debug("MAC_addr[%d] %x \n",i,MAC_addr[i]);
		}

	}else{
	
		unsigned char default_MAC_addr[MAC_ADDRESS_LEN]={0x00,0x12,0x34,0x56,0x78,0x00};
		for (i=0; i<MAC_ADDRESS_LEN; i++)	{
			dev->dev_addr[i] = default_MAC_addr[i];
		}
	}
	
#else
	if(atheros_wifi_mac != NULL)
	{
		for(i=0;i<MAC_ADDRESS_LEN;i++)
		{
			MAC_addr[i]=(char_to_num(atheros_wifi_mac[i*3+1]) &0xF)|(char_to_num(atheros_wifi_mac[i*3])&0xF)<<4;
		dev->dev_addr[i] = MAC_addr[i];
			pr_debug("MAC_addr[%d] %x mac %x %x \n",i,MAC_addr[i],atheros_wifi_mac[i*3],atheros_wifi_mac[i*3+1]);
		}
	}
		
		if( !(MAC_addr[0]||MAC_addr[1]||MAC_addr[2]||MAC_addr[3]||MAC_addr[4]||MAC_addr[5])){
	
			unsigned char default_MAC_addr[MAC_ADDRESS_LEN]={0x00,0x12,0x34,0x56,0x78,0x00};
			for (i=0; i<MAC_ADDRESS_LEN; i++)	{
				dev->dev_addr[i] = default_MAC_addr[i];
			}
		}
		
#endif

	DBG(probe, DEBUG, "%s: Scatter/Gather: %s - HW checksums: %s\n",
		dev->name, (dev->features & NETIF_F_SG) ? "on" : "off",
		(dev->features & NETIF_F_HW_CSUM) ? "on" : "off");

	spin_lock_init(&priv->lock);

	
	return ret;
}

static int tcc_gmac_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct tcc_gmac_priv *priv = netdev_priv(dev);
	int i;

	pr_info("%s:\n\tremoving driver\n", __func__);

	for (i=0; i < NUMS_OF_DMA_CH; i++) {
		priv->hw->dma[i]->stop_rx(dev->base_addr);
		priv->hw->dma[i]->stop_tx(dev->base_addr);
	}

	tcc_gmac_disable_rx(dev->base_addr);
	tcc_gmac_disable_tx(dev->base_addr);
	netif_carrier_off(dev);

	tcc_gmac_mdio_unregister(dev);

	platform_set_drvdata(pdev, NULL);
	unregister_netdev(dev);
	free_netdev(dev);

	return 0;
}

static struct platform_driver tcc_gmac_driver = {
	.probe		= tcc_gmac_probe,
	.remove		= tcc_gmac_remove,
	.suspend	= tcc_gmac_suspend,
	.resume 	= tcc_gmac_resume,
	.driver		= {
		.name = "tcc-gmac",
		.owner = THIS_MODULE,
	}
};

static int __init tcc_gmac_init(void)
{
	pr_debug(">>> tcc_gmac_init: :\n");
	return platform_driver_register(&tcc_gmac_driver);
}

static void __exit tcc_gmac_exit(void)
{
	platform_driver_unregister(&tcc_gmac_driver);
}

module_init(tcc_gmac_init);
module_exit(tcc_gmac_exit);

MODULE_AUTHOR("Telechips <linux@telechips.com>");
MODULE_DESCRIPTION("Telechips 10/100/1000 Ethernet Driver\n");
MODULE_LICENSE("GPL");

