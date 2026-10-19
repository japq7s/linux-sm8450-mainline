/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QC_MSM_CAMSS_VFE_880_PP_H
#define QC_MSM_CAMSS_VFE_880_PP_H

#include <linux/types.h>

struct camss_params_color_correct;
struct camss_params_gamma;
struct v4l2_rect;
struct vfe_device;

/* What differs between the IFEs that run this pixel pipe */
struct vfe_pp_desc {
	u32 wb_gain;	/* WB gain register value for full scale */
	u8 wb_gain_shift;	/* position of the 15uQ10 gain field */
	bool demux_bls;	/* subtract the black level in demux + BLS */
	bool wb_clamp;	/* the WB clamp resets to a value that clips green */
};

extern const struct vfe_pp_desc vfe_pp_desc_680;
extern const struct vfe_pp_desc vfe_pp_desc_780;
extern const struct vfe_pp_desc vfe_pp_desc_880;

void vfe_880_setup_pixel_pipe(struct vfe_device *vfe, const struct vfe_pp_desc *desc,
			      u32 in_width, u32 in_height, const struct v4l2_rect *compose,
			      const struct v4l2_rect *crop, u32 code);
void vfe_880_set_black_level(struct vfe_device *vfe, u32 g, u32 b, u32 r);
void vfe_880_set_wb_gain(struct vfe_device *vfe, const struct vfe_pp_desc *desc,
			 u32 g, u32 b, u32 r);
void vfe_880_set_color_correct(struct vfe_device *vfe,
			       const struct camss_params_color_correct *cc);
void vfe_880_set_gamma(struct vfe_device *vfe, const struct camss_params_gamma *gamma);
void vfe_880_set_aec_bhist_region(struct vfe_device *vfe, u32 h_offset,
				  u32 v_offset, u32 h_num, u32 v_num);

#endif
