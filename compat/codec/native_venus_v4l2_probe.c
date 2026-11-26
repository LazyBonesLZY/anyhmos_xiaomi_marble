/* SPDX-License-Identifier: MIT */
/*
 * Read-only Qualcomm Venus V4L2 M2M capability discovery tool for SM7475 (/dev/video32, /dev/video33).
 * Uses strictly read-only VIDIOC_QUERYCAP, VIDIOC_ENUM_FMT, and VIDIOC_G_FMT ioctls.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void fourcc_str(uint32_t f, char out[5])
{
	out[0] = (char)(f & 0xffu);
	out[1] = (char)((f >> 8) & 0xffu);
	out[2] = (char)((f >> 16) & 0xffu);
	out[3] = (char)((f >> 24) & 0xffu);
	for (int i = 0; i < 4; i++) {
		if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e)
			out[i] = '?';
	}
	out[4] = '\0';
}

static void probe_queue(int fd, const char *qname, uint32_t buf_type)
{
	for (uint32_t i = 0; i < 64; i++) {
		struct v4l2_fmtdesc fdsc;
		memset(&fdsc, 0, sizeof(fdsc));
		fdsc.index = i;
		fdsc.type = buf_type;
		if (ioctl(fd, VIDIOC_ENUM_FMT, &fdsc) != 0)
			break;
		char fcc[5];
		fourcc_str(fdsc.pixelformat, fcc);
		printf("VENUS_FMT queue=%s idx=%u fourcc=%s raw=0x%08x flags=0x%x desc=\"%s\"\n",
		       qname, i, fcc, fdsc.pixelformat, fdsc.flags, (const char *)fdsc.description);
	}

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = buf_type;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
		char fcc[5];
		fourcc_str(fmt.fmt.pix_mp.pixelformat, fcc);
		printf("VENUS_GFMT queue=%s w=%u h=%u fourcc=%s planes=%u\n",
		       qname, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
		       fcc, fmt.fmt.pix_mp.num_planes);
	}
}

static int probe_dev(const char *path)
{
	int fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("VENUS_DEV path=%s err=%d(%s)\n", path, errno, strerror(errno));
		return -1;
	}
	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
		printf("VENUS_DEV path=%s querycap_err=%d(%s)\n", path, errno, strerror(errno));
		close(fd);
		return -1;
	}
	printf("VENUS_DEV path=%s driver=\"%s\" card=\"%s\" bus=\"%s\" caps=0x%08x dev_caps=0x%08x\n",
	       path, (const char *)cap.driver, (const char *)cap.card,
	       (const char *)cap.bus_info, cap.capabilities, cap.device_caps);
	probe_queue(fd, "OUT", V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	probe_queue(fd, "CAP", V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *defaults[2] = { "/dev/video32", "/dev/video33" };
	int ok = 0;
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			if (probe_dev(argv[i]) == 0)
				ok++;
		}
	} else {
		for (int i = 0; i < 2; i++) {
			if (probe_dev(defaults[i]) == 0)
				ok++;
		}
	}
	printf("VENUS_PROBE_OK_DEVS=%d\n", ok);
	return ok > 0 ? 0 : 1;
}
