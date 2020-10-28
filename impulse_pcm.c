/* alsaimpulse, a convolution filter plugin for ALSA.
 * Copyright (C) 2020 Romain "Artefact2" Dal Maso <romain.dalmaso@artefact2.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <complex.h>
#include <fftw3.h>

#include <alsa/asoundlib.h>
#include <alsa/error.h>
#include <alsa/pcm_external.h>

#define MAX_CHN 16

struct channel_context {
	float* impulse_data;
	int impulse_length, impulse_orig_length, rate;
	float orig_gain, gain;

	int N; /* FFT size, always a power of 2 */
	fftwf_complex* impulse_fft;
	fftwf_complex* pcm_fft;
	float* pcm_data;

	float* ring_buffer; /* for overlapping data */
	int ring_buffer_position;

	fftwf_plan pcm_to_fft;
	fftwf_plan fft_to_pcm;
};

struct plugin_context {
	snd_pcm_extplug_t ext;
	struct channel_context c[MAX_CHN];
};

static snd_pcm_sframes_t transfer_callback(snd_pcm_extplug_t* ext,
										   const snd_pcm_channel_area_t* dst_areas, snd_pcm_uframes_t dst_offset,
										   const snd_pcm_channel_area_t* src_areas, snd_pcm_uframes_t src_offset,
										   snd_pcm_uframes_t size) {
	struct plugin_context* pctx = ext->private_data;

	for(int i = 0; i < ext->channels; ++i) {
		struct channel_context* c = &(pctx->c[i]);

		if(c->impulse_orig_length == 0) {
			/* Pass through */
			snd_pcm_area_copy(&(dst_areas[i]), dst_offset, &(src_areas[i]), src_offset, size, SND_PCM_FORMAT_FLOAT);
			continue;
		}

		/* Get samples from ALSA */
		snd_pcm_channel_area_t a = {
			.addr = c->pcm_data,
			.first = 0,
			.step = sizeof(float) * 8,
		};
		snd_pcm_area_copy(&a, 0, &(src_areas[i]), src_offset, size, SND_PCM_FORMAT_FLOAT);
		memset(&(c->pcm_data[size]), 0, sizeof(float) * (c->N - size));

		/* Do the convolution */
		fftwf_execute(c->pcm_to_fft);
		for(int i = 0; i < c->N/2 + 1; ++i) {
			c->pcm_fft[i] *= c->impulse_fft[i]; /* Complex multiplication */
		}
		fftwf_execute(c->fft_to_pcm);

		/* Update overlaps */
		for(int i = 0; i < c->N; ++i) {
			c->ring_buffer[(c->ring_buffer_position + i) % (c->N)] += c->pcm_data[i] * c->gain;
		}

		/* Give samples to ALSA */
		a.addr = &(c->ring_buffer[c->ring_buffer_position]);
		int over = c->ring_buffer_position + size - c->N;

		if(over <= 0) {
			snd_pcm_area_copy(&(dst_areas[i]), dst_offset, &a, 0, size, SND_PCM_FORMAT_FLOAT);
			memset(a.addr, 0, sizeof(float) * size);
			c->ring_buffer_position += size;
		} else {
			snd_pcm_area_copy(&(dst_areas[i]), dst_offset, &a, 0, size - over, SND_PCM_FORMAT_FLOAT);
			memset(a.addr, 0, sizeof(float) * (size - over));

			a.addr = c->ring_buffer;
			snd_pcm_area_copy(&(dst_areas[i]), dst_offset + (size - over), &a, 0, over, SND_PCM_FORMAT_FLOAT);
			memset(a.addr, 0, sizeof(float) * over);

			c->ring_buffer_position += size;
			c->ring_buffer_position -= c->N;
		}
	}

	return size;
}

static int hw_params_callback(snd_pcm_extplug_t* ext, snd_pcm_hw_params_t* params) {
	struct plugin_context* pctx = ext->private_data;

	snd_pcm_uframes_t psize;
	int ret, dir;
	if((ret = snd_pcm_hw_params_get_period_size_max(params, &psize, &dir)) < 0) {
		return ret;
	}
	if(dir == 1) {
		SNDERR("could not query max period size");
		return -EINVAL;
	}

	for(int i = 0; i < MAX_CHN; ++i) {
		struct channel_context* c = &(pctx->c[i]);

		if(c->impulse_orig_length == 0) {
			continue;
		}

		if(c->rate != ext->rate) {
			SNDERR("resampling not implemented yet");
			return -EINVAL;
		}

		if(c->impulse_fft) {
			/* XXX: might be overkill to re-do everything */
			fftwf_free(c->impulse_fft);
			fftwf_free(c->pcm_fft);
			fftwf_free(c->pcm_data);
			fftwf_free(c->ring_buffer);
			fftwf_destroy_plan(c->pcm_to_fft);
			fftwf_destroy_plan(c->fft_to_pcm);
		}

		c->N = 1 << (int)ceilf(log2f(c->impulse_orig_length + psize + 1));
		c->gain = c->orig_gain / (c->N);
		c->impulse_fft = fftwf_alloc_complex(c->N/2 + 1);
		c->pcm_fft = fftwf_alloc_complex(c->N/2 + 1);
		c->pcm_data = fftwf_alloc_real(c->N);
		c->ring_buffer = fftwf_alloc_real(c->N);
		memset(c->ring_buffer, 0, c->N * sizeof(float));
		c->ring_buffer_position = 0;

		/* XXX: use better heuristics and cache wisdom */
		c->pcm_to_fft = fftwf_plan_dft_r2c_1d(c->N, c->pcm_data, c->pcm_fft, FFTW_ESTIMATE);
		c->fft_to_pcm = fftwf_plan_dft_c2r_1d(c->N, c->pcm_fft, c->pcm_data, FFTW_ESTIMATE);

		if(c->impulse_length < c->N) {
			/* Reallocate impulse_data to length N, pad zith zeroes */
			float* imp = fftwf_alloc_real(c->N);
			memcpy(imp, c->impulse_data, c->impulse_length * sizeof(float));
			memset(&(imp[c->impulse_length]), 0, sizeof(float) * (c->N - c->impulse_length));
			fftwf_free(c->impulse_data);
			c->impulse_data = imp;
			c->impulse_length = c->N;
		}

		fftwf_plan p = fftwf_plan_dft_r2c_1d(c->N, c->impulse_data, c->impulse_fft, FFTW_ESTIMATE);
		fftwf_execute(p);
		fftwf_destroy_plan(p);
	}

	return 0;
}

static snd_pcm_extplug_callback_t callbacks = {
    .transfer = transfer_callback,
	.hw_params = hw_params_callback,
};

static int copy_impulse_file(const char* path, float** out, int* out_len) {
	FILE* f = fopen(path, "rb");
	if(f == 0) {
		SYSERR("could not open impulse file");
		return -1;
	}
	if(fseek(f, 0, SEEK_END) < 0) {
		SYSERR("could not seek in impulse file");
		return -1;
	}
	*out_len = ftell(f) / sizeof(float);
	rewind(f);

	*out = fftwf_alloc_real(*out_len);
	fread(*out, sizeof(float) * (*out_len), 1, f); /* XXX: check for errors/allocation success */
	fclose(f);
	return 0;
}

/* _snd_pcm_impulse_open(snd_pcm_t** pcmp, const char* name, snd_config_t* root, snd_config_t* conf, snd_pcm_stream_t stream, int mode) { */
SND_PCM_PLUGIN_DEFINE_FUNC(impulse) {
	snd_config_iterator_t i, next;
	snd_config_t* slave = 0;
	snd_config_t* impulses = 0;
	int ret, k;

	/* Parse main plugin options */
	snd_config_for_each(i, next, conf) {
		snd_config_t* n = snd_config_iterator_entry(i);
		const char* id;

		if(snd_config_get_id(n, &id) < 0) continue;
		if(strcmp("type", id) == 0) continue;
		if(strcmp("comment", id) == 0) continue;
		if(strcmp("type", id) == 0) continue;
		if(strcmp("hint", id) == 0) continue;

		if(strcmp("slave", id) == 0) {
			slave = n;
			continue;
		}

		if(strcmp("impulse", id) == 0) {
			if(!snd_config_is_array(n)) {
				SNDERR("impulse must be of type array");
				return -EINVAL;
			}

			impulses = n;
			continue;
		}

		SNDERR("unknown config entry: %s", id);
		return -EINVAL;
	}

	if(slave == 0) {
		SNDERR("no slave config entry found");
		return -EINVAL;
	}

	struct plugin_context* pctx = calloc(1, sizeof(struct plugin_context));
	if(pctx == 0) {
		SNDERR("could not allocate plugin context");
		return -ENOMEM;
	}

	/* Parse impulse.0, impulse.1, etc. options and load impulse data */
	k = 0;
	snd_config_for_each(i, next, impulses) {
		snd_config_iterator_t j, next2;
		snd_config_t* impulse = 0;
		long rate = 0;
		double gain = 0.0; /* in dB for now */
		const char* ipath = "\0";

		impulse = snd_config_iterator_entry(i);
		snd_config_for_each(j, next2, impulse) {
			snd_config_t* m = snd_config_iterator_entry(j);
			const char* id;

			if(snd_config_get_id(m, &id) < 0) continue;

			if(strcmp("path", id) == 0) {
				snd_config_get_string(m, &ipath);
				continue;
			}

			if(strcmp("rate", id) == 0) {
				snd_config_get_integer(m, &rate);
				continue;
			}

			if(strcmp("gain", id) == 0) {
				snd_config_get_ireal(m, &gain);
				continue;
			}

			SNDERR("unknown impulse config entry: %s", id);
			ret = -EINVAL;
			goto abort;
		}

		if(k >= MAX_CHN) {
			SNDERR("too many impulses specified, maximum is %d channels", MAX_CHN);
			ret = -EINVAL;
			goto abort;
		}

		if(*ipath == 0) {
			/* No impulse, this means pass through this channel's samples */
			/* Leave everything as zeroes */
			++k;
			continue;
		}

		if(rate == 0) {
			SNDERR("impulse %s has no specified rate", ipath);
			ret = -EINVAL;
			goto abort;
		}

		struct channel_context* c = &(pctx->c[k]);
		if((ret = copy_impulse_file(ipath, &(c->impulse_data), &(c->impulse_orig_length))) < 0) {
			goto abort;
		}

		c->impulse_length = c->impulse_orig_length;
		c->gain = c->orig_gain = (float)pow(1.12201845430196343559, gain);
		c->rate = rate;
		++k;
	}

	pctx->ext.version = SND_PCM_EXTPLUG_VERSION;
	pctx->ext.name = "impulse";
	pctx->ext.callback = &callbacks;
	pctx->ext.private_data = pctx;

	if((ret = snd_pcm_extplug_create(&(pctx->ext), name, root, slave, stream, mode)) < 0) goto abort;
	*pcmp = pctx->ext.pcm;

	/* Force float samples */
	if((ret = snd_pcm_extplug_set_param(&(pctx->ext), SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT)) < 0) goto abort;
	if((ret = snd_pcm_extplug_set_slave_param(&(pctx->ext), SND_PCM_EXTPLUG_HW_FORMAT, SND_PCM_FORMAT_FLOAT)) < 0) goto abort;

	return 0;

abort:
	for(k = 0; k < MAX_CHN; ++k) {
		if(pctx->c[k].impulse_data) {
			fftwf_free(pctx->c[k].impulse_data);
		}
	}
	free(pctx);
	return ret;
}

SND_PCM_PLUGIN_SYMBOL(impulse);