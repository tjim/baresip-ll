/**
 * @file decode.c  Video codecs using FFmpeg libavcodec -- decoder
 *
 * Copyright (C) 2010 - 2013 Creytiv.com
 */
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <libavcodec/avcodec.h>
#include "h26x.h"
#include "avcodec.h"


#define DEBUG_MODULE "avcodec"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct viddec_state {
	AVCodec *codec;
	AVCodecContext *ctx;
	AVFrame *pict;
	struct mbuf *mb;
	bool got_keyframe;
};


static void destructor(void *arg)
{
	struct viddec_state *st = arg;

	mem_deref(st->mb);

	if (st->ctx) {
		if (st->ctx->codec)
			avcodec_close(st->ctx);
		av_free(st->ctx);
	}

	if (st->pict)
		av_free(st->pict);
}


static int init_decoder(struct viddec_state *st, const char *name)
{
	enum CodecID codec_id;

	codec_id = avcodec_resolve_codecid(name);
	if (codec_id == CODEC_ID_NONE)
		return EINVAL;

	st->codec = avcodec_find_decoder(codec_id);
	if (!st->codec)
		return ENOENT;

#if LIBAVCODEC_VERSION_INT >= ((52<<16)+(92<<8)+0)
	st->ctx = avcodec_alloc_context3(st->codec);
#else
	st->ctx = avcodec_alloc_context();
#endif

	st->pict = avcodec_alloc_frame();

	if (!st->ctx || !st->pict)
		return ENOMEM;

#if LIBAVCODEC_VERSION_INT >= ((53<<16)+(8<<8)+0)
	if (avcodec_open2(st->ctx, st->codec, NULL) < 0)
		return ENOENT;
#else
	if (avcodec_open(st->ctx, st->codec) < 0)
		return ENOENT;
#endif

	return 0;
}


int decode_update(struct viddec_state **vdsp, const struct vidcodec *vc,
		  const char *fmtp)
{
	struct viddec_state *st;
	int err = 0;

	if (!vdsp || !vc)
		return EINVAL;

	if (*vdsp)
		return 0;

	(void)fmtp;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->mb = mbuf_alloc(1024);
	if (!st->mb) {
		err = ENOMEM;
		goto out;
	}

	err = init_decoder(st, vc->name);
	if (err) {
		DEBUG_WARNING("%s: could not init decoder\n", vc->name);
		goto out;
	}

	re_printf("video decoder %s (%s)\n", vc->name, fmtp);

 out:
	if (err)
		mem_deref(st);
	else
		*vdsp = st;

	return err;
}


/*
 * TODO: check input/output size
 */
static int ffdecode(struct viddec_state *st, struct vidframe *frame,
		    bool eof, struct mbuf *src)
{
	int i, got_picture, ret, err;

	/* assemble packets in "mbuf" */
	err = mbuf_write_mem(st->mb, mbuf_buf(src), mbuf_get_left(src));
	if (err)
		return err;

	if (!eof)
		return 0;

	st->mb->pos = 0;

	if (!st->got_keyframe) {
		err = EPROTO;
		goto out;
	}

#if LIBAVCODEC_VERSION_INT <= ((52<<16)+(23<<8)+0)
	ret = avcodec_decode_video(st->ctx, st->pict, &got_picture,
				   st->mb->buf,
				   (int)mbuf_get_left(st->mb));
#else
	do {
		AVPacket avpkt;

		av_init_packet(&avpkt);
		avpkt.data = st->mb->buf;
		avpkt.size = (int)mbuf_get_left(st->mb);

		ret = avcodec_decode_video2(st->ctx, st->pict,
					    &got_picture, &avpkt);
	} while (0);
#endif

	if (ret < 0) {
		err = EBADMSG;
		goto out;
	}
	else if (ret && ret != (int)mbuf_get_left(st->mb)) {
		DEBUG_NOTICE("decoded only %d of %u bytes (got_pict=%d)\n",
			     ret, mbuf_get_left(st->mb), got_picture);
	}

	mbuf_skip_to_end(src);

	if (got_picture) {
		for (i=0; i<4; i++) {
			frame->data[i]     = st->pict->data[i];
			frame->linesize[i] = st->pict->linesize[i];
		}
		frame->size.w = st->ctx->width;
		frame->size.h = st->ctx->height;
		frame->fmt    = VID_FMT_YUV420P;
	}

 out:
	if (eof)
		mbuf_rewind(st->mb);

	return err;
}


int h264_decode(struct viddec_state *st, struct mbuf *src)
{
	struct h264_hdr h264_hdr;
	const uint8_t nal_seq[3] = {0, 0, 1};
	int err;

	err = h264_hdr_decode(&h264_hdr, src);
	if (err)
		return err;

	if (h264_hdr.f) {
		DEBUG_WARNING("H264 forbidden bit set!\n");
		return EBADMSG;
	}

	/* handle NAL types */
	if (1 <= h264_hdr.type && h264_hdr.type <= 23) {

		if (!st->got_keyframe) {
			switch (h264_hdr.type) {

			case H264_NAL_PPS:
			case H264_NAL_SPS:
				st->got_keyframe = true;
				break;
			}
		}

		/* prepend H.264 NAL start sequence */
		mbuf_write_mem(st->mb, nal_seq, 3);

		/* encode NAL header back to buffer */
		err = h264_hdr_encode(&h264_hdr, st->mb);
	}
	else if (H264_NAL_FU_A == h264_hdr.type) {
		struct fu fu;

		err = fu_hdr_decode(&fu, src);
		if (err)
			return err;
		h264_hdr.type = fu.type;

		if (fu.s) {
			/* prepend H.264 NAL start sequence */
			mbuf_write_mem(st->mb, nal_seq, 3);

			/* encode NAL header back to buffer */
			err = h264_hdr_encode(&h264_hdr, st->mb);
		}
	}
	else {
		DEBUG_WARNING("unknown NAL type %u\n", h264_hdr.type);
		return EBADMSG;
	}

	return err;
}


int decode_h264(struct viddec_state *st, struct vidframe *frame,
		bool eof, uint16_t seq, struct mbuf *src)
{
	int err;

	(void)seq;

	if (!src)
		return 0;

	err = h264_decode(st, src);
	if (err)
		return err;

	return ffdecode(st, frame, eof, src);
}


int decode_mpeg4(struct viddec_state *st, struct vidframe *frame,
		 bool eof, uint16_t seq, struct mbuf *src)
{
	if (!src)
		return 0;

	(void)seq;

	/* let the decoder handle this */
	st->got_keyframe = true;

	return ffdecode(st, frame, eof, src);
}


int decode_h263(struct viddec_state *st, struct vidframe *frame,
		bool marker, uint16_t seq, struct mbuf *src)
{
	struct h263_hdr hdr;
	int err;

	if (!st || !frame)
		return EINVAL;

	if (!src)
		return 0;

	(void)seq;

	err = h263_hdr_decode(&hdr, src);
	if (err)
		return err;

#if 0
	re_printf(".....[%s seq=%5u ] MODE %s -"
		  " SBIT=%u EBIT=%u I=%s"
		  " (%5u/%5u bytes)\n",
		  marker ? "M" : " ", seq,
		  h263_hdr_mode(&hdr) == H263_MODE_A ? "A" : "B",
		  hdr.sbit, hdr.ebit, hdr.i ? "Inter" : "Intra",
		  mbuf_get_left(src), st->mb->end);
#endif

	if (!hdr.i)
		st->got_keyframe = true;

#if 0
	if (st->mb->pos == 0) {
		uint8_t *p = mbuf_buf(src);

		if (p[0] != 0x00 || p[1] != 0x00) {
			re_printf("invalid PSC detected (%02x %02x)\n",
				  p[0], p[1]);
			return EPROTO;
		}
	}
#endif

	/*
	 * The H.263 Bit-stream can be fragmented on bit-level,
	 * indicated by SBIT and EBIT. Example:
	 *
	 *               8 bit  2 bit
	 *            .--------.--.
	 * Packet 1   |        |  |
	 * SBIT=0     '--------'--'
	 * EBIT=6
	 *                        .------.--------.--------.
	 * Packet 2               |      |        |        |
	 * SBIT=2                 '------'--------'--------'
	 * EBIT=0                   6bit    8bit     8bit
	 *
	 */

	if (hdr.sbit > 0) {
		const uint8_t mask  = (1 << (8 - hdr.sbit)) - 1;
		const uint8_t sbyte = mbuf_read_u8(src) & mask;

		st->mb->buf[st->mb->end - 1] |= sbyte;
	}

	return ffdecode(st, frame, marker, src);
}
