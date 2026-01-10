#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enc-m00_png/enc_png.h"
#include "enc-m01_riff/enc_riff.h"
#include "enc-m04_yuv/enc_rgb_to_yuv.h"
#include "enc-m07_tokens/enc_vp8_tokens.h"
#include "enc-m08_filter/enc_loopfilter.h"
#include "enc-m08_recon/enc_recon.h"

typedef enum {
	ENC_MODE_BPRED = 0,
	ENC_MODE_BPRED_RDO = 1,
	ENC_MODE_I16 = 2,
	ENC_MODE_DC = 3,
} EncMode;

static void usage(const char* argv0) {
	fprintf(stderr,
	        "Usage: %s [--q <0..100>] [--mode <bpred|bpred-rdo|i16|dc>] [--loopfilter] [--token-probs <default|adaptive|adaptive2>] [--mb-skip] [--bpred-rdo-lambda-mul N] [--bpred-rdo-lambda-div N] [--bpred-rdo-rate <proxy|entropy|dry-run>] [--bpred-rdo-signal <proxy|entropy>] [--bpred-rdo-quant <default|ac-deadzone>] [--bpred-rdo-ac-deadzone N] [--bpred-rdo-qscale-y-ac N] [--bpred-rdo-qscale-uv-ac N] [--bpred-rdo-satd-prune-k N] <in.png> <out.webp>\n"
	        "\n"
	        "Standalone VP8 keyframe (lossy) encoder producing a simple WebP container.\n"
	        "\n"
	        "Options:\n"
	        "  --q <0..100>           Quality (mapped to VP8 qindex). Default: 75\n"
	        "  --mode <bpred|bpred-rdo|i16|dc>  Intra mode strategy. Default: bpred-rdo\n"
	        "  --loopfilter | --lf    Write deterministic loopfilter header params derived from qindex\n"
	        "  --token-probs <default|adaptive|adaptive2>  Emit coefficient token prob updates. Default: adaptive\n"
	        "  --mb-skip              Experimental: signal mb_skip_coeff and omit tokens for all-zero MBs\n"
	        "  --bpred-rdo-lambda-mul N  Tune bpred-rdo: multiply lambda(qindex) by N (default 10)\n"
	        "  --bpred-rdo-lambda-div N  Tune bpred-rdo: divide lambda(qindex) by N (default 1)\n"
	        "  --bpred-rdo-rate <proxy|entropy|dry-run>  Tune bpred-rdo: rate estimator (default dry-run)\n"
	        "  --bpred-rdo-signal <proxy|entropy>  Tune bpred-rdo: mode signaling cost model (default proxy)\n"
	        "  --bpred-rdo-quant <default|ac-deadzone>  Tune bpred-rdo: quantization tweak (default ac-deadzone)\n"
	        "  --bpred-rdo-ac-deadzone N  Tune bpred-rdo: AC deadzone threshold percent (default 70)\n"
	        "  --bpred-rdo-qscale-y-dc N  Tune bpred-rdo: scale Y DC quant step percent (default 100)\n"
	        "  --bpred-rdo-qscale-y-ac N  Tune bpred-rdo: scale Y AC quant step percent (default 100)\n"
	        "  --bpred-rdo-qscale-uv-dc N  Tune bpred-rdo: scale UV DC quant step percent (default 100)\n"
	        "  --bpred-rdo-qscale-uv-ac N  Tune bpred-rdo: scale UV AC quant step percent (default 130)\n"
	        "  --bpred-rdo-satd-prune-k N  Tune bpred-rdo: keep best N 4x4 modes by SATD before full eval (default 0=off)\n",
	        argv0);
}

static int parse_int(const char* s, int* out) {
	char* end = NULL;
	long v = strtol(s, &end, 10);
	if (!s[0] || (end && *end)) return -1;
	if (v < -2147483647L || v > 2147483647L) return -1;
	*out = (int)v;
	return 0;
}

static int parse_mode(const char* s, EncMode* out) {
	if (strcmp(s, "bpred") == 0) {
		*out = ENC_MODE_BPRED;
		return 0;
	}
	if (strcmp(s, "bpred-rdo") == 0 || strcmp(s, "bpred_rdo") == 0) {
		*out = ENC_MODE_BPRED_RDO;
		return 0;
	}
	if (strcmp(s, "i16") == 0 || strcmp(s, "i16x16") == 0) {
		*out = ENC_MODE_I16;
		return 0;
	}
	if (strcmp(s, "dc") == 0 || strcmp(s, "dc_pred") == 0) {
		*out = ENC_MODE_DC;
		return 0;
	}
	return -1;
}

int main(int argc, char** argv) {
	int quality = 75;
	int enable_loopfilter = 0;
	int enable_mb_skip = 0;
	EncMode mode = ENC_MODE_BPRED_RDO;
	EncVp8TokenProbsMode token_probs_mode = ENC_VP8_TOKEN_PROBS_ADAPTIVE;
	int bpred_rdo_lambda_mul = 10;
	int bpred_rdo_lambda_div = 1;
	int bpred_rdo_rate_mode = 2;
	int bpred_rdo_signal_mode = 0;
	int bpred_rdo_quant_mode = 1;
	int bpred_rdo_ac_deadzone_pct = 70;
	int bpred_rdo_qscale_y_dc_pct = 100;
	int bpred_rdo_qscale_y_ac_pct = 100;
	int bpred_rdo_qscale_uv_dc_pct = 100;
	int bpred_rdo_qscale_uv_ac_pct = 130;
	int bpred_rdo_satd_prune_k = 0;

	int argi = 1;
	while (argi < argc) {
		if (argi + 1 < argc && strcmp(argv[argi], "--q") == 0) {
			if (parse_int(argv[argi + 1], &quality) != 0 || quality < 0 || quality > 100) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--mode") == 0) {
			if (parse_mode(argv[argi + 1], &mode) != 0) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (strcmp(argv[argi], "--loopfilter") == 0 || strcmp(argv[argi], "--lf") == 0) {
			enable_loopfilter = 1;
			argi += 1;
			continue;
		}
		if (strcmp(argv[argi], "--mb-skip") == 0) {
			enable_mb_skip = 1;
			argi += 1;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--token-probs") == 0) {
			const char* s = argv[argi + 1];
			if (strcmp(s, "default") == 0) {
				token_probs_mode = ENC_VP8_TOKEN_PROBS_DEFAULT;
			} else if (strcmp(s, "adaptive") == 0) {
				token_probs_mode = ENC_VP8_TOKEN_PROBS_ADAPTIVE;
			} else if (strcmp(s, "adaptive2") == 0) {
				token_probs_mode = ENC_VP8_TOKEN_PROBS_ADAPTIVE2;
			} else {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-lambda-mul") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_lambda_mul) != 0 || bpred_rdo_lambda_mul <= 0) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-lambda-div") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_lambda_div) != 0 || bpred_rdo_lambda_div <= 0) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-rate") == 0) {
			const char* s = argv[argi + 1];
			if (strcmp(s, "proxy") == 0) {
				bpred_rdo_rate_mode = 0;
			} else if (strcmp(s, "entropy") == 0) {
				bpred_rdo_rate_mode = 1;
			} else if (strcmp(s, "dry-run") == 0 || strcmp(s, "dryrun") == 0) {
				bpred_rdo_rate_mode = 2;
			} else {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-signal") == 0) {
			const char* s = argv[argi + 1];
			if (strcmp(s, "proxy") == 0) {
				bpred_rdo_signal_mode = 0;
			} else if (strcmp(s, "entropy") == 0) {
				bpred_rdo_signal_mode = 1;
			} else {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-quant") == 0) {
			const char* s = argv[argi + 1];
			if (strcmp(s, "default") == 0) {
				bpred_rdo_quant_mode = 0;
			} else if (strcmp(s, "ac-deadzone") == 0 || strcmp(s, "ac_deadzone") == 0) {
				bpred_rdo_quant_mode = 1;
			} else {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-ac-deadzone") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_ac_deadzone_pct) != 0 || bpred_rdo_ac_deadzone_pct < 0 ||
			    bpred_rdo_ac_deadzone_pct > 99) {
				usage(argv[0]);
				return 2;
			}
			// Convenience: specifying a deadzone implies enabling the deadzone quantization.
			bpred_rdo_quant_mode = 1;
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-qscale-y-ac") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_qscale_y_ac_pct) != 0 || bpred_rdo_qscale_y_ac_pct <= 0 ||
			    bpred_rdo_qscale_y_ac_pct > 400) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-qscale-y-dc") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_qscale_y_dc_pct) != 0 || bpred_rdo_qscale_y_dc_pct <= 0 ||
			    bpred_rdo_qscale_y_dc_pct > 400) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-qscale-uv-ac") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_qscale_uv_ac_pct) != 0 || bpred_rdo_qscale_uv_ac_pct <= 0 ||
			    bpred_rdo_qscale_uv_ac_pct > 400) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-qscale-uv-dc") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_qscale_uv_dc_pct) != 0 || bpred_rdo_qscale_uv_dc_pct <= 0 ||
			    bpred_rdo_qscale_uv_dc_pct > 400) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		if (argi + 1 < argc && strcmp(argv[argi], "--bpred-rdo-satd-prune-k") == 0) {
			if (parse_int(argv[argi + 1], &bpred_rdo_satd_prune_k) != 0 || bpred_rdo_satd_prune_k < 0 ||
			    bpred_rdo_satd_prune_k > 10) {
				usage(argv[0]);
				return 2;
			}
			argi += 2;
			continue;
		}
		break;
	}

	if (argc - argi != 2) {
		usage(argv[0]);
		return 2;
	}
	const char* in_path = argv[argi++];
	const char* out_path = argv[argi++];

	EncPngImage img;
	if (enc_png_read_file(in_path, &img) != 0) {
		fprintf(stderr,
		        "enc_png_read_file failed for %s (errno=%d: %s)\n",
		        in_path,
		        errno,
		        (errno != 0) ? strerror(errno) : "unknown");
		return 1;
	}
	if (!(img.channels == 3 || img.channels == 4)) {
		fprintf(stderr, "%s: unsupported channels=%u\n", in_path, img.channels);
		enc_png_free(&img);
		return 1;
	}

	EncYuv420Image yuv;
	const uint32_t stride = img.width * (uint32_t)img.channels;
	if (enc_yuv420_from_rgb_libwebp(img.data, img.width, img.height, stride, img.channels, &yuv) != 0) {
		fprintf(stderr, "%s: RGB->YUV failed (errno=%d)\n", in_path, errno);
		enc_png_free(&img);
		return 1;
	}

	uint8_t* y_modes = NULL;
	size_t y_modes_count = 0;
	uint8_t* b_modes = NULL;
	size_t b_modes_count = 0;
	uint8_t* uv_modes = NULL;
	size_t uv_modes_count = 0;
	int16_t* coeffs = NULL;
	size_t coeffs_count = 0;
	uint8_t qindex = 0;

	int rc = 0;
	if (mode == ENC_MODE_DC) {
		rc = enc_vp8_encode_dc_pred_inloop(&yuv, quality, &coeffs, &coeffs_count, &qindex);
	} else if (mode == ENC_MODE_I16) {
		rc = enc_vp8_encode_i16x16_uv_sad_inloop(&yuv,
		                                         quality,
		                                         &y_modes,
		                                         &y_modes_count,
		                                         &uv_modes,
		                                         &uv_modes_count,
		                                         &coeffs,
		                                         &coeffs_count,
		                                         &qindex);
	} else if (mode == ENC_MODE_BPRED_RDO) {
		EncBpredRdoTuning tuning;
		tuning.lambda_mul = (uint32_t)bpred_rdo_lambda_mul;
		tuning.lambda_div = (uint32_t)bpred_rdo_lambda_div;
		tuning.rate_mode = (uint32_t)bpred_rdo_rate_mode;
		tuning.signal_mode = (uint32_t)bpred_rdo_signal_mode;
		tuning.quant_mode = (uint32_t)bpred_rdo_quant_mode;
		tuning.ac_deadzone_pct = (uint32_t)bpred_rdo_ac_deadzone_pct;
		tuning.qscale_y_dc_pct = (uint32_t)bpred_rdo_qscale_y_dc_pct;
		tuning.qscale_y_ac_pct = (uint32_t)bpred_rdo_qscale_y_ac_pct;
		tuning.qscale_uv_dc_pct = (uint32_t)bpred_rdo_qscale_uv_dc_pct;
		tuning.qscale_uv_ac_pct = (uint32_t)bpred_rdo_qscale_uv_ac_pct;
		tuning.satd_prune_k = (uint32_t)bpred_rdo_satd_prune_k;
		rc = enc_vp8_encode_bpred_uv_rdo_inloop(&yuv,
		                                       quality,
						       token_probs_mode,
		                                       &y_modes,
		                                       &y_modes_count,
		                                       &b_modes,
		                                       &b_modes_count,
		                                       &uv_modes,
		                                       &uv_modes_count,
		                                       &coeffs,
		                                       &coeffs_count,
									   &qindex,
									   &tuning);
	} else {
		rc = enc_vp8_encode_bpred_uv_sad_inloop(&yuv,
		                                       quality,
		                                       &y_modes,
		                                       &y_modes_count,
		                                       &b_modes,
		                                       &b_modes_count,
		                                       &uv_modes,
		                                       &uv_modes_count,
		                                       &coeffs,
		                                       &coeffs_count,
		                                       &qindex);
	}
	if (rc != 0) {
		fprintf(stderr, "%s: VP8 analysis/quant/recon failed (errno=%d)\n", in_path, errno);
		free(coeffs);
		free(uv_modes);
		free(b_modes);
		free(y_modes);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	uint8_t* vp8 = NULL;
	size_t vp8_size = 0;
	if (enable_loopfilter) {
		EncVp8LoopFilterParams lf;
		enc_vp8_loopfilter_from_qindex(qindex, &lf);
		if (mode == ENC_MODE_DC) {
			rc = enc_vp8_build_keyframe_dc_coeffs_ex(img.width,
			                                     img.height,
			                                     qindex,
			                                     0,
			                                     0,
			                                     0,
			                                     0,
			                                     0,
			                                     &lf,
			                                     coeffs,
			                                     coeffs_count,
			                                     &vp8,
			                                     &vp8_size);
		} else if (mode == ENC_MODE_I16) {
			rc = enc_vp8_build_keyframe_i16_coeffs_ex(img.width,
			                                      img.height,
			                                      qindex,
			                                      0,
			                                      0,
			                                      0,
			                                      0,
			                                      0,
			                                      y_modes,
			                                      uv_modes,
			                                      &lf,
			                                      coeffs,
			                                      coeffs_count,
			                                      &vp8,
			                                      &vp8_size);
		} else {
			if (token_probs_mode == ENC_VP8_TOKEN_PROBS_DEFAULT) {
				rc = enc_vp8_build_keyframe_intra_coeffs_ex(img.width,
											img.height,
											qindex,
											0,
											0,
											0,
											0,
											0,
										enable_mb_skip,
											y_modes,
											uv_modes,
											b_modes,
											&lf,
											coeffs,
											coeffs_count,
											&vp8,
											&vp8_size);
			} else {
				rc = enc_vp8_build_keyframe_intra_coeffs_ex_probs(img.width,
												img.height,
												qindex,
												0,
												0,
												0,
												0,
												0,
											enable_mb_skip,
												y_modes,
												uv_modes,
												b_modes,
												&lf,
												token_probs_mode,
												coeffs,
												coeffs_count,
												&vp8,
												&vp8_size);
			}
		}
	} else {
		if (mode == ENC_MODE_DC) {
			rc = enc_vp8_build_keyframe_dc_coeffs(img.width,
			                                  img.height,
			                                  qindex,
			                                  0,
			                                  0,
			                                  0,
			                                  0,
			                                  0,
			                                  coeffs,
			                                  coeffs_count,
			                                  &vp8,
			                                  &vp8_size);
		} else if (mode == ENC_MODE_I16) {
			rc = enc_vp8_build_keyframe_i16_coeffs(img.width,
			                                   img.height,
			                                   qindex,
			                                   0,
			                                   0,
			                                   0,
			                                   0,
			                                   0,
			                                   y_modes,
			                                   uv_modes,
			                                   coeffs,
			                                   coeffs_count,
			                                   &vp8,
			                                   &vp8_size);
		} else {
			if (token_probs_mode == ENC_VP8_TOKEN_PROBS_DEFAULT) {
				rc = enc_vp8_build_keyframe_intra_coeffs_ex(img.width,
										img.height,
										qindex,
										0,
										0,
										0,
										0,
										0,
										enable_mb_skip,
										y_modes,
										uv_modes,
										b_modes,
										/*lf=*/NULL,
										coeffs,
										coeffs_count,
										&vp8,
										&vp8_size);
			} else {
				rc = enc_vp8_build_keyframe_intra_coeffs_ex_probs(img.width,
												img.height,
												qindex,
												0,
												0,
												0,
												0,
												0,
											enable_mb_skip,
												y_modes,
												uv_modes,
												b_modes,
												/*lf=*/NULL,
												token_probs_mode,
												coeffs,
												coeffs_count,
												&vp8,
												&vp8_size);
			}
		}
	}

	if (rc != 0 || !vp8 || vp8_size == 0) {
		fprintf(stderr, "%s: VP8 bitstream build failed (errno=%d)\n", in_path, errno);
		free(vp8);
		free(coeffs);
		free(uv_modes);
		free(b_modes);
		free(y_modes);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	if (enc_webp_write_vp8_file(out_path, vp8, vp8_size) != 0) {
		fprintf(stderr, "%s: enc_webp_write_vp8_file failed (errno=%d)\n", out_path, errno);
		free(vp8);
		free(coeffs);
		free(uv_modes);
		free(b_modes);
		free(y_modes);
		enc_yuv420_free(&yuv);
		enc_png_free(&img);
		return 1;
	}

	free(vp8);
	free(coeffs);
	free(uv_modes);
	free(b_modes);
	free(y_modes);
	enc_yuv420_free(&yuv);
	enc_png_free(&img);
	return 0;
}
