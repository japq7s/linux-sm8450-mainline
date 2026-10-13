/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QC_MSM_CAMSS_VFE_880_PP_H
#define QC_MSM_CAMSS_VFE_880_PP_H

#include <linux/types.h>

struct vfe_device;

/* What differs between the IFEs that run this pixel pipe */
struct vfe_pp_desc {
	u32 wb_gain;	/* WB gain register value for full scale */
};

extern const struct vfe_pp_desc vfe_pp_desc_880;

void vfe_880_setup_pixel_pipe(struct vfe_device *vfe, const struct vfe_pp_desc *desc,
			      u32 width, u32 height);
void vfe_880_set_aec_bhist_region(struct vfe_device *vfe, u32 h_offset,
				  u32 v_offset, u32 h_num, u32 v_num);

#endif
