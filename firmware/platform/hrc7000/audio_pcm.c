// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 PCM audio (audio_pcm.h): mic samples from the modem's PCM buffer, speaker samples to its prompt FIFO
// (docs/spec/hardware-dm32.md 9; modem-hrc7000.md 3, 8.1, 8.4).
//
// With AUDIO_CONTROL b4 set the modem fills 80 mic samples at 0x18000000 every 10 ms and raises PCM_wr (PIC 28). The
// mic interrupt paces capture: two halves make a 20 ms block. With capture off the task uses its own timer.
// The task runs the block callback and queues its speaker PCM. While routed (AUDIO_CONTROL b7, set by the board's hook)
// timer 6 pushes the queue to the prompt FIFO after the chip has had time to switch banks. The
// CPU-fed speaker buffer (b5, 0x180000A0) sounded rough and choppy on the bench with every sample layout tried.
//
// Sample format: mic buffer 16-bit native words; prompt FIFO 16-bit samples, low byte first (bench).
//
// The prompt FIFO also feeds the TX modulator (modem-hrc7000.md 8.4), so while the DMR engine transmits (the gate,
// hrc_audio_pcm_tx_gate, from the DMR glue) ring play stays clear and nothing is pushed. Blocks queued meanwhile are
// dropped when it ends: a beep still running plays on from where it is, not from where it was cut. The boards set
// ring play through hrc_audio_pcm_ring, which applies the gate.
//
// Mic level for VOX (CONFIG_VOX, hrc_audio_pcm_mic_level): these radios have no analog mic level tap, so the level is
// measured on the same mic PCM. The first call turns the PCM interrupt on without the stream (the meter), and the
// interrupt turns it off again once nobody has asked for METER_IDLE_BLOCKS halves. The stream and the meter share
// AUDIO_CONTROL b4 and the interrupt: each turns them off only when the other isn't using them.
#include <string.h>
#include "config.h"
#include "audio_pcm.h"

#if CONFIG_M17
#include "platform.h"
#include "dmr_glue.h"
#include "FreeRTOS.h"
#include "task.h"

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	1536	// words: the vocoder in the callback
#define HALF			C7_PCM_SAMPLES	// 80
#define SPK_BLOCKS		4	// three queued blocks and one reserved for the producer
#define SPK_PRIME_BLOCKS	2
#define SPK_POLL_US		2000u

static int16_t mic_blk[2][AUDIO_BLOCK_SAMPLES];	// double buffered: the ISR fills one while the task reads the other
static int16_t spk_q[SPK_BLOCKS][AUDIO_BLOCK_SAMPLES];
static volatile uint8_t spk_head, spk_tail;	// task writes head, ISR reads tail
static uint8_t spk_pos;			// samples of spk_q[spk_tail] already pushed
static volatile uint8_t mic_fill, mic_half;
static volatile int8_t mic_ready = -1;		// block index ready for the task

static hrc7000_t *modem;
static void (*speaker_pcm_cb)(bool on);
static TaskHandle_t task;
static audio_block_cb_t block_cb;
static void *block_ctx;
static volatile bool running;
static volatile bool mic_capture_on;
static TickType_t playback_due;
static volatile bool spk_routed;	// the last speaker_pcm() state
static bool spk_open_pending;	// open the amp on the first FIFO write
static bool spk_primed;
static bool fifo_settling;
static bool spk_sent;
static uint32_t fifo_ready_at, fifo_poll_at;
static uint32_t play_diag[16];
static volatile bool fifo_held;	// another user (the board's AFSK) owns the FIFO: speaker PCM waits
static volatile uint32_t blocks, overruns, underruns;	// underruns: the FIFO does not report them (stays 0)
static bool spk_loud[SPK_BLOCKS];	// the queued block has a sample other than 0 (a beep, voice)
static volatile uint32_t loud_pushed;	// hrc_micros() when the last chunk of a loud block went to the FIFO
static volatile bool loud_recent;	// loud_pushed is set and less than FIFO_PLAY_MS old when last looked at
static volatile uint8_t spk_gen;	// bumped by spk_drop: a block the task filled meanwhile is not queued
static volatile bool tx_gated;		// the DMR engine transmits: no ring play, no pushes
static bool ring_want;			// the board's ring play (hrc_audio_pcm_ring), applied when not gated
static uint32_t gate_at;		// hrc_micros() at the last close
// Gate closings; of those, with loud audio still queued or playing; checks that found ring play on while gated
static volatile uint32_t gates, loud_at_gate, ring_on_air;
static bool isr_registered;


static volatile int16_t *const pcm_mic = (volatile int16_t *)C7_PCM_MIC_ADDR;

static void isr_pcm_wr(void)
{
	hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_PCM_WR);
	if (!running || !mic_capture_on) {
		return;
	}
	int16_t *dst = &mic_blk[mic_fill][mic_half * HALF];
	for (uint32_t i = 0; i < HALF; i++) {
		dst[i] = pcm_mic[i];
	}
	if (++mic_half < 2) {
		return;
	}
	mic_half = 0;
	if (mic_ready >= 0) {
		overruns++;	// the task hasn't taken the previous block: it is overwritten
	}
	mic_ready = (int8_t)mic_fill;
	mic_fill ^= 1u;
	if (task != NULL) {
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(task, &woken);
		portYIELD_FROM_ISR(woken);
	}
}

static void gate_check(void);

static void playback_stats(uint32_t out[16])
{
	uint32_t irq = hrc_irq_save();
	unsigned count = (spk_head + SPK_BLOCKS - spk_tail) % SPK_BLOCKS;
	play_diag[10] = count * AUDIO_BLOCK_SAMPLES - (count ? spk_pos : 0);
	play_diag[13] = (uint32_t)running | ((uint32_t)spk_routed << 1) |
	                ((uint32_t)fifo_held << 2) | ((uint32_t)tx_gated << 3);
	play_diag[14] = hrc7000_rd(modem, C7_AUDIO_CONTROL);
	play_diag[15] = blocks;
	memcpy(out, play_diag, sizeof play_diag);
	hrc_irq_restore(irq);
}

static void speaker_open(void)
{
	if (spk_open_pending) {
		spk_open_pending = false;
		if (speaker_pcm_cb != NULL)
			speaker_pcm_cb(true);
	}
}

// RadioFoundry PR #660: check for space independently of decoding, then defer
// the write until the modem's bank switch has settled. Neither IRQ busy-waits.
static uint32_t fifo_feed(void)
{
	if (!spk_routed || fifo_held || tx_gated || spk_tail == spk_head) {
		fifo_settling = false;
		return SPK_POLL_US;
	}

	if (!spk_primed) {
		if ((spk_head + SPK_BLOCKS - spk_tail) % SPK_BLOCKS < SPK_PRIME_BLOCKS)
			return SPK_POLL_US;
		spk_primed = true;
	}

	if (!fifo_settling) {
		if (hrc7000_rd(modem, C7_AUDIO_CONTROL) & C7_AUDIO_RING_FULL) {
			// Leftover FIFO samples can only drain once the route opens.
			speaker_open();
			play_diag[3]++;
			return SPK_POLL_US;
		}
		uint32_t now = hrc_micros();
		fifo_ready_at = now + C7_PROMPT_SETTLE_US;
		fifo_poll_at = now + SPK_POLL_US;
		fifo_settling = true;
		return C7_PROMPT_SETTLE_US;
	}

	int32_t wait = (int32_t)(fifo_ready_at - hrc_micros());
	if (wait > 0)
		return (uint32_t)wait;
	fifo_settling = false;

	// Queue resets, route changes and FIFO ownership changes cancel the
	// deferred write under the IRQ lock. The raw push also rechecks room.
	if (!hrc7000_prompt_push(modem, &spk_q[spk_tail][spk_pos])) {
		play_diag[3]++;
		return SPK_POLL_US;
	}
	speaker_open();
	uint32_t now = hrc_micros();
	if (spk_sent) {
		uint32_t gap = now - play_diag[12];
		if (gap < play_diag[5])
			play_diag[5] = gap;
		if (gap > play_diag[6])
			play_diag[6] = gap;
	}
	spk_sent = true;
	play_diag[12] = now;
	play_diag[2]++;
	if (spk_loud[spk_tail]) {
		loud_pushed = now;
		loud_recent = true;
	}
	spk_pos += C7_PROMPT_CHUNK;
	if (spk_pos >= AUDIO_BLOCK_SAMPLES) {
		spk_pos = 0;
		spk_tail = (uint8_t)((spk_tail + 1u) % SPK_BLOCKS);
	}

	wait = (int32_t)(fifo_poll_at - hrc_micros());
	return wait > 0 ? (uint32_t)wait : 1u;
}

static void fifo_tick(void)
{
	hrc_timer_stop(TIMER_PCM);
	if (running)
		hrc_timer_oneshot(TIMER_PCM, fifo_feed());
}

static void audio_task(void *arg)
{
	(void)arg;
	const TickType_t period = pdMS_TO_TICKS(20);
	static int16_t mic[AUDIO_BLOCK_SAMPLES];
	for (;;) {
		TickType_t wait = tx_gated ? pdMS_TO_TICKS(10) : portMAX_DELAY;
		if (running && !mic_capture_on) {
			int32_t until = (int32_t)(playback_due - xTaskGetTickCount());
			TickType_t ticks = until > 0 ? (TickType_t)until : 0;
			if (ticks < wait)
				wait = ticks;
		}
		ulTaskNotifyTake(pdTRUE, wait);
		gate_check();
		uint32_t s = hrc_irq_save();
		const int16_t *samples = NULL;
		bool ready = false;
		if (mic_capture_on && mic_ready >= 0) {
			memcpy(mic, mic_blk[mic_ready], sizeof mic);
			mic_ready = -1;
			samples = mic;
			ready = true;
		} else if (running && !mic_capture_on) {
			TickType_t now = xTaskGetTickCount();
			int32_t late = (int32_t)(now - playback_due);
			if (late >= 0) {
				/* Do not consume multiple voice blocks to catch up after a stall. */
				playback_due = late >= (int32_t)period ? now + period : playback_due + period;
				ready = true;
			}
		}
		hrc_irq_restore(s);
		audio_block_cb_t cb = block_cb;
		if (!ready || !running || cb == NULL) {
			continue;
		}
		uint8_t next = (uint8_t)((spk_head + 1u) % SPK_BLOCKS);
		if (samples == NULL && spk_routed && next == spk_tail) {
			// Playback-only M17 can leave the PCM in its source queue.
			play_diag[8]++;
			continue;
		}

		// The head slot stays reserved while the timer drains other slots.
		int16_t *q = spk_q[spk_head];
		uint8_t gen = spk_gen;
		cb(samples, q, block_ctx);
		bool loud = false;
		for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES && !loud; i++)
			loud = q[i] != 0;

		s = hrc_irq_save();
		if (gen == spk_gen && spk_routed && next != spk_tail) {
			spk_loud[spk_head] = loud;
			spk_head = next;
		}
		hrc_irq_restore(s);
		blocks++;
	}
}

void hrc_audio_pcm_attach(hrc7000_t *m, void (*speaker_pcm)(bool on))
{
	modem = m;
	speaker_pcm_cb = speaker_pcm;
	// The meter's interrupt changes AUDIO_CONTROL b4 (isr_pcm_wr): from now on every shadow update, the board's FM
	// receive, transmit, idle and DMR entry included, runs with interrupts off
	hrc7000_set_crit(m, hrc_irq_save, hrc_irq_restore);
}

void hrc_audio_pcm_hold(bool on)
{
	uint32_t irq = hrc_irq_save();
	if (fifo_held != on)
		fifo_settling = false;
	fifo_held = on;
	hrc_irq_restore(irq);
}

// Empty the software queue and cancel a pending write with interrupts off.
// The generation check also rejects a block the audio task was still filling.
static void spk_drop(void)
{
	spk_head = spk_tail = 0;
	spk_pos = 0;
	spk_primed = false;
	spk_sent = false;
	fifo_settling = false;
	spk_gen++;
}

static void ring_apply(void)
{
	bool on = ring_want && !tx_gated;
	hrc7000_audio_bits(modem, on ? C7_AUDIO_RING_PLAY : 0, on ? 0 : C7_AUDIO_RING_PLAY);
}

// The board's ring play changes (a beep, APRS AFSK): the gate may be due to open first. The audio task, which opens it
// otherwise, starts with the first stream (a beep, voice): with none since boot, a DMR data or hotspot transmission
// left the gate closed, and APRS's tones off the modulator
void hrc_audio_pcm_ring(hrc7000_t *m, bool want)
{
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	modem = m;
	ring_want = want;
	gate_check();
	ring_apply();
	hrc_irq_restore(s);
}

// Speaker audio queued or still in the FIFO: the FIFO has no empty flag, so a push is taken to play out within
// FIFO_PLAY_MS (an upper bound on its depth, a few 4 ms chunks)
#define FIFO_PLAY_MS	32u

bool hrc_audio_pcm_spk_pending(void)
{
	uint32_t s = hrc_irq_save();
	bool loud = false;
	for (uint8_t i = spk_tail; i != spk_head && !loud; i = (uint8_t)((i + 1u) % SPK_BLOCKS)) {
		loud = spk_loud[i];
	}
	if (loud_recent && hrc_micros() - loud_pushed >= FIFO_PLAY_MS * 1000u) {
		loud_recent = false;	// played out: an old time can't look recent again when the counter wraps
	}
	loud = loud || loud_recent;
	hrc_irq_restore(s);
	return loud;
}

// Closed by the DMR glue as the engine starts to transmit; opened here once it has stopped (gate_check, from the
// audio task and every speaker_pcm call). GATE_MIN_US: the engine's TX start runs right after the close, and the gate
// must not open before it does
#define GATE_MIN_US	50000u

void hrc_audio_pcm_tx_gate(void)
{
	uint32_t s = hrc_irq_save();
	gate_at = hrc_micros();
	if (!tx_gated) {
		gates++;
		if (spk_routed && hrc_audio_pcm_spk_pending()) {
			loud_at_gate++;	// a beep was still sounding: its tail is cut, never sent
		}
		tx_gated = true;
		spk_drop();
		if (modem != NULL) {
			ring_apply();
		}
	}
	hrc_irq_restore(s);
	if (task != NULL) {
		xTaskNotifyGive(task);	// to open it again once the engine is done
	}
}

// Interrupts on or off. All of it under the lock: a close from another task between a check and the opening (a new
// tx_begin moves gate_at) must leave the gate closed
static void gate_check(void)
{
	if (!tx_gated || modem == NULL) {
		return;
	}
	uint32_t s = hrc_irq_save();
	if ((modem->audio_ctl & C7_AUDIO_RING_PLAY) != 0) {
		ring_on_air++;	// never: ring play came on while gated
	}
	if (tx_gated && hrc_micros() - gate_at >= GATE_MIN_US && !hrc_dmr_tx_active()) {
		tx_gated = false;
		spk_drop();	// what was queued meanwhile is stale
		ring_apply();
	}
	hrc_irq_restore(s);
}

void hrc_audio_pcm_gate_stats(uint32_t *closings, uint32_t *late, uint32_t *ring_on)
{
	*closings = gates;
	*late = loud_at_gate;
	*ring_on = ring_on_air;
}

void hrc_audio_pcm_gate_reset(void)
{
	gates = loud_at_gate = ring_on_air = 0;
}

// Once: registering masks the source, so never while the meter or the stream has it on
static void isr_register(void)
{
	uint32_t s = hrc_irq_save();	// the stream (audio users' task) and the meter (radio task) can both get here first
	if (!isr_registered) {
		hrc_irq_register(IRQ_PCM_WR, isr_pcm_wr);
		hrc_irq_register(IRQ_TIMER6, fifo_tick);
		hrc_irq_enable(IRQ_TIMER6, true);
		isr_registered = true;
	}
	hrc_irq_restore(s);
}

/* Call under the IRQ lock. Discard partial mic blocks across direction changes. */
static void set_mic_capture(bool on)
{
	mic_capture_on = on;
	mic_fill = mic_half = 0;
	mic_ready = -1;
	if (on) {
		hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_PCM_WR);
		hrc_irq_enable(IRQ_PCM_WR, true);
		hrc7000_audio_bits(modem, C7_AUDIO_PCM_MIC, 0);
	} else {
		hrc_irq_enable(IRQ_PCM_WR, false);
		hrc7000_audio_bits(modem, 0, C7_AUDIO_PCM_MIC);
		playback_due = xTaskGetTickCount() + pdMS_TO_TICKS(20);
	}
}

static void mic_capture(bool on)
{
	uint32_t s = hrc_irq_save();
	bool changed = running && mic_capture_on != on;
	if (changed)
		set_mic_capture(on);
	hrc_irq_restore(s);
	if (changed)
		xTaskNotifyGive(task);
}

static bool start(audio_block_cb_t cb, void *ctx)
{
	if (task == NULL) {
		isr_register();
		if (xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY, &task) != pdPASS) {
			task = NULL;
			return false;
		}
	}
	uint32_t s = hrc_irq_save();
	block_ctx = ctx;
	block_cb = cb;
	spk_drop();
	running = true;
	set_mic_capture(true);
	memset(play_diag, 0, sizeof play_diag);
	play_diag[5] = UINT32_MAX;
	hrc_timer_oneshot(TIMER_PCM, SPK_POLL_US);
	hrc_irq_restore(s);
	return true;
}

static void stop(void)
{
	uint32_t s = hrc_irq_save();	// the meter's interrupt decides on the same state
	running = false;
	fifo_settling = false;
	hrc_timer_stop(TIMER_PCM);
	block_cb = NULL;
	set_mic_capture(false);
	hrc_irq_restore(s);
}

// Opening waits for the first hardware FIFO write. Closing cancels any
// pending write before the board switches the route and amplifier off.
static void speaker_pcm(bool on)
{
	uint32_t irq = hrc_irq_save();
	if (on != spk_routed) {
		if (!on)
			spk_drop();
		spk_open_pending = on;
		spk_routed = on;
		if (!on && speaker_pcm_cb != NULL)
			speaker_pcm_cb(false);
	}
	gate_check();
	hrc_irq_restore(irq);
}

const hal_audio_t hrc_audio_pcm = {
	.start = start,
	.stop = stop,
	.mic_capture = mic_capture,
	.speaker_pcm = speaker_pcm,
	.spk_pending = hrc_audio_pcm_spk_pending,
	.playback_stats = playback_stats,
};

void hrc_audio_pcm_stats(uint32_t *b, uint32_t *ov, uint32_t *un)
{
	*b = blocks;
	*ov = overruns;
	*un = underruns;
}

void hrc_audio_pcm_reset_stats(void)
{
	blocks = overruns = underruns = 0;
}


#endif	// CONFIG_M17
