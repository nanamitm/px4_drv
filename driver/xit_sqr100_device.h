// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for PIXELA XIT-SQR100 device (xit_sqr100_device.h)
 */

#ifndef __XIT_SQR100_DEVICE_H__
#define __XIT_SQR100_DEVICE_H__

#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/device.h>

#include "ptx_chrdev.h"
#include "px4_card.h"
#include "it930x.h"
#include "cxd2856er.h"
#include "cxd6866.h"

#define XITSQR100_CHRDEV_NUM	1

struct xit_sqr100_chrdev {
	struct ptx_chrdev *chrdev;
	struct cxd2856er_demod cxd2856er;
	struct cxd6866_tuner cxd6866;
};

struct xit_sqr100_device {
	struct kref kref;
	/* チューナーと内蔵カードリーダーによる IT930x の操作、基板電源の参照、利用可能状態の切り替えを直列化する */
	struct mutex lock;
	atomic_t available;
	struct px4_card *card;
	struct device *dev;
	struct completion *quit_completion;
	/* チューナーを開いているか (受信機は1台) */
	bool tuner_open;
	/* 内蔵カードリーダーが基板電源の参照を持っているか */
	bool card_powered;
	struct ptx_chrdev_group *chrdev_group;
	struct xit_sqr100_chrdev chrdevx;
	struct it930x_bridge it930x;
	void *stream_ctx;
};

int xit_sqr100_device_init(struct xit_sqr100_device *xit, struct device *dev,
			   struct ptx_chrdev_context *chrdev_ctx,
			   struct completion *quit_completion);
void xit_sqr100_device_term(struct xit_sqr100_device *xit);

#endif
