/**
 * @file vidloop.c  Video loop
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _BSD_SOURCE 1
#include <string.h>
#include <time.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>


#define DEBUG_MODULE "vidloop"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Video Statistics */
struct vstat {
	uint64_t tsamp;
	uint32_t frames;
	size_t bytes;
	uint32_t bitrate;
	double efps;
};


/** Video loop */
struct video_loop {
	const struct vidcodec *vc;
	struct videnc_state *enc;
	struct viddec_state *dec;
	struct vidisp_st *vidisp;
	struct vidsrc_st *vsrc;
	struct vstat stat;
	struct tmr tmr_bw;
	uint16_t seq;
};


static struct video_loop *gvl;


static int packet_handler(bool marker, const uint8_t *hdr, size_t hdr_len,
			  const uint8_t *pld, size_t pld_len, void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe frame;
	struct mbuf *mb;
	int err;

	mb = mbuf_alloc(hdr_len + pld_len);
	if (!mb)
		return ENOMEM;

	if (hdr_len)
		mbuf_write_mem(mb, hdr, hdr_len);
	mbuf_write_mem(mb, pld, pld_len);

	mb->pos = 0;

	vl->stat.bytes += mbuf_get_left(mb);

	/* decode */
	frame.data[0] = NULL;
	if (vl->dec) {
		err = vl->vc->dech(vl->dec, &frame, marker, vl->seq++, mb);
		if (err) {
			DEBUG_WARNING("codec_decode: %m\n", err);
			return err;
		}
	}

	/* display - if valid picture frame */
	if (vidframe_isvalid(&frame))
		(void)vidisp_display(vl->vidisp, "Video Loop", &frame);

	mem_deref(mb);

	return 0;
}


static void vidsrc_frame_handler(const struct vidframe *frame, void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe *f2 = NULL;

	++vl->stat.frames;

	if (frame->fmt != VID_FMT_YUV420P) {

		if (vidframe_alloc(&f2, VID_FMT_YUV420P, &frame->size))
			return;

		vidconv(f2, frame, 0);

		frame = f2;
	}

	if (vl->enc) {
		(void)vl->vc->ench(vl->enc, false, frame,
				   packet_handler, vl);
	}
	else {
		vl->stat.bytes += vidframe_size(frame->fmt, &frame->size);
		(void)vidisp_display(vl->vidisp, "Video Loop", frame);
	}

	mem_deref(f2);
}


static void vidloop_destructor(void *arg)
{
	struct video_loop *vl = arg;

	tmr_cancel(&vl->tmr_bw);
	mem_deref(vl->vsrc);
	mem_deref(vl->vidisp);
	mem_deref(vl->enc);
	mem_deref(vl->dec);
}


static void vidisp_input_handler(char key, void *arg)
{
	(void)arg;
	ui_input(key);
}


static int enable_codec(struct video_loop *vl)
{
	struct videnc_param prm;
	int err;

	prm.fps     = config.video.fps;
	prm.pktsize = 1024;
	prm.bitrate = config.video.bitrate;
	prm.max_fs  = -1;

	/* Use the first video codec */
	vl->vc = vidcodec_find(NULL, NULL);
	if (!vl->vc)
		return ENOENT;

	err = vl->vc->encupdh(&vl->enc, vl->vc, &prm, NULL);
	if (err) {
		DEBUG_WARNING("update encoder: %m\n", err);
		return err;
	}

	err = vl->vc->decupdh(&vl->dec, vl->vc, NULL);
	if (err) {
		DEBUG_WARNING("update decoder: %m\n", err);
		return err;
	}

	return 0;
}


static void disable_codec(struct video_loop *vl)
{
	vl->enc = mem_deref(vl->enc);
	vl->dec = mem_deref(vl->dec);
	vl->vc = NULL;
}


static void print_status(struct video_loop *vl)
{
	(void)re_fprintf(stderr, "\rstatus: EFPS=%.1f      %u kbit/s       \r",
			 vl->stat.efps, vl->stat.bitrate);
}


static void calc_bitrate(struct video_loop *vl)
{
	const uint64_t now = tmr_jiffies();

	if (now > vl->stat.tsamp) {

		const uint32_t dur = (uint32_t)(now - vl->stat.tsamp);

		vl->stat.efps = 1000.0f * vl->stat.frames / dur;

		vl->stat.bitrate = (uint32_t) (8 * vl->stat.bytes / dur);
	}

	vl->stat.frames = 0;
	vl->stat.bytes = 0;
	vl->stat.tsamp = now;
}


static void timeout_bw(void *arg)
{
	struct video_loop *vl = arg;

	tmr_start(&vl->tmr_bw, 5000, timeout_bw, vl);

	calc_bitrate(vl);
	print_status(vl);
}


static int vsrc_reopen(struct video_loop *vl, const struct vidsz *sz)
{
	struct vidsrc_prm prm;
	int err;

	(void)re_printf("%s,%s: open video source: %u x %u\n",
			config.video.src_mod, config.video.src_dev,
			sz->w, sz->h);

	prm.orient = VIDORIENT_PORTRAIT;
	prm.fps    = config.video.fps;

	vl->vsrc = mem_deref(vl->vsrc);
	err = vidsrc_alloc(&vl->vsrc, config.video.src_mod, NULL, &prm, sz,
			   NULL, config.video.src_dev, vidsrc_frame_handler,
			   NULL, vl);
	if (err) {
		DEBUG_WARNING("vidsrc %s failed: %m\n",
			      config.video.src_dev, err);
	}

	return err;
}


static int video_loop_alloc(struct video_loop **vlp, const struct vidsz *size)
{
	struct video_loop *vl;
	int err;

	vl = mem_zalloc(sizeof(*vl), vidloop_destructor);
	if (!vl)
		return ENOMEM;

	tmr_init(&vl->tmr_bw);

	err = vsrc_reopen(vl, size);
	if (err)
		goto out;

	err = vidisp_alloc(&vl->vidisp, NULL, NULL, NULL, NULL,
			   vidisp_input_handler, NULL, vl);
	if (err) {
		DEBUG_WARNING("video display failed: %m\n", err);
		goto out;
	}

	tmr_start(&vl->tmr_bw, 1000, timeout_bw, vl);

 out:
	if (err)
		mem_deref(vl);
	else
		*vlp = vl;

	return err;
}


/**
 * Start the video loop (for testing)
 */
static int vidloop_start(struct re_printf *pf, void *arg)
{
	struct vidsz size;
	int err = 0;

	(void)arg;

	size.w = config.video.width;
	size.h = config.video.height;

	if (gvl) {
		if (gvl->vc)
			disable_codec(gvl);
		else
			(void)enable_codec(gvl);

		(void)re_hprintf(pf, "%sabled codec: %s\n",
				 gvl->vc ? "En" : "Dis",
				 gvl->vc ? gvl->vc->name : "");
	}
	else {
		(void)re_hprintf(pf, "Enable video-loop on %s,%s: %u x %u\n",
				 config.video.src_mod, config.video.src_dev,
				 size.w, size.h);

		err = video_loop_alloc(&gvl, &size);
		if (err) {
			DEBUG_WARNING("vidloop alloc: %m\n", err);
		}
	}

	return err;
}


static int vidloop_stop(struct re_printf *pf, void *arg)
{
	(void)arg;

	if (gvl)
		(void)re_hprintf(pf, "Disable video-loop\n");
	gvl = mem_deref(gvl);
	return 0;
}


static const struct cmd cmdv[] = {
	{'v', 0, "Start video-loop", vidloop_start },
	{'V', 0, "Stop video-loop",  vidloop_stop  },
};


static int module_init(void)
{
	return cmd_register(cmdv, ARRAY_SIZE(cmdv));
}


static int module_close(void)
{
	vidloop_stop(NULL, NULL);
	cmd_unregister(cmdv);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(vidloop) = {
	"vidloop",
	"application",
	module_init,
	module_close,
};
