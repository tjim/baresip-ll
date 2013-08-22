/**
 * @file winwave/play.c Windows sound driver -- playback
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <rem.h>
#include <windows.h>
#include <mmsystem.h>
#include <baresip.h>
#include "winwave.h"


#define DEBUG_MODULE "winwave"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


#define WRITE_BUFFERS  4
#define INC_WPOS(a) ((a) = (((a) + 1) % WRITE_BUFFERS))


struct auplay_st {
	struct auplay *ap;      /* inheritance */
	struct dspbuf bufs[WRITE_BUFFERS];
	int pos;
	HWAVEOUT waveout;
	bool rdy;
	size_t inuse;
	auplay_write_h *wh;
	void *arg;
};


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;
	int i;

	st->wh = NULL;

	/* Mark the device for closing, and wait for all the
	 * buffers to be returned by the driver
	 */
	st->rdy = false;
	while (st->inuse > 0)
		Sleep(50);

	waveOutClose(st->waveout);

	for (i = 0; i < WRITE_BUFFERS; i++) {
		waveOutUnprepareHeader(st->waveout, &st->bufs[i].wh,
				       sizeof(WAVEHDR));
		mem_deref(st->bufs[i].mb);
	}

	mem_deref(st->ap);
}


static int dsp_write(struct auplay_st *st)
{
	MMRESULT res;
	WAVEHDR *wh;
	struct mbuf *mb;

	if (!st->rdy)
		return EINVAL;

	wh = &st->bufs[st->pos].wh;
	if (wh->dwFlags & WHDR_PREPARED) {
		return EINVAL;
	}
	mb = st->bufs[st->pos].mb;
	wh->lpData = (LPSTR)mb->buf;

	if (st->wh) {
		st->wh(mb->buf, mb->size, st->arg);
	}

	wh->dwBufferLength = mb->size;
	wh->dwFlags = 0;
	wh->dwUser = (DWORD_PTR) mb;

	waveOutPrepareHeader(st->waveout, wh, sizeof(*wh));

	INC_WPOS(st->pos);

	res = waveOutWrite(st->waveout, wh, sizeof(*wh));
	if (res != MMSYSERR_NOERROR)
		DEBUG_WARNING("dsp_write: waveOutWrite: failed: %08x\n", res);
	else
		st->inuse++;

	return 0;
}


static void CALLBACK waveOutCallback(HWAVEOUT hwo,
				     UINT uMsg,
				     DWORD_PTR dwInstance,
				     DWORD_PTR dwParam1,
				     DWORD_PTR dwParam2)
{
	struct auplay_st *st = (struct auplay_st *)dwInstance;
	WAVEHDR *wh = (WAVEHDR *)dwParam1;

	(void)hwo;
	(void)dwParam2;

	switch (uMsg) {

	case WOM_OPEN:
		st->rdy = true;
		break;

	case WOM_DONE:
		/*LOCK();*/
		waveOutUnprepareHeader(st->waveout, wh, sizeof(*wh));
		/*UNLOCK();*/
		st->inuse--;
		dsp_write(st);
		break;

	case WOM_CLOSE:
		st->rdy = false;
		break;

	default:
		break;
	}
}


static int write_stream_open(struct auplay_st *st,
			     const struct auplay_prm *prm)
{
	WAVEFORMATEX wfmt;
	MMRESULT res;
	int i;

	/* Open an audio I/O stream. */
	st->waveout = NULL;
	st->pos = 0;
	st->rdy = false;

	for (i = 0; i < WRITE_BUFFERS; i++) {
		memset(&st->bufs[i].wh, 0, sizeof(WAVEHDR));
		st->bufs[i].mb = mbuf_alloc(2 * prm->frame_size);
	}

	wfmt.wFormatTag      = WAVE_FORMAT_PCM;
	wfmt.nChannels       = prm->ch;
	wfmt.nSamplesPerSec  = prm->srate;
	wfmt.wBitsPerSample  = 16;
	wfmt.nBlockAlign     = (prm->ch * wfmt.wBitsPerSample) / 8;
	wfmt.nAvgBytesPerSec = wfmt.nSamplesPerSec * wfmt.nBlockAlign;
	wfmt.cbSize          = 0;

	res = waveOutOpen(&st->waveout, WAVE_MAPPER, &wfmt,
			  (DWORD_PTR) waveOutCallback,
			  (DWORD_PTR) st,
			  CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);
	if (res != MMSYSERR_NOERROR) {
		DEBUG_WARNING("waveOutOpen: failed %d\n", res);
		return EINVAL;
	}
	waveOutClose(st->waveout);
	res = waveOutOpen(&st->waveout, WAVE_MAPPER, &wfmt,
			  (DWORD_PTR) waveOutCallback,
			  (DWORD_PTR) st,
			  CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);
	if (res != MMSYSERR_NOERROR) {
		DEBUG_WARNING("waveOutOpen: failed %d\n", res);
		return EINVAL;
	}

	return 0;
}


int winwave_play_alloc(struct auplay_st **stp, struct auplay *ap,
		       struct auplay_prm *prm, const char *device,
		       auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int i, err;
	(void)device;

	if (!stp || !ap || !prm)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->ap  = mem_ref(ap);
	st->wh  = wh;
	st->arg = arg;

	prm->fmt = AUFMT_S16LE;

	err = write_stream_open(st, prm);
	if (err)
		goto out;

	/* The write runs at 100ms intervals
	 * prepare enough buffers to suite its needs
	 */
	for (i = 0; i < 5; i++)
		dsp_write(st);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
