/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <linux/sysrq.h>
#include <linux/slab.h>
#include "i915_drm.h"
#include "drm_crtc.h"
#include "i915_drv.h"
#include "intel_drv.h"
#include "i915_trace.h"
#include "intel_drv.h"

/*
 * DPST (Display Power Savings Technology) is a power savings features
 * which reduces the backlight while enhancing the image such that the
 * user does not perceive any difference in the image quality. The backlight
 * reduction can provide power savings
 *
 * The DPST IOCTL implemented in this file can be used by the DPST a user-mode
 * module. The IOCTL provides methods to initialize the DPST hardware,
 * manage DPST interrupts, and to apply the new backlight and image enhancement
 * values.
 *
 * The user mode module will initialize the DPST hardware when it starts up.
 * The kernel will notify user mode module of any DPST histogram interrupts.
 * When the user mode module receives a notification of these interrupts, it
 * will query the kernel for all the DPST histogram data. Using this data,
 * the user mode module will calculate new backlight and image enhancement
 * values and provides those values to the kernel to program into the DPST
 * hardware.
 */

static struct intel_connector
*get_intel_connector_on_edp(struct drm_device *dev)
{
	struct intel_connector *i_connector = NULL;
	struct drm_connector *d_connector;

	list_for_each_entry(d_connector, &dev->mode_config.connector_list, head)
	{
		i_connector = to_intel_connector(d_connector);
		if (i_connector->encoder \
				&& i_connector->encoder->type == INTEL_OUTPUT_EDP)
			return i_connector;
	}
	return NULL;
}

static int
i915_dpst_clear_hist_interrupt(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	I915_WRITE(dev_priv->dpst.reg.blm_hist_guard,
			I915_READ(dev_priv->dpst.reg.blm_hist_guard) | HISTOGRAM_EVENT_STATUS);
	return 0;
}

static int
i915_dpst_enable_hist_interrupt(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	u32 blm_hist_ctl;

	dev_priv->dpst.enabled = true;
	dev_priv->dpst.blc_adjustment = DPST_MAX_FACTOR ;

	/* Enable histogram logic to collect data */
	blm_hist_ctl = I915_READ(dev_priv->dpst.reg.blm_hist_ctl);
	blm_hist_ctl |= IE_HISTOGRAM_ENABLE | HSV_INTENSITY_MODE;
	I915_WRITE(dev_priv->dpst.reg.blm_hist_ctl, blm_hist_ctl);

	/* Wait for VBLANK since the histogram enabling logic takes affect
	 * at the next vblank */
	intel_wait_for_vblank(dev, PIPE_A);

	/* Clear pending interrupt bit. Clearing the pending interrupt bit
	 * must be not be done at the same time as enabling the
	 * interrupt. */
	I915_WRITE(dev_priv->dpst.reg.blm_hist_guard,
			I915_READ(dev_priv->dpst.reg.blm_hist_guard) | HISTOGRAM_EVENT_STATUS);

	/* Enable histogram interrupts */
	I915_WRITE(dev_priv->dpst.reg.blm_hist_guard,
			I915_READ(dev_priv->dpst.reg.blm_hist_guard) | HISTOGRAM_INTERRUPT_ENABLE);

	/* DPST interrupt in DE_IER is enabled in irq_postinstall */

	return 0;
}

static int
i915_dpst_disable_hist_interrupt(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_connector *i_connector = get_intel_connector_on_edp(dev);
	struct intel_panel *panel;

	u32 blm_hist_guard, blm_hist_ctl;
	unsigned long spin_lock_flags;

	if (NULL == i_connector)
		return -EINVAL;
	panel = &i_connector->panel;

	dev_priv->dpst.enabled = false;
	dev_priv->dpst.blc_adjustment = DPST_MAX_FACTOR;

	/* Disable histogram interrupts. It is OK to clear pending interrupts
	 * and disable interrupts at the same time. */
	blm_hist_guard = I915_READ(dev_priv->dpst.reg.blm_hist_guard);
	blm_hist_guard |= HISTOGRAM_EVENT_STATUS; /* clear pending interrupts */
	blm_hist_guard &= ~HISTOGRAM_INTERRUPT_ENABLE;
	I915_WRITE(dev_priv->dpst.reg.blm_hist_guard, blm_hist_guard);

	/* Disable histogram logic */
	blm_hist_ctl = I915_READ(dev_priv->dpst.reg.blm_hist_ctl);
	blm_hist_ctl &= ~IE_HISTOGRAM_ENABLE;
	blm_hist_ctl &= ~IE_MOD_TABLE_ENABLE;
	I915_WRITE(dev_priv->dpst.reg.blm_hist_ctl, blm_hist_ctl);

	/* DPST interrupt in DE_IER register is disabled in irq_uninstall */

	/* Setting blc level to what it would be without dpst adjustment */

	spin_lock_irqsave(&dev_priv->backlight_lock, spin_lock_flags);
	intel_panel_actually_set_backlight(i_connector, panel->backlight.level);
	spin_unlock_irqrestore(&dev_priv->backlight_lock, spin_lock_flags);

	return 0;
}

static int
i915_dpst_set_user_enable(struct drm_device *dev, bool enable)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	dev_priv->dpst.user_enable = enable;

	if (enable) {
		if (!dev_priv->dpst.kernel_disable && !dev_priv->dpst.enabled)
			return i915_dpst_enable_hist_interrupt(dev);
	} else {
		/* User disabling invalidates any saved settings */
		dev_priv->dpst.saved.is_valid = false;

		if (dev_priv->dpst.enabled)
			return i915_dpst_disable_hist_interrupt(dev);
	}

	return 0;
}

static int
i915_dpst_apply_luma(struct drm_device *dev,
		struct dpst_initialize_context *ioctl_data)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_connector *i_connector = get_intel_connector_on_edp(dev);
	struct intel_panel *panel;

	u32 diet_factor, i;
	u32 blm_hist_ctl;
	unsigned long spin_lock_flags;

	if (NULL == i_connector)
		return -EINVAL;
	panel = &i_connector->panel;

	/* This is an invalid call if we are disabled by the user */
	if (!dev_priv->dpst.user_enable)
		return -EINVAL;

	/* This is not an invalid call if we are disabled by the kernel,
	 * because kernel disabling is transparent to the user and can easily
	 * occur before user has completed in-progress adjustments. If in fact
	 * we are disabled by the kernel, we must store the incoming values for
	 * later restore. Image enhancement values are stored on the hardware,
	 * because they will be safely ignored if the table is not enabled. */

	/* Setup register to access image enhancement value from
	 * index 0.*/
	blm_hist_ctl = I915_READ(dev_priv->dpst.reg.blm_hist_ctl);
	blm_hist_ctl |= BIN_REG_FUNCTION_SELECT_IE;
	blm_hist_ctl &= ~BIN_REGISTER_INDEX_MASK;
	I915_WRITE(dev_priv->dpst.reg.blm_hist_ctl, blm_hist_ctl);

	/* Program the image enhancement data passed from user mode. */
	for (i = 0; i < DPST_DIET_ENTRY_COUNT; i++) {
		diet_factor = ioctl_data->ie_container.
			dpst_ie_st.factor_present[i] * 0x200 / 10000;
		I915_WRITE(dev_priv->dpst.reg.blm_hist_bin, diet_factor);
	}

	if (dev_priv->dpst.kernel_disable) {
		dev_priv->dpst.saved.is_valid = true;
		dev_priv->dpst.saved.blc_adjustment =
			ioctl_data->ie_container.dpst_blc_factor;
		return 0;
	}

	/* Backlight settings */
	dev_priv->dpst.blc_adjustment =
	ioctl_data->ie_container.dpst_blc_factor;

	spin_lock_irqsave(&dev_priv->backlight_lock, spin_lock_flags);
	i915_dpst_set_brightness(dev, panel->backlight.level);
	spin_unlock_irqrestore(&dev_priv->backlight_lock, spin_lock_flags);

	/* Enable Image Enhancement Table */
	blm_hist_ctl = I915_READ(dev_priv->dpst.reg.blm_hist_ctl);
	blm_hist_ctl |= IE_MOD_TABLE_ENABLE | ENHANCEMENT_MODE_MULT;
	I915_WRITE(dev_priv->dpst.reg.blm_hist_ctl, blm_hist_ctl);

	return 0;
}

static void
i915_dpst_save_luma(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	/* Only save if user mode has indeed applied valid settings which
	 * we determine by checking that the IE mod table was enabled */
	if (!(I915_READ(dev_priv->dpst.reg.blm_hist_ctl) & IE_MOD_TABLE_ENABLE))
		return;

	/* IE mod table entries are saved in the hardware even if the table
	 * is disabled, so we only need to save the backlight adjustment */
	dev_priv->dpst.saved.is_valid = true;
	dev_priv->dpst.saved.blc_adjustment = dev_priv->dpst.blc_adjustment;
}

static void
i915_dpst_restore_luma(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_panel *panel;
	struct intel_connector *i_connector;
	u32 blm_hist_ctl;
	unsigned long spin_lock_flags;

	i_connector = get_intel_connector_on_edp(dev);
	if (NULL == i_connector)
		return;
	panel = &i_connector->panel;

	/* Only restore if valid settings were previously saved */
	if (!dev_priv->dpst.saved.is_valid)
		return;

	dev_priv->dpst.blc_adjustment = dev_priv->dpst.saved.blc_adjustment;

	spin_lock_irqsave(&dev_priv->backlight_lock, spin_lock_flags);
	i915_dpst_set_brightness(dev, panel->backlight.level);
	spin_unlock_irqrestore(&dev_priv->backlight_lock, spin_lock_flags);

	/* IE mod table entries are saved in the hardware even if the table
	 * is disabled, so we only need to re-enable the table */
	blm_hist_ctl = I915_READ(dev_priv->dpst.reg.blm_hist_ctl);
	blm_hist_ctl |= IE_MOD_TABLE_ENABLE | ENHANCEMENT_MODE_MULT;
	I915_WRITE(dev_priv->dpst.reg.blm_hist_ctl, blm_hist_ctl);
}

static int
i915_dpst_get_bin_data(struct drm_device *dev,
		struct dpst_initialize_context *ioctl_data)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	u32 blm_hist_ctl, blm_hist_bin;
	int index;

	/* We may be disabled by request from kernel or user. Kernel mode
	 * disablement is without user mode knowledge. Kernel mode disablement
	 * can occur between the signal to user and user's follow-up call to
	 * retrieve the data, so return the data as usual. User mode
	 * disablement makes this an invalid call, so return error. */
	if (!dev_priv->dpst.enabled && !dev_priv->dpst.user_enable)
		return -EINVAL;

	/* Setup register to access bin data from index 0 */
	blm_hist_ctl = I915_READ(dev_priv->dpst.reg.blm_hist_ctl);
	blm_hist_ctl = blm_hist_ctl & ~(BIN_REGISTER_INDEX_MASK |
						BIN_REG_FUNCTION_SELECT_IE);
	I915_WRITE(dev_priv->dpst.reg.blm_hist_ctl, blm_hist_ctl);

	/* Read all bin data */
	for (index = 0; index < HIST_BIN_COUNT; index++) {
		blm_hist_bin = I915_READ(dev_priv->dpst.reg.blm_hist_bin);

		if (!(blm_hist_bin & BUSY_BIT)) {
			ioctl_data->hist_status.histogram_bins.
				status[index] = blm_hist_bin & BIN_COUNT_MASK;
		} else {
			/* Engine is busy. Reset index to 0 to grab
			 * fresh histogram data */
			index = -1;
			blm_hist_ctl = I915_READ(dev_priv->dpst.reg.blm_hist_ctl);
			blm_hist_ctl = blm_hist_ctl & ~BIN_REGISTER_INDEX_MASK;
			I915_WRITE(dev_priv->dpst.reg.blm_hist_ctl, blm_hist_ctl);
		}
	}

	return 0;
}

static int i915_dpst_update_registers(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;

	if (IS_HASWELL(dev)) {
		dev_priv->dpst.reg.blm_hist_ctl = BLM_HIST_CTL;
		dev_priv->dpst.reg.blm_hist_guard = BLM_HIST_GUARD;
		dev_priv->dpst.reg.blm_hist_bin = BLM_HIST_BIN;
	} else if (IS_VALLEYVIEW(dev)) {
		dev_priv->dpst.reg.blm_hist_ctl = VLV_BLC_HIST_CTL(PIPE_A);
		dev_priv->dpst.reg.blm_hist_guard = VLV_BLC_HIST_GUARD(PIPE_A);
		dev_priv->dpst.reg.blm_hist_bin = VLV_BLC_HIST_BIN(PIPE_A);
	} else {
		DRM_ERROR("DPST not supported on this platform\n");
		return -EINVAL;
	}
	return 0;
};

static int
i915_dpst_init(struct drm_device *dev,
		struct dpst_initialize_context *ioctl_data)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct drm_crtc *crtc;
	struct drm_display_mode *mode = NULL;
	u32 blm_hist_guard, gb_val;

	/* Get information about current display mode */
	crtc = intel_get_crtc_for_pipe(dev, PIPE_A);
	if (crtc) {
		mode = intel_crtc_mode_get(dev, crtc);
		if (mode) {
			gb_val = (DEFAULT_GUARDBAND_VAL *
					mode->hdisplay * mode->vdisplay)/1000;

			ioctl_data->init_data.threshold_gb = gb_val;
			ioctl_data->init_data.image_res =
					mode->hdisplay*mode->vdisplay;
		}
	}

	if (0 != i915_dpst_update_registers(dev))
		return -EINVAL;

	/* Store info needed to talk to user mode */
	dev_priv->dpst.task = current;
	dev_priv->dpst.signal = ioctl_data->init_data.sig_num;

	/* Setup guardband delays and threshold */
	blm_hist_guard = I915_READ(dev_priv->dpst.reg.blm_hist_guard);
	blm_hist_guard |= (ioctl_data->init_data.gb_delay << 22)
			| ioctl_data->init_data.threshold_gb;
	I915_WRITE(dev_priv->dpst.reg.blm_hist_guard, blm_hist_guard);

	/* Init is complete so request enablement */
	return i915_dpst_set_user_enable(dev, true);
}


u32
i915_dpst_get_brightness(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct intel_panel *panel;
	struct intel_connector *i_connector = get_intel_connector_on_edp(dev);
	if (NULL == i_connector)
		return -EINVAL;
	panel = &i_connector->panel;

	if (!dev_priv->dpst.enabled)
		return 0;

	/* return the last (non-dpst) set backlight level */
	return panel->backlight.level;
}

/* called by multi-process, be cautious to avoid race condition */
void
i915_dpst_set_brightness(struct drm_device *dev, u32 brightness_val)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct intel_connector *i_connector = get_intel_connector_on_edp(dev);
	u32 backlight_level = brightness_val;

	if (NULL == i_connector)
		return;

	if (!dev_priv->dpst.enabled)
		return;

	/* Calculate the backlight after it has been reduced by "dpst
	 * blc adjustment" percent . blc_adjustment value is stored
	 * after multiplying by 100, so we have to divide by 100 2nd time
	 * to get to the correct value */
	backlight_level = ((brightness_val *
				dev_priv->dpst.blc_adjustment)/100)/100;
	intel_panel_actually_set_backlight(i_connector, backlight_level);
}

void
i915_dpst_irq_handler(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;

	/* Notify user mode of the interrupt */
	if (dev_priv->dpst.task != NULL)
		send_sig_info(dev_priv->dpst.signal, SEND_SIG_FORCED,
							dev_priv->dpst.task);
}

int
i915_dpst_context(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct dpst_initialize_context *ioctl_data = NULL;
	drm_i915_private_t *dev_priv = dev->dev_private;

	int ret = -EINVAL;

	if (!data)
		return -EINVAL;

	if (!I915_HAS_DPST(dev))
		return -EINVAL;

	/* Can be called from multiple usermode, prevent race condition */
	mutex_lock(&dev_priv->dpst.ioctl_lock);

	ioctl_data = (struct dpst_initialize_context *) data;
	switch (ioctl_data->dpst_ioctl_type) {
	case DPST_ENABLE:
		ret = i915_dpst_set_user_enable(dev, true);
	break;

	case DPST_DISABLE:
		ret = i915_dpst_set_user_enable(dev, false);
	break;

	case DPST_INIT_DATA:
		ret = i915_dpst_init(dev, ioctl_data);
	break;

	case DPST_GET_BIN_DATA:
		ret = i915_dpst_get_bin_data(dev, ioctl_data);
	break;

	case DPST_APPLY_LUMA:
		ret = i915_dpst_apply_luma(dev, ioctl_data);
	break;

	case DPST_RESET_HISTOGRAM_STATUS:
		ret = i915_dpst_clear_hist_interrupt(dev);
	break;

	default:
		DRM_ERROR("Invalid DPST ioctl type\n");
	break;
	}

	mutex_unlock(&dev_priv->dpst.ioctl_lock);
	return ret;
}

int
i915_dpst_set_kernel_disable(struct drm_device *dev, bool disable)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	int ret = 0;

	if (!I915_HAS_DPST(dev))
		return -EINVAL;

	mutex_lock(&dev_priv->dpst.ioctl_lock);

	dev_priv->dpst.kernel_disable = disable;

	if (disable && dev_priv->dpst.enabled) {
		i915_dpst_save_luma(dev);
		ret = i915_dpst_disable_hist_interrupt(dev);
	} else if (!disable && dev_priv->dpst.user_enable) {
		ret = i915_dpst_enable_hist_interrupt(dev);
		if (!ret)
			i915_dpst_restore_luma(dev);
	}

	mutex_unlock(&dev_priv->dpst.ioctl_lock);

	return ret;
}
