/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include "core.h"
#include "head.h"

#include <nvif/class.h>
#include <nvif/pushc97b.h>

#include <nvhw/class/clca7d.h>

#include <nouveau_bo.h>

static int
coreca7d_update(struct nv50_core *core, u32 *interlock, bool ntfy)
{
	const u64 ntfy_addr = core->disp->sync->offset + NV50_DISP_CORE_NTFY;
	const u32 ntfy_hi = upper_32_bits(ntfy_addr);
	const u32 ntfy_lo = lower_32_bits(ntfy_addr);
	struct nvif_push *push = &core->chan.push;
	int ret;

	ret = PUSH_WAIT(push, 5 + (ntfy ? 5 + 2 : 0));
	if (ret)
		return ret;

	if (ntfy) {
		PUSH_MTHD(push, NVCA7D, SET_SURFACE_ADDRESS_HI_NOTIFIER, ntfy_hi,

					SET_SURFACE_ADDRESS_LO_NOTIFIER,
			  NVVAL(NVCA7D, SET_SURFACE_ADDRESS_LO_NOTIFIER, ADDRESS_LO, ntfy_lo >> 4) |
			  NVDEF(NVCA7D, SET_SURFACE_ADDRESS_LO_NOTIFIER, TARGET, PHYSICAL_NVM) |
			  NVDEF(NVCA7D, SET_SURFACE_ADDRESS_LO_NOTIFIER, ENABLE, ENABLE));

		PUSH_MTHD(push, NVCA7D, SET_NOTIFIER_CONTROL,
			  NVDEF(NVCA7D, SET_NOTIFIER_CONTROL, MODE, WRITE) |
			  NVDEF(NVCA7D, SET_NOTIFIER_CONTROL, NOTIFY, ENABLE));
	}

	PUSH_MTHD(push, NVCA7D, SET_INTERLOCK_FLAGS, interlock[NV50_DISP_INTERLOCK_CURS],
				SET_WINDOW_INTERLOCK_FLAGS, interlock[NV50_DISP_INTERLOCK_WNDW]);

	PUSH_MTHD(push, NVCA7D, UPDATE,
		  NVDEF(NVCA7D, UPDATE, RELEASE_ELV, TRUE) |
		  NVDEF(NVCA7D, UPDATE, SPECIAL_HANDLING, NONE) |
		  NVDEF(NVCA7D, UPDATE, INHIBIT_INTERRUPTS, FALSE));

	if (ntfy) {
		PUSH_MTHD(push, NVCA7D, SET_NOTIFIER_CONTROL,
			  NVDEF(NVCA7D, SET_NOTIFIER_CONTROL, NOTIFY, DISABLE));
	}

	return PUSH_KICK(push);
}

/* Usage bounds of a window with a phywin, or none for one without: like
 * nvkms, which leaves unused windows at zero formats and pixels fetched.
 */
static void
coreca7d_wndw_bounds(struct nvif_push *push, int wndw, bool usable)
{
	PUSH_MTHD(push, NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(wndw), usable ?
		  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED1BPP, TRUE) |
		  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED2BPP, TRUE) |
		  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED4BPP, TRUE) |
		  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED8BPP, TRUE) : 0,

				WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(wndw), 0x00000000);

	PUSH_MTHD(push, NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS(wndw),
		  NVVAL(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, MAX_PIXELS_FETCHED_PER_LINE,
			usable ? 0x7fff : 0) |
		  NVDEF(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, ILUT_ALLOWED, TRUE) |
		  NVDEF(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, INPUT_SCALER_TAPS, TAPS_2) |
		  NVDEF(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, UPSCALING_ALLOWED, FALSE));
}

/* Multi-tile: one head scanning out through several tiles, each doing a
 * strip of the line, for pixel clocks above HEAD_CLK_CAP.  Head i keeps
 * tile i and borrows spare tile 4 + i.  Every window of an active head needs
 * a phywin per tile (EvoSetMultiTileConfigCA()), so it also borrows the
 * phywins of its partner head i ^ 2, which has to stay off meanwhile.  Only
 * an inactive head's windows may be left without a phywin.
 */
static u32
coreca7d_tile_phywin(const int *ntiles, const bool *active, int wndw)
{
	const int head = wndw / 2, partner = head ^ 2;

	if (active[head] && ntiles[head] > 1)
		return BIT(wndw) | BIT(partner * 2 + wndw % 2);
	if (active[partner] && ntiles[partner] > 1)
		return 0;

	return BIT(wndw);
}

/* Reconfigure heads that are off both before and after the commit (!busy),
 * in a core update of its own ahead of it.  A head owning tiles needs a
 * phywin per tile on each of its windows (else an INVALID_STATE exception
 * at UPDATE), so a head lending its phywins also gives up its tile, like
 * nvkms leaves inactive heads (EvoInitWindowMappingCA()), and gets both
 * back together.  A window without a phywin gets no usage bounds either.
 * A phywin can't be attached to two windows at once or move between them
 * within one update, so only phywins freed in an earlier update come back.
 */
static bool
coreca7d_tile_prepare(struct nv50_core *core, const int *ntiles,
		      const bool *active, const bool *busy)
{
	struct nvif_push *push = &core->chan.push;
	u32 attached = 0, freed = 0;
	bool pushed = false;
	int head, i;

	/* Lend phywins, or drop borrowed ones from a head that went off. */
	for (head = 0; head < 4; head++) {
		const u32 want[2] = {
			coreca7d_tile_phywin(ntiles, active, head * 2),
			coreca7d_tile_phywin(ntiles, active, head * 2 + 1),
		};
		const u8 tiles = want[0] ? BIT(head) : 0;

		if (busy[head] ||
		    ((core->phywin[head * 2] & ~want[0]) == 0 &&
		     (core->phywin[head * 2 + 1] & ~want[1]) == 0))
			continue;
		if (PUSH_WAIT(push, 2 + 2 * 7))
			return pushed;

		PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(head), tiles);
		core->tiles[head] = tiles;
		for (i = 0; i < 2; i++) {
			const int wndw = head * 2 + i;
			const u32 keep = core->phywin[wndw] & want[i];

			if (!keep)
				coreca7d_wndw_bounds(push, wndw, false);
			PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(wndw), keep);
			freed |= core->phywin[wndw] & ~keep;
			core->phywin[wndw] = keep;
		}
		pushed = true;
	}

	for (i = 0; i < 8; i++)
		attached |= core->phywin[i];

	/* Give a lending head its tile and phywins back once they're free. */
	for (head = 0; head < 4; head++) {
		const u32 want = BIT(head * 2) | BIT(head * 2 + 1);

		if (busy[head] || core->tiles[head] ||
		    !coreca7d_tile_phywin(ntiles, active, head * 2) ||
		    (want & (attached | freed)))
			continue;
		if (PUSH_WAIT(push, 2 + 2 * 7))
			return pushed;

		for (i = 0; i < 2; i++) {
			const int wndw = head * 2 + i;

			coreca7d_wndw_bounds(push, wndw, true);
			PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(wndw), BIT(wndw));
			core->phywin[wndw] = BIT(wndw);
		}
		PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(head), BIT(head));
		core->tiles[head] = BIT(head);
		attached |= want;
		pushed = true;
	}

	return pushed;
}

/* Give an active head its tiles and its windows their phywins, and split
 * the active width across the tiles in tile order: tile head gets width0
 * pixels.  Returns 1 if the assignment changed, or -EBUSY if a phywin it
 * needs is still attached to another head's window.
 */
static int
coreca7d_tile_set(struct nv50_core *core, const int *ntiles, const bool *active,
		  int head, u16 width0, u16 width)
{
	struct nvif_push *push = &core->chan.push;
	const u8 tiles = ntiles[head] > 1 ? BIT(head) | BIT(4 + head) : BIT(head);
	u32 phywin[2], others = 0;
	bool changed;
	int i;

	for (i = 0; i < 2; i++)
		phywin[i] = coreca7d_tile_phywin(ntiles, active, head * 2 + i);

	for (i = 0; i < 8; i++) {
		if (i / 2 != head)
			others |= core->phywin[i];
	}
	if ((phywin[0] | phywin[1]) & others)
		return -EBUSY;

	changed = core->tiles[head] != tiles ||
		  core->phywin[head * 2] != phywin[0] ||
		  core->phywin[head * 2 + 1] != phywin[1];

	if (PUSH_WAIT(push, 10 + 2 * 5))
		return -EBUSY;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(head), tiles);
	for (i = 0; i < 2; i++) {
		/* back from lending its phywins (coreca7d_tile_prepare()) */
		if (!core->phywin[head * 2 + i])
			coreca7d_wndw_bounds(push, head * 2 + i, true);
		PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(head * 2 + i), phywin[i]);
		core->phywin[head * 2 + i] = phywin[i];
	}
	core->tiles[head] = tiles;

	if (ntiles[head] > 1) {
		PUSH_MTHD(push, NVCA7D, TILE_SET_TILE_SIZE(head),
			  NVVAL(NVCA7D, TILE_SET_TILE_SIZE, START, 0) |
			  NVVAL(NVCA7D, TILE_SET_TILE_SIZE, WIDTH, width0));
		PUSH_MTHD(push, NVCA7D, TILE_SET_TILE_SIZE(4 + head),
			  NVVAL(NVCA7D, TILE_SET_TILE_SIZE, START, width0) |
			  NVVAL(NVCA7D, TILE_SET_TILE_SIZE, WIDTH, width - width0));
	} else {
		/* as coreca7d_init() */
		PUSH_MTHD(push, NVCA7D, TILE_SET_TILE_SIZE(head), 0);
	}

	return changed;
}

static int
coreca7d_init(struct nv50_core *core)
{
	struct nvif_push *push = &core->chan.push;
	const u32 windows = 8, heads = 4;
	int ret, i;

	ret = PUSH_WAIT(push, windows * 7 + heads * 6 + (8 - heads) * 2);
	if (ret)
		return ret;

	for (i = 0; i < windows; i++) {
		coreca7d_wndw_bounds(push, i, true);
		PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(i), BIT(i));
		core->phywin[i] = BIT(i);
	}

	for (i = 0; i < heads; i++) {
		PUSH_MTHD(push, NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS(i),
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, CURSOR, USAGE_W256_H256) |
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, OLUT_ALLOWED, TRUE) |
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, OUTPUT_SCALER_TAPS, TAPS_2) |
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, UPSCALING_ALLOWED, TRUE));

		PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(i), BIT(i));

		PUSH_MTHD(push, NVCA7D, TILE_SET_TILE_SIZE(i), 0);
		core->tiles[i] = BIT(i);
	}

	/* Tiles 4-7 have no head of their own here but default to heads 4-7;
	 * clear that so coreca7d_tile_set() can hand them out.  A tile owned
	 * by two heads is an XID 56 (EvoInitWindowMappingCA()).
	 */
	for (; i < 8; i++) {
		PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(i), 0);
		core->tiles[i] = 0;
	}

	core->assign_windows = true;
	return PUSH_KICK(push);
}

/* Pixel clock limits and multi-tile caps, as nvkms reads them
 * (EvoParseCapabilityNotifierCA(), and nvkms-evo3.c for HEAD_CLK_CAP).  A
 * head can take a second tile, 4 + head, when both its tiles support
 * multi-tile and so do the phywins coreca7d_tile_phywin() gives it.
 */
static int
coreca7d_caps_init(struct nouveau_drm *drm, struct nv50_disp *disp)
{
	u32 capc, capf, multi;
	int ret, i;

	ret = corec37d_caps_init(drm, disp);
	if (ret)
		return ret;

	/* SYS_CAPC: TILEn_EXISTS in 7:0, TILEn_SUPPORT_MULTI_TILE in 15:8 */
	capc = nvif_rd32(&disp->caps, 0x000020);
	/* IHUB_COMMON_CAPF: PHYWINn_SUPPORT_MULTI_TILE in 7:0 */
	capf = nvif_rd32(&disp->caps, 0x000028);
	multi = capc & (capc >> 8) & 0xff;

	for (i = 0; i < 8; i++) {
		/* POSTCOMP_HDR_CAPA(i): SCLR_PRESENT and VFILTER_PRESENT */
		const u32 scaler = BIT(18) | BIT(23);

		if ((capc & BIT(i)) &&
		    (nvif_rd32(&disp->caps, 0x000680 + i * 32) & scaler) == scaler)
			disp->tile_scaler |= BIT(i);
	}

	for_each_set_bit(i, &disp->disp->head_mask, ARRAY_SIZE(disp->head_max_khz)) {
		const int partner = i ^ 2;
		const u32 tiles = BIT(i) | BIT(4 + i);
		const u32 phywins = BIT(i * 2) | BIT(i * 2 + 1) |
				    BIT(partner * 2) | BIT(partner * 2 + 1);

		/* HEAD_CLK_CAP(i).PCLK_MAX, in 10MHz */
		disp->head_max_khz[i] = (nvif_rd32(&disp->caps, 0x0005e8 + i * 4) & 0xff) * 10000;

		if (i < 4 && (multi & tiles) == tiles && (capf & phywins) == phywins)
			disp->tile_heads |= BIT(i);
	}

	NV_DEBUG(drm, "tiles: SYS_CAPC %08x IHUB_COMMON_CAPF %08x scaler %02x 2-tile heads %x\n",
		 capc, capf, disp->tile_scaler, disp->tile_heads);
	return 0;
}

static const struct nv50_core_func
coreca7d = {
	.init = coreca7d_init,
	.ntfy_init = corec37d_ntfy_init,
	.caps_init = coreca7d_caps_init,
	.caps_class = GB202_DISP_CAPS,
	.ntfy_wait_done = corec37d_ntfy_wait_done,
	.update = coreca7d_update,
	.wndw.owner = corec37d_wndw_owner,
	.tile.prepare = coreca7d_tile_prepare,
	.tile.set = coreca7d_tile_set,
	.head = &headca7d,
	.sor = &sorc37d,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.crc = &crcca7d,
#endif
};

int
coreca7d_new(struct nouveau_drm *drm, s32 oclass, struct nv50_core **pcore)
{
	return core507d_new_(&coreca7d, drm, oclass, pcore);
}
