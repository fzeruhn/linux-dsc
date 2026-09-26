/*
 * Copyright 2009 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */

#include <drm/display/drm_dp_helper.h>
#include <drm/display/drm_dsc_helper.h>

#include "nouveau_drv.h"
#include "nouveau_connector.h"
#include "nouveau_encoder.h"
#include "nouveau_crtc.h"

#include <nvif/if0011.h>

MODULE_PARM_DESC(mst, "Enable DisplayPort multi-stream (default: enabled)");
static int nouveau_mst = 1;
module_param_named(mst, nouveau_mst, int, 0400);

static bool
nouveau_dp_has_sink_count(struct drm_connector *connector,
			  struct nouveau_encoder *outp)
{
	return drm_dp_read_sink_count_cap(connector, outp->dp.dpcd, &outp->dp.desc);
}

static bool
nouveau_dp_probe_lttpr(struct nouveau_encoder *outp)
{
	u8 rev, size = sizeof(rev);
	int ret;

	ret = nvif_outp_dp_aux_xfer(&outp->outp, DP_AUX_NATIVE_READ, &size,
				    DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV,
				    &rev);
	if (ret || size < sizeof(rev) || rev < 0x14)
		return false;

	return true;
}

static enum drm_connector_status
nouveau_dp_probe_dpcd(struct nouveau_connector *nv_connector,
		      struct nouveau_encoder *outp)
{
	struct drm_connector *connector = &nv_connector->base;
	struct drm_dp_aux *aux = &nv_connector->aux;
	struct nv50_mstm *mstm = NULL;
	enum drm_connector_status status = connector_status_disconnected;
	int ret;
	u8 *dpcd = outp->dp.dpcd;

	outp->dp.lttpr.nr = 0;
	outp->dp.rate_nr  = 0;
	outp->dp.link_nr  = 0;
	outp->dp.link_bw  = 0;

	if (connector->connector_type != DRM_MODE_CONNECTOR_eDP &&
	    nouveau_dp_probe_lttpr(outp) &&
	    !drm_dp_read_dpcd_caps(aux, dpcd) &&
	    !drm_dp_read_lttpr_common_caps(aux, dpcd, outp->dp.lttpr.caps)) {
		int nr = drm_dp_lttpr_count(outp->dp.lttpr.caps);

		if (!drm_dp_lttpr_init(aux, nr))
			outp->dp.lttpr.nr = nr;
	}

	ret = drm_dp_read_dpcd_caps(aux, dpcd);
	if (ret < 0)
		goto out;

	outp->dp.link_nr = dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
	if (outp->dcb->dpconf.link_nr < outp->dp.link_nr)
		outp->dp.link_nr = outp->dcb->dpconf.link_nr;

	if (outp->dp.lttpr.nr) {
		int links = drm_dp_lttpr_max_lane_count(outp->dp.lttpr.caps);

		if (links && links < outp->dp.link_nr)
			outp->dp.link_nr = links;
	}

	if (connector->connector_type == DRM_MODE_CONNECTOR_eDP && dpcd[DP_DPCD_REV] >= 0x13) {
		__le16 rates[DP_MAX_SUPPORTED_RATES];

		ret = drm_dp_dpcd_read(aux, DP_SUPPORTED_LINK_RATES, rates, sizeof(rates));
		if (ret == sizeof(rates)) {
			for (int i = 0; i < ARRAY_SIZE(rates); i++) {
				u32 rate = (le16_to_cpu(rates[i]) * 200) / 10;
				int j;

				if (!rate)
					break;

				for (j = 0; j < outp->dp.rate_nr; j++) {
					if (rate > outp->dp.rate[j].rate) {
						for (int k = outp->dp.rate_nr; k > j; k--)
							outp->dp.rate[k] = outp->dp.rate[k - 1];
						break;
					}
				}

				outp->dp.rate[j].dpcd = i;
				outp->dp.rate[j].rate = rate;
				outp->dp.rate_nr++;
			}
		}
	}

	/* DSC sink caps (DP 1.4+), DPCD 0x60-0x6F */
	if (dpcd[DP_DPCD_REV] >= 0x14) {
		u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE];

		ret = drm_dp_dpcd_read(aux, DP_DSC_SUPPORT, dsc_dpcd,
					sizeof(dsc_dpcd));
		if (ret == sizeof(dsc_dpcd)) {
			if (memcmp(outp->dp.dsc.dsc_dpcd, dsc_dpcd,
				   sizeof(dsc_dpcd)))
				NV_INFO(nouveau_drm(connector->dev),
					"%s: DSC DPCD %*ph\n", connector->name,
					(int)sizeof(dsc_dpcd), dsc_dpcd);
			memcpy(outp->dp.dsc.dsc_dpcd, dsc_dpcd,
			       sizeof(dsc_dpcd));
			outp->dp.dsc.supported =
				drm_dp_sink_supports_dsc(dsc_dpcd);
		}
	}

	if (!outp->dp.rate_nr) {
		const u32 rates[] = { 810000, 540000, 270000, 162000 };
		u32 max_rate = dpcd[DP_MAX_LINK_RATE] * 27000;

		if (outp->dp.lttpr.nr) {
			int rate = drm_dp_lttpr_max_link_rate(outp->dp.lttpr.caps);

			if (rate && rate < max_rate)
				max_rate = rate;
		}

		max_rate = min_t(int, max_rate, outp->dcb->dpconf.link_bw);

		for (int i = 0; i < ARRAY_SIZE(rates); i++) {
			if (rates[i] <= max_rate) {
				outp->dp.rate[outp->dp.rate_nr].dpcd = -1;
				outp->dp.rate[outp->dp.rate_nr].rate = rates[i];
				outp->dp.rate_nr++;
			}
		}

		if (WARN_ON(!outp->dp.rate_nr))
			goto out;
	}

	ret = nvif_outp_dp_rates(&outp->outp, outp->dp.rate, outp->dp.rate_nr);
	if (ret)
		goto out;

	for (int i = 0; i < outp->dp.rate_nr; i++) {
		u32 link_bw = outp->dp.rate[i].rate;

		if (link_bw > outp->dp.link_bw)
			outp->dp.link_bw = link_bw;
	}

	ret = drm_dp_read_desc(aux, &outp->dp.desc, drm_dp_is_branch(dpcd));
	if (ret < 0)
		goto out;

	if (nouveau_mst) {
		mstm = outp->dp.mstm;
		if (mstm)
			mstm->can_mst = drm_dp_read_mst_cap(aux, dpcd) == DRM_DP_MST;
	}

	if (nouveau_dp_has_sink_count(connector, outp)) {
		ret = drm_dp_read_sink_count(aux);
		if (ret < 0)
			goto out;

		outp->dp.sink_count = ret;

		/*
		 * Dongle connected, but no display. Don't bother reading
		 * downstream port info
		 */
		if (!outp->dp.sink_count)
			return connector_status_disconnected;
	}

	ret = drm_dp_read_downstream_info(aux, dpcd,
					  outp->dp.downstream_ports);
	if (ret < 0)
		goto out;

	status = connector_status_connected;
out:
	if (status != connector_status_connected) {
		/* Clear any cached info */
		outp->dp.sink_count = 0;
	}
	return status;
}

int
nouveau_dp_detect(struct nouveau_connector *nv_connector,
		  struct nouveau_encoder *nv_encoder)
{
	struct drm_device *dev = nv_encoder->base.base.dev;
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct drm_connector *connector = &nv_connector->base;
	struct nv50_mstm *mstm = nv_encoder->dp.mstm;
	enum drm_connector_status status;
	u8 *dpcd = nv_encoder->dp.dpcd;
	int ret = NOUVEAU_DP_NONE, hpd;

	/* eDP ports don't support hotplugging - so there's no point in probing eDP ports unless we
	 * haven't probed them once before.
	 */
	if (connector->connector_type == DRM_MODE_CONNECTOR_eDP) {
		if (connector->status == connector_status_connected)
			return NOUVEAU_DP_SST;
		else if (connector->status == connector_status_disconnected)
			return NOUVEAU_DP_NONE;
	}

	// Ensure that the aux bus is enabled for probing
	drm_dp_dpcd_set_powered(&nv_connector->aux, true);

	mutex_lock(&nv_encoder->dp.hpd_irq_lock);
	if (mstm) {
		/* If we're not ready to handle MST state changes yet, just
		 * report the last status of the connector. We'll reprobe it
		 * once we've resumed.
		 */
		if (mstm->suspended) {
			if (mstm->is_mst)
				ret = NOUVEAU_DP_MST;
			else if (connector->status ==
				 connector_status_connected)
				ret = NOUVEAU_DP_SST;

			goto out;
		}
	}

	hpd = nvif_outp_detect(&nv_encoder->outp);
	if (hpd == NOT_PRESENT) {
		nvif_outp_dp_aux_pwr(&nv_encoder->outp, false);
		goto out;
	}
	nvif_outp_dp_aux_pwr(&nv_encoder->outp, true);

	status = nouveau_dp_probe_dpcd(nv_connector, nv_encoder);
	if (status == connector_status_disconnected) {
		nvif_outp_dp_aux_pwr(&nv_encoder->outp, false);
		goto out;
	}

	/* If we're in MST mode, we're done here */
	if (mstm && mstm->can_mst && mstm->is_mst) {
		ret = NOUVEAU_DP_MST;
		goto out;
	}

	NV_DEBUG(drm, "sink dpcd version: 0x%02x\n", dpcd[DP_DPCD_REV]);
	for (int i = 0; i < nv_encoder->dp.rate_nr; i++)
		NV_DEBUG(drm, "sink rate %d: %d\n", i, nv_encoder->dp.rate[i].rate);

	NV_DEBUG(drm, "encoder: %dx%d\n", nv_encoder->dcb->dpconf.link_nr,
					  nv_encoder->dcb->dpconf.link_bw);
	NV_DEBUG(drm, "maximum: %dx%d\n", nv_encoder->dp.link_nr,
					  nv_encoder->dp.link_bw);

	if (mstm && mstm->can_mst) {
		ret = nv50_mstm_detect(nv_encoder);
		if (ret == 1) {
			ret = NOUVEAU_DP_MST;
			goto out;
		} else if (ret != 0) {
			nvif_outp_dp_aux_pwr(&nv_encoder->outp, false);
			goto out;
		}
	}
	ret = NOUVEAU_DP_SST;

out:
	if (mstm && !mstm->suspended && ret != NOUVEAU_DP_MST)
		nv50_mstm_remove(mstm);

	/* GSP doesn't like when we try to do aux transactions on a port it considers disconnected,
	 * and since we don't really have a usecase for that anyway - just disable the aux bus here
	 * if we've decided the connector is disconnected
	 */
	if (ret == NOUVEAU_DP_NONE)
		drm_dp_dpcd_set_powered(&nv_connector->aux, false);

	mutex_unlock(&nv_encoder->dp.hpd_irq_lock);
	return ret;
}

void
nouveau_dp_power_down(struct nouveau_encoder *outp)
{
	struct drm_dp_aux *aux = &outp->conn->aux;
	int ret;
	u8 pwr;

	mutex_lock(&outp->dp.hpd_irq_lock);

	ret = drm_dp_dpcd_readb(aux, DP_SET_POWER, &pwr);
	if (ret == 1) {
		pwr &= ~DP_SET_POWER_MASK;
		pwr |=  DP_SET_POWER_D3;
		drm_dp_dpcd_writeb(aux, DP_SET_POWER, pwr);
	}

	outp->dp.lt.nr = 0;
	mutex_unlock(&outp->dp.hpd_irq_lock);
}

static bool
nouveau_dp_train_link(struct nouveau_encoder *outp, bool retrain)
{
	struct drm_dp_aux *aux = &outp->conn->aux;
	bool post_lt = false;
	int ret, retries = 0;

	if ( (outp->dp.dpcd[DP_MAX_LANE_COUNT] & 0x20) &&
	    !(outp->dp.dpcd[DP_MAX_DOWNSPREAD] & DP_TPS4_SUPPORTED))
	    post_lt = true;

retry:
	ret = nvif_outp_dp_train(&outp->outp, outp->dp.dpcd,
					      outp->dp.lttpr.nr,
					      outp->dp.lt.nr,
					      outp->dp.lt.bw,
					      outp->dp.lt.mst,
					      post_lt,
					      retrain);
	if (ret)
		return false;

	if (post_lt) {
		u8 stat[DP_LINK_STATUS_SIZE];
		u8 prev[2];
		u8 time = 0, adjusts = 0, tmp;

		ret = drm_dp_dpcd_read_phy_link_status(aux, DP_PHY_DPRX, stat);
		if (ret)
			return false;

		for (;;) {
			if (!drm_dp_channel_eq_ok(stat, outp->dp.lt.nr)) {
				ret = 1;
				break;
			}

			if (!(stat[2] & 0x02))
				break;

			msleep(5);
			time += 5;

			memcpy(prev, &stat[4], sizeof(prev));
			ret = drm_dp_dpcd_read_phy_link_status(aux, DP_PHY_DPRX, stat);
			if (ret)
				break;

			if (!memcmp(prev, &stat[4], sizeof(prev))) {
				if (time > 200)
					break;
			} else {
				u8 pe[4], vs[4];

				if (adjusts++ == 6)
					break;

				for (int i = 0; i < outp->dp.lt.nr; i++) {
					pe[i] = drm_dp_get_adjust_request_pre_emphasis(stat, i) >>
							DP_TRAIN_PRE_EMPHASIS_SHIFT;
					vs[i] = drm_dp_get_adjust_request_voltage(stat, i) >>
							DP_TRAIN_VOLTAGE_SWING_SHIFT;
				}

				ret = nvif_outp_dp_drive(&outp->outp, outp->dp.lt.nr, pe, vs);
				if (ret)
					break;

				time = 0;
			}
		}

		if (drm_dp_dpcd_readb(aux, DP_LANE_COUNT_SET, &tmp) == 1) {
			tmp &= ~0x20;
			drm_dp_dpcd_writeb(aux, DP_LANE_COUNT_SET, tmp);
		}
	}

	if (ret == 1 && retries++ < 3)
		goto retry;

	return ret == 0;
}

bool
nouveau_dp_train(struct nouveau_encoder *outp, bool mst, u32 khz, u8 bpc)
{
	struct nouveau_drm *drm = nouveau_drm(outp->base.base.dev);
	struct drm_dp_aux *aux = &outp->conn->aux;
	u32 min_rate;
	u8 pwr;
	bool ret = true;

	if (mst)
		min_rate = outp->dp.link_nr * outp->dp.rate[0].rate;
	else
		min_rate = DIV_ROUND_UP(khz * bpc * 3, 8);

	NV_DEBUG(drm, "%s link training (mst:%d min_rate:%d)\n",
		 outp->base.base.name, mst, min_rate);

	mutex_lock(&outp->dp.hpd_irq_lock);

	if (drm_dp_dpcd_readb(aux, DP_SET_POWER, &pwr) == 1) {
		if ((pwr & DP_SET_POWER_MASK) != DP_SET_POWER_D0) {
			pwr &= ~DP_SET_POWER_MASK;
			pwr |=  DP_SET_POWER_D0;
			drm_dp_dpcd_writeb(aux, DP_SET_POWER, pwr);
		}
	}

	for (int nr = outp->dp.link_nr; nr; nr >>= 1) {
		for (int rate = 0; rate < outp->dp.rate_nr; rate++) {
			if (outp->dp.rate[rate].rate * nr >= min_rate) {
				outp->dp.lt.nr = nr;
				outp->dp.lt.bw = outp->dp.rate[rate].rate;
				outp->dp.lt.mst = mst;
				if (nouveau_dp_train_link(outp, false))
					goto done;
			}
		}
	}

	ret = false;
done:
	mutex_unlock(&outp->dp.hpd_irq_lock);
	return ret;
}

static bool
nouveau_dp_link_check_locked(struct nouveau_encoder *outp)
{
	u8 link_status[DP_LINK_STATUS_SIZE];

	if (!outp || !outp->dp.lt.nr)
		return true;

	if (drm_dp_dpcd_read_phy_link_status(&outp->conn->aux, DP_PHY_DPRX, link_status) < 0)
		return false;

	if (drm_dp_channel_eq_ok(link_status, outp->dp.lt.nr))
		return true;

	return nouveau_dp_train_link(outp, true);
}

bool
nouveau_dp_link_check(struct nouveau_connector *nv_connector)
{
	struct nouveau_encoder *outp = nv_connector->dp_encoder;
	bool link_ok = true;

	if (outp) {
		mutex_lock(&outp->dp.hpd_irq_lock);
		if (outp->dp.lt.nr)
			link_ok = nouveau_dp_link_check_locked(outp);
		mutex_unlock(&outp->dp.hpd_irq_lock);
	}

	return link_ok;
}

void
nouveau_dp_irq(struct work_struct *work)
{
	struct nouveau_connector *nv_connector =
		container_of(work, typeof(*nv_connector), irq_work);
	struct drm_connector *connector = &nv_connector->base;
	struct nouveau_encoder *outp = find_encoder(connector, DCB_OUTPUT_DP);
	struct nouveau_drm *drm = nouveau_drm(outp->base.base.dev);
	struct nv50_mstm *mstm;
	u64 hpd = 0;
	int ret;

	if (!outp)
		return;

	mstm = outp->dp.mstm;
	NV_DEBUG(drm, "service %s\n", connector->name);

	mutex_lock(&outp->dp.hpd_irq_lock);

	if (mstm && mstm->is_mst) {
		if (!nv50_mstm_service(drm, nv_connector, mstm))
			hpd |= NVIF_CONN_EVENT_V0_UNPLUG;
	} else {
		drm_dp_cec_irq(&nv_connector->aux);

		if (nouveau_dp_has_sink_count(connector, outp)) {
			ret = drm_dp_read_sink_count(&nv_connector->aux);
			if (ret != outp->dp.sink_count)
				hpd |= NVIF_CONN_EVENT_V0_PLUG;
			if (ret >= 0)
				outp->dp.sink_count = ret;
		}
	}

	mutex_unlock(&outp->dp.hpd_irq_lock);

	nouveau_connector_hpd(nv_connector, NVIF_CONN_EVENT_V0_IRQ | hpd);
}

/* Smallest slice count the sink advertises that keeps each slice within the
 * sink's max slice width and per-slice pixel throughput.  0 if none does.
 */
static u32
nouveau_dp_dsc_slice_count(struct nouveau_encoder *outp,
			   const struct drm_display_mode *mode)
{
	const u8 *dsc_dpcd = outp->dp.dsc.dsc_dpcd;
	const u32 max_slice_width = drm_dp_dsc_sink_max_slice_width(dsc_dpcd);
	const u32 mask = drm_dp_dsc_sink_slice_count_mask(dsc_dpcd, false);
	const int throughput =
		drm_dp_dsc_sink_max_slice_throughput(dsc_dpcd, mode->clock, true);
	u32 min_count;

	min_count = max_slice_width ?
		    DIV_ROUND_UP(mode->hdisplay, max_slice_width) : 1;
	if (throughput > 0)
		min_count = max_t(u32, min_count,
				  DIV_ROUND_UP(mode->clock, throughput));

	for (u32 count = min_count; count <= 24; count++) {
		if (mask & drm_dp_dsc_slice_count_to_mask(count))
			return count;
	}

	return 0;
}

/* drm_dsc_compute_rc_parameters() truncates scale_increment_interval into
 * its 16-bit PPS field, which happens when slices are too tall.  Same checks
 * as NVIDIA's DSC_PpsCalcScaleInterval().
 */
static bool
nouveau_dp_dsc_scale_valid(const struct drm_dsc_config *cfg)
{
	u32 final_scale = 8 * cfg->rc_model_size /
			  (cfg->rc_model_size - cfg->final_offset);

	if (final_scale > 63)
		return false;

	if (final_scale > 9 &&
	    ((u32)cfg->final_offset << 11) /
	    ((final_scale - 9) * (cfg->nfl_bpg_offset + cfg->slice_bpg_offset)) > 0xffff)
		return false;

	return cfg->scale_decrement_interval >= 1 &&
	       cfg->scale_decrement_interval <= DSC_SCALE_DECREMENT_INTERVAL_MAX;
}

/* Fill in the DSC config (the PPS contents) for compressing mode from bpc to
 * bpp_x16 (1/16 bpp).  Slices are as few as the sink allows; slice height is
 * the tallest vdisplay / 1..16 whose rate-control parameters fit the PPS, as
 * NVIDIA's Dsc_PpsCalcHeight() does for DP.
 */
int
nouveau_dp_dsc_compute_config(struct nouveau_encoder *outp,
			      const struct drm_display_mode *mode,
			      u8 bpc, u16 bpp_x16, struct drm_dsc_config *cfg)
{
	const u8 *dsc_dpcd = outp->dp.dsc.dsc_dpcd;
	const u8 rev = dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT];
	u32 slice_count = nouveau_dp_dsc_slice_count(outp, mode);
	int ret;

	if (!slice_count)
		return -EINVAL;

	memset(cfg, 0, sizeof(*cfg));
	cfg->dsc_version_major = 1;
	cfg->dsc_version_minor = min_t(u8, (rev & DP_DSC_MINOR_MASK) >>
					   DP_DSC_MINOR_SHIFT, 2);
	cfg->pic_width = mode->hdisplay;
	cfg->pic_height = mode->vdisplay;
	cfg->slice_count = slice_count;
	cfg->slice_width = DIV_ROUND_UP(mode->hdisplay, slice_count);
	cfg->bits_per_component = bpc;
	cfg->bits_per_pixel = bpp_x16;
	cfg->convert_rgb = true;
	/* 13: the most NVIDIA's DSC encoder accepts (nvt_dsc_pps.h) */
	cfg->line_buf_depth = min_t(u8, drm_dp_dsc_sink_line_buf_depth(dsc_dpcd), 13);
	cfg->block_pred_enable =
		dsc_dpcd[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] &
		DP_DSC_BLK_PREDICTION_IS_SUPPORTED;

	if (cfg->line_buf_depth < 8)
		return -EINVAL;

	drm_dsc_set_const_params(cfg);
	drm_dsc_set_rc_buf_thresh(cfg);
	ret = drm_dsc_setup_rc_params(cfg, DRM_DSC_1_2_444);
	if (ret)
		return ret;

	for (int i = 1; i <= 16 && mode->vdisplay / i >= 8; i++) {
		if (mode->vdisplay % i)
			continue;

		cfg->slice_height = mode->vdisplay / i;
		/* compute_rc_parameters() may lower it for narrow slices */
		cfg->initial_scale_value = drm_dsc_initial_scale_value(cfg);
		if (!drm_dsc_compute_rc_parameters(cfg) &&
		    nouveau_dp_dsc_scale_valid(cfg))
			return 0;
	}

	return -ERANGE;
}

/* TODO:
 * - Validate against the DP caps advertised by the GPU (we don't check these
 *   yet)
 */
enum drm_mode_status
nv50_dp_mode_valid(struct nouveau_encoder *outp,
		   const struct drm_display_mode *mode,
		   unsigned *out_clock)
{
	const unsigned int min_clock = 25000;
	unsigned int max_rate, mode_rate, ds_max_dotclock, clock = mode->clock;
	/* Check with the minmum bpc always, so we can advertise better modes.
	 * In particlar not doing this causes modes to be dropped on HDR
	 * displays as we might check with a bpc of 16 even.
	 */
	u8 bpp = 6 * 3;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE && !outp->caps.dp_interlace)
		return MODE_NO_INTERLACE;

	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) == DRM_MODE_FLAG_3D_FRAME_PACKING)
		clock *= 2;

	/* When DSC is supported, use effective bpp (compressed) for bandwidth check.
	 * DSC 1.2 typically compresses ~3:1, so 18 bpp → 6 bpp.
	 * The GSP will do final validation via CALCULATE_DP_IMP during mode set.
	 */
	if (outp->dp.dsc.supported)
		bpp = 6;

	max_rate = outp->dp.link_nr * outp->dp.link_bw;
	mode_rate = DIV_ROUND_UP(clock * bpp, 8);
	if (mode_rate > max_rate)
		return MODE_CLOCK_HIGH;

	ds_max_dotclock = drm_dp_downstream_max_dotclock(outp->dp.dpcd, outp->dp.downstream_ports);
	if (ds_max_dotclock && clock > ds_max_dotclock)
		return MODE_CLOCK_HIGH;

	if (clock < min_clock)
		return MODE_CLOCK_LOW;

	if (out_clock)
		*out_clock = clock;

	return MODE_OK;
}
