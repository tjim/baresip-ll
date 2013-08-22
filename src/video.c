/**
 * @file src/video.c  Video stream
 *
 * Copyright (C) 2010 Creytiv.com
 *
 * \ref GenericVideoStream
 */
#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "video"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Magic number */
#define MAGIC 0x00070d10
#include "magic.h"


/* Useful macro switches for development/testing */
#define ENABLE_ENCODER 1
#define ENABLE_DECODER 1


enum {
	SRATE = 90000,
	MAX_MUTED_FRAMES = 3,
};


/**
 * \page GenericVideoStream Generic Video Stream
 *
 * Implements a generic video stream. The application can allocate multiple
 * instances of a video stream, mapping it to a particular SDP media line.
 * The video object has a Video Display and Source, and a video encoder
 * and decoder. A particular video object is mapped to a generic media
 * stream object.
 *
 *<pre>
 *            recv  send
 *              |    /|\
 *             \|/    |
 *            .---------.    .-------.
 *            |  video  |--->|encoder|
 *            |         |    |-------|
 *            | object  |--->|decoder|
 *            '---------'    '-------'
 *              |    /|\
 *              |     |
 *             \|/    |
 *        .-------.  .-------.
 *        |Video  |  |Video  |
 *        |Display|  |Source |
 *        '-------'  '-------'
 *</pre>
 */

/** Video stream - transmitter/encoder direction */
struct vtx {
	struct video *video;               /**< Parent                    */
	const struct vidcodec *vc;         /**< Current Video encoder     */
	struct videnc_state *enc;          /**< Video encoder state       */
	struct vidsrc_prm vsrc_prm;        /**< Video source parameters   */
	struct vidsz vsrc_size;            /**< Video source size         */
	struct vidsrc_st *vsrc;            /**< Video source              */
	struct lock *lock;                 /**< Lock for encoder          */
	struct vidframe *frame;            /**< Source frame              */
	struct vidframe *mute_frame;       /**< Frame with muted video    */
	struct mbuf *mb;
	int muted_frames;                  /**< # of muted frames sent    */
	uint32_t ts_tx;                    /**< Outgoing RTP timestamp    */
	bool picup;                        /**< Send picture update       */
	bool muted;                        /**< Muted flag                */
	int frames;                        /**< Number of frames sent     */
	int efps;                          /**< Estimated frame-rate      */
};


/** Video stream - receiver/decoder direction */
struct vrx {
	struct video *video;               /**< Parent                    */
	const struct vidcodec *vc;         /**< Current video decoder     */
	struct viddec_state *dec;          /**< Video decoder state       */
	struct vidisp_prm vidisp_prm;      /**< Video display parameters  */
	struct vidisp_st *vidisp;          /**< Video display             */
	struct lock *lock;                 /**< Lock for decoder          */
	enum vidorient orient;             /**< Display orientation       */
	bool fullscreen;                   /**< Fullscreen flag           */
	int pt_rx;                         /**< Incoming RTP payload type */
	int frames;                        /**< Number of frames received */
	int efps;                          /**< Estimated frame-rate      */
};


/** Generic Video stream */
struct video {
	MAGIC_DECL              /**< Magic number for debugging           */
	struct stream *strm;    /**< Generic media stream                 */
	struct vtx vtx;         /**< Transmit/encoder direction           */
	struct vrx vrx;         /**< Receive/decoder direction            */
	struct list filtl;      /**< Filters in order (struct vidfilt_st) */
	struct tmr tmr;         /**< Timer for frame-rate estimation      */
	size_t max_rtp_size;    /**< Maximum size of outgoing RTP packets */
	char *peer;             /**< Peer URI                             */
	bool nack_pli;          /**< Send NACK/PLI to peer                */
};


static void video_destructor(void *arg)
{
	struct video *v = arg;
	struct vtx *vtx = &v->vtx;
	struct vrx *vrx = &v->vrx;

	/* transmit */
	mem_deref(vtx->vsrc);
	lock_write_get(vtx->lock);
	mem_deref(vtx->frame);
	mem_deref(vtx->mute_frame);
	mem_deref(vtx->enc);
	mem_deref(vtx->mb);
	lock_rel(vtx->lock);
	mem_deref(vtx->lock);

	/* receive */
	lock_write_get(vrx->lock);
	mem_deref(vrx->dec);
	mem_deref(vrx->vidisp);
	lock_rel(vrx->lock);
	mem_deref(vrx->lock);

	list_flush(&v->filtl);
	tmr_cancel(&v->tmr);
	mem_deref(v->strm);
	mem_deref(v->peer);
}


#if ENABLE_ENCODER
static int get_fps(const struct video *v)
{
	const char *attr;

	/* RFC4566 */
	attr = sdp_media_rattr(stream_sdpmedia(v->strm), "framerate");
	if (attr) {
		/* NOTE: fractional values are ignored */
		const double fps = atof(attr);
		return (int)fps;
	}
	else
		return config.video.fps;
}


static int packet_handler(bool marker, const uint8_t *hdr, size_t hdr_len,
			  const uint8_t *pld, size_t pld_len, void *arg)
{
	struct vtx *tx = arg;
	int err = 0;

	tx->mb->pos = tx->mb->end = STREAM_PRESZ;

	if (hdr_len) err |= mbuf_write_mem(tx->mb, hdr, hdr_len);
	if (pld_len) err |= mbuf_write_mem(tx->mb, pld, pld_len);

	tx->mb->pos = STREAM_PRESZ;

	if (!err) {
		err = stream_send(tx->video->strm, marker, -1,
				  tx->ts_tx, tx->mb);
	}

	return err;
}


/**
 * Encode video and send via RTP stream
 *
 * @note This function has REAL-TIME properties
 */
static void encode_rtp_send(struct vtx *vtx, const struct vidframe *frame)
{
	struct le *le;
	int err = 0;

	if (!vtx->enc)
		return;

	lock_write_get(vtx->lock);

	/* Convert image */
	if (frame->fmt != VID_FMT_YUV420P ||
	    !vidsz_cmp(&frame->size, &vtx->vsrc_size)) {

		if (!vtx->frame) {

			err = vidframe_alloc(&vtx->frame, VID_FMT_YUV420P,
					     &vtx->vsrc_size);
			if (err)
				goto unlock;
		}

		vidconv(vtx->frame, frame, 0);
		frame = vtx->frame;
	}

	/* Process video frame through all Video Filters */
	for (le = vtx->video->filtl.head; le; le = le->next) {

		struct vidfilt_st *st = le->data;

		if (st->vf->ench)
			err |= st->vf->ench(st, (struct vidframe *)frame);
	}

 unlock:
	lock_rel(vtx->lock);

	if (err)
		return;

	/* Encode the whole picture frame */
	err = vtx->vc->ench(vtx->enc, vtx->picup, frame, packet_handler, vtx);
	if (err) {
		DEBUG_WARNING("encode: %m\n", err);
		return;
	}

	vtx->ts_tx += (SRATE/vtx->vsrc_prm.fps);
	vtx->picup = false;
}


/**
 * Read frames from video source
 *
 * @note This function has REAL-TIME properties
 */
static void vidsrc_frame_handler(const struct vidframe *frame, void *arg)
{
	struct vtx *vtx = arg;

	++vtx->frames;

	/* Is the video muted? If so insert video mute image */
	if (vtx->muted)
		frame = vtx->mute_frame;

	if (vtx->muted && vtx->muted_frames >= MAX_MUTED_FRAMES)
		return;

	/* Encode and send */
	encode_rtp_send(vtx, frame);
	vtx->muted_frames++;
}


static void vidsrc_error_handler(int err, void *arg)
{
	struct vtx *vtx = arg;

	DEBUG_WARNING("Video-source error: %m\n", err);

	vtx->vsrc = mem_deref(vtx->vsrc);
}
#endif


static int vtx_alloc(struct vtx *vtx, struct video *video)
{
	int err;

	err = lock_alloc(&vtx->lock);
	if (err)
		return err;

	vtx->mb = mbuf_alloc(STREAM_PRESZ + 512);
	if (!vtx->mb)
		return ENOMEM;

	vtx->video = video;
	vtx->ts_tx = 160;

	return err;
}


static int vrx_alloc(struct vrx *vrx, struct video *video)
{
	int err;

	err = lock_alloc(&vrx->lock);
	if (err)
		goto out;

	vrx->video  = video;
	vrx->pt_rx  = -1;
	vrx->orient = VIDORIENT_PORTRAIT;

 out:
	return err;
}


#if ENABLE_DECODER
/**
 * Decode incoming RTP packets using the Video decoder
 *
 * NOTE: mb=NULL if no packet received
 */
static int video_stream_decode(struct vrx *vrx, const struct rtp_header *hdr,
			       struct mbuf *mb)
{
	struct video *v = vrx->video;
	struct vidframe frame;
	struct le *le;
	int err = 0;

	if (!hdr || !mbuf_get_left(mb))
		return 0;

	lock_write_get(vrx->lock);

	/* No decoder set */
	if (!vrx->dec) {
		DEBUG_WARNING("No video decoder!\n");
		goto out;
	}

	frame.data[0] = NULL;
	err = vrx->vc->dech(vrx->dec, &frame, hdr->m, hdr->seq, mb);
	if (err) {

		if (err != EPROTO) {
			DEBUG_WARNING("%s decode error"
				      " (seq=%u, %u bytes): %m\n",
				      vrx->vc->name, hdr->seq,
				      mbuf_get_left(mb), err);
		}

		/* send RTCP FIR to peer */
		stream_send_fir(v->strm, v->nack_pli);

		/* XXX: if RTCP is not enabled, send XML in SIP INFO ? */

		goto out;
	}

	/* Got a full picture-frame? */
	if (!vidframe_isvalid(&frame))
		goto out;

	/* Process video frame through all Video Filters */
	for (le = v->filtl.head; le; le = le->next) {

		struct vidfilt_st *st = le->data;

		if (st->vf->dech)
			err |= st->vf->dech(st, &frame);
	}

	err = vidisp_display(vrx->vidisp, v->peer, &frame);

	++vrx->frames;

out:
	lock_rel(vrx->lock);

	return err;
}
#else
static int video_stream_decode(struct vrx *vrx, const struct rtp_header *hdr,
			       struct mbuf *mb)
{
	(void)vrx;
	(void)hdr;
	(void)mb;
	return 0;
}
#endif


static int pt_handler(struct video *v, uint8_t pt_old, uint8_t pt_new)
{
	const struct sdp_format *lc;

	lc = sdp_media_lformat(stream_sdpmedia(v->strm), pt_new);
	if (!lc)
		return ENOENT;

	(void)re_fprintf(stderr, "Video decoder changed payload %u -> %u\n",
			 pt_old, pt_new);

	v->vrx.pt_rx = pt_new;

	return video_decoder_set(v, lc->data, lc->pt, lc->rparams);
}


/* Handle incoming stream data from the network */
static void stream_recv_handler(const struct rtp_header *hdr,
				struct mbuf *mb, void *arg)
{
	struct video *v = arg;
	int err;

	if (!mb)
		goto out;

	/* Video payload-type changed? */
	if (hdr->pt == v->vrx.pt_rx)
		goto out;

	err = pt_handler(v, v->vrx.pt_rx, hdr->pt);
	if (err)
		return;

 out:
	(void)video_stream_decode(&v->vrx, hdr, mb);
}


static void rtcp_handler(struct rtcp_msg *msg, void *arg)
{
	struct video *v = arg;

	switch (msg->hdr.pt) {

	case RTCP_FIR:
		v->vtx.picup = true;
		break;

	case RTCP_PSFB:
		if (msg->hdr.count == RTCP_PSFB_PLI)
			v->vtx.picup = true;
		break;

	default:
		break;
	}
}


int video_alloc(struct video **vp, struct call *call,
		struct sdp_session *sdp_sess, int label,
		const struct mnat *mnat, struct mnat_sess *mnat_sess,
		const struct menc *menc, struct menc_sess *menc_sess,
		const char *content,
		const struct list *vidcodecl)
{
	struct video *v;
	struct le *le;
	int err = 0;

	if (!vp)
		return EINVAL;

	v = mem_zalloc(sizeof(*v), video_destructor);
	if (!v)
		return ENOMEM;

	MAGIC_INIT(v);

	tmr_init(&v->tmr);

	err = stream_alloc(&v->strm, call, sdp_sess, "video", label,
			   mnat, mnat_sess, menc, menc_sess,
			   stream_recv_handler, rtcp_handler, v);
	if (err)
		goto out;

	err |= sdp_media_set_lattr(stream_sdpmedia(v->strm), true,
				   "framerate", "%d", config.video.fps);

	/* RFC 4585 */
	err |= sdp_media_set_lattr(stream_sdpmedia(v->strm), true,
				   "rtcp-fb", "* nack pli");

	/* RFC 4796 */
	if (content) {
		err |= sdp_media_set_lattr(stream_sdpmedia(v->strm), true,
					   "content", "%s", content);
	}

	if (err)
		goto out;

	err  = vtx_alloc(&v->vtx, v);
	err |= vrx_alloc(&v->vrx, v);
	if (err)
		goto out;

	v->max_rtp_size = 1024;

	/* Video codecs */
	for (le = list_head(vidcodecl); le; le = le->next) {
		struct vidcodec *vc = le->data;
		err |= sdp_format_add(NULL, stream_sdpmedia(v->strm), false,
				      vc->pt, vc->name, 90000, 1,
				      vc->fmtp_ench, vc->fmtp_cmph, vc, false,
				      "%s", vc->fmtp);
	}

	/* Video filters */
	for (le = list_head(vidfilt_list()); le; le = le->next) {
		struct vidfilt *vf = le->data;
		struct vidfilt_st *st = NULL;

		if (!vf->updh)
			continue;

		err = vf->updh(&st, vf);
		if (err) {
			DEBUG_WARNING("video-filter '%s' failed (%m)\n",
				      vf->name, err);
			goto out;
		}

		st->vf = vf;
		list_append(&v->filtl, &st->le, st);
	}

 out:
	if (err)
		mem_deref(v);
	else
		*vp = v;

	return err;
}


#if ENABLE_DECODER
static void vidisp_input_handler(char key, void *arg)
{
	struct vrx *vrx = arg;

	(void)vrx;

	ui_input(key);
}


static void vidisp_resize_handler(const struct vidsz *sz, void *arg)
{
	struct vrx *vrx = arg;
	(void)vrx;

	(void)re_printf("resize: %u x %u\n", sz->w, sz->h);

	/* XXX: update wanted picturesize and send re-invite to peer */
}


/* Set the video display - can be called multiple times */
static int set_vidisp(struct vrx *vrx)
{
	struct vidisp *vd;

	vrx->vidisp = mem_deref(vrx->vidisp);
	vrx->vidisp_prm.view = NULL;

	vd = (struct vidisp *)vidisp_find(NULL);
	if (!vd)
		return ENOENT;

	return vd->alloch(&vrx->vidisp, NULL, vd, &vrx->vidisp_prm, NULL,
			  vidisp_input_handler, vidisp_resize_handler, vrx);
}
#endif


#if ENABLE_ENCODER
/* Set the encoder format - can be called multiple times */
static int set_encoder_format(struct vtx *vtx, const char *src,
			      const char *dev, struct vidsz *size)
{
	struct vidsrc *vs = (struct vidsrc *)vidsrc_find(src);
	int err;

	if (!vs)
		return ENOENT;

	vtx->vsrc_size       = *size;
	vtx->vsrc_prm.fps    = get_fps(vtx->video);
	vtx->vsrc_prm.orient = VIDORIENT_PORTRAIT;

	vtx->vsrc = mem_deref(vtx->vsrc);

	err = vs->alloch(&vtx->vsrc, vs, NULL, &vtx->vsrc_prm,
			 &vtx->vsrc_size, NULL, dev, vidsrc_frame_handler,
			 vidsrc_error_handler, vtx);
	if (err) {
		DEBUG_NOTICE("No video source: %m\n", err);
		return err;
	}

	vtx->mute_frame = mem_deref(vtx->mute_frame);
	err = vidframe_alloc(&vtx->mute_frame, VID_FMT_YUV420P, size);
	if (err)
		return err;

	vidframe_fill(vtx->mute_frame, 0xff, 0xff, 0xff);

	return err;
}
#endif


enum {TMR_INTERVAL = 5};
static void tmr_handler(void *arg)
{
	struct video *v = arg;

	tmr_start(&v->tmr, TMR_INTERVAL * 1000, tmr_handler, v);

	/* Estimate framerates */
	v->vtx.efps = v->vtx.frames / TMR_INTERVAL;
	v->vrx.efps = v->vrx.frames / TMR_INTERVAL;

	v->vtx.frames = 0;
	v->vrx.frames = 0;
}


int video_start(struct video *v, const char *src, const char *dev,
		const char *peer)
{
	struct vidsz size;
	int err;

	if (!v)
		return EINVAL;

	if (peer) {
		mem_deref(v->peer);
		err = str_dup(&v->peer, peer);
		if (err)
			return err;
	}

	stream_set_srate(v->strm, SRATE, SRATE);

	err = stream_start(v->strm);
	if (err)
		return err;

#if ENABLE_DECODER
	err = set_vidisp(&v->vrx);
	if (err) {
		DEBUG_WARNING("could not set vidisp: %m\n", err);
	}
#endif

#if ENABLE_ENCODER
	size.w = config.video.width;
	size.h = config.video.height;
	err = set_encoder_format(&v->vtx, src, dev, &size);
	if (err) {
		DEBUG_WARNING("could not set encoder format to"
			      " [%u x %u] %m\n",
			      size.w, size.h, err);
	}
#else
	(void)src;
	(void)dev;
	(void)size;
#endif

	tmr_start(&v->tmr, TMR_INTERVAL * 1000, tmr_handler, v);

	return 0;
}


void video_stop(struct video *v)
{
	if (!v)
		return;

	v->vtx.vsrc = mem_deref(v->vtx.vsrc);
}


/**
 * Mute the video stream
 *
 * @param v     Video stream
 * @param muted True to mute, false to un-mute
 */
void video_mute(struct video *v, bool muted)
{
	struct vtx *vtx;

	if (!v)
		return;

	vtx = &v->vtx;

	vtx->muted        = muted;
	vtx->muted_frames = 0;
	vtx->picup        = true;

	video_update_picture(v);
}


static int vidisp_update(struct vrx *vrx)
{
	struct vidisp *vd = vidisp_get(vrx->vidisp);
	int err = 0;

	if (vd->updateh) {
		err = vd->updateh(vrx->vidisp, vrx->fullscreen,
				  vrx->orient, NULL);
	}

	return err;
}


/**
 * Enable video display fullscreen
 *
 * @param v  Video stream
 * @param fs True for fullscreen, otherwise false
 *
 * @return 0 if success, otherwise errorcode
 */
int video_set_fullscreen(struct video *v, bool fs)
{
	if (!v)
		return EINVAL;

	v->vrx.fullscreen = fs;

	return vidisp_update(&v->vrx);
}


static void vidsrc_update(struct vtx *vtx, const char *dev)
{
	struct vidsrc *vs = vidsrc_get(vtx->vsrc);

	if (vs && vs->updateh)
		vs->updateh(vtx->vsrc, &vtx->vsrc_prm, dev);
}


/**
 * Set the orientation of the Video source and display
 *
 * @param v      Video stream
 * @param orient Video orientation (enum vidorient)
 *
 * @return 0 if success, otherwise errorcode
 */
int video_set_orient(struct video *v, int orient)
{
	if (!v)
		return EINVAL;

	v->vtx.vsrc_prm.orient = v->vrx.orient = orient;
	vidsrc_update(&v->vtx, NULL);
	return vidisp_update(&v->vrx);
}


#if ENABLE_ENCODER
int video_encoder_set(struct video *v, struct vidcodec *vc,
		      int pt_tx, const char *params)
{
	struct vtx *vtx;
	int err = 0;

	if (!v)
		return EINVAL;

	vtx = &v->vtx;

	if (vc != vtx->vc) {

		struct videnc_param prm;

		prm.bitrate = config.video.bitrate;
		prm.pktsize = 1300;
		prm.fps     = get_fps(v);
		prm.max_fs  = -1;

		(void)re_fprintf(stderr, "Set video encoder: %s %s"
				 " (%u bit/s, %u fps)\n",
				 vc->name, vc->variant,
				 prm.bitrate, prm.fps);

		vtx->enc = mem_deref(vtx->enc);
		err = vc->encupdh(&vtx->enc, vc, &prm, params);
		if (err) {
			DEBUG_WARNING("encoder alloc: %m\n", err);
			return err;
		}

		vtx->vc = vc;
	}

	stream_update_encoder(v->strm, pt_tx);

	return err;
}
#else
int video_encoder_set(struct video *v, struct vidcodec *vc,
		      int pt_tx, const char *params)
{
	(void)v;
	(void)vc;
	(void)pt_tx;
	(void)params;

	return 0;
}
#endif


int video_decoder_set(struct video *v, struct vidcodec *vc, int pt_rx,
		      const char *fmtp)
{
	struct vrx *vrx;
	int err = 0;

	if (!v)
		return EINVAL;

	(void)re_fprintf(stderr, "Set video decoder: %s %s\n",
			 vc->name, vc->variant);

	vrx = &v->vrx;

#if ENABLE_DECODER
	vrx->pt_rx = pt_rx;

	if (vc != vrx->vc) {

		vrx->dec = mem_deref(vrx->dec);

		err = vc->decupdh(&vrx->dec, vc, fmtp);
		if (err) {
			DEBUG_WARNING("decoder alloc: %m\n", err);
			return err;
		}

		vrx->vc = vc;
	}
#else
	(void)vc;
	(void)pt_rx;
#endif

	return err;
}


struct stream *video_strm(const struct video *v)
{
	return v ? v->strm : NULL;
}


void video_update_picture(struct video *v)
{
	if (!v)
		return;
	v->vtx.picup = true;
}


/**
 * Get the driver-specific view of the video stream
 *
 * @param v Video stream
 *
 * @return Opaque view
 */
void *video_view(const struct video *v)
{
	if (!v)
		return NULL;

	return v->vrx.vidisp_prm.view;
}


/**
 * Set the current Video Source device name
 *
 * @param v   Video stream
 * @param dev Device name
 */
void video_vidsrc_set_device(struct video *v, const char *dev)
{
	if (!v)
		return;

	vidsrc_update(&v->vtx, dev);
}


static bool sdprattr_contains(struct stream *s, const char *name,
			      const char *str)
{
	const char *attr = sdp_media_rattr(stream_sdpmedia(s), name);
	return attr ? (NULL != strstr(attr, str)) : false;
}


void video_sdp_attr_decode(struct video *v)
{
	if (!v)
		return;

	/* RFC 4585 */
	v->nack_pli = sdprattr_contains(v->strm, "rtcp-fb", "nack");
}


int video_debug(struct re_printf *pf, const struct video *v)
{
	const struct vtx *vtx;
	const struct vrx *vrx;
	int err;

	if (!v)
		return 0;

	vtx = &v->vtx;
	vrx = &v->vrx;

	err = re_hprintf(pf, "\n--- Video stream ---\n");
	err |= re_hprintf(pf, " tx: %d x %d, fps=%d\n",
			  vtx->vsrc_size.w,
			  vtx->vsrc_size.h, vtx->vsrc_prm.fps);
	err |= re_hprintf(pf, " rx: pt=%d\n", vrx->pt_rx);

	err |= stream_debug(pf, v->strm);

	return err;
}


int video_print(struct re_printf *pf, const struct video *v)
{
	if (!v)
		return 0;

	return re_hprintf(pf, " efps=%d/%d", v->vtx.efps, v->vrx.efps);
}


int video_set_source(struct video *v, const char *name, const char *dev)
{
	struct vidsrc *vs = (struct vidsrc *)vidsrc_find(name);
	struct vtx *vtx;

	if (!v)
		return EINVAL;

	if (!vs)
		return ENOENT;

	vtx = &v->vtx;

#if ENABLE_ENCODER
	vtx->vsrc = mem_deref(vtx->vsrc);

	return vs->alloch(&vtx->vsrc, vs, NULL, &vtx->vsrc_prm,
			  &vtx->vsrc_size, NULL, dev,
			  vidsrc_frame_handler, vidsrc_error_handler, vtx);
#else
	(void)dev;
	return ENOSYS;
#endif
}
