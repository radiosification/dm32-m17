/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included only in the separate derivative's board radio translation unit
 * (radio_dm32.c, at its end): the flat FM audio paths the NXDN48 modem uses
 * (hal_audio_tap.h AUDIO_TAP_FM_RX_FLAT / AUDIO_TAP_FM_TX_FLAT).
 *
 * Hardware-unverified. What each step relies on, and how to check it on the
 * radio, is in docs/nxdn-modem.md ("What the radio must confirm") and
 * docs/hil-checklist.md.
 *
 * Receive: the FD6818's AF output reaches the HR_C7000's AF input while the
 * speaker listens to the codec (speaker_source with data_listen); the
 * HR_C7000 copies it to RX RAM, 8 kHz (fm_rx_audio.c, modem-hrc7000.md 8.5).
 * The FD6818's AF filter word 0x2B = 0x0700, the stock firmware's digital
 * word (hardware-dm32.md 8.2 "Enter DMR"; the BK4819's own digital set writes
 * 0x8707): bits 10, 9, 8 bypass the receive 300 Hz high-pass, 3 kHz low-pass
 * and de-emphasis in the BK4819's layout (bits 2, 1, 0 the transmit ones;
 * bk4819.h's "RX 15, 14, 13" doesn't match the stock digital words). The FM
 * entry's 0xC000 leaves them on (the de-emphasis measured with APRS,
 * dm32-bringup.md). P25 receives through 0x0700 on this radio. The HR_C7000's
 * FM band-pass is off (FM_BANDWIDTH b6).
 *
 * Transmit: TX RAM is the documented FM source (AUDIO_CONTROL b0, datasheet
 * pp. 271, 339-340). SYS_INTERP b15 requests the 256-sample half named by
 * FM_ADDR_SW (1 low, 2 high); samples are high byte first. Keep prompt ring
 * play off: off-air captures on 2026-11-13 had periodic block distortion and
 * zero decoded frames even with the microphone disconnected. The microphone
 * still reaches the codec's CPU PCM for the vocoder. FD6818 0x2B=0xC007
 * removes the measured TX pre-emphasis. The 0x7E=0x3006 experiment did not
 * remove the low-frequency droop and is not retained. TX RAM's deviation
 * scale uses the off-air M17 measurement below; its frequency response and
 * the corrected deviation still need verification on the radio. */
#if CONFIG_M17
#define FD_AF_FILTER		0x2Bu
#define FD_AF_FILTER_FM		0xC000u		/* fd6818.c's FM table */
#define FD_AF_FILTER_FLAT	0x0700u		/* receive high-pass, low-pass and de-emphasis bypassed (stock digital word) */
#define FD_AF_FILTER_TX_FLAT	(FD_AF_FILTER_FM | 0x0007u)	/* transmit high-pass, low-pass and pre-emphasis bypassed */
#define FD_AF_GAIN		0x48u
/* bits 3:0 = AF DAC gain (0..15). 15 is the stock FM and digital sets' value (0x48 = 0xBBBF), the level P25 receives
 * at; APRS measured peaks about 19000 of 32767 at 3 kHz deviation through it (dm32-bringup.md), NXDN peaks at about
 * 1.4 kHz. Noise alone reaches full scale (modem-hrc7000.md 8.5): no harm, the demodulator hunts on it. Tune against
 * the RX RAM peak with the signal on (work/rx_ram_capture.py) */
#define FLAT_RX_AF_GAIN		15
static uint16_t flat_af_gain_saved;
#define C7_FM_BW_BANDPASS	(1u << 6)
#define C7_FM_BW_PREEMPH	(1u << 4)
#define C7_AUDIO_TXRAM_SOURCE	(1u << 0)	/* FM / DMR voice from TX RAM instead of the codec */
/* TX RAM calibration from the 2026-11-17 M17 I/Q capture at 434.400 MHz:
 * the FIFO-derived 1375 LSB level gave 473.7 Hz per symbol unit instead of
 * 800 Hz. Scale the 1500 Hz nominal sample peak by 800 / 473.7. This is an
 * initial correction for the measured radio; verify off air after flashing. */
#define FLAT_TX_NOMINAL_PEAK	2322
/* With the narrow FM limiter at 5, the clean txcal1 capture measured inner
 * levels near 783 Hz but outer levels near 1650 Hz. Give the shaped M17
 * waveform headroom without changing the board's FM voice settings. */
#define FLAT_TX_LIMIT		15u
#define FLAT_TX_LIMIT_MASK	0x00FF0000u
static uint32_t flat_tx_dev_saved;

static audio_tap_pull_t flat_pull;
static int16_t flat_chunk[HRC7000_FM_TX_SAMPLES];

/* System interrupt: only the half the modem releases, never the one playing. */
static HRC_RAMFUNC void flat_half_ready(void)
{
	uint32_t half = hrc7000_rd(&modem, C7_FM_ADDR_SW) & 3u;
	if (flat_pull == NULL || (half != 1u && half != 2u)) {
		return;
	}
	flat_pull(flat_chunk, HRC7000_FM_TX_SAMPLES);
	hrc7000_fm_tx_audio(&modem, half, flat_chunk);
}

static bool flat_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t m)
{
	if (m != AUDIO_TAP_FLAT) {
		return false;
	}
	if (pull != NULL) {
		if (!transmitting || mode != RADIO_MODE_FM || data_tx) {
			return false;
		}
		if (!hrc_dmr_fm_tx_hook(flat_half_ready)) {
			return false;
		}
		for (uint32_t i = 0; i < HRC7000_FM_TX_SAMPLES * 4u; i++) {
			modem.buf[C7_FM_TX_AUDIO + i] = 0;
		}
		hrc_audio_pcm_hold(true);
		flat_tx_dev_saved = hrc7000_rd(&modem, C7_FM_DEV_COEF);
		hrc7000_wr(&modem, C7_FM_DEV_COEF,
			(flat_tx_dev_saved & ~FLAT_TX_LIMIT_MASK) | (FLAT_TX_LIMIT << 16));
		flat_pull = pull;
		data_tx = true;
		ring_play();			// TX RAM must not be mixed with the prompt path
		uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
		hrc7000_audio_bits(&modem, C7_AUDIO_TXRAM_SOURCE, 0);
		hrc_irq_restore(s);
		hrc7000_wr(&modem, C7_FM_BANDWIDTH, hrc7000_rd(&modem, C7_FM_BANDWIDTH) & ~(C7_FM_BW_BANDPASS | C7_FM_BW_PREEMPH));
		hrc_pin_write(PIN_MIC_PATH, false);	// the microphone to the codec, as in DMR (dm32_native_mic_route)
		fd6818_write(&fd, FD_AF_FILTER, FD_AF_FILTER_TX_FLAT);
		hrc7000_wr(&modem, C7_SYS_INTERP_CLEAR, C7_SYS_FM_TX);
		hrc7000_fm_tx_audio_irq(&modem, true);
		return true;
	}
	uint32_t s = hrc_irq_save();
	hrc7000_fm_tx_audio_irq(&modem, false);
	hrc_dmr_fm_tx_hook(NULL);
	flat_pull = NULL;
	hrc_irq_restore(s);
	if (data_tx) {
		data_tx = false;
		hrc7000_wr(&modem, C7_FM_DEV_COEF, flat_tx_dev_saved);
		fd6818_write(&fd, FD_AF_FILTER, FD_AF_FILTER_FM);
		ring_play();
		hrc_audio_pcm_hold(false);
		s = hrc_irq_save();
		hrc7000_audio_bits(&modem, 0, C7_AUDIO_TXRAM_SOURCE);
		hrc_irq_restore(s);
	}
	return true;
}

static bool flat_rx(bool on)
{
	if (on) {
		if (mode != RADIO_MODE_FM || transmitting || !have_rx || data_listen) {
			return false;
		}
		data_listen = true;
		speaker_source();		// the FD6818's audio stays on the HR_C7000's input
		fd6818_write(&fd, FD_AF_FILTER, FD_AF_FILTER_FLAT);
		flat_af_gain_saved = fd6818_read(&fd, FD_AF_GAIN);
		fd6818_write(&fd, FD_AF_GAIN, (uint16_t)((flat_af_gain_saved & 0xFFF0u) | FLAT_RX_AF_GAIN));
		hrc7000_wr(&modem, C7_FM_BANDWIDTH, hrc7000_rd(&modem, C7_FM_BANDWIDTH) & ~C7_FM_BW_BANDPASS);
		if (hrc_fm_rx_start()) {
			return true;
		}
	}
	hrc_fm_rx_stop();
	if (data_listen) {
		data_listen = false;
		fd6818_write(&fd, FD_AF_FILTER, FD_AF_FILTER_FM);
		fd6818_write(&fd, FD_AF_GAIN, flat_af_gain_saved);
		if (mode == RADIO_MODE_FM && !transmitting) {
			hrc7000_fm_rx(&modem, rx_bw == RADIO_BW_25K);	// the band-pass back
		}
		speaker_source();
	}
	return !on;
}

const hal_audio_tap_t dm32_audio_tap = {
	.caps = AUDIO_TAP_FM_TX_FLAT | AUDIO_TAP_FM_RX_FLAT,
	.fm_tx_inject = flat_tx_inject,
	.fm_tx_nominal_peak = FLAT_TX_NOMINAL_PEAK,
	.fm_rx_flat = flat_rx,
	.fm_rx_read = hrc_fm_rx_read,
	/* the receive and transmit shape of the flat paths: not measured yet (docs/nxdn-modem.md) */
	.flat_rx_hpf_hz = 0,
	.flat_tx_hpf_hz = 0,
	/* 0: 0x2B = 0x0700 bypasses the de-emphasis. The radio tests of 750 and of the AF gain ran with 0x2B = 0xE000 (filters
	 * on) and before the sync heard an inverted stream: they don't tell. Set from work/rx_ram_capture.py's tone response */
	.flat_rx_deemph_us = 0,
};
#endif
