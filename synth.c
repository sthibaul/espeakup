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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espeakup.h"

/* default voice settings */
const int defaultFrequency = 5;
const int defaultPitch = 5;
const int defaultRate = 5;
const int defaultVolume = 5;
char *defaultVoice = NULL;

/* multipliers and offsets */
const int frequencyMultiplier = 11;
const int pitchMultiplier = 11;
const int rateMultiplier = 34;
const int rateOffset = 84;
const int volumeMultiplier = 22;

volatile int runner_must_stop = 0;

static espeak_ERROR set_frequency(struct synth_t *s, int freq, enum adjust_t adj)
{
	espeak_ERROR rc;

	if (adj == ADJ_DEC)
		freq = -freq;
	if (adj != ADJ_SET)
		freq += s->frequency;
	rc = espeak_SetParameter(espeakRANGE, freq * frequencyMultiplier, 0);
	if (rc == EE_OK)
		s->frequency = freq;
	return rc;
}

static espeak_ERROR set_pitch(struct synth_t * s, int pitch, enum adjust_t adj)
{
	espeak_ERROR rc;

	if (adj == ADJ_DEC)
		pitch = -pitch;
	if (adj != ADJ_SET)
		pitch += s->pitch;
	rc = espeak_SetParameter(espeakPITCH, pitch * pitchMultiplier, 0);
	if (rc == EE_OK)
		s->pitch = pitch;
	return rc;
}

static espeak_ERROR set_punctuation(struct synth_t * s, int punct,
							 enum adjust_t adj)
{
	espeak_ERROR rc;

	if (adj == ADJ_DEC)
		punct = -punct;
	if (adj != ADJ_SET)
		punct += s->punct;
	rc = espeak_SetParameter(espeakPUNCTUATION, punct, 0);
	if (rc == EE_OK)
		s->punct = punct;
	return rc;
}

static espeak_ERROR set_rate(struct synth_t * s, int rate, enum adjust_t adj)
{
	espeak_ERROR rc;

	if (adj == ADJ_DEC)
		rate = -rate;
	if (adj != ADJ_SET)
		rate += s->rate;
	rc = espeak_SetParameter(espeakRATE,
							 rate * rateMultiplier + rateOffset, 0);
	if (rc == EE_OK)
		s->rate = rate;
	return rc;
}

static espeak_ERROR set_voice(struct synth_t * s, char *voice)
{
	espeak_ERROR rc;

	rc = espeak_SetVoiceByName(voice);
	if (rc == EE_OK)
		strcpy(s->voice, voice);
	return rc;
}

static espeak_ERROR set_volume(struct synth_t * s, int vol, enum adjust_t adj)
{
	espeak_ERROR rc;

	if (adj == ADJ_DEC)
		vol = -vol;
	if (adj != ADJ_SET)
		vol += s->volume;
	rc = espeak_SetParameter(espeakVOLUME, (vol + 1) * volumeMultiplier,
							 0);
	if (rc == EE_OK)
		s->volume = vol;

	return rc;
}

static espeak_ERROR stop_speech(void)
{
	lock_audio_mutex();
	stopped = 1;
	unlock_audio_mutex();
	return (espeak_Cancel());
}

static espeak_ERROR speak_text(struct synth_t * s)
{
	espeak_ERROR rc;

	lock_audio_mutex();
	stopped = 0;
	unlock_audio_mutex();
	rc = espeak_Synth(s->buf, s->len + 1, 0, POS_CHARACTER, 0, 0, NULL,
					  NULL);
	return rc;
}

static void queue_process_entry(struct synth_t *s)
{
	espeak_ERROR error;
	struct espeak_entry_t *current;

	current = (struct espeak_entry_t *) queue_peek();
	pthread_mutex_unlock(&queue_guard);
	if (current) {
		switch (current->cmd) {
		case CMD_SET_FREQUENCY:
			error = set_frequency(s, current->value, current->adjust);
			break;
		case CMD_SET_PITCH:
			error = set_pitch(s, current->value, current->adjust);
			break;
		case CMD_SET_PUNCTUATION:
			error = set_punctuation(s, current->value, current->adjust);
			break;
		case CMD_SET_RATE:
			error = set_rate(s, current->value, current->adjust);
			break;
		case CMD_SET_VOICE:
			error = EE_OK;
			break;
		case CMD_SET_VOLUME:
			error = set_volume(s, current->value, current->adjust);
			break;
		case CMD_SPEAK_TEXT:
			s->buf = current->buf;
			s->len = current->len;
			error = speak_text(s);
			break;
		default:
			break;
		}

		if (error == EE_OK) {
			pthread_mutex_lock(&queue_guard);
			queue_remove();
			pthread_mutex_unlock(&queue_guard);
		}
	}
}

static void free_entry(struct espeak_entry_t *entry)
{
	if (entry->cmd == CMD_SPEAK_TEXT)
		free(entry->buf);
	free(entry);
}

static void queue_clear()
{
	struct espeak_entry_t *current;

	current = (struct espeak_entry_t *) queue_peek();
	while (current) {
		free_entry(current);
		queue_remove();
		current = (struct espeak_entry_t *) queue_peek();
	}
}

/* espeak_thread is the "main" function of our secondary (queue-processing)
 * thread.
 * First, lock queue_guard, because it needs to be locked when we call
 * pthread_cond_wait on the runner_awake condition variable.
 * Next, enter an infinite loop.
 * The wait call also unlocks queue_guard, so that the other thread can
 * manipulate the queue.
 * When runner_awake is signaled, the pthread_cond_wait call re-locks
 * queue_guard, and the "queue processor" thread has access to the queue.
 * While there is an entry in the queue, call queue_process_entry.
 * queue_process_entry unlocks queue_guard after removing an item from the
 * queue, so that the main thread doesn't have to wait for us to finish
 * processing the entry.  So re-lock queue_guard after each call to
 * queue_process_entry.
 *
 * The main thread can add items to the queue in exactly two situations:
 * 1. We are waiting on runner_awake, or
 * 2. We are processing an entry that has just been removed from the queue.
*/

void *espeak_thread(void *arg)
{
	struct synth_t *s = (struct synth_t *) arg;
	int rate;

	/* initialize espeak */
	select_audio_mode();
	rate = espeak_Initialize(audio_mode, 0, NULL, 0);
	if (rate < 0) {
		fprintf(stderr, "Unable to initialize espeak.\n");
		should_run = 0;
	}

	if (init_audio((unsigned int) rate) < 0) {
		should_run = 0;
	}

	/* Setup initial voice parameters */
	if (defaultVoice) {
		set_voice(s, defaultVoice);
		free(defaultVoice);
		defaultVoice = NULL;
	}
	set_frequency(s, defaultFrequency, ADJ_SET);
	set_pitch(s, defaultPitch, ADJ_SET);
	set_rate(s, defaultRate, ADJ_SET);
	set_volume(s, defaultVolume, ADJ_SET);
	espeak_SetParameter(espeakCAPITALS, 0, 0);

	pthread_mutex_lock(&queue_guard);
	while (should_run) {
		pthread_cond_wait(&runner_awake, &queue_guard);

		while (should_run && queue_peek() && !runner_must_stop) {
			queue_process_entry(s);
			pthread_mutex_lock(&queue_guard);
		}

		if (runner_must_stop) {
			queue_clear();
			stop_speech();
			runner_must_stop = 0;
			pthread_cond_signal(&stop_acknowledged);
		}
	}
	pthread_mutex_unlock(&queue_guard);
	espeak_Terminate();
	return NULL;
}
