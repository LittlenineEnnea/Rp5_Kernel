// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Broadcom
 */

/**
 * DOC: VC4 HVS module.
 *
 * The Hardware Video Scaler (HVS) is the piece of hardware that does
 * translation, scaling, colorspace conversion, and compositing of
 * pixels stored in framebuffers into a FIFO of pixels going out to
 * the Pixel Valve (CRTC).  It operates at the system clock rate (the
 * system audio clock gate, specifically), which is much higher than
 * the pixel clock rate.
 *
 * There is a single global HVS, with multiple output FIFOs that can
 * be consumed by the PVs.  This file just manages the resources for
 * the HVS, while the vc4_crtc.c code actually drives HVS setup for
 * each CRTC.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_vblank.h>

#include <soc/bcm2835/raspberrypi-firmware.h>

#include "vc4_drv.h"
#include "vc4_regs.h"

static const struct debugfs_reg32 vc4_hvs_regs[] = {
	VC4_REG32(SCALER_DISPCTRL),
	VC4_REG32(SCALER_DISPSTAT),
	VC4_REG32(SCALER_DISPID),
	VC4_REG32(SCALER_DISPECTRL),
	VC4_REG32(SCALER_DISPPROF),
	VC4_REG32(SCALER_DISPDITHER),
	VC4_REG32(SCALER_DISPEOLN),
	VC4_REG32(SCALER_DISPLIST0),
	VC4_REG32(SCALER_DISPLIST1),
	VC4_REG32(SCALER_DISPLIST2),
	VC4_REG32(SCALER_DISPLSTAT),
	VC4_REG32(SCALER_DISPLACT0),
	VC4_REG32(SCALER_DISPLACT1),
	VC4_REG32(SCALER_DISPLACT2),
	VC4_REG32(SCALER_DISPCTRL0),
	VC4_REG32(SCALER_DISPBKGND0),
	VC4_REG32(SCALER_DISPSTAT0),
	VC4_REG32(SCALER_DISPBASE0),
	VC4_REG32(SCALER_DISPCTRL1),
	VC4_REG32(SCALER_DISPBKGND1),
	VC4_REG32(SCALER_DISPSTAT1),
	VC4_REG32(SCALER_DISPBASE1),
	VC4_REG32(SCALER_DISPCTRL2),
	VC4_REG32(SCALER_DISPBKGND2),
	VC4_REG32(SCALER_DISPSTAT2),
	VC4_REG32(SCALER_DISPBASE2),
	VC4_REG32(SCALER_DISPALPHA2),
	VC4_REG32(SCALER_OLEDOFFS),
	VC4_REG32(SCALER_OLEDCOEF0),
	VC4_REG32(SCALER_OLEDCOEF1),
	VC4_REG32(SCALER_OLEDCOEF2),
};

static const struct debugfs_reg32 vc6_hvs_regs[] = {
	VC4_REG32(SCALER6_VERSION),
	VC4_REG32(SCALER6_CXM_SIZE),
	VC4_REG32(SCALER6_LBM_SIZE),
	VC4_REG32(SCALER6_UBM_SIZE),
	VC4_REG32(SCALER6_COBA_SIZE),
	VC4_REG32(SCALER6_COB_SIZE),
	VC4_REG32(SCALER6_CONTROL),
	VC4_REG32(SCALER6_FETCHER_STATUS),
	VC4_REG32(SCALER6_FETCH_STATUS),
	VC4_REG32(SCALER6_HANDLE_ERROR),
	VC4_REG32(SCALER6_DISP0_CTRL0),
	VC4_REG32(SCALER6_DISP0_CTRL1),
	VC4_REG32(SCALER6_DISP0_BGND),
	VC4_REG32(SCALER6_DISP0_LPTRS),
	VC4_REG32(SCALER6_DISP0_COB),
	VC4_REG32(SCALER6_DISP0_STATUS),
	VC4_REG32(SCALER6_DISP0_DL),
	VC4_REG32(SCALER6_DISP0_RUN),
	VC4_REG32(SCALER6_DISP1_CTRL0),
	VC4_REG32(SCALER6_DISP1_CTRL1),
	VC4_REG32(SCALER6_DISP1_BGND),
	VC4_REG32(SCALER6_DISP1_LPTRS),
	VC4_REG32(SCALER6_DISP1_COB),
	VC4_REG32(SCALER6_DISP1_STATUS),
	VC4_REG32(SCALER6_DISP1_DL),
	VC4_REG32(SCALER6_DISP1_RUN),
	VC4_REG32(SCALER6_DISP2_CTRL0),
	VC4_REG32(SCALER6_DISP2_CTRL1),
	VC4_REG32(SCALER6_DISP2_BGND),
	VC4_REG32(SCALER6_DISP2_LPTRS),
	VC4_REG32(SCALER6_DISP2_COB),
	VC4_REG32(SCALER6_DISP2_STATUS),
	VC4_REG32(SCALER6_DISP2_DL),
	VC4_REG32(SCALER6_DISP2_RUN),
	VC4_REG32(SCALER6_EOLN),
	VC4_REG32(SCALER6_DL_STATUS),
	VC4_REG32(SCALER6_BFG_MISC),
	VC4_REG32(SCALER6_QOS0),
	VC4_REG32(SCALER6_PROF0),
	VC4_REG32(SCALER6_QOS1),
	VC4_REG32(SCALER6_PROF1),
	VC4_REG32(SCALER6_QOS2),
	VC4_REG32(SCALER6_PROF2),
	VC4_REG32(SCALER6_PRI_MAP0),
	VC4_REG32(SCALER6_PRI_MAP1),
	VC4_REG32(SCALER6_HISTCTRL),
	VC4_REG32(SCALER6_HISTBIN0),
	VC4_REG32(SCALER6_HISTBIN1),
	VC4_REG32(SCALER6_HISTBIN2),
	VC4_REG32(SCALER6_HISTBIN3),
	VC4_REG32(SCALER6_HISTBIN4),
	VC4_REG32(SCALER6_HISTBIN5),
	VC4_REG32(SCALER6_HISTBIN6),
	VC4_REG32(SCALER6_HISTBIN7),
	VC4_REG32(SCALER6_HDR_CFG_REMAP),
	VC4_REG32(SCALER6_COL_SPACE),
	VC4_REG32(SCALER6_HVS_ID),
	VC4_REG32(SCALER6_CFC1),
	VC4_REG32(SCALER6_DISP_UPM_ISO0),
	VC4_REG32(SCALER6_DISP_UPM_ISO1),
	VC4_REG32(SCALER6_DISP_UPM_ISO2),
	VC4_REG32(SCALER6_DISP_LBM_ISO0),
	VC4_REG32(SCALER6_DISP_LBM_ISO1),
	VC4_REG32(SCALER6_DISP_LBM_ISO2),
	VC4_REG32(SCALER6_DISP_COB_ISO0),
	VC4_REG32(SCALER6_DISP_COB_ISO1),
	VC4_REG32(SCALER6_DISP_COB_ISO2),
	VC4_REG32(SCALER6_BAD_COB),
	VC4_REG32(SCALER6_BAD_LBM),
	VC4_REG32(SCALER6_BAD_UPM),
	VC4_REG32(SCALER6_BAD_AXI),
};

static const struct debugfs_reg32 vc6_d_hvs_regs[] = {
	VC4_REG32(SCALER6D_VERSION),
	VC4_REG32(SCALER6D_CXM_SIZE),
	VC4_REG32(SCALER6D_LBM_SIZE),
	VC4_REG32(SCALER6D_UBM_SIZE),
	VC4_REG32(SCALER6D_COBA_SIZE),
	VC4_REG32(SCALER6D_COB_SIZE),
	VC4_REG32(SCALER6D_CONTROL),
	VC4_REG32(SCALER6D_FETCHER_STATUS),
	VC4_REG32(SCALER6D_FETCH_STATUS),
	VC4_REG32(SCALER6D_HANDLE_ERROR),
	VC4_REG32(SCALER6D_DISP0_CTRL0),
	VC4_REG32(SCALER6D_DISP0_CTRL1),
	VC4_REG32(SCALER6D_DISP0_BGND0),
	VC4_REG32(SCALER6D_DISP0_BGND1),
	VC4_REG32(SCALER6D_DISP0_LPTRS),
	VC4_REG32(SCALER6D_DISP0_COB),
	VC4_REG32(SCALER6D_DISP0_STATUS),
	VC4_REG32(SCALER6D_DISP0_DL),
	VC4_REG32(SCALER6D_DISP0_RUN),
	VC4_REG32(SCALER6D_DISP1_CTRL0),
	VC4_REG32(SCALER6D_DISP1_CTRL1),
	VC4_REG32(SCALER6D_DISP1_BGND0),
	VC4_REG32(SCALER6D_DISP1_BGND1),
	VC4_REG32(SCALER6D_DISP1_LPTRS),
	VC4_REG32(SCALER6D_DISP1_COB),
	VC4_REG32(SCALER6D_DISP1_STATUS),
	VC4_REG32(SCALER6D_DISP1_DL),
	VC4_REG32(SCALER6D_DISP1_RUN),
	VC4_REG32(SCALER6D_DISP2_CTRL0),
	VC4_REG32(SCALER6D_DISP2_CTRL1),
	VC4_REG32(SCALER6D_DISP2_BGND0),
	VC4_REG32(SCALER6D_DISP2_BGND1),
	VC4_REG32(SCALER6D_DISP2_LPTRS),
	VC4_REG32(SCALER6D_DISP2_COB),
	VC4_REG32(SCALER6D_DISP2_STATUS),
	VC4_REG32(SCALER6D_DISP2_DL),
	VC4_REG32(SCALER6D_DISP2_RUN),
	VC4_REG32(SCALER6D_EOLN),
	VC4_REG32(SCALER6D_DL_STATUS),
	VC4_REG32(SCALER6D_QOS0),
	VC4_REG32(SCALER6D_PROF0),
	VC4_REG32(SCALER6D_QOS1),
	VC4_REG32(SCALER6D_PROF1),
	VC4_REG32(SCALER6D_QOS2),
	VC4_REG32(SCALER6D_PROF2),
	VC4_REG32(SCALER6D_PRI_MAP0),
	VC4_REG32(SCALER6D_PRI_MAP1),
	VC4_REG32(SCALER6D_HISTCTRL),
	VC4_REG32(SCALER6D_HISTBIN0),
	VC4_REG32(SCALER6D_HISTBIN1),
	VC4_REG32(SCALER6D_HISTBIN2),
	VC4_REG32(SCALER6D_HISTBIN3),
	VC4_REG32(SCALER6D_HISTBIN4),
	VC4_REG32(SCALER6D_HISTBIN5),
	VC4_REG32(SCALER6D_HISTBIN6),
	VC4_REG32(SCALER6D_HISTBIN7),
	VC4_REG32(SCALER6D_HVS_ID),
};

void vc4_hvs_dump_state(struct vc4_hvs *hvs)
{
	struct drm_device *drm = &hvs->vc4->base;
	struct drm_printer p = drm_info_printer(&hvs->pdev->dev);
	int idx, i;

	if (!drm_dev_enter(drm, &idx))
		return;

	drm_print_regset32(&p, &hvs->regset);

	DRM_INFO("HVS ctx:\n");
	for (i = 0; i < 64; i += 4) {
		DRM_INFO("0x%08x (%s): 0x%08x 0x%08x 0x%08x 0x%08x\n",
			 i * 4, i < HVS_BOOTLOADER_DLIST_END ? "B" : "D",
			 readl((u32 __iomem *)hvs->dlist + i + 0),
			 readl((u32 __iomem *)hvs->dlist + i + 1),
			 readl((u32 __iomem *)hvs->dlist + i + 2),
			 readl((u32 __iomem *)hvs->dlist + i + 3));
	}

	drm_dev_exit(idx);
}

static int vc4_hvs_debugfs_underrun(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_printer p = drm_seq_file_printer(m);

	drm_printf(&p, "%d\n", atomic_read(&vc4->underrun));

	return 0;
}

static int vc4_hvs_debugfs_dlist(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_printer p = drm_seq_file_printer(m);
	unsigned int dlist_mem_size = hvs->dlist_mem_size;
	unsigned int next_entry_start;
	unsigned int i, j;
	u32 dlist_word, dispstat;

	for (i = 0; i < SCALER_CHANNELS_COUNT; i++) {
		dispstat = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTATX(i)),
					 SCALER_DISPSTATX_MODE);
		if (dispstat == SCALER_DISPSTATX_MODE_DISABLED ||
		    dispstat == SCALER_DISPSTATX_MODE_EOF) {
			drm_printf(&p, "HVS chan %u disabled\n", i);
			continue;
		}

		drm_printf(&p, "HVS chan %u:\n", i);
		next_entry_start = 0;

		for (j = HVS_READ(SCALER_DISPLISTX(i)); j < dlist_mem_size; j++) {
			dlist_word = readl((u32 __iomem *)vc4->hvs->dlist + j);
			drm_printf(&p, "dlist: %02d: 0x%08x\n", j,
				   dlist_word);
			if (!next_entry_start ||
			    next_entry_start == j) {
				if (dlist_word & SCALER_CTL0_END)
					break;
				next_entry_start = j +
					VC4_GET_FIELD(dlist_word,
						      SCALER_CTL0_SIZE);
			}
		}
	}

	return 0;
}

static int vc6_hvs_debugfs_dlist(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_printer p = drm_seq_file_printer(m);
	unsigned int dlist_mem_size = hvs->dlist_mem_size;
	unsigned int next_entry_start;
	unsigned int i;

	for (i = 0; i < SCALER_CHANNELS_COUNT; i++) {
		unsigned int active_dlist, dispstat;
		unsigned int j;

		dispstat = VC4_GET_FIELD(HVS_READ(SCALER6_DISPX_STATUS(i)),
					 SCALER6_DISPX_STATUS_MODE);
		if (dispstat == SCALER6_DISPX_STATUS_MODE_DISABLED ||
		    dispstat == SCALER6_DISPX_STATUS_MODE_EOF) {
			drm_printf(&p, "HVS chan %u disabled\n", i);
			continue;
		}

		drm_printf(&p, "HVS chan %u:\n", i);

		active_dlist = VC4_GET_FIELD(HVS_READ(SCALER6_DISPX_DL(i)),
					     SCALER6_DISPX_DL_LACT);
		next_entry_start = 0;

		for (j = active_dlist; j < dlist_mem_size; j++) {
			u32 dlist_word;

			dlist_word = readl((u32 __iomem *)vc4->hvs->dlist + j);
			drm_printf(&p, "dlist: %02d: 0x%08x\n", j,
				   dlist_word);
			if (!next_entry_start ||
			    next_entry_start == j) {
				if (dlist_word & SCALER_CTL0_END)
					break;
				next_entry_start = j +
					VC4_GET_FIELD(dlist_word,
						      SCALER_CTL0_SIZE);
			}
		}
	}

	return 0;
}

static int vc6_hvs_debugfs_upm_allocs(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_printer p = drm_seq_file_printer(m);
	struct vc4_upm_refcounts *refcount;
	unsigned int i;

	drm_printf(&p, "UPM Handles:\n");
	for (i = 1; i <= VC4_NUM_UPM_HANDLES; i++) {
		refcount = &hvs->upm_refcounts[i];
		drm_printf(&p, "handle %u: refcount %u, size %zu [%08llx + %08llx]\n",
			   i, refcount_read(&refcount->refcount), refcount->size,
			   refcount->upm.start, refcount->upm.size);
	}

	return 0;
}

static int vc4_hvs_debugfs_dlist_allocs(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_printer p = drm_seq_file_printer(m);
	struct vc4_hvs_dlist_allocation *cur, *next;
	struct drm_mm_node *mm_node;
	unsigned long flags;

	spin_lock_irqsave(&hvs->mm_lock, flags);

	drm_printf(&p, "Allocated nodes:\n");
	list_for_each_entry(mm_node, drm_mm_nodes(&hvs->dlist_mm), node_list) {
		drm_printf(&p, "node [%08llx + %08llx]\n", mm_node->start, mm_node->size);
	}

	drm_printf(&p, "Stale nodes:\n");
	list_for_each_entry_safe(cur, next, &hvs->stale_dlist_entries, node) {
		drm_printf(&p, "node [%08llx + %08llx] channel %u frcnt %u\n",
			   cur->mm_node.start, cur->mm_node.size, cur->channel,
			   cur->target_frame_count);
	}

	spin_unlock_irqrestore(&hvs->mm_lock, flags);

	return 0;
}

static int vc4_hvs_debugfs_lbm_allocs(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_printer p = drm_seq_file_printer(m);
	struct vc4_lbm_refcounts *refcount;
	unsigned int i;

	drm_printf(&p, "LBM Handles:\n");
	for (i = 0; i < VC4_NUM_LBM_HANDLES; i++) {
		refcount = &hvs->lbm_refcounts[i];
		drm_printf(&p, "handle %u: refcount %u, size %zu [%08llx + %08llx]\n",
			   i, refcount_read(&refcount->refcount), refcount->size,
			   refcount->lbm.start, refcount->lbm.size);
	}

	return 0;
}

/* The filter kernel is composed of dwords each containing 3 9-bit
 * signed integers packed next to each other.
 */
#define VC4_INT_TO_COEFF(coeff) (coeff & 0x1ff)
#define VC4_PPF_FILTER_WORD(c0, c1, c2)				\
	((((c0) & 0x1ff) << 0) |				\
	 (((c1) & 0x1ff) << 9) |				\
	 (((c2) & 0x1ff) << 18))

/* The whole filter kernel is arranged as the coefficients 0-16 going
 * up, then a pad, then 17-31 going down and reversed within the
 * dwords.  This means that a linear phase kernel (where it's
 * symmetrical at the boundary between 15 and 16) has the last 5
 * dwords matching the first 5, but reversed.
 */
#define VC4_LINEAR_PHASE_KERNEL(c0, c1, c2, c3, c4, c5, c6, c7, c8,	\
				c9, c10, c11, c12, c13, c14, c15)	\
	{VC4_PPF_FILTER_WORD(c0, c1, c2),				\
	 VC4_PPF_FILTER_WORD(c3, c4, c5),				\
	 VC4_PPF_FILTER_WORD(c6, c7, c8),				\
	 VC4_PPF_FILTER_WORD(c9, c10, c11),				\
	 VC4_PPF_FILTER_WORD(c12, c13, c14),				\
	 VC4_PPF_FILTER_WORD(c15, c15, 0)}

#define VC4_LINEAR_PHASE_KERNEL_DWORDS 6
#define VC4_KERNEL_DWORDS (VC4_LINEAR_PHASE_KERNEL_DWORDS * 2 - 1)

/* Recommended B=1/3, C=1/3 filter choice from Mitchell/Netravali.
 * http://www.cs.utexas.edu/~fussell/courses/cs384g/lectures/mitchell/Mitchell.pdf
 */
static const u32 mitchell_netravali_1_3_1_3_kernel[] =
	VC4_LINEAR_PHASE_KERNEL(0, -2, -6, -8, -10, -8, -3, 2, 18,
				50, 82, 119, 155, 187, 213, 227);
static const u32 nearest_neighbour_kernel[] =
	VC4_LINEAR_PHASE_KERNEL(0, 0, 0, 0, 0, 0, 0, 0,
				1, 1, 1, 1, 255, 255, 255, 255);

static int vc4_hvs_upload_linear_kernel(struct vc4_hvs *hvs,
					struct drm_mm_node *space,
					const u32 *kernel)
{
	int ret, i;
	u32 __iomem *dst_kernel;

	/*
	 * NOTE: We don't need a call to drm_dev_enter()/drm_dev_exit()
	 * here since that function is only called from vc4_hvs_bind().
	 */

	ret = drm_mm_insert_node(&hvs->dlist_mm, space, VC4_KERNEL_DWORDS);
	if (ret) {
		drm_err(&hvs->vc4->base, "Failed to allocate space for filter kernel: %d\n",
			ret);
		return ret;
	}

	dst_kernel = hvs->dlist + space->start;

	for (i = 0; i < VC4_KERNEL_DWORDS; i++) {
		if (i < VC4_LINEAR_PHASE_KERNEL_DWORDS)
			writel(kernel[i], &dst_kernel[i]);
		else {
			writel(kernel[VC4_KERNEL_DWORDS - i - 1],
			       &dst_kernel[i]);
		}
	}

	return 0;
}

static void vc4_hvs_lut_load(struct vc4_hvs *hvs,
			     struct vc4_crtc *vc4_crtc)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	struct drm_crtc *crtc = &vc4_crtc->base;
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	int idx;
	u32 i;

	WARN_ON_ONCE(vc4->gen > VC4_GEN_5);

	if (!drm_dev_enter(drm, &idx))
		return;

	if (hvs->vc4->gen != VC4_GEN_4)
		goto exit;

	/* The LUT memory is laid out with each HVS channel in order,
	 * each of which takes 256 writes for R, 256 for G, then 256
	 * for B.
	 */
	HVS_WRITE(SCALER_GAMADDR,
		  SCALER_GAMADDR_AUTOINC |
		  (vc4_state->assigned_channel * 3 * crtc->gamma_size));

	for (i = 0; i < crtc->gamma_size; i++)
		HVS_WRITE(SCALER_GAMDATA, vc4_crtc->lut_r[i]);
	for (i = 0; i < crtc->gamma_size; i++)
		HVS_WRITE(SCALER_GAMDATA, vc4_crtc->lut_g[i]);
	for (i = 0; i < crtc->gamma_size; i++)
		HVS_WRITE(SCALER_GAMDATA, vc4_crtc->lut_b[i]);

exit:
	drm_dev_exit(idx);
}

static void vc4_hvs_update_gamma_lut(struct vc4_hvs *hvs,
				     struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc_state *crtc_state = vc4_crtc->base.state;
	struct drm_color_lut *lut = crtc_state->gamma_lut->data;
	u32 length = drm_color_lut_size(crtc_state->gamma_lut);
	u32 i;

	for (i = 0; i < length; i++) {
		vc4_crtc->lut_r[i] = drm_color_lut_extract(lut[i].red, 8);
		vc4_crtc->lut_g[i] = drm_color_lut_extract(lut[i].green, 8);
		vc4_crtc->lut_b[i] = drm_color_lut_extract(lut[i].blue, 8);
	}

	vc4_hvs_lut_load(hvs, vc4_crtc);
}

static void vc4_hvs_irq_enable_eof(struct vc4_hvs *hvs,
				   unsigned int channel)
{
	struct vc4_dev *vc4 = hvs->vc4;

	if (hvs->eof_irq[channel].enabled)
		return;

	switch (vc4->gen) {
	case VC4_GEN_4:
		HVS_WRITE(SCALER_DISPCTRL,
			  HVS_READ(SCALER_DISPCTRL) |
			  SCALER_DISPCTRL_DSPEIEOF(channel));
		break;

	case VC4_GEN_5:
		HVS_WRITE(SCALER_DISPCTRL,
			  HVS_READ(SCALER_DISPCTRL) |
			  SCALER5_DISPCTRL_DSPEIEOF(channel));
		break;

	case VC4_GEN_6_C:
	case VC4_GEN_6_D:
		enable_irq(hvs->eof_irq[channel].desc);
		break;

	default:
		break;
	}

	hvs->eof_irq[channel].enabled = true;
}

static void vc4_hvs_irq_clear_eof(struct vc4_hvs *hvs,
				  unsigned int channel)
{
	struct vc4_dev *vc4 = hvs->vc4;

	if (!hvs->eof_irq[channel].enabled)
		return;

	switch (vc4->gen) {
	case VC4_GEN_4:
		HVS_WRITE(SCALER_DISPCTRL,
			  HVS_READ(SCALER_DISPCTRL) &
			  ~SCALER_DISPCTRL_DSPEIEOF(channel));
		break;

	case VC4_GEN_5:
		HVS_WRITE(SCALER_DISPCTRL,
			  HVS_READ(SCALER_DISPCTRL) &
			  ~SCALER5_DISPCTRL_DSPEIEOF(channel));
		break;

	case VC4_GEN_6_C:
	case VC4_GEN_6_D:
		disable_irq_nosync(hvs->eof_irq[channel].desc);
		break;

	default:
		break;
	}

	hvs->eof_irq[channel].enabled = false;
}

static void vc4_hvs_free_dlist_entry_locked(struct vc4_hvs *hvs,
					    struct vc4_hvs_dlist_allocation *alloc);

static struct vc4_hvs_dlist_allocation *
vc4_hvs_alloc_dlist_entry(struct vc4_hvs *hvs,
			  unsigned int channel,
			  size_t dlist_count)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *dev = &vc4->base;
	struct vc4_hvs_dlist_allocation *alloc;
	struct vc4_hvs_dlist_allocation *cur, *next;
	unsigned long flags;
	int ret;

	if (channel == VC4_HVS_CHANNEL_DISABLED)
		return NULL;

	alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&alloc->node);

	spin_lock_irqsave(&hvs->mm_lock, flags);
	ret = drm_mm_insert_node(&hvs->dlist_mm, &alloc->mm_node,
				 dlist_count);
	spin_unlock_irqrestore(&hvs->mm_lock, flags);

	if (ret) {
		drm_err(dev, "Failed to allocate DLIST entry. Requested size=%zu. ret=%d. DISPCTRL is %08x\n",
			dlist_count, ret, HVS_READ(SCALER_DISPCTRL));

		/* This should never happen as stale entries should get released
		 * as the frame counter interrupt triggers.
		 * However we've seen this fail for reasons currently unknown.
		 * Free all stale entries now so we should be able to complete
		 * this allocation.
		 */
		spin_lock_irqsave(&hvs->mm_lock, flags);
		list_for_each_entry_safe(cur, next, &hvs->stale_dlist_entries, node) {
			vc4_hvs_free_dlist_entry_locked(hvs, cur);
		}

		ret = drm_mm_insert_node(&hvs->dlist_mm, &alloc->mm_node,
					 dlist_count);
		spin_unlock_irqrestore(&hvs->mm_lock, flags);

		if (ret)
			return ERR_PTR(ret);
	}

	alloc->channel = channel;

	return alloc;
}

static void vc4_hvs_free_dlist_entry_locked(struct vc4_hvs *hvs,
					    struct vc4_hvs_dlist_allocation *alloc)
{
	lockdep_assert_held(&hvs->mm_lock);

	if (!list_empty(&alloc->node))
		list_del(&alloc->node);

	drm_mm_remove_node(&alloc->mm_node);
	kfree(alloc);
}

void vc4_hvs_mark_dlist_entry_stale(struct vc4_hvs *hvs,
				    struct vc4_hvs_dlist_allocation *alloc)
{
	unsigned long flags;
	u8 frcnt;

	if (!alloc)
		return;

	if (!drm_mm_node_allocated(&alloc->mm_node))
		return;

	/*
	 * Kunit tests run with a mock device and we consider any hardware
	 * access a test failure. Let's free the dlist allocation right away if
	 * we're running under kunit, we won't risk a dlist corruption anyway.
	 *
	 * Likewise if the allocation was only checked and never programmed, we
	 * can destroy the allocation immediately.
	 */
	if (kunit_get_current_test() || !alloc->dlist_programmed) {
		spin_lock_irqsave(&hvs->mm_lock, flags);
		vc4_hvs_free_dlist_entry_locked(hvs, alloc);
		spin_unlock_irqrestore(&hvs->mm_lock, flags);
		return;
	}

	frcnt = vc4_hvs_get_fifo_frame_count(hvs, alloc->channel);
	alloc->target_frame_count = (frcnt + 1) & ((1 << 6) - 1);

	spin_lock_irqsave(&hvs->mm_lock, flags);

	list_add_tail(&alloc->node, &hvs->stale_dlist_entries);

	HVS_WRITE(SCALER_DISPSTAT, SCALER_DISPSTAT_EOF(alloc->channel));
	vc4_hvs_irq_enable_eof(hvs, alloc->channel);

	spin_unlock_irqrestore(&hvs->mm_lock, flags);
}

static void vc4_hvs_schedule_dlist_sweep(struct vc4_hvs *hvs,
					 unsigned int channel)
{
	unsigned long flags;

	spin_lock_irqsave(&hvs->mm_lock, flags);

	if (!list_empty(&hvs->stale_dlist_entries))
		queue_work(system_unbound_wq, &hvs->free_dlist_work);

	if (list_empty(&hvs->stale_dlist_entries))
		vc4_hvs_irq_clear_eof(hvs, channel);

	spin_unlock_irqrestore(&hvs->mm_lock, flags);
}

/*
 * Frame counts are essentially sequence numbers over 6 bits, and we
 * thus can use sequence number arithmetic and follow the RFC1982 to
 * implement proper comparison between them.
 */
static bool vc4_hvs_frcnt_lte(u8 cnt1, u8 cnt2)
{
	return (s8)((cnt1 << 2) - (cnt2 << 2)) <= 0;
}

static bool vc4_hvs_check_channel_active(struct vc4_hvs *hvs, unsigned int fifo)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	bool enabled = false;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return 0;

	if (vc4->gen >= VC4_GEN_6_C)
		enabled = HVS_READ(SCALER6_DISPX_CTRL0(fifo)) & SCALER6_DISPX_CTRL0_ENB;
	else
		enabled = HVS_READ(SCALER_DISPCTRLX(fifo)) & SCALER_DISPCTRLX_ENABLE;

	drm_dev_exit(idx);
	return enabled;
}

/*
 * Some atomic commits (legacy cursor updates, mostly) will not wait for
 * the next vblank and will just return once the commit has been pushed
 * to the hardware.
 *
 * On the hardware side, our HVS stores the planes parameters in its
 * context RAM, and will use part of the RAM to store data during the
 * frame rendering.
 *
 * This interacts badly if we get multiple commits before the next
 * vblank since we could end up overwriting the DLIST entries used by
 * previous commits if our dlist allocation reuses that entry. In such a
 * case, we would overwrite the data currently being used by the
 * hardware, resulting in a corrupted frame.
 *
 * In order to work around this, we'll queue the dlist entries in a list
 * once the associated CRTC state is destroyed. The HVS only allows us
 * to know which entry is being active, but not which one are no longer
 * being used, so in order to avoid freeing entries that are still used
 * by the hardware we add a guesstimate of the frame count where our
 * entry will no longer be used, and thus will only free those entries
 * when we will have reached that frame count.
 */
static void vc4_hvs_dlist_free_work(struct work_struct *work)
{
	struct vc4_hvs *hvs = container_of(work, struct vc4_hvs, free_dlist_work);
	struct vc4_hvs_dlist_allocation *cur, *next;
	unsigned long flags;
	bool active[3];
	u8 frcnt[3];
	int i;

	spin_lock_irqsave(&hvs->mm_lock, flags);
	for (i = 0; i < 3; i++) {
		frcnt[i] = vc4_hvs_get_fifo_frame_count(hvs, i);
		active[i] = vc4_hvs_check_channel_active(hvs, i);
	}
	list_for_each_entry_safe(cur, next, &hvs->stale_dlist_entries, node) {
		if (active[cur->channel] &&
		    !vc4_hvs_frcnt_lte(cur->target_frame_count, frcnt[cur->channel]))
			continue;

		vc4_hvs_free_dlist_entry_locked(hvs, cur);
	}
	spin_unlock_irqrestore(&hvs->mm_lock, flags);
}

u8 vc4_hvs_get_fifo_frame_count(struct vc4_hvs *hvs, unsigned int fifo)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	u8 field = 0;
	int idx;

	WARN_ON_ONCE(vc4->gen > VC4_GEN_6_D);

	if (!drm_dev_enter(drm, &idx))
		return 0;

	switch (vc4->gen) {
	case VC4_GEN_6_C:
	case VC4_GEN_6_D:
		field = VC4_GET_FIELD(HVS_READ(SCALER6_DISPX_STATUS(fifo)),
				      SCALER6_DISPX_STATUS_FRCNT);
		break;
	case VC4_GEN_5:
		switch (fifo) {
		case 0:
			field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT1),
					      SCALER5_DISPSTAT1_FRCNT0);
			break;
		case 1:
			field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT1),
					      SCALER5_DISPSTAT1_FRCNT1);
			break;
		case 2:
			field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT2),
					      SCALER5_DISPSTAT2_FRCNT2);
			break;
		}
		break;
	case VC4_GEN_4:
		switch (fifo) {
		case 0:
			field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT1),
					      SCALER_DISPSTAT1_FRCNT0);
			break;
		case 1:
			field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT1),
					      SCALER_DISPSTAT1_FRCNT1);
			break;
		case 2:
			field = VC4_GET_FIELD(HVS_READ(SCALER_DISPSTAT2),
					      SCALER_DISPSTAT2_FRCNT2);
			break;
		}
		break;
	default:
		drm_err(drm, "Unknown VC4 generation: %d", vc4->gen);
		return 0;
	}

	drm_dev_exit(idx);
	return field;
}

int vc4_hvs_get_fifo_from_output(struct vc4_hvs *hvs, unsigned int output)
{
	struct vc4_dev *vc4 = hvs->vc4;
	u32 reg;
	int ret;

	WARN_ON_ONCE(vc4->gen > VC4_GEN_6_D);

	switch (vc4->gen) {
	case VC4_GEN_4:
		return output;

	case VC4_GEN_5:
		/*
		 * NOTE: We should probably use
		 * drm_dev_enter()/drm_dev_exit() here, but this
		 * function is only used during the DRM device
		 * initialization, so we should be fine.
		 */

		switch (output) {
		case 0:
			return 0;

		case 1:
			return 1;

		case 2:
			reg = HVS_READ(SCALER_DISPECTRL);
			ret = FIELD_GET(SCALER_DISPECTRL_DSP2_MUX_MASK, reg);
			if (ret == 0)
				return 2;

			return 0;

		case 3:
			reg = HVS_READ(SCALER_DISPCTRL);
			ret = FIELD_GET(SCALER_DISPCTRL_DSP3_MUX_MASK, reg);
			if (ret == 3)
				return -EPIPE;

			return ret;

		case 4:
			reg = HVS_READ(SCALER_DISPEOLN);
			ret = FIELD_GET(SCALER_DISPEOLN_DSP4_MUX_MASK, reg);
			if (ret == 3)
				return -EPIPE;

			return ret;

		case 5:
			reg = HVS_READ(SCALER_DISPDITHER);
			ret = FIELD_GET(SCALER_DISPDITHER_DSP5_MUX_MASK, reg);
			if (ret == 3)
				return -EPIPE;

			return ret;

		default:
			return -EPIPE;
		}

	case VC4_GEN_6_C:
	case VC4_GEN_6_D:
		switch (output) {
		case 0:
			return 0;

		case 2:
			return 2;

		case 1:
		case 3:
		case 4:
			return 1;

		default:
			return -EPIPE;
		}

	default:
		return -EPIPE;
	}
}

static int vc4_hvs_init_channel(struct vc4_hvs *hvs, struct drm_crtc *crtc,
				struct drm_display_mode *mode, bool oneshot)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_crtc_state = to_vc4_crtc_state(crtc->state);
	unsigned int chan = vc4_crtc_state->assigned_channel;
	bool interlace = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 dispbkgndx;
	u32 dispctrl;
	int idx;

	WARN_ON_ONCE(vc4->gen > VC4_GEN_5);

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	HVS_WRITE(SCALER_DISPCTRLX(chan), 0);
	HVS_WRITE(SCALER_DISPCTRLX(chan), SCALER_DISPCTRLX_RESET);
	HVS_WRITE(SCALER_DISPCTRLX(chan), 0);

	/* Turn on the scaler, which will wait for vstart to start
	 * compositing.
	 * When feeding the transposer, we should operate in oneshot
	 * mode.
	 */
	dispctrl = SCALER_DISPCTRLX_ENABLE;
	dispbkgndx = HVS_READ(SCALER_DISPBKGNDX(chan));

	if (vc4->gen == VC4_GEN_4) {
		dispctrl |= VC4_SET_FIELD(mode->hdisplay,
					  SCALER_DISPCTRLX_WIDTH) |
			    VC4_SET_FIELD(mode->vdisplay,
					  SCALER_DISPCTRLX_HEIGHT) |
			    (oneshot ? SCALER_DISPCTRLX_ONESHOT : 0);
		dispbkgndx |= SCALER_DISPBKGND_AUTOHS;
	} else {
		dispctrl |= VC4_SET_FIELD(mode->hdisplay,
					  SCALER5_DISPCTRLX_WIDTH) |
			    VC4_SET_FIELD(mode->vdisplay,
					  SCALER5_DISPCTRLX_HEIGHT) |
			    (oneshot ? SCALER5_DISPCTRLX_ONESHOT : 0);
		dispbkgndx &= ~SCALER5_DISPBKGND_BCK2BCK;
	}

	HVS_WRITE(SCALER_DISPCTRLX(chan), dispctrl);

	dispbkgndx &= ~SCALER_DISPBKGND_GAMMA;
	dispbkgndx &= ~SCALER_DISPBKGND_INTERLACE;

	HVS_WRITE(SCALER_DISPBKGNDX(chan), dispbkgndx |
		  ((vc4->gen == VC4_GEN_4) ? SCALER_DISPBKGND_GAMMA : 0) |
		  (interlace ? SCALER_DISPBKGND_INTERLACE : 0));

	/* Reload the LUT, since the SRAMs would have been disabled if
	 * all CRTCs had SCALER_DISPBKGND_GAMMA unset at once.
	 */
	vc4_hvs_lut_load(hvs, vc4_crtc);

	drm_dev_exit(idx);

	return 0;
}

static int vc6_hvs_init_channel(struct vc4_hvs *hvs, struct drm_crtc *crtc,
				struct drm_display_mode *mode, bool oneshot)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	struct vc4_crtc_state *vc4_crtc_state = to_vc4_crtc_state(crtc->state);
	unsigned int chan = vc4_crtc_state->assigned_channel;
	bool interlace = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 disp_ctrl1;
	int idx;

	WARN_ON_ONCE(vc4->gen < VC4_GEN_6_C);

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	HVS_WRITE(SCALER6_DISPX_CTRL0(chan), SCALER6_DISPX_CTRL0_RESET);

	disp_ctrl1 = HVS_READ(SCALER6_DISPX_CTRL1(chan));
	disp_ctrl1 &= ~SCALER6_DISPX_CTRL1_INTLACE;
	HVS_WRITE(SCALER6_DISPX_CTRL1(chan),
		  disp_ctrl1 | (interlace ? SCALER6_DISPX_CTRL1_INTLACE : 0));

	HVS_WRITE(SCALER6_DISPX_CTRL0(chan),
		  SCALER6_DISPX_CTRL0_ENB |
		  VC4_SET_FIELD(mode->hdisplay - 1,
				SCALER6_DISPX_CTRL0_FWIDTH) |
		  (oneshot ? SCALER6_DISPX_CTRL0_ONESHOT : 0) |
		  VC4_SET_FIELD(mode->vdisplay - 1,
				SCALER6_DISPX_CTRL0_LINES));

	drm_dev_exit(idx);

	return 0;
}

static void __vc4_hvs_stop_channel(struct vc4_hvs *hvs, unsigned int chan)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	int idx;

	WARN_ON_ONCE(vc4->gen > VC4_GEN_5);

	if (!drm_dev_enter(drm, &idx))
		return;

	if (!(HVS_READ(SCALER_DISPCTRLX(chan)) & SCALER_DISPCTRLX_ENABLE))
		goto out;

	HVS_WRITE(SCALER_DISPCTRLX(chan), SCALER_DISPCTRLX_RESET);
	HVS_WRITE(SCALER_DISPCTRLX(chan), 0);

	/* Once we leave, the scaler should be disabled and its fifo empty. */
	WARN_ON_ONCE(HVS_READ(SCALER_DISPCTRLX(chan)) & SCALER_DISPCTRLX_RESET);

	WARN_ON_ONCE(VC4_GET_FIELD(HVS_READ(SCALER_DISPSTATX(chan)),
				   SCALER_DISPSTATX_MODE) !=
		     SCALER_DISPSTATX_MODE_DISABLED);

	WARN_ON_ONCE((HVS_READ(SCALER_DISPSTATX(chan)) &
		      (SCALER_DISPSTATX_FULL | SCALER_DISPSTATX_EMPTY)) !=
		     SCALER_DISPSTATX_EMPTY);

out:
	drm_dev_exit(idx);
}

static void __vc6_hvs_stop_channel(struct vc4_hvs *hvs, unsigned int chan)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	int idx;

	WARN_ON_ONCE(vc4->gen < VC4_GEN_6_C);

	if (!drm_dev_enter(drm, &idx))
		return;

	if (!(HVS_READ(SCALER6_DISPX_CTRL0(chan)) & SCALER6_DISPX_CTRL0_ENB))
		goto out;

	HVS_WRITE(SCALER6_DISPX_CTRL0(chan),
		  HVS_READ(SCALER6_DISPX_CTRL0(chan)) | SCALER6_DISPX_CTRL0_RESET);

	HVS_WRITE(SCALER6_DISPX_CTRL0(chan),
		  HVS_READ(SCALER6_DISPX_CTRL0(chan)) & ~SCALER6_DISPX_CTRL0_ENB);

	WARN_ON_ONCE(VC4_GET_FIELD(HVS_READ(SCALER6_DISPX_STATUS(chan)),
				   SCALER6_DISPX_STATUS_MODE) !=
		     SCALER6_DISPX_STATUS_MODE_DISABLED);

out:
	drm_dev_exit(idx);
}

void vc4_hvs_stop_channel(struct vc4_hvs *hvs, unsigned int chan)
{
	struct vc4_dev *vc4 = hvs->vc4;

	if (vc4->gen >= VC4_GEN_6_C)
		__vc6_hvs_stop_channel(hvs, chan);
	else
		__vc4_hvs_stop_channel(hvs, chan);
}

int vc4_hvs_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc_state);
	struct vc4_hvs_dlist_allocation *alloc;
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_plane *plane;
	const struct drm_plane_state *plane_state;
	u32 dlist_count = 0;

	/* The pixelvalve can only feed one encoder (and encoders are
	 * 1:1 with connectors.)
	 */
	if (hweight32(crtc_state->connector_mask) > 1)
		return -EINVAL;

	drm_atomic_crtc_state_for_each_plane_state(plane, plane_state, crtc_state) {
		u32 plane_dlist_count = vc4_plane_dlist_size(plane_state);

		drm_dbg_driver(dev, "[CRTC:%d:%s] Found [PLANE:%d:%s] with DLIST size: %u\n",
			       crtc->base.id, crtc->name,
			       plane->base.id, plane->name,
			       plane_dlist_count);

		dlist_count += plane_dlist_count;
	}

	dlist_count++; /* Account for SCALER_CTL0_END. */

	drm_dbg_driver(dev, "[CRTC:%d:%s] Allocating DLIST block with size: %u\n",
		       crtc->base.id, crtc->name, dlist_count);
	alloc = vc4_hvs_alloc_dlist_entry(vc4->hvs, vc4_state->assigned_channel, dlist_count);
	if (IS_ERR(alloc))
		return PTR_ERR(alloc);

	vc4_state->mm = alloc;

	return 0;
}

static void vc4_hvs_install_dlist(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	WARN_ON(!vc4_state->mm);

	vc4_state->mm->dlist_programmed = true;

	if (vc4->gen >= VC4_GEN_6_C)
		HVS_WRITE(SCALER6_DISPX_LPTRS(vc4_state->assigned_channel),
			  VC4_SET_FIELD(vc4_state->mm->mm_node.start,
					SCALER6_DISPX_LPTRS_HEADE));
	else
		HVS_WRITE(SCALER_DISPLISTX(vc4_state->assigned_channel),
			  vc4_state->mm->mm_node.start);

	drm_dev_exit(idx);
}

static void vc4_hvs_update_dlist(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	unsigned long flags;

	if (crtc->state->event) {
		crtc->state->event->pipe = drm_crtc_index(crtc);

		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&dev->event_lock, flags);

		if (!vc4_crtc->feeds_txp || vc4_state->txp_armed) {
			vc4_crtc->event = crtc->state->event;
			crtc->state->event = NULL;
		}

		spin_unlock_irqrestore(&dev->event_lock, flags);
	}

	WARN_ON(!vc4_state->mm);

	spin_lock_irqsave(&vc4_crtc->irq_lock, flags);
	vc4_crtc->current_dlist = vc4_state->mm->mm_node.start;
	spin_unlock_irqrestore(&vc4_crtc->irq_lock, flags);
}

void vc4_hvs_atomic_begin(struct drm_crtc *crtc,
			  struct drm_atomic_state *state)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	unsigned long flags;

	spin_lock_irqsave(&vc4_crtc->irq_lock, flags);
	vc4_crtc->current_hvs_channel = vc4_state->assigned_channel;
	spin_unlock_irqrestore(&vc4_crtc->irq_lock, flags);
}

void vc4_hvs_atomic_enable(struct drm_crtc *crtc,
			   struct drm_atomic_state *state)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	bool oneshot = vc4_crtc->feeds_txp;

	vc4_hvs_install_dlist(crtc);
	vc4_hvs_update_dlist(crtc);

	if (vc4->gen >= VC4_GEN_6_C)
		vc6_hvs_init_channel(vc4->hvs, crtc, mode, oneshot);
	else
		vc4_hvs_init_channel(vc4->hvs, crtc, mode, oneshot);
}

void vc4_hvs_atomic_disable(struct drm_crtc *crtc,
			    struct drm_atomic_state *state)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_crtc_state *old_state = drm_atomic_get_old_crtc_state(state, crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(old_state);
	unsigned int chan = vc4_state->assigned_channel;

	vc4_hvs_stop_channel(vc4->hvs, chan);
}

void vc4_hvs_atomic_flush(struct drm_crtc *crtc,
			  struct drm_atomic_state *state)
{
	struct drm_crtc_state *old_state = drm_atomic_get_old_crtc_state(state,
									 crtc);
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc->state);
	unsigned int channel = vc4_state->assigned_channel;
	struct drm_plane *plane;
	struct vc4_plane_state *vc4_plane_state;
	bool debug_dump_regs = false;
	bool enable_bg_fill = true;
	u32 __iomem *dlist_start, *dlist_next;
	unsigned int zpos = 0;
	bool found = false;
	int idx;

	WARN_ON_ONCE(vc4->gen > VC4_GEN_6_D);

	if (!drm_dev_enter(dev, &idx)) {
		vc4_crtc_send_vblank(crtc);
		return;
	}

	if (vc4_state->assigned_channel == VC4_HVS_CHANNEL_DISABLED)
		goto exit;

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d HVS before:\n", drm_crtc_index(crtc));
		vc4_hvs_dump_state(hvs);
	}

	dlist_start = vc4->hvs->dlist + vc4_state->mm->mm_node.start;
	dlist_next = dlist_start;

	/* Copy all the active planes' dlist contents to the hardware dlist. */
	do {
		found = false;

		drm_atomic_crtc_for_each_plane(plane, crtc) {
			if (plane->state->normalized_zpos != zpos)
				continue;

			/* Is this the first active plane? */
			if (dlist_next == dlist_start) {
				/* We need to enable background fill when a plane
				 * could be alpha blending from the background, i.e.
				 * where no other plane is underneath. It suffices to
				 * consider the first active plane here since we set
				 * needs_bg_fill such that either the first plane
				 * already needs it or all planes on top blend from
				 * the first or a lower plane.
				 */
				vc4_plane_state = to_vc4_plane_state(plane->state);
				enable_bg_fill = vc4_plane_state->needs_bg_fill;
			}

			dlist_next += vc4_plane_write_dlist(plane, dlist_next);

			found = true;
		}

		zpos++;
	} while (found);

	writel(SCALER_CTL0_END, dlist_next);
	dlist_next++;

	WARN_ON(!vc4_state->mm);
	WARN_ON_ONCE(dlist_next - dlist_start != vc4_state->mm->mm_node.size);

	if (vc4->gen >= VC4_GEN_6_C) {
		/* This sets a black background color fill, as is the case
		 * with other DRM drivers.
		 */
		hvs->bg_fill[channel] = enable_bg_fill;
	} else {
		/* we can actually run with a lower core clock when background
		 * fill is enabled on VC4_GEN_5 so leave it enabled always.
		 */
		HVS_WRITE(SCALER_DISPBKGNDX(channel),
			  HVS_READ(SCALER_DISPBKGNDX(channel)) |
			  SCALER_DISPBKGND_FILL);
	}

	/* Only update DISPLIST if the CRTC was already running and is not
	 * being disabled.
	 * vc4_crtc_enable() takes care of updating the dlist just after
	 * re-enabling VBLANK interrupts and before enabling the engine.
	 * If the CRTC is being disabled, there's no point in updating this
	 * information.
	 */
	if (crtc->state->active && old_state->active) {
		vc4_hvs_install_dlist(crtc);
		vc4_hvs_update_dlist(crtc);
	}

	if (crtc->state->color_mgmt_changed) {
		u32 dispbkgndx = HVS_READ(SCALER_DISPBKGNDX(channel));

		WARN_ON_ONCE(vc4->gen > VC4_GEN_5);

		if (crtc->state->gamma_lut) {
			vc4_hvs_update_gamma_lut(hvs, vc4_crtc);
			dispbkgndx |= SCALER_DISPBKGND_GAMMA;
		} else {
			/* Unsetting DISPBKGND_GAMMA skips the gamma lut step
			 * in hardware, which is the same as a linear lut that
			 * DRM expects us to use in absence of a user lut.
			 */
			dispbkgndx &= ~SCALER_DISPBKGND_GAMMA;
		}
		HVS_WRITE(SCALER_DISPBKGNDX(channel), dispbkgndx);
	}

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d HVS after:\n", drm_crtc_index(crtc));
		vc4_hvs_dump_state(hvs);
	}

exit:
	drm_dev_exit(idx);
}

void vc4_hvs_mask_underrun(struct vc4_hvs *hvs, int channel)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	u32 dispctrl;
	int idx;

	WARN_ON(vc4->gen > VC4_GEN_5);

	if (!drm_dev_enter(drm, &idx))
		return;

	dispctrl = HVS_READ(SCALER_DISPCTRL);
	dispctrl &= ~((vc4->gen == VC4_GEN_5) ?
		      SCALER5_DISPCTRL_DSPEISLUR(channel) :
		      SCALER_DISPCTRL_DSPEISLUR(channel));

	HVS_WRITE(SCALER_DISPCTRL, dispctrl);

	drm_dev_exit(idx);
}

void vc4_hvs_unmask_underrun(struct vc4_hvs *hvs, int channel)
{
	struct vc4_dev *vc4 = hvs->vc4;
	struct drm_device *drm = &vc4->base;
	u32 dispctrl;
	int idx;

	WARN_ON(vc4->gen > VC4_GEN_5);

	if (!drm_dev_enter(drm, &idx))
		return;

	dispctrl = HVS_READ(SCALER_DISPCTRL);
	dispctrl |= ((vc4->gen == VC4_GEN_5) ?
		     SCALER5_DISPCTRL_DSPEISLUR(channel) :
		     SCALER_DISPCTRL_DSPEISLUR(channel));

	HVS_WRITE(SCALER_DISPSTAT,
		  SCALER_DISPSTAT_EUFLOW(channel));
	HVS_WRITE(SCALER_DISPCTRL, dispctrl);

	drm_dev_exit(idx);
}

static void vc4_hvs_report_underrun(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	atomic_inc(&vc4->underrun);
	DRM_DEV_ERROR(dev->dev, "HVS underrun\n");
}

static irqreturn_t vc4_hvs_irq_handler(int irq, void *data)
{
	struct drm_device *dev = data;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	irqreturn_t irqret = IRQ_NONE;
	int channel;
	u32 control;
	u32 status;
	u32 dspeislur;

	WARN_ON(vc4->gen > VC4_GEN_5);

	/*
	 * NOTE: We don't need to protect the register access using
	 * drm_dev_enter() there because the interrupt handler lifetime
	 * is tied to the device itself, and not to the DRM device.
	 *
	 * So when the device will be gone, one of the first thing we
	 * will be doing will be to unregister the interrupt handler,
	 * and then unregister the DRM device. drm_dev_enter() would
	 * thus always succeed if we are here.
	 */

	status = HVS_READ(SCALER_DISPSTAT);
	control = HVS_READ(SCALER_DISPCTRL);

	for (channel = 0; channel < SCALER_CHANNELS_COUNT; channel++) {
		dspeislur = (vc4->gen == VC4_GEN_5) ?
			SCALER5_DISPCTRL_DSPEISLUR(channel) :
			SCALER_DISPCTRL_DSPEISLUR(channel);

		/* Interrupt masking is not always honored, so check it here. */
		if (status & SCALER_DISPSTAT_EUFLOW(channel) &&
		    control & dspeislur) {
			vc4_hvs_mask_underrun(hvs, channel);
			vc4_hvs_report_underrun(dev);

			irqret = IRQ_HANDLED;
		}

		if (status & SCALER_DISPSTAT_EOF(channel)) {
			vc4_hvs_schedule_dlist_sweep(hvs, channel);
			irqret = IRQ_HANDLED;
		}
	}

	/* Clear every per-channel interrupt flag. */
	HVS_WRITE(SCALER_DISPSTAT, SCALER_DISPSTAT_IRQMASK(0) |
				   SCALER_DISPSTAT_IRQMASK(1) |
				   SCALER_DISPSTAT_IRQMASK(2));

	return irqret;
}

static irqreturn_t vc6_hvs_eof_irq_handler(int irq, void *data)
{
	struct drm_device *dev = data;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	unsigned int i;

	for (i = 0; i < HVS_NUM_CHANNELS; i++) {
		if (!hvs->eof_irq[i].enabled)
			continue;

		if (hvs->eof_irq[i].desc != irq)
			continue;

		if (hvs->bg_fill[i])
			HVS_WRITE(SCALER6_DISPX_CTRL1(i),
				  HVS_READ(SCALER6_DISPX_CTRL1(i)) |
				  SCALER6_DISPX_CTRL1_BGENB);
		else
			HVS_WRITE(SCALER6_DISPX_CTRL1(i),
				  HVS_READ(SCALER6_DISPX_CTRL1(i)) &
				  ~SCALER6_DISPX_CTRL1_BGENB);

		vc4_hvs_schedule_dlist_sweep(hvs, i);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

int vc4_hvs_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *drm = minor->dev;
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_hvs *hvs = vc4->hvs;

	if (!vc4->hvs)
		return -ENODEV;

	if (vc4->gen == VC4_GEN_4)
		debugfs_create_bool("hvs_load_tracker", S_IRUGO | S_IWUSR,
				    minor->debugfs_root,
				    &vc4->load_tracker_enabled);

	if (vc4->gen >= VC4_GEN_6_C) {
		drm_debugfs_add_file(drm, "hvs_dlists", vc6_hvs_debugfs_dlist, NULL);
		drm_debugfs_add_file(drm, "hvs_upm", vc6_hvs_debugfs_upm_allocs, NULL);
	} else {
		drm_debugfs_add_file(drm, "hvs_dlists", vc4_hvs_debugfs_dlist, NULL);
	}

	drm_debugfs_add_file(drm, "hvs_lbm", vc4_hvs_debugfs_lbm_allocs, NULL);

	drm_debugfs_add_file(drm, "hvs_underrun", vc4_hvs_debugfs_underrun, NULL);

	drm_debugfs_add_file(drm, "hvs_dlist_allocs", vc4_hvs_debugfs_dlist_allocs, NULL);

	vc4_debugfs_add_regset32(drm, "hvs_regs", &hvs->regset);

	return 0;
}

struct vc4_hvs *__vc4_hvs_alloc(struct vc4_dev *vc4,
				void __iomem *regs,
				struct platform_device *pdev)
{
	struct drm_device *drm = &vc4->base;
	struct vc4_hvs *hvs;
	unsigned int dlist_start;
	size_t dlist_size;
	size_t lbm_size;
	unsigned int i;

	hvs = drmm_kzalloc(drm, sizeof(*hvs), GFP_KERNEL);
	if (!hvs)
		return ERR_PTR(-ENOMEM);

	hvs->vc4 = vc4;
	hvs->regs = regs;
	hvs->pdev = pdev;

	spin_lock_init(&hvs->mm_lock);

	switch (vc4->gen) {
	case VC4_GEN_4:
	case VC4_GEN_5:
		/* Set up the HVS display list memory manager. We never
		 * overwrite the setup from the bootloader (just 128b
		 * out of our 16K), since we don't want to scramble the
		 * screen when transitioning from the firmware's boot
		 * setup to runtime.
		 */
		dlist_start = HVS_BOOTLOADER_DLIST_END;
		dlist_size = (SCALER_DLIST_SIZE >> 2) - HVS_BOOTLOADER_DLIST_END;
		break;

	case VC4_GEN_6_C:
	case VC4_GEN_6_D:
		dlist_start = HVS_BOOTLOADER_DLIST_END;

		/*
		 * If we are running a test, it means that we can't
		 * access a register. Use a plausible size then.
		 */
		if (!kunit_get_current_test())
			dlist_size = HVS_READ(SCALER6_CXM_SIZE);
		else
			dlist_size = 4096;

		for (i = 0; i < VC4_NUM_UPM_HANDLES; i++) {
			refcount_set(&hvs->upm_refcounts[i].refcount, 0);
			hvs->upm_refcounts[i].hvs = hvs;
		}

		break;

	default:
		drm_err(drm, "Unknown VC4 generation: %d", vc4->gen);
		return ERR_PTR(-ENODEV);
	}

	drm_mm_init(&hvs->dlist_mm, dlist_start, dlist_size);

	hvs->dlist_mem_size = dlist_size;

	INIT_LIST_HEAD(&hvs->stale_dlist_entries);
	INIT_WORK(&hvs->free_dlist_work, vc4_hvs_dlist_free_work);

	/* Set up the HVS display list memory manager.  We never
	 * overwrite the setup from the bootloader (just 128b out of
	 * our 16K), since we don't want to scramble the screen when
	 * transitioning from the firmware's boot setup to runtime.
	 */
	drm_mm_init(&hvs->dlist_mm,
		    HVS_BOOTLOADER_DLIST_END,
		    (SCALER_DLIST_SIZE >> 2) - HVS_BOOTLOADER_DLIST_END);

	/* Set up the HVS LBM memory manager.  We could have some more
	 * complicated data structure that allowed reuse of LBM areas
	 * between planes when they don't overlap on the screen, but
	 * for now we just allocate globally.
	 */

	switch (vc4->gen) {
	case VC4_GEN_4:
		/* 48k words of 2x12-bit pixels */
		lbm_size = 48 * SZ_1K;
		break;

	case VC4_GEN_5:
		/* 60k words of 4x12-bit pixels */
		lbm_size = 60 * SZ_1K;
		break;

	case VC4_GEN_6_C:
	case VC4_GEN_6_D:
		/*
		 * If we are running a test, it means that we can't
		 * access a register. Use a plausible size then.
		 */
		lbm_size = 1024;
		break;

	default:
		drm_err(drm, "Unknown VC4 generation: %d", vc4->gen);
		return ERR_PTR(-ENODEV);
	}

	drm_mm_init(&hvs->lbm_mm, 0, lbm_size);
	ida_init(&hvs->lbm_handles);

	if (vc4->gen >= VC4_GEN_6_C) {
		ida_init(&hvs->upm_handles);

		/*
		 * NOTE: On BCM2712, the size can also be read through
		 * the SCALER_UBM_SIZE register. We would need to do a
		 * register access though, which we can't do with kunit
		 * that also uses this function to create its mock
		 * device.
		 */
		drm_mm_init(&hvs->upm_mm, 0, 1024 * HVS_UBM_WORD_SIZE);
	}


	vc4->hvs = hvs;

	return hvs;
}

static int vc4_hvs_hw_init(struct vc4_hvs *hvs)
{
	struct vc4_dev *vc4 = hvs->vc4;
	u32 dispctrl, reg;

	dispctrl = HVS_READ(SCALER_DISPCTRL);
	dispctrl |= SCALER_DISPCTRL_ENABLE;
	HVS_WRITE(SCALER_DISPCTRL, dispctrl);

	reg = HVS_READ(SCALER_DISPECTRL);
	reg &= ~SCALER_DISPECTRL_DSP2_MUX_MASK;
	HVS_WRITE(SCALER_DISPECTRL,
		  reg | VC4_SET_FIELD(0, SCALER_DISPECTRL_DSP2_MUX));

	reg = HVS_READ(SCALER_DISPCTRL);
	reg &= ~SCALER_DISPCTRL_DSP3_MUX_MASK;
	HVS_WRITE(SCALER_DISPCTRL,
		  reg | VC4_SET_FIELD(3, SCALER_DISPCTRL_DSP3_MUX));

	reg = HVS_READ(SCALER_DISPEOLN);
	reg &= ~SCALER_DISPEOLN_DSP4_MUX_MASK;
	HVS_WRITE(SCALER_DISPEOLN,
		  reg | VC4_SET_FIELD(3, SCALER_DISPEOLN_DSP4_MUX));

	reg = HVS_READ(SCALER_DISPDITHER);
	reg &= ~SCALER_DISPDITHER_DSP5_MUX_MASK;
	HVS_WRITE(SCALER_DISPDITHER,
		  reg | VC4_SET_FIELD(3, SCALER_DISPDITHER_DSP5_MUX));

	dispctrl = HVS_READ(SCALER_DISPCTRL);
	dispctrl |= SCALER_DISPCTRL_DISPEIRQ(0) |
		    SCALER_DISPCTRL_DISPEIRQ(1) |
		    SCALER_DISPCTRL_DISPEIRQ(2);

	if (vc4->gen == VC4_GEN_4)
		dispctrl &= ~(SCALER_DISPCTRL_DMAEIRQ |
			      SCALER_DISPCTRL_SLVWREIRQ |
			      SCALER_DISPCTRL_SLVRDEIRQ |
			      SCALER_DISPCTRL_DSPEIEOF(0) |
			      SCALER_DISPCTRL_DSPEIEOF(1) |
			      SCALER_DISPCTRL_DSPEIEOF(2) |
			      SCALER_DISPCTRL_DSPEIEOLN(0) |
			      SCALER_DISPCTRL_DSPEIEOLN(1) |
			      SCALER_DISPCTRL_DSPEIEOLN(2) |
			      SCALER_DISPCTRL_DSPEISLUR(0) |
			      SCALER_DISPCTRL_DSPEISLUR(1) |
			      SCALER_DISPCTRL_DSPEISLUR(2) |
			      SCALER_DISPCTRL_SCLEIRQ);
	else
		dispctrl &= ~(SCALER_DISPCTRL_DMAEIRQ |
			      SCALER5_DISPCTRL_SLVEIRQ |
			      SCALER5_DISPCTRL_DSPEIEOF(0) |
			      SCALER5_DISPCTRL_DSPEIEOF(1) |
			      SCALER5_DISPCTRL_DSPEIEOF(2) |
			      SCALER5_DISPCTRL_DSPEIEOLN(0) |
			      SCALER5_DISPCTRL_DSPEIEOLN(1) |
			      SCALER5_DISPCTRL_DSPEIEOLN(2) |
			      SCALER5_DISPCTRL_DSPEISLUR(0) |
			      SCALER5_DISPCTRL_DSPEISLUR(1) |
			      SCALER5_DISPCTRL_DSPEISLUR(2) |
			      SCALER_DISPCTRL_SCLEIRQ);


	/* Set AXI panic mode.
	 * VC4 panics when < 2 lines in FIFO.
	 * VC5 panics when less than 1 line in the FIFO.
	 */
	dispctrl &= ~(SCALER_DISPCTRL_PANIC0_MASK |
		      SCALER_DISPCTRL_PANIC1_MASK |
		      SCALER_DISPCTRL_PANIC2_MASK);
	dispctrl |= VC4_SET_FIELD(2, SCALER_DISPCTRL_PANIC0);
	dispctrl |= VC4_SET_FIELD(2, SCALER_DISPCTRL_PANIC1);
	dispctrl |= VC4_SET_FIELD(2, SCALER_DISPCTRL_PANIC2);

	/* Set AXI panic mode.
	 * VC4 panics when < 2 lines in FIFO.
	 * VC5 panics when less than 1 line in the FIFO.
	 */
	dispctrl &= ~(SCALER_DISPCTRL_PANIC0_MASK |
		      SCALER_DISPCTRL_PANIC1_MASK |
		      SCALER_DISPCTRL_PANIC2_MASK);
	dispctrl |= VC4_SET_FIELD(2, SCALER_DISPCTRL_PANIC0);
	dispctrl |= VC4_SET_FIELD(2, SCALER_DISPCTRL_PANIC1);
	dispctrl |= VC4_SET_FIELD(2, SCALER_DISPCTRL_PANIC2);

	HVS_WRITE(SCALER_DISPCTRL, dispctrl);

	return 0;
}

#define CFC1_N_NL_CSC_CTRL(x)		(0xa000 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C00(x)	(0xa008 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C01(x)	(0xa00c + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C02(x)	(0xa010 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C03(x)	(0xa014 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C04(x)	(0xa018 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C10(x)	(0xa01c + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C11(x)	(0xa020 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C12(x)	(0xa024 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C13(x)	(0xa028 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C14(x)	(0xa02c + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C20(x)	(0xa030 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C21(x)	(0xa034 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C22(x)	(0xa038 + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C23(x)	(0xa03c + ((x) * 0x3000))
#define CFC1_N_MA_CSC_COEFF_C24(x)	(0xa040 + ((x) * 0x3000))

#define SCALER_PI_CMP_CSC_RED0(x)		(0x200 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_RED1(x)		(0x204 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_RED_CLAMP(x)		(0x208 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_CFG(x)		(0x20c + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_GREEN0(x)		(0x210 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_GREEN1(x)		(0x214 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_GREEN_CLAMP(x)	(0x218 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_BLUE0(x)		(0x220 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_BLUE1(x)		(0x224 + ((x) * 0x40))
#define SCALER_PI_CMP_CSC_BLUE_CLAMP(x)		(0x228 + ((x) * 0x40))

/* 4 S2.22 multiplication factors, and 1 S9.15 addititive element for each of 3
 * output components
 */
struct vc6_csc_coeff_entry {
	u32 csc[3][5];
};

static const struct vc6_csc_coeff_entry csc_coeffs[2][3] = {
	[DRM_COLOR_YCBCR_LIMITED_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			.csc = {
				{ 0x004A8542, 0x0, 0x0066254A, 0x0, 0xFF908A0D },
				{ 0x004A8542, 0xFFE6ED5D, 0xFFCBF856, 0x0, 0x0043C9A3 },
				{ 0x004A8542, 0x00811A54, 0x0, 0x0, 0xFF759502 }
			}
		},
		[DRM_COLOR_YCBCR_BT709] = {
			.csc = {
				{ 0x004A8542, 0x0, 0x0072BC44, 0x0, 0xFF83F312 },
				{ 0x004A8542, 0xFFF25A22, 0xFFDDE4D0, 0x0, 0x00267064 },
				{ 0x004A8542, 0x00873197, 0x0, 0x0, 0xFF6F7DC0 }
			}
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			.csc = {
				{ 0x004A8542, 0x0, 0x006B4A17, 0x0, 0xFF8B653F },
				{ 0x004A8542, 0xFFF402D9, 0xFFDDE4D0, 0x0, 0x0024C7AE },
				{ 0x004A8542, 0x008912CC, 0x0, 0x0, 0xFF6D9C8B }
			}
		}
	},
	[DRM_COLOR_YCBCR_FULL_RANGE] = {
		[DRM_COLOR_YCBCR_BT601] = {
			.csc = {
				{ 0x00400000, 0x0, 0x0059BA5E, 0x0, 0xFFA645A1 },
				{ 0x00400000, 0xFFE9F9AC, 0xFFD24B97, 0x0, 0x0043BABB },
				{ 0x00400000, 0x00716872, 0x0, 0x0, 0xFF8E978D }
			}
		},
		[DRM_COLOR_YCBCR_BT709] = {
			.csc = {
				{ 0x00400000, 0x0, 0x0064C985, 0x0, 0xFF9B367A },
				{ 0x00400000, 0xFFF402E1, 0xFFE20A40, 0x0, 0x0029F2DE },
				{ 0x00400000, 0x0076C226, 0x0, 0x0, 0xFF893DD9 }
			}
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			.csc = {
				{ 0x00400000, 0x0, 0x005E3F14, 0x0, 0xFFA1C0EB },
				{ 0x00400000, 0xFFF577F6, 0xFFDB580F, 0x0, 0x002F2FFA },
				{ 0x00400000, 0x007868DB, 0x0, 0x0, 0xFF879724 }
			}
		}
	}
};

static int vc6_hvs_hw_init(struct vc4_hvs *hvs)
{
	const struct vc6_csc_coeff_entry *coeffs;
	unsigned int i;

	HVS_WRITE(SCALER6_CONTROL,
		  SCALER6_CONTROL_HVS_EN |
		  VC4_SET_FIELD(8, SCALER6_CONTROL_PF_LINES) |
		  VC4_SET_FIELD(15, SCALER6_CONTROL_MAX_REQS));

	/* Set HVS arbiter priority to max */
	HVS_WRITE(SCALER6(PRI_MAP0), 0xffffffff);
	HVS_WRITE(SCALER6(PRI_MAP1), 0xffffffff);

	if (hvs->vc4->gen == VC4_GEN_6_C) {
		for (i = 0; i < 6; i++) {
			coeffs = &csc_coeffs[i / 3][i % 3];

			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C00(i), coeffs->csc[0][0]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C01(i), coeffs->csc[0][1]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C02(i), coeffs->csc[0][2]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C03(i), coeffs->csc[0][3]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C04(i), coeffs->csc[0][4]);

			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C10(i), coeffs->csc[1][0]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C11(i), coeffs->csc[1][1]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C12(i), coeffs->csc[1][2]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C13(i), coeffs->csc[1][3]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C14(i), coeffs->csc[1][4]);

			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C20(i), coeffs->csc[2][0]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C21(i), coeffs->csc[2][1]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C22(i), coeffs->csc[2][2]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C23(i), coeffs->csc[2][3]);
			HVS_WRITE(CFC1_N_MA_CSC_COEFF_C24(i), coeffs->csc[2][4]);

			HVS_WRITE(CFC1_N_NL_CSC_CTRL(i), BIT(15));
		}
	} else {
		for (i = 0; i < 8; i++) {
			HVS_WRITE(SCALER_PI_CMP_CSC_RED0(i), 0x1f002566);
			HVS_WRITE(SCALER_PI_CMP_CSC_RED1(i), 0x3994);
			HVS_WRITE(SCALER_PI_CMP_CSC_RED_CLAMP(i), 0xfff00000);
			HVS_WRITE(SCALER_PI_CMP_CSC_CFG(i), 0x1);
			HVS_WRITE(SCALER_PI_CMP_CSC_GREEN0(i), 0x18002566);
			HVS_WRITE(SCALER_PI_CMP_CSC_GREEN1(i), 0xf927eee2);
			HVS_WRITE(SCALER_PI_CMP_CSC_GREEN_CLAMP(i), 0xfff00000);
			HVS_WRITE(SCALER_PI_CMP_CSC_BLUE0(i), 0x18002566);
			HVS_WRITE(SCALER_PI_CMP_CSC_BLUE1(i), 0x43d80000);
			HVS_WRITE(SCALER_PI_CMP_CSC_BLUE_CLAMP(i), 0xfff00000);
		}
	}

	return 0;
}

static int vc4_hvs_cob_init(struct vc4_hvs *hvs)
{
	struct vc4_dev *vc4 = hvs->vc4;
	u32 reg, top, base;

	/*
	 * Recompute Composite Output Buffer (COB) allocations for the
	 * displays
	 */
	switch (vc4->gen) {
	case VC4_GEN_4:
		/* The COB is 20736 pixels, or just over 10 lines at 2048 wide.
		 * The bottom 2048 pixels are full 32bpp RGBA (intended for the
		 * TXP composing RGBA to memory), whilst the remainder are only
		 * 24bpp RGB.
		 *
		 * Assign 3 lines to channels 1 & 2, and just over 4 lines to
		 * channel 0.
		 */
		#define VC4_COB_SIZE		20736
		#define VC4_COB_LINE_WIDTH	2048
		#define VC4_COB_NUM_LINES	3
		reg = 0;
		top = VC4_COB_LINE_WIDTH * VC4_COB_NUM_LINES;
		reg |= (top - 1) << 16;
		HVS_WRITE(SCALER_DISPBASE2, reg);
		reg = top;
		top += VC4_COB_LINE_WIDTH * VC4_COB_NUM_LINES;
		reg |= (top - 1) << 16;
		HVS_WRITE(SCALER_DISPBASE1, reg);
		reg = top;
		top = VC4_COB_SIZE;
		reg |= (top - 1) << 16;
		HVS_WRITE(SCALER_DISPBASE0, reg);
		break;

	case VC4_GEN_5:
		/* The COB is 44416 pixels, or 10.8 lines at 4096 wide.
		 * The bottom 4096 pixels are full RGBA (intended for the TXP
		 * composing RGBA to memory), whilst the remainder are only
		 * RGB. Addressing is always pixel wide.
		 *
		 * Assign 3 lines of 4096 to channels 1 & 2, and just over 4
		 * lines. to channel 0.
		 */
		#define VC5_COB_SIZE		44416
		#define VC5_COB_LINE_WIDTH	4096
		#define VC5_COB_NUM_LINES	3
		reg = 0;
		top = VC5_COB_LINE_WIDTH * VC5_COB_NUM_LINES;
		reg |= top << 16;
		HVS_WRITE(SCALER_DISPBASE2, reg);
		top += 16;
		reg = top;
		top += VC5_COB_LINE_WIDTH * VC5_COB_NUM_LINES;
		reg |= top << 16;
		HVS_WRITE(SCALER_DISPBASE1, reg);
		top += 16;
		reg = top;
		top = VC5_COB_SIZE;
		reg |= top << 16;
		HVS_WRITE(SCALER_DISPBASE0, reg);
		break;

	case VC4_GEN_6_C:
	case VC4_GEN_6_D:
		#define VC6_COB_LINE_WIDTH	3840
		#define VC6_COB_NUM_LINES	4
		base = 0;
		top = 3840;

		HVS_WRITE(SCALER6_DISPX_COB(2),
			  VC4_SET_FIELD(top, SCALER6_DISPX_COB_TOP) |
			  VC4_SET_FIELD(base, SCALER6_DISPX_COB_BASE));

		base = top + 16;
		top += VC6_COB_LINE_WIDTH * VC6_COB_NUM_LINES;

		HVS_WRITE(SCALER6_DISPX_COB(1),
			  VC4_SET_FIELD(top, SCALER6_DISPX_COB_TOP) |
			  VC4_SET_FIELD(base, SCALER6_DISPX_COB_BASE));

		base = top + 16;
		top += VC6_COB_LINE_WIDTH * VC6_COB_NUM_LINES;

		HVS_WRITE(SCALER6_DISPX_COB(0),
			  VC4_SET_FIELD(top, SCALER6_DISPX_COB_TOP) |
			  VC4_SET_FIELD(base, SCALER6_DISPX_COB_BASE));
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int vc4_hvs_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_hvs *hvs = NULL;
	void __iomem *regs;
	int ret;

	regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	hvs = __vc4_hvs_alloc(vc4, regs, pdev);
	if (IS_ERR(hvs))
		return PTR_ERR(hvs);

	hvs->regset.base = hvs->regs;

	if (vc4->gen == VC4_GEN_6_C) {
		hvs->regset.regs = vc6_hvs_regs;
		hvs->regset.nregs = ARRAY_SIZE(vc6_hvs_regs);

		if (VC4_GET_FIELD(HVS_READ(SCALER6_VERSION), SCALER6_VERSION) ==
						SCALER6_VERSION_D0) {
			vc4->gen = VC4_GEN_6_D;
			hvs->regset.regs = vc6_d_hvs_regs;
			hvs->regset.nregs = ARRAY_SIZE(vc6_d_hvs_regs);
		}
	} else {
		hvs->regset.regs = vc4_hvs_regs;
		hvs->regset.nregs = ARRAY_SIZE(vc4_hvs_regs);
	}

	if (vc4->gen >= VC4_GEN_5) {
		struct rpi_firmware *firmware;
		struct device_node *node;
		unsigned int max_rate;

		node = rpi_firmware_find_node();
		if (!node)
			return -EINVAL;

		firmware = rpi_firmware_get(node);
		of_node_put(node);
		if (!firmware)
			return -EPROBE_DEFER;

		hvs->core_clk = devm_clk_get(&pdev->dev,
					     (vc4->gen >= VC4_GEN_6_C) ? "core" : NULL);
		if (IS_ERR(hvs->core_clk)) {
			dev_err(&pdev->dev, "Couldn't get core clock\n");
			return PTR_ERR(hvs->core_clk);
		}

		hvs->disp_clk = devm_clk_get(&pdev->dev,
					     (vc4->gen >= VC4_GEN_6_C) ? "disp" : NULL);
		if (IS_ERR(hvs->disp_clk)) {
			dev_err(&pdev->dev, "Couldn't get disp clock\n");
			return PTR_ERR(hvs->disp_clk);
		}

		max_rate = rpi_firmware_clk_get_max_rate(firmware,
							 RPI_FIRMWARE_CORE_CLK_ID);
		rpi_firmware_put(firmware);
		if (max_rate >= 550000000)
			hvs->vc5_hdmi_enable_hdmi_20 = true;

		if (max_rate >= 600000000)
			hvs->vc5_hdmi_enable_4096by2160 = true;

		hvs->max_core_rate = max_rate;

		ret = clk_prepare_enable(hvs->core_clk);
		if (ret) {
			dev_err(&pdev->dev, "Couldn't enable the core clock\n");
			return ret;
		}

		ret = clk_prepare_enable(hvs->disp_clk);
		if (ret) {
			dev_err(&pdev->dev, "Couldn't enable the disp clock\n");
			return ret;
		}
	}

	if (vc4->gen >= VC4_GEN_5)
		hvs->dlist = hvs->regs + SCALER5_DLIST_START;
	else
		hvs->dlist = hvs->regs + SCALER_DLIST_START;

	if (vc4->gen >= VC4_GEN_6_C)
		ret = vc6_hvs_hw_init(hvs);
	else
		ret = vc4_hvs_hw_init(hvs);
	if (ret)
		return ret;

	/* Upload filter kernels.  We only have the two for now, so we
	 * keep them around for the lifetime of the driver.
	 */
	ret = vc4_hvs_upload_linear_kernel(hvs,
					   &hvs->mitchell_netravali_filter,
					   mitchell_netravali_1_3_1_3_kernel);
	if (ret)
		return ret;
	ret = vc4_hvs_upload_linear_kernel(hvs,
					   &hvs->nearest_neighbour_filter,
					   nearest_neighbour_kernel);
	if (ret)
		return ret;

	ret = vc4_hvs_cob_init(hvs);
	if (ret)
		return ret;

	if (vc4->gen < VC4_GEN_6_C) {
		ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
				       vc4_hvs_irq_handler, 0, "vc4 hvs", drm);
		if (ret)
			return ret;
	} else {
		unsigned int i;

		for (i = 0; i < HVS_NUM_CHANNELS; i++) {
			char irq_name[16];
			int irq;

			snprintf(irq_name, sizeof(irq_name), "ch%u-eof", i);

			irq = platform_get_irq_byname(pdev, irq_name);
			if (irq < 0) {
				dev_err(&pdev->dev,
					"Couldn't get %s interrupt: %d\n",
					irq_name, irq);

				return irq;
			}

			ret = devm_request_irq(&pdev->dev,
					       irq,
					       vc6_hvs_eof_irq_handler,
					       IRQF_NO_AUTOEN,
					       dev_name(&pdev->dev),
					       drm);

			hvs->eof_irq[i].desc = irq;
		}
	}

	return 0;
}

static void vc4_hvs_unbind(struct device *dev, struct device *master,
			   void *data)
{
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_mm_node *node, *next;

	if (drm_mm_node_allocated(&vc4->hvs->mitchell_netravali_filter))
		drm_mm_remove_node(&vc4->hvs->mitchell_netravali_filter);
	if (drm_mm_node_allocated(&vc4->hvs->nearest_neighbour_filter))
		drm_mm_remove_node(&vc4->hvs->nearest_neighbour_filter);

	drm_mm_for_each_node_safe(node, next, &vc4->hvs->dlist_mm)
		drm_mm_remove_node(node);

	drm_mm_takedown(&vc4->hvs->dlist_mm);

	drm_mm_for_each_node_safe(node, next, &vc4->hvs->lbm_mm)
		drm_mm_remove_node(node);
	drm_mm_takedown(&vc4->hvs->lbm_mm);

	/* we no longer require a minimum clock rate */
	clk_set_min_rate(hvs->disp_clk, 0);
	clk_disable_unprepare(hvs->disp_clk);
	clk_set_min_rate(hvs->core_clk, 0);
	clk_disable_unprepare(hvs->core_clk);

	vc4->hvs = NULL;
}

static const struct component_ops vc4_hvs_ops = {
	.bind   = vc4_hvs_bind,
	.unbind = vc4_hvs_unbind,
};

static int vc4_hvs_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_hvs_ops);
}

static void vc4_hvs_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_hvs_ops);
}

static const struct of_device_id vc4_hvs_dt_match[] = {
	{ .compatible = "brcm,bcm2711-hvs" },
	{ .compatible = "brcm,bcm2712-hvs" },
	{ .compatible = "brcm,bcm2835-hvs" },
	{}
};

struct platform_driver vc4_hvs_driver = {
	.probe = vc4_hvs_dev_probe,
	.remove_new = vc4_hvs_dev_remove,
	.driver = {
		.name = "vc4_hvs",
		.of_match_table = vc4_hvs_dt_match,
	},
};
