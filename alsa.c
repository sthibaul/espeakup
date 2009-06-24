/*
 *  espeakup - interface which allows speakup to use espeak
 *
 *  Copyright (C) 2008 William Hubbs
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * File: alsa.c
 * Description: Produce audio by calling the ALSA library directly.
*/

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#define ALSA_PCM_NEW_HW_PARAMS_API
#include <alsa/asoundlib.h>
#include "espeakup.h"

static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static snd_pcm_t *handle;
static snd_pcm_hw_params_t *params;
snd_pcm_status_t *status;
static int dir = 0;

void lock_audio_mutex(void)
{
	pthread_mutex_lock(&audio_mutex);
}

void unlock_audio_mutex(void)
{
	pthread_mutex_unlock(&audio_mutex);
}

int sound_error(int err, const char *msg)
{
	fprintf(stderr, "%s: %s\n", msg, snd_strerror(err));
	return err;
}

int minimum(int x, int y)
{
	if (x <= y)
		return x;
	else
		return y;
}

static int alsa_play_callback(short *audio, int numsamples,
							  espeak_EVENT * events)
{
	int samples_written = 0;
	int avail;
	int to_write;
	snd_pcm_state_t state;

	lock_audio_mutex();
	if (stopped) {
		snd_pcm_drop(handle);
		stopped = 0;
		unlock_audio_mutex();
		return 1;
	}
	unlock_audio_mutex();

	snd_pcm_status(handle, status);
	state = snd_pcm_status_get_state(status);
	if (state != SND_PCM_STATE_RUNNING)
		snd_pcm_prepare(handle);

	while (numsamples > 0) {
		avail = snd_pcm_avail_update(handle);
		if (avail <= 0)
			continue;
		avail = minimum(avail, 32 * 2);
		to_write = minimum(avail, numsamples);
		samples_written = snd_pcm_writei(handle, audio, to_write);
		if (samples_written < 0) {
			snd_pcm_prepare(handle);
		} else {
			numsamples -= samples_written;
			audio += samples_written;
		}
	}
	return 0;
}

void select_audio_mode(void)
{
	audio_mode = AUDIO_OUTPUT_RETRIEVAL;
}

int init_audio(unsigned int rate)
{
	int rc;


	/* Open PCM device for playback. */
	rc = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0)
		return sound_error(rc, "unable to open pcm device");

	/* Allocate a hardware parameters object. */
	rc = snd_pcm_hw_params_malloc(&params);
	if (rc < 0)
		return sound_error(rc,
						   "Unable to allocate memory to store audio parameters");

	rc = snd_pcm_status_malloc(&status);
	if (rc < 0)
		return sound_error(rc,
						   "Unable to allocate memory to store PCM status");

	/* Fill it in with default values. */
	rc = snd_pcm_hw_params_any(handle, params);

	if (rc < 0)
		return sound_error(rc,
						   "Unable to establish defaults for hardware parameters.");

	/* Set the desired hardware parameters. */

	/* Interleaved mode */
	rc = snd_pcm_hw_params_set_access(handle, params,
									  SND_PCM_ACCESS_RW_INTERLEAVED);

	if (rc < 0)
		return sound_error(rc, "Error selecting interleaved mode.");

	/* Signed 16-bit little-endian format */
	rc = snd_pcm_hw_params_set_format(handle, params,
									  SND_PCM_FORMAT_S16_LE);
	if (rc < 0)
		return sound_error(rc, "Unable to select signed 16-bit samples");

	/* One channel  */
	rc = snd_pcm_hw_params_set_channels(handle, params, 1);

	if (rc < 0)
		return sound_error(rc, "Unable to use mono output.");

	rc = snd_pcm_hw_params_set_rate_near(handle, params, &rate, &dir);
	if (rc < 0)
		return sound_error(rc, "Unable to set sample rate");

	/* Write the parameters to the driver */
	rc = snd_pcm_hw_params(handle, params);
	if (rc < 0)
		return sound_error(rc, "unable to set hw parameters");

	espeak_SetSynthCallback(alsa_play_callback);
	return 0;
}