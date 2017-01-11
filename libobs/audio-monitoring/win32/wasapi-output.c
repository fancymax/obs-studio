#include "../../media-io/audio-resampler.h"
#include "../../util/circlebuf.h"
#include "../../util/platform.h"
#include "../../util/darray.h"
#include "../../obs-internal.h"

#include "wasapi-output.h"

#define ACTUALLY_DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        EXTERN_C const GUID DECLSPEC_SELECTANY name \
                = { l, w1, w2, { b1, b2,  b3,  b4,  b5,  b6,  b7,  b8 } }

ACTUALLY_DEFINE_GUID(CLSID_MMDeviceEnumerator,
	0xBCDE0395, 0xE52F, 0x467C,
	0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
ACTUALLY_DEFINE_GUID(IID_IMMDeviceEnumerator,
	0xA95664D2, 0x9614, 0x4F35,
	0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
ACTUALLY_DEFINE_GUID(IID_IAudioClient,
	0x1CB9AD4C, 0xDBFA, 0x4C32,
	0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
ACTUALLY_DEFINE_GUID(IID_IAudioRenderClient,
	0xF294ACFC, 0x3146, 0x4483,
	0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

// #define ENABLE_CORRECTION

struct audio_monitor {
	obs_source_t       *source;
	IMMDevice          *device;
	IAudioClient       *client;
	IAudioRenderClient *render;

	audio_resampler_t  *resampler;
	uint32_t           sample_rate;
	uint32_t           channels;
#ifdef ENABLE_CORRECTION
	uint32_t           peak_frames;
	uint32_t           correction_frames;
	uint32_t           cur_correction_frames;
	bool               correcting : 1;
#endif
	bool               source_has_video : 1;

	int64_t            lowest_audio_offset;
	struct circlebuf   delay_buffer;
	uint32_t           delay_size;

	DARRAY(float)      buf;
	pthread_mutex_t    playback_mutex;
};

static bool process_audio_delay(struct audio_monitor *monitor,
		float **data, uint32_t *frames, uint64_t ts, uint32_t pad)
{
	obs_source_t *s = monitor->source;
	uint64_t last_frame_ts = s->last_frame_ts;
	uint64_t front_ts;
	uint64_t cur_ts;
	int64_t diff;
	uint32_t blocksize = monitor->channels * sizeof(float);

	circlebuf_push_back(&monitor->delay_buffer, &ts, sizeof(ts));
	circlebuf_push_back(&monitor->delay_buffer, frames, sizeof(*frames));
	circlebuf_push_back(&monitor->delay_buffer, *data,
			*frames * blocksize);

	while (monitor->delay_buffer.size != 0) {
		size_t size;

		circlebuf_peek_front(&monitor->delay_buffer, &cur_ts, sizeof(ts));
		front_ts = cur_ts - ((uint64_t)pad * 1000000000ULL / (uint64_t)monitor->sample_rate);
		diff = (int64_t)front_ts - (int64_t)last_frame_ts;

		if (diff > 75000000) {
			return false;
		}

		circlebuf_pop_front(&monitor->delay_buffer, NULL, sizeof(ts));
		circlebuf_pop_front(&monitor->delay_buffer, frames,
				sizeof(*frames));

		size = *frames * blocksize;
		da_resize(monitor->buf, size);
		circlebuf_pop_front(&monitor->delay_buffer, monitor->buf.array,
				size);

		//blog(LOG_DEBUG, "diff: %lld, playback ts: %llu, last_frame_ts: %llu", diff, front_ts, last_frame_ts);

		if (diff < -75000000) {
			blog(LOG_DEBUG, "cutting off audio: %lld", diff);
			continue;
		}

		*data = monitor->buf.array;
		return true;
	}

	return false;
}

static void on_audio_playback(void *param, obs_source_t *source,
		const struct audio_data *audio_data, bool muted)
{
	struct audio_monitor *monitor = param;
	IAudioRenderClient *render = monitor->render;
	uint8_t *resample_data[MAX_AV_PLANES];
	float vol = source->user_volume;
	uint32_t resample_frames;
	uint64_t ts_offset;
	bool success;
	BYTE *output;

	if (pthread_mutex_trylock(&monitor->playback_mutex) != 0) {
		return;
	}
	if (os_atomic_load_long(&source->activate_refs) == 0) {
		goto unlock;
	}

	success = audio_resampler_resample(monitor->resampler, resample_data,
			&resample_frames, &ts_offset,
			(const uint8_t *const *)audio_data->data,
			(uint32_t)audio_data->frames);
	if (!success) {
		blog(LOG_DEBUG, "wtf 1");
		goto unlock;
	}

	UINT32 pad = 0;
	monitor->client->lpVtbl->GetCurrentPadding(monitor->client, &pad);

	if (monitor->source_has_video) {
		//blog(LOG_DEBUG, "audio_data->ts: %llu", audio_data->timestamp);
		uint64_t ts = audio_data->timestamp - ts_offset;
		if (!process_audio_delay(monitor, (float**)(&resample_data[0]),
					&resample_frames, ts, pad)) {
			blog(LOG_DEBUG, "wtf 2");
			goto unlock;
		}
	}

#ifdef ENABLE_CORRECTION
	if (monitor->peak_frames < resample_frames * 2) {
		monitor->peak_frames = resample_frames * 2;
	}

	monitor->cur_correction_frames += resample_frames;
	if (monitor->cur_correction_frames >= monitor->correction_frames) {
		monitor->correcting = true;
		monitor->cur_correction_frames = 0;
	}

	if (monitor->correcting && pad > monitor->peak_frames) {
		blog(LOG_DEBUG, "wtf 3: pad %ld, peak: %ld, frame size: %ld",
				pad,
				monitor->peak_frames,
				resample_frames);
		goto unlock;
	} else {
		monitor->correcting = false;
	}
#endif

	HRESULT hr = render->lpVtbl->GetBuffer(render, resample_frames,
			&output);
	if (FAILED(hr)) {
		blog(LOG_DEBUG, "wtf 4");
		goto unlock;
	}

	if (!muted) {
		/* apply volume */
		if (!close_float(vol, 1.0f, EPSILON)) {
			register float *cur = (float*)resample_data[0];
			register float *end = cur +
				resample_frames * monitor->channels;

			while (cur < end)
				*(cur++) *= vol;
		}
		memcpy(output, resample_data[0],
				resample_frames * monitor->channels *
				sizeof(float));
	}

	render->lpVtbl->ReleaseBuffer(render, resample_frames,
			muted ? AUDCLNT_BUFFERFLAGS_SILENT : 0);

unlock:
	pthread_mutex_unlock(&monitor->playback_mutex);
}

static inline void audio_monitor_free(struct audio_monitor *monitor)
{
	if (monitor->source) {
		obs_source_remove_audio_capture_callback(
				monitor->source, on_audio_playback, monitor);
	}

	if (monitor->client)
		monitor->client->lpVtbl->Stop(monitor->client);

	safe_release(monitor->device);
	safe_release(monitor->client);
	safe_release(monitor->render);
	audio_resampler_destroy(monitor->resampler);
	circlebuf_free(&monitor->delay_buffer);
	da_free(monitor->buf);
}

static enum speaker_layout convert_speaker_layout(DWORD layout, WORD channels)
{
	switch (layout) {
	case KSAUDIO_SPEAKER_QUAD:             return SPEAKERS_QUAD;
	case KSAUDIO_SPEAKER_2POINT1:          return SPEAKERS_2POINT1;
	case KSAUDIO_SPEAKER_4POINT1:          return SPEAKERS_4POINT1;
	case KSAUDIO_SPEAKER_SURROUND:         return SPEAKERS_SURROUND;
	case KSAUDIO_SPEAKER_5POINT1:          return SPEAKERS_5POINT1;
	case KSAUDIO_SPEAKER_5POINT1_SURROUND: return SPEAKERS_5POINT1_SURROUND;
	case KSAUDIO_SPEAKER_7POINT1:          return SPEAKERS_7POINT1;
	case KSAUDIO_SPEAKER_7POINT1_SURROUND: return SPEAKERS_7POINT1_SURROUND;
	}

	return (enum speaker_layout)channels;
}

static bool audio_monitor_init(struct audio_monitor *monitor)
{
	IMMDeviceEnumerator *immde = NULL;
	WAVEFORMATEX *wfex = NULL;
	bool success = false;
	UINT32 frames;
	HRESULT hr;

	const char *id = obs->audio.monitoring_device_id;
	if (!id) {
		return false;
	}

	pthread_mutex_init_value(&monitor->playback_mutex);

	/* ------------------------------------------ *
	 * Init device                                */

	hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
			&IID_IMMDeviceEnumerator, (void**)&immde);
	if (FAILED(hr)) {
		return false;
	}

	if (strcmp(id, "default") == 0) {
		hr = immde->lpVtbl->GetDefaultAudioEndpoint(immde,
				eRender, eConsole, &monitor->device);
	} else {
		wchar_t w_id[512];
		os_utf8_to_wcs(id, 0, w_id, 512);

		hr = immde->lpVtbl->GetDevice(immde, w_id, &monitor->device);
	}

	if (FAILED(hr)) {
		goto fail;
	}

	/* ------------------------------------------ *
	 * Init client                                */

	hr = monitor->device->lpVtbl->Activate(monitor->device,
			&IID_IAudioClient, CLSCTX_ALL, NULL,
			(void**)&monitor->client);
	if (FAILED(hr)) {
		goto fail;
	}

	hr = monitor->client->lpVtbl->GetMixFormat(monitor->client, &wfex);
	if (FAILED(hr)) {
		goto fail;
	}

	hr = monitor->client->lpVtbl->Initialize(monitor->client,
			AUDCLNT_SHAREMODE_SHARED, 0,
			10000000, 0, wfex, NULL);
	if (FAILED(hr)) {
		goto fail;
	}

	/* ------------------------------------------ *
	 * Init resampler                             */

	const struct audio_output_info *info = audio_output_get_info(
			obs->audio.audio);
	WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE*)wfex;
	struct resample_info from;
	struct resample_info to;

	from.samples_per_sec = info->samples_per_sec;
	from.speakers = info->speakers;
	from.format = AUDIO_FORMAT_FLOAT_PLANAR;

	to.samples_per_sec = (uint32_t)wfex->nSamplesPerSec;
	to.speakers = convert_speaker_layout(ext->dwChannelMask,
			wfex->nChannels);
	to.format = AUDIO_FORMAT_FLOAT;

	monitor->sample_rate = (uint32_t)wfex->nSamplesPerSec;
#ifdef ENABLE_CORRECTION
	monitor->peak_frames = monitor->sample_rate * 75 / 1000;
	monitor->correction_frames = monitor->sample_rate * 5;
#endif
	monitor->channels = wfex->nChannels;
	monitor->resampler = audio_resampler_create(&to, &from);
	if (!monitor->resampler) {
		goto fail;
	}

	/* ------------------------------------------ *
	 * Init client                                */

	hr = monitor->client->lpVtbl->GetBufferSize(monitor->client, &frames);
	if (FAILED(hr)) {
		goto fail;
	}

	hr = monitor->client->lpVtbl->GetService(monitor->client,
			&IID_IAudioRenderClient, (void**)&monitor->render);
	if (FAILED(hr)) {
		goto fail;
	}

	if (pthread_mutex_init(&monitor->playback_mutex, NULL) != 0) {
		goto fail;
	}

	hr = monitor->client->lpVtbl->Start(monitor->client);
	if (FAILED(hr)) {
		goto fail;
	}

	success = true;

fail:
	safe_release(immde);
	if (wfex)
		CoTaskMemFree(wfex);
	return success;
}

static void audio_monitor_init_final(struct audio_monitor *monitor,
		obs_source_t *source)
{
	monitor->source = source;
	monitor->source_has_video =
		(source->info.output_flags & OBS_SOURCE_VIDEO) != 0;
	obs_source_add_audio_capture_callback(source, on_audio_playback,
			monitor);
}

struct audio_monitor *audio_monitor_create(obs_source_t *source)
{
	struct audio_monitor monitor = {0};
	struct audio_monitor *out;

	if (!audio_monitor_init(&monitor)) {
		goto fail;
	}

	out = bmemdup(&monitor, sizeof(monitor));

	pthread_mutex_lock(&obs->audio.monitoring_mutex);
	da_push_back(obs->audio.monitors, &out);
	pthread_mutex_unlock(&obs->audio.monitoring_mutex);

	audio_monitor_init_final(out, source);
	return out;

fail:
	audio_monitor_free(&monitor);
	return NULL;
}

void audio_monitor_reset(struct audio_monitor *monitor)
{
	struct audio_monitor new_monitor = {0};
	bool success;

	pthread_mutex_lock(&monitor->playback_mutex);
	success = audio_monitor_init(&new_monitor);
	pthread_mutex_unlock(&monitor->playback_mutex);

	if (success) {
		obs_source_t *source = monitor->source;
		audio_monitor_free(monitor);
		*monitor = new_monitor;
		audio_monitor_init_final(monitor, source);
	} else {
		audio_monitor_free(&new_monitor);
	}
}

void audio_monitor_destroy(struct audio_monitor *monitor)
{
	if (monitor) {
		audio_monitor_free(monitor);

		pthread_mutex_lock(&obs->audio.monitoring_mutex);
		da_erase_item(obs->audio.monitors, &monitor);
		pthread_mutex_unlock(&obs->audio.monitoring_mutex);

		bfree(monitor);
	}
}
