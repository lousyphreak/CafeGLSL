/*
 * Copyright 2013 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include "r600_pipe_common.h"
#include "sfn/sfn_nir.h"

void r600_init_nir_options(struct nir_shader_compiler_options *options,
			   enum amd_gfx_level gfx_level,
			   enum radeon_family family)
{
	const bool fp64_emulation = gfx_level < CAYMAN;

	const struct nir_shader_compiler_options nir_options = {
		.float_mul_add16 = nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
		.float_mul_add32 = nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
		.float_mul_add64 = fp64_emulation ? 0 : nir_float_muladd_support_has_ffma | nir_float_muladd_support_fuse,
		.lower_flrp32 = true,
		.lower_flrp64 = true,
		.lower_fdiv = true,
		.lower_isign = true,
		.lower_ineg = true,
		.lower_fsign = true,
		.lower_fmod = true,
		.lower_uadd_carry = true,
		.lower_usub_borrow = true,
		.lower_bitfield_extract = true,
		.lower_bitfield_extract16 = true,
		.lower_bitfield_extract8 = true,
		.lower_bitfield_insert = true,
		.lower_extract_byte = true,
		.lower_extract_word = true,
		.lower_insert_byte = true,
		.lower_insert_word = true,
		/* due to a bug in the shader compiler, some loops hang
		 * if they are not unrolled, see:
		 *    https://bugs.freedesktop.org/show_bug.cgi?id=86720
		 */
		.max_unroll_iterations = 255,
		.lower_interpolate_at = true,
		.has_umad24 = true,
		.has_umul24 = true,
		.has_fmulz = true,
		.has_fsub = true,
		.has_isub = true,
		.has_find_msb_rev = true,
		.lower_iabs = true,
		.lower_uadd_sat = true,
		.lower_usub_sat = true,
		.has_fused_comp_and_csel = true,
		.lower_ifind_msb = true,
		.lower_ufind_msb = true,
		.lower_to_scalar = true,
		.lower_to_scalar_filter = r600_lower_to_scalar_instr_filter,
		.lower_fpow = true,
		.lower_int64_options = ~0,
		.lower_cs_local_index_to_id = true,
		.lower_uniforms_to_ubo = true,
		.lower_image_offset_to_range_base = 1,
		.vectorize_tess_levels = 1,
		.io_options = nir_io_mediump_is_32bit,
		.vertex_id_zero_based = gfx_level >= EVERGREEN,
		.avoid_ternary_with_fabs = 1,
	};

	*options = nir_options;

	if (family < CHIP_CEDAR)
		options->force_indirect_unrolling_sampler = true;

	if (gfx_level >= EVERGREEN) {
		options->has_bfe = true;
		options->has_bfm = true;
		options->has_bitfield_select = true;
	}

	if (gfx_level < EVERGREEN) {
		/* Pre-EG doesn't have these ALU ops */
		options->lower_bit_count = true;
		options->lower_bitfield_reverse = true;
	}

	if (fp64_emulation) {
		options->lower_atomic_offset_to_range_base = true;

		options->lower_doubles_options =
			nir_lower_fp64_full_software |
			nir_lower_dceil |
			nir_lower_dsqrt |
			nir_lower_drcp |
			nir_lower_drsq;
	} else {
		options->lower_doubles_options =
			nir_lower_ddiv |
			nir_lower_dfloor |
			nir_lower_dceil |
			nir_lower_dmod |
			nir_lower_dsub |
			nir_lower_dtrunc |
			nir_lower_dround_even;
	}

	uint8_t indirect_supported_mask =
		(uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES) &
		~BITFIELD_BIT(MESA_SHADER_FRAGMENT);
	options->support_indirect_inputs = indirect_supported_mask;
	options->support_indirect_outputs = indirect_supported_mask;
}
