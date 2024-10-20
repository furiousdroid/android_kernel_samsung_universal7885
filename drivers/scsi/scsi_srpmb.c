/*
 * Secure RPMB Driver for Exynos scsi rpmb
 *
 * Copyright (C) 2016 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/smc.h>

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_ioctl.h>

#include "scsi_srpmb.h"

#define SRPMB_DEVICE_PROPNAME	"samsung,ufs-srpmb"

struct platform_device *sr_pdev;

#if defined(DEBUG_SRPMB)
static void dump_packet(u8 *data, int len)
{
	u8 s[17];
	int i, j;

	s[16]='\0';
	for (i = 0; i < len; i += 16) {
		printk("%06x :", i);
		for (j=0; j<16; j++) {
			printk(" %02x", data[i+j]);
			s[j] = (data[i+j]<' ' ? '.' : (data[i+j]>'}' ? '.' : data[i+j]));
		}
		printk(" |%s|\n",s);
	}
	printk("\n");
}
#endif

static void swap_packet(u8 *p, u8 *d)
{
	int i;
	for (i = 0; i < RPMB_PACKET_SIZE; i++)
		d[i] = p[RPMB_PACKET_SIZE - 1 - i];
}

static void srpmb_worker(struct work_struct *data)
{
	int ret;
	struct rpmb_packet packet;
	struct rpmb_irq_ctx *rpmb_ctx;
	struct scsi_device *sdp;
	Rpmb_Req *req;

	if (!data) {
		dev_err(&sr_pdev->dev, "rpmb work_struct data invalid\n");
		return ;
	}
	rpmb_ctx = container_of(data, struct rpmb_irq_ctx, work);

	if (!rpmb_ctx) {
		dev_err(&sr_pdev->dev, "rpmb_ctx invalid\n");
		return ;
	}

	if (!rpmb_ctx->dev) {
		dev_err(&sr_pdev->dev, "rpmb_ctx->dev invalid\n");
		return ;
	}

	sdp = to_scsi_device(rpmb_ctx->dev);
	if (!sdp) {
		dev_err(&sr_pdev->dev, "sdp invalid\n");
		return ;
	}

	if (!rpmb_ctx->vir_addr) {
		dev_err(&sr_pdev->dev, "rpmb_ctx->vir_addr invalid\n");
		return ;
	}
	req = (Rpmb_Req *)rpmb_ctx->vir_addr;

	switch(req->type) {
	case GET_WRITE_COUNTER:
		if (req->data_len != RPMB_PACKET_SIZE) {
			req->status_flag = WRITE_COUTNER_DATA_LEN_ERROR;
			dev_err(&sr_pdev->dev, "data len is invalid\n");
			break;
		}

		req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
		req->outlen = RPMB_PACKET_SIZE;

		ret = srpmb_scsi_ioctl(sdp, req);
		if (ret < 0) {
			req->status_flag = WRITE_COUTNER_SECURITY_OUT_ERROR;
			dev_err(&sr_pdev->dev, "ioctl read_counter error: %x\n", ret);
			break;
		}

		memset(req->rpmb_data, 0x0, req->data_len);
		req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_IN;
		req->inlen = req->data_len;

		ret = srpmb_scsi_ioctl(sdp, req);
		if (ret < 0) {
			req->status_flag = WRITE_COUTNER_SECURITY_IN_ERROR;
			dev_err(&sr_pdev->dev, "ioctl error : %x\n", ret);
			break;
		}
		req->status_flag = PASS_STATUS;

		break;
	case WRITE_DATA:
		if (req->data_len < RPMB_PACKET_SIZE ||
			req->data_len > RPMB_PACKET_SIZE * 64) {
			req->status_flag = WRITE_DATA_LEN_ERROR;
			dev_err(&sr_pdev->dev, "data len is invalid\n");
			break;
		}

		req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
		req->outlen = req->data_len;

		ret = srpmb_scsi_ioctl(sdp, req);
		if (ret < 0) {
			req->status_flag = WRITE_DATA_SECURITY_OUT_ERROR;
			dev_err(&sr_pdev->dev, "ioctl write data error: %x\n", ret);
			break;
		}

		memset(req->rpmb_data, 0x0, req->data_len);
		memset(&packet, 0x0, RPMB_PACKET_SIZE);
		packet.request = RESULT_READ_REQ;
		swap_packet((uint8_t *)&packet, req->rpmb_data);
		req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
		req->outlen = RPMB_PACKET_SIZE;

		ret = srpmb_scsi_ioctl(sdp, req);
		if (ret < 0) {
			req->status_flag = WRITE_DATA_RESULT_SECURITY_OUT_ERROR;
			dev_err(&sr_pdev->dev,
					"ioctl write_data result error: %x\n", ret);
			break;
		}

		memset(req->rpmb_data, 0x0, req->data_len);
		req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_IN;
		req->inlen = RPMB_PACKET_SIZE;

		ret = srpmb_scsi_ioctl(sdp, req);
		if (ret < 0) {
			req->status_flag = WRITE_DATA_SECURITY_IN_ERROR;
			dev_err(&sr_pdev->dev,
					"ioctl write_data result error: %x\n", ret);
			break;
		}

		swap_packet(req->rpmb_data, (uint8_t *)&packet);
		if (packet.result == 0) {
			req->status_flag = PASS_STATUS;
		} else {
			req->status_flag = packet.result;
			dev_err(&sr_pdev->dev,
					"packet result error: %x\n", req->status_flag);
		}
		break;
	case READ_DATA:
		if (req->data_len < RPMB_PACKET_SIZE ||
			req->data_len > RPMB_PACKET_SIZE * 64) {
			req->status_flag = READ_LEN_ERROR;
			dev_err(&sr_pdev->dev, "data len is invalid\n");
			break;
		}

		req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
		req->outlen = RPMB_PACKET_SIZE;

		ret = srpmb_scsi_ioctl(sdp, req);
		if (ret < 0) {
			req->status_flag = READ_DATA_SECURITY_OUT_ERROR;
			dev_err(&sr_pdev->dev, "ioctl read data error: %x\n", ret);
			break;
		}

		memset(req->rpmb_data, 0x0, req->data_len);
		req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_IN;
		req->inlen = req->data_len;

		ret = srpmb_scsi_ioctl(sdp, req);
		if (ret < 0) {
			req->status_flag = READ_DATA_SECURITY_IN_ERROR;
			dev_err(&sr_pdev->dev,
					"ioctl result read data error : %x\n", ret);
			break;
		}
		req->status_flag = PASS_STATUS;

		break;
	default:
		dev_err(&sr_pdev->dev, "invalid requset type : %x\n", req->type);
	}
}

static irqreturn_t rpmb_irq_handler(int intr, void *arg)
{
	struct rpmb_irq_ctx *rpmb_ctx = (struct rpmb_irq_ctx *)arg;

	INIT_WORK(&rpmb_ctx->work, srpmb_worker);
	queue_work(rpmb_ctx->srpmb_queue, &rpmb_ctx->work);

	return IRQ_HANDLED;
}

int init_wsm(struct device *dev)
{
	int ret;
	struct rpmb_irq_ctx *rpmb_ctx;
	struct irq_data *rpmb_irqd = NULL;
	irq_hw_number_t hwirq = 0;

	rpmb_ctx = kzalloc(sizeof(struct rpmb_irq_ctx), GFP_KERNEL);
	if (!rpmb_ctx) {
		dev_err(&sr_pdev->dev, "kzalloc failed\n");
		goto out_srpmb_ctx_alloc_fail;
	}

	/* buffer init */
	rpmb_ctx->vir_addr = dma_alloc_coherent(&sr_pdev->dev,
			RPMB_BUF_MAX_SIZE, &rpmb_ctx->phy_addr, GFP_KERNEL);

	if (rpmb_ctx->vir_addr && rpmb_ctx->phy_addr) {
		rpmb_ctx->irq = irq_of_parse_and_map(sr_pdev->dev.of_node, 0);
		if (rpmb_ctx->irq <= 0) {
			dev_err(&sr_pdev->dev, "No IRQ number, aborting\n");
			return -EINVAL;
		}

		/* Get irq_data for secure log */
		rpmb_irqd = irq_get_irq_data(rpmb_ctx->irq);
		if (!rpmb_irqd) {
			dev_err(&sr_pdev->dev, "Fail to get irq_data\n");
			return -EINVAL;
		}

		/* Get hardware interrupt number */
		hwirq = irqd_to_hwirq(rpmb_irqd);
		dev_dbg(&sr_pdev->dev, "hwirq for srpmb (%ld)\n", hwirq);

		rpmb_ctx->dev = dev;
		rpmb_ctx->srpmb_queue = alloc_workqueue("srpmb_wq",
						WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
		if (!rpmb_ctx->srpmb_queue) {
			dev_err(&sr_pdev->dev,
				"Fail to alloc workqueue for ufs sprmb\n");
			goto out_srpmb_init_fail;
		}

		ret = request_irq(rpmb_ctx->irq, rpmb_irq_handler,
				IRQF_TRIGGER_RISING | IRQF_PERF_AFFINE, sr_pdev->name, rpmb_ctx);
		if (ret) {
			dev_err(&sr_pdev->dev, "request irq failed: %x\n", ret);
			goto out_srpmb_init_fail;
		}

		ret = exynos_smc(SMC_SRPMB_WSM, rpmb_ctx->phy_addr, hwirq, 0);
		if (ret) {
			dev_err(&sr_pdev->dev, "wsm smc init failed: %x\n", ret);
			goto out_srpmb_init_fail;
		}
	} else {
		dev_err(&sr_pdev->dev, "wsm dma alloc failed\n");
		goto out_srpmb_dma_alloc_fail;
	}

	return 0;

out_srpmb_init_fail:
	if (rpmb_ctx->srpmb_queue)
		destroy_workqueue(rpmb_ctx->srpmb_queue);

	dma_free_coherent(&sr_pdev->dev, RPMB_BUF_MAX_SIZE,
			rpmb_ctx->vir_addr, rpmb_ctx->phy_addr);

out_srpmb_dma_alloc_fail:
	kfree(rpmb_ctx);

out_srpmb_ctx_alloc_fail:
	return -ENOMEM;
}

static int srpmb_probe(struct platform_device *pdev)
{
	sr_pdev = pdev;

	return 0;
}

static const struct of_device_id of_match_table[] = {
	{ .compatible = SRPMB_DEVICE_PROPNAME },
	{ }
};

static struct platform_driver srpmb_plat_driver = {
	.probe = srpmb_probe,
	.driver = {
		.name = "exynos-ufs-srpmb",
		.owner = THIS_MODULE,
		.of_match_table = of_match_table,
	}
};

static int __init srpmb_init(void)
{
	return platform_driver_register(&srpmb_plat_driver);
}

static void __exit srpmb_exit(void)
{
	platform_driver_unregister(&srpmb_plat_driver);
}

subsys_initcall(srpmb_init);
module_exit(srpmb_exit);

MODULE_AUTHOR("Yongtaek Kwon <ycool.kwon@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UFS SRPMB driver");
