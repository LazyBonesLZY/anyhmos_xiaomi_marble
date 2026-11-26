/* Sample one linear KMS plane framebuffer, without changing display state. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>

/* Kernel values for GETCONNECTOR.connection and the plane's immutable "type". */
enum { KMS_CONNECTED = 1, PLANE_OVERLAY = 0, PLANE_PRIMARY = 1, PLANE_CURSOR = 2 };
enum { MAX_OBJECTS = 256, MAX_PROPERTIES = 128, MAX_DIMENSION = 16384 };

struct route {
    uint32_t connector, connector_type, connector_index, crtc;
    unsigned width, height;
};

/* The legacy positional arguments still work; '-' sends only P6 to stdout. */
static void usage(const char *name)
{
    fprintf(stderr, "usage: %s [--crtc ID] [--plane ID] [--] <output.ppm|-> [step:1..16]\n", name);
}

static int parse_positive(const char *s, uint32_t max, uint32_t *value)
{
    char *end;
    if (*s < '0' || *s > '9')
        return -1;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || !n || n > max)
        return -1;
    *value = (uint32_t)n;
    return 0;
}

static int call(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static int read_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags | DMA_BUF_SYNC_READ };
    for (int i = 0; i < 4; ++i) {
        int ret = call(fd, DMA_BUF_IOCTL_SYNC, &sync);
        if (!ret || errno != EAGAIN)
            return ret;
    }
    return -1;
}

static int writeall(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n) {
        ssize_t written = write(fd, p, n);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            if (!written)
                errno = EIO;
            return -1;
        }
        p += written;
        n -= (size_t)written;
    }
    return 0;
}

static int property(int drm, uint32_t id, uint32_t type, const char *name, uint64_t *value)
{
    uint32_t ids[MAX_PROPERTIES];
    uint64_t values[MAX_PROPERTIES];
    struct drm_mode_obj_get_properties props = {
        .obj_id = id, .obj_type = type, .count_props = MAX_PROPERTIES,
        .props_ptr = (uintptr_t)ids, .prop_values_ptr = (uintptr_t)values,
    };
    if (call(drm, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props))
        return -1;
    if (props.count_props > MAX_PROPERTIES) {
        errno = EOVERFLOW;
        return -1;
    }
    for (uint32_t i = 0; i < props.count_props; ++i) {
        struct drm_mode_get_property prop = { .prop_id = ids[i] };
        if (call(drm, DRM_IOCTL_MODE_GETPROPERTY, &prop))
            return -1;
        if (!strncmp(prop.name, name, sizeof(prop.name))) {
            *value = values[i];
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

/* Follow the current encoder, not its possible_crtcs mask. */
static int connected_route(int drm, int atomic, uint32_t id, uint32_t requested_crtc,
                           struct route *out)
{
    struct drm_mode_modeinfo unused_mode;
    /* count_modes=0 can force a connector probe; use the cached state. */
    struct drm_mode_get_connector connector = {
        .connector_id = id, .count_modes = 1, .modes_ptr = (uintptr_t)&unused_mode,
    };
    if (call(drm, DRM_IOCTL_MODE_GETCONNECTOR, &connector)) {
        perror("GETCONNECTOR");
        return -1;
    }
    if (connector.connection != KMS_CONNECTED || !connector.encoder_id ||
        connector.connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
        return 0;

    struct drm_mode_get_encoder encoder = { .encoder_id = connector.encoder_id };
    if (call(drm, DRM_IOCTL_MODE_GETENCODER, &encoder)) {
        perror("GETENCODER");
        return -1;
    }
    if (!encoder.crtc_id || (requested_crtc && encoder.crtc_id != requested_crtc))
        return 0;

    struct drm_mode_crtc crtc = { .crtc_id = encoder.crtc_id };
    if (call(drm, DRM_IOCTL_MODE_GETCRTC, &crtc)) {
        perror("GETCRTC");
        return -1;
    }
    if (!crtc.mode_valid)
        return 0;
    if (atomic) {
        uint64_t active;
        /* GETCRTC.mode_valid alone means enabled, not necessarily ACTIVE. */
        if (property(drm, crtc.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &active)) {
            perror("CRTC ACTIVE");
            return -1;
        }
        if (active != 1)
            return 0;
    }
    *out = (struct route){
        .connector = id, .connector_type = connector.connector_type,
        .connector_index = connector.connector_type_id, .crtc = crtc.crtc_id,
        .width = crtc.mode.hdisplay, .height = crtc.mode.vdisplay,
    };
    return 1;
}

static int select_route(int drm, int atomic, uint32_t requested_crtc, struct route *route)
{
    uint32_t ids[MAX_OBJECTS];
    struct drm_mode_card_res res = {
        .connector_id_ptr = (uintptr_t)ids, .count_connectors = MAX_OBJECTS,
    };
    if (call(drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        perror("GETRESOURCES");
        return -1;
    }
    if (res.count_connectors > MAX_OBJECTS) {
        fprintf(stderr, "too many DRM connectors (%u)\n", res.count_connectors);
        return -1;
    }

    int priority = 0, ambiguous = 0;
    for (uint32_t i = 0; i < res.count_connectors; ++i) {
        struct route candidate;
        int found = connected_route(drm, atomic, ids[i], requested_crtc, &candidate);
        if (found < 0)
            return -1;
        if (!found)
            continue;
        /* marble's built-in OLED is connected via DSI, not a fixed object ID. */
        int rank = candidate.connector_type == DRM_MODE_CONNECTOR_DSI ? 2 : 1;
        if (rank > priority) {
            *route = candidate;
            priority = rank;
            ambiguous = 0;
        } else if (rank == priority) {
            if (candidate.crtc != route->crtc)
                ambiguous = 1;
            else if (candidate.connector < route->connector)
                *route = candidate;
        }
    }
    if (!priority)
        fprintf(stderr, "no connected active CRTC (requested=%u)\n", requested_crtc);
    else if (ambiguous)
        fprintf(stderr, "ambiguous connected active CRTCs; select --crtc ID or --plane ID\n");
    return priority && !ambiguous ? 0 : -1;
}

static int get_plane(int drm, uint32_t id, struct drm_mode_get_plane *plane)
{
    *plane = (struct drm_mode_get_plane){ .plane_id = id };
    return call(drm, DRM_IOCTL_MODE_GETPLANE, plane);
}

static int select_plane(int drm, uint32_t crtc, struct drm_mode_get_plane *plane, uint64_t *type)
{
    uint32_t ids[MAX_OBJECTS];
    struct drm_mode_get_plane_res res = {
        .plane_id_ptr = (uintptr_t)ids, .count_planes = MAX_OBJECTS,
    };
    if (call(drm, DRM_IOCTL_MODE_GETPLANERESOURCES, &res)) {
        perror("GETPLANERESOURCES");
        return -1;
    }
    if (res.count_planes > MAX_OBJECTS) {
        fprintf(stderr, "too many DRM planes (%u)\n", res.count_planes);
        return -1;
    }
    for (uint32_t i = 0; i < res.count_planes; ++i) {
        struct drm_mode_get_plane candidate;
        if (get_plane(drm, ids[i], &candidate)) {
            perror("GETPLANE");
            return -1;
        }
        if (candidate.crtc_id != crtc || !candidate.fb_id)
            continue;
        uint64_t kind;
        if (property(drm, candidate.plane_id, DRM_MODE_OBJECT_PLANE, "type", &kind)) {
            perror("plane type");
            return -1;
        }
        if (kind != PLANE_PRIMARY && kind != PLANE_OVERLAY)
            continue;
        if (!plane->plane_id || kind > *type || (kind == *type && candidate.plane_id < plane->plane_id)) {
            *plane = candidate;
            *type = kind;
        }
    }
    if (plane->plane_id)
        return 0;
    fprintf(stderr, "no non-cursor plane with FB on CRTC %u\n", crtc);
    return -1;
}

static const char *plane_type_name(uint64_t type)
{
    switch (type) {
    case PLANE_PRIMARY: return "primary";
    case PLANE_OVERLAY: return "overlay";
    case PLANE_CURSOR: return "cursor";
    default: return "unknown";
    }
}

int main(int argc, char **argv)
{
    uint32_t plane_id = 0, crtc_id = 0, step = 1;
    int arg = 1;
    while (arg < argc && !strncmp(argv[arg], "--", 2)) {
        const char *option = argv[arg++];
        if (!strcmp(option, "--"))
            break;
        uint32_t *id = !strcmp(option, "--plane") ? &plane_id :
                       !strcmp(option, "--crtc") ? &crtc_id : NULL;
        if (!id || *id || arg >= argc || parse_positive(argv[arg++], UINT32_MAX, id)) {
            usage(argv[0]);
            return 2;
        }
    }
    if (argc - arg < 1 || argc - arg > 2 ||
        (argc - arg == 2 && parse_positive(argv[arg + 1], 16, &step))) {
        usage(argv[0]);
        return 2;
    }
    const char *path = argv[arg];
    int to_stdout = !strcmp(path, "-");
    if (to_stdout && signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("SIGPIPE");
        return 2;
    }

    /* Opening the primary node can get the kernel's default master if idle;
     * never request master or displace a running compositor. */
    int drm = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
    if (drm < 0) {
        perror("drm");
        return 3;
    }
    int ret = 4, dma = -1, output = -1, synced = 0, atomic = 1;
    const unsigned char *map = MAP_FAILED;
    unsigned char *row = NULL;
    size_t size = 0;
    uint32_t width = 0, height = 0;
    uint64_t nonzero_pixels = 0, nonzero_bytes = 0;
    struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1 };
    if (call(drm, DRM_IOCTL_SET_CLIENT_CAP, &cap)) {
        perror("universal planes client cap");
        goto done;
    }
    /* Client-local property visibility only; there is no atomic commit. */
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    if (call(drm, DRM_IOCTL_SET_CLIENT_CAP, &cap)) {
        if (errno != EINVAL && errno != EOPNOTSUPP) {
            perror("atomic property client cap");
            goto done;
        }
        atomic = 0;
    }

    ret = 5;
    uint64_t type = UINT64_MAX;
    struct drm_mode_get_plane plane = {0};
    struct route route;
    if (plane_id) {
        if (get_plane(drm, plane_id, &plane)) {
            perror("GETPLANE explicit");
            goto done;
        }
        if (!plane.crtc_id || !plane.fb_id) {
            fprintf(stderr, "plane %u not bound to a CRTC and FB\n", plane_id);
            goto done;
        }
        if (crtc_id && plane.crtc_id != crtc_id) {
            fprintf(stderr, "plane %u on CRTC %u, expected CRTC %u\n", plane_id, plane.crtc_id, crtc_id);
            goto done;
        }
        if (property(drm, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type))
            type = UINT64_MAX;
    }
    if (select_route(drm, atomic, plane_id ? plane.crtc_id : crtc_id, &route) ||
        (!plane_id && select_plane(drm, route.crtc, &plane, &type)))
        goto done;

    ret = 6;
    struct drm_mode_fb_cmd2 fb = { .fb_id = plane.fb_id };
    if (call(drm, DRM_IOCTL_MODE_GETFB2, &fb)) {
        perror("GETFB2");
        goto done;
    }
    fprintf(stderr, "SCANOUT_CAPTURE plane=%u fb=%u width=%u height=%u pitch=%u format=%08x "
            "modifier=%" PRIx64 " offset=%u crtc=%u connector=%u connector_type=%u connector_index=%u "
            "mode=%ux%u plane_type=%s selection=%s crtc_state=%s\n",
            plane.plane_id, fb.fb_id, fb.width, fb.height, fb.pitches[0], fb.pixel_format,
            (uint64_t)fb.modifier[0], fb.offsets[0], route.crtc, route.connector,
            route.connector_type, route.connector_index, route.width, route.height,
            plane_type_name(type), plane_id ? "plane" : crtc_id ? "crtc" : "auto",
            atomic ? "atomic_active" : "legacy_mode_valid");

    ret = 7;
    if (!fb.handles[0]) {
        fprintf(stderr, "GETFB2 supplied no GEM handle (check driver/access permissions)\n");
        goto done;
    }
    int rgba = fb.pixel_format == DRM_FORMAT_ABGR8888 || fb.pixel_format == DRM_FORMAT_XBGR8888;
    int bgra = fb.pixel_format == DRM_FORMAT_ARGB8888 || fb.pixel_format == DRM_FORMAT_XRGB8888;
    if ((!rgba && !bgra) || fb.modifier[0] != DRM_FORMAT_MOD_LINEAR ||
        !fb.width || !fb.height || fb.width > MAX_DIMENSION || fb.height > MAX_DIMENSION ||
        fb.pitches[0] < (uint64_t)fb.width * 4) {
        fprintf(stderr, "unsupported FB layout (need bounded linear RGB/BGR 8888 with valid pitch)\n");
        goto done;
    }
    /* No need to map padding after the last pixel row. Avoid 32-bit overflow. */
    uint64_t end = (uint64_t)fb.offsets[0] + (uint64_t)fb.pitches[0] * (fb.height - 1) +
                   (uint64_t)fb.width * 4;
    if (end > SIZE_MAX || end > PTRDIFF_MAX) {
        fprintf(stderr, "FB mapping length out of range\n");
        goto done;
    }
    size = (size_t)end;

    ret = 8;
    struct drm_prime_handle prime = { .handle = fb.handles[0], .flags = DRM_CLOEXEC, .fd = -1 };
    if (call(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
        perror("PRIME export");
        goto done;
    }
    dma = prime.fd;
    ret = 9;
    off_t dma_size = lseek(dma, 0, SEEK_END);
    if (dma_size < 0) {
        perror("DMA-BUF size");
        goto done;
    }
    if ((uint64_t)dma_size < end) {
        fprintf(stderr, "FB exceeds DMA-BUF size (need=%" PRIu64 ", have=%" PRIu64 ")\n",
                end, (uint64_t)dma_size);
        goto done;
    }
    map = mmap(NULL, size, PROT_READ, MAP_SHARED, dma, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        goto done;
    }
    ret = 10;
    if (read_sync(dma, DMA_BUF_SYNC_START)) {
        perror("sync start");
        goto done;
    }
    synced = 1;

    /* Refuse a detectable route change while preparing the DMA-BUF read. */
    ret = 5;
    struct route current;
    if (connected_route(drm, atomic, route.connector, route.crtc, &current) != 1 ||
        current.width != route.width || current.height != route.height) {
        fprintf(stderr, "display route changed or unavailable before sampling\n");
        goto done;
    }
    struct drm_mode_get_plane binding;
    if (get_plane(drm, plane.plane_id, &binding) ||
        binding.crtc_id != plane.crtc_id || binding.fb_id != plane.fb_id) {
        fprintf(stderr, "plane binding changed before sampling or became unavailable\n");
        goto done;
    }

    width = (fb.width + step - 1) / step;
    height = (fb.height + step - 1) / step;
    ret = 13;
    row = malloc((size_t)width * 3);
    if (!row) {
        perror("PPM row allocation");
        goto done;
    }
    ret = 11;
    output = to_stdout ? STDOUT_FILENO : open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) {
        perror("output");
        goto done;
    }
    ret = 12;
    char header[64];
    int n = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", width, height);
    if (n < 0 || (size_t)n >= sizeof(header) || writeall(output, header, (size_t)n)) {
        perror("write PPM header");
        goto done;
    }
    ret = 14;
    for (uint32_t y = 0; y < fb.height; y += step) {
        const unsigned char *src = map + fb.offsets[0] + (size_t)y * fb.pitches[0];
        for (uint32_t x = 0, j = 0; x < fb.width; x += step, j += 3) {
            unsigned r = src[x * 4 + (rgba ? 0 : 2)], g = src[x * 4 + 1];
            unsigned b = src[x * 4 + (rgba ? 2 : 0)];
            row[j] = r;
            row[j + 1] = g;
            row[j + 2] = b;
            nonzero_pixels += (r | g | b) != 0;
            nonzero_bytes += (r != 0) + (g != 0) + (b != 0);
        }
        if (writeall(output, row, (size_t)width * 3)) {
            perror("write PPM row");
            goto done;
        }
    }
    ret = 0;

done:
    if (output >= 0 && !to_stdout && close(output)) {
        perror("close PPM output");
        if (!ret)
            ret = 14;
    }
    if (synced && read_sync(dma, DMA_BUF_SYNC_END)) {
        perror("sync end");
        if (!ret)
            ret = 15;
    }
    free(row);
    if (map != MAP_FAILED)
        munmap((void *)map, size);
    if (dma >= 0)
        close(dma);
    close(drm); /* Releases the temporary GETFB2 GEM handles. */
    if (!ret)
        fprintf(stderr, "SCANOUT_CAPTURE_OK source=actual_KMS_DMA_BUF no_modeset=1 step=%u "
                "output=%ux%u sampled_pixels=%" PRIu64 " nonzero_pixels=%" PRIu64
                " nonzero_bytes=%" PRIu64 " scope=single_plane_fb_rgb "
                "composited=0 atomic_snapshot=0 panel_visibility=unverified\n",
                step, width, height, (uint64_t)width * height, nonzero_pixels, nonzero_bytes);
    return ret;
}
