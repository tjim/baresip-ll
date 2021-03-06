/**
 * @file avcapture.m AVFoundation video capture
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <AVFoundation/AVFoundation.h>


static struct vidsrc *vidsrcv[4];


@interface avcap : NSObject < AVCaptureVideoDataOutputSampleBufferDelegate >
{
	AVCaptureSession *sess;
	AVCaptureDeviceInput *input;
	AVCaptureVideoDataOutput *output;
	struct vidsrc_st *vsrc;
}
- (void)setCamera:(const char *)name;
@end


struct vidsrc_st {
	struct vidsrc *vs;
	avcap *cap;
	vidsrc_frame_h *frameh;
	void *arg;
};


static void vidframe_set_pixbuf(struct vidframe *f, const CVImageBufferRef b)
{
	OSType type;
	int i;

	if (!f || !b)
		return;

	type = CVPixelBufferGetPixelFormatType(b);

	switch (type) {

	case kCVPixelFormatType_32BGRA:
		f->fmt = VID_FMT_ARGB;
		break;

	case kCVPixelFormatType_422YpCbCr8:
		f->fmt = VID_FMT_UYVY422;
		break;

	case kCVPixelFormatType_420YpCbCr8Planar:
		f->fmt = VID_FMT_YUV420P;
		break;

	case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
		f->fmt = VID_FMT_NV12;
		break;

	default:
		re_printf("avcapture: pixfmt %c%c%c%c\n",
			  type>>24, type>>16, type>>8, type>>0);
		f->fmt = -1;
		f->data[0] = NULL;
		return;
	}

	f->size.w = (int)CVPixelBufferGetWidth(b);
	f->size.h = (int)CVPixelBufferGetHeight(b);

	if (!CVPixelBufferIsPlanar(b)) {

		f->data[0]     =      CVPixelBufferGetBaseAddress(b);
		f->linesize[0] = (int)CVPixelBufferGetBytesPerRow(b);
		f->data[1]     = f->data[2]     = f->data[3]     = NULL;
		f->linesize[1] = f->linesize[2] = f->linesize[3] = 0;

		return;
	}

	for (i=0; i<4; i++) {
		f->data[i]     = CVPixelBufferGetBaseAddressOfPlane(b, i);
		f->linesize[i] = CVPixelBufferGetBytesPerRowOfPlane(b, i);
	}
}


@implementation avcap


- (NSString *)map_preset:(AVCaptureDevice *)dev sz:(const struct vidsz *)sz
{
	static const struct {
		struct vidsz sz;
		NSString * const * preset;
	} mapv[] = {
		{{ 192, 144}, &AVCaptureSessionPresetLow     },
		{{ 480, 360}, &AVCaptureSessionPresetMedium  },
		{{ 640, 480}, &AVCaptureSessionPresetHigh    },
		{{1280, 720}, &AVCaptureSessionPreset1280x720}
	};
	int i, best = -1;

	for (i=ARRAY_SIZE(mapv)-1; i>=0; i--) {

		NSString *preset = *mapv[i].preset;

		if (![sess canSetSessionPreset:preset] ||
		    ![dev supportsAVCaptureSessionPreset:preset])
			continue;

		if (mapv[i].sz.w >= sz->w && mapv[i].sz.h >= sz->h)
			best = i;
		else
			break;
	}

	if (best >= 0)
		return *mapv[best].preset;
	else {
		NSLog(@"no suitable preset found for %d x %d", sz->w, sz->h);
		return AVCaptureSessionPresetHigh;
	}
}


+ (AVCaptureDevicePosition)get_position:(const char *)name
{
	if (0 == str_casecmp(name, "back"))
		return AVCaptureDevicePositionBack;
	else if (0 == str_casecmp(name, "front"))
		return AVCaptureDevicePositionFront;
	else
		return -1;
}


+ (AVCaptureDevice *)get_device:(AVCaptureDevicePosition)pos
{
	AVCaptureDevice *dev;

	for (dev in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) {
		if (dev.position == pos)
			return dev;
	}

	return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
}


- (void)start:(id)unused
{
	(void)unused;

	[sess startRunning];
}


- (id)init:(struct vidsrc_st *)st
       dev:(const char *)name
      size:(const struct vidsz *)sz
{
	dispatch_queue_t queue;
	AVCaptureDevice *dev;

	self = [super init];
	if (!self)
		return nil;

	vsrc = st;

	dev = [avcap get_device:[avcap get_position:name]];
	if (!dev)
		return nil;

	input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:nil];
	output = [[AVCaptureVideoDataOutput alloc] init];
	sess = [[AVCaptureSession alloc] init];
	if (!input || !output || !sess)
		return nil;

	output.alwaysDiscardsLateVideoFrames = YES;

	queue = dispatch_queue_create("avcapture", NULL);
	[output setSampleBufferDelegate:self queue:queue];
	dispatch_release(queue);

	sess.sessionPreset = [self map_preset:dev sz:sz];

	[sess addInput:input];
	[sess addOutput:output];

	[self start:nil];

	return self;
}


- (void)stop:(id)unused
{
	(void)unused;

	[sess stopRunning];

	if (output) {
		AVCaptureConnection *conn;

		for (conn in output.connections)
			conn.enabled = NO;
	}

	[sess beginConfiguration];
	if (input)
		[sess removeInput:input];
	if (output)
		[sess removeOutput:output];
	[sess commitConfiguration];

	[sess release];
}


- (void)captureOutput:(AVCaptureOutput *)captureOutput
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)conn
{
	const CVImageBufferRef b = CMSampleBufferGetImageBuffer(sampleBuffer);
	struct vidframe vf;

	(void)captureOutput;
	(void)conn;

	if (!vsrc->frameh)
		return;

	CVPixelBufferLockBaseAddress(b, 0);

	vidframe_set_pixbuf(&vf, b);

	if (vidframe_isvalid(&vf))
		vsrc->frameh(&vf, vsrc->arg);

	CVPixelBufferUnlockBaseAddress(b, 0);
}


- (void)setCamera:(const char *)name
{
	AVCaptureDevicePosition pos;
	AVCaptureDevice *dev;

	pos = [avcap get_position:name];

	if (pos == input.device.position)
		return;

	dev = [avcap get_device:pos];
	if (!dev)
		return;

	[sess beginConfiguration];
	[sess removeInput:input];
	input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:nil];
	[sess addInput:input];
	[sess commitConfiguration];
}


@end


static void destructor(void *arg)
{
	struct vidsrc_st *st = arg;

	st->frameh = NULL;

	[st->cap performSelectorOnMainThread:@selector(stop:)
	        withObject:nil
	        waitUntilDone:YES];

	[st->cap release];

	mem_deref(st->vs);
}


static int alloc(struct vidsrc_st **stp, struct vidsrc *vs,
		 struct media_ctx **ctx, struct vidsrc_prm *prm,
		 const struct vidsz *size, const char *fmt,
		 const char *dev, vidsrc_frame_h *frameh,
		 vidsrc_error_h *errorh, void *arg)
{
	NSAutoreleasePool *pool;
	struct vidsrc_st *st;
	int err = 0;

	(void)ctx;
	(void)prm;
	(void)fmt;
	(void)dev;
	(void)errorh;

	if (!stp || !size)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	pool = [NSAutoreleasePool new];

	st->vs     = mem_ref(vs);
	st->frameh = frameh;
	st->arg    = arg;

	st->cap = [[avcap alloc] init:st
				 dev:dev ? dev : "front"
				 size:size];
	if (!st->cap) {
		err = ENODEV;
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	[pool release];

	return err;
}


static void update(struct vidsrc_st *st, struct vidsrc_prm *prm,
		   const char *dev)
{
	(void)prm;

	if (!st)
		return;

	if (dev)
		[st->cap setCamera:dev];
}


static int module_init(void)
{
	AVCaptureDevice *dev = nil;
	NSAutoreleasePool *pool;
	Class cls = NSClassFromString(@"AVCaptureDevice");
	size_t i = 0;
	int err = 0;
	if (!cls)
		return ENOSYS;

	pool = [NSAutoreleasePool new];

	/* populate devices */
	for (dev in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) {

		const char *name = [[dev localizedName] UTF8String];

		if (i >= ARRAY_SIZE(vidsrcv))
			break;

		err = vidsrc_register(&vidsrcv[i++], name, alloc, update);
		if (err)
			break;
	}

	[pool drain];

	return err;
}


static int module_close(void)
{
	size_t i;

	for (i=0; i<ARRAY_SIZE(vidsrcv); i++)
		vidsrcv[i] = mem_deref(vidsrcv[i]);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(avcapture) = {
	"avcapture",
	"vidsrc",
	module_init,
	module_close
};
