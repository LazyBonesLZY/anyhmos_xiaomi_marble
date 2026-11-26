#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Clean-room C loader for gated HDF drivers under preload=2 (libhdi.z.so ABI). */

struct HdfRemoteService;

struct HDIServiceManager {
	struct HdfRemoteService *(*GetService)(struct HDIServiceManager *self, const char *serviceName);
	void *reserved_slot1;
	void *reserved_slot2;
	void *reserved_slot3;
	struct HdfRemoteService *remote;
};
_Static_assert(sizeof(struct HDIServiceManager) == 40, "HDIServiceManager must be 40 bytes");
_Static_assert(offsetof(struct HDIServiceManager, GetService) == 0x00, "GetService offset");
_Static_assert(offsetof(struct HDIServiceManager, remote) == 0x20, "HDIServiceManager.remote offset");

struct HDIDeviceManager {
	struct HdfRemoteService *remote;
	void *reserved_slot1;
	void *reserved_slot2;
	void *reserved_slot3;
	int32_t (*LoadDevice)(struct HDIDeviceManager *self, const char *serviceName);
	int32_t (*UnloadDevice)(struct HDIDeviceManager *self, const char *serviceName);
};
_Static_assert(sizeof(struct HDIDeviceManager) == 48, "HDIDeviceManager must be 48 bytes");
_Static_assert(offsetof(struct HDIDeviceManager, LoadDevice) == 0x20, "LoadDevice offset");
_Static_assert(offsetof(struct HDIDeviceManager, UnloadDevice) == 0x28, "UnloadDevice offset");

typedef struct HDIDeviceManager *(*HDIDeviceManagerGetFn)(void);
typedef void (*HDIDeviceManagerReleaseFn)(struct HDIDeviceManager *);
typedef struct HDIServiceManager *(*HDIServiceManagerGetFn)(void);
typedef void (*HDIServiceManagerReleaseFn)(struct HDIServiceManager *);
typedef void (*HdfRemoteServiceRecycleFn)(struct HdfRemoteService *);

static const char *const kRilService = "ril_service";

int main(int argc, char **argv)
{
	int unload = 0;
	if (argc > 1 && strcmp(argv[1], "--unload") == 0) {
		unload = 1;
	} else if (argc > 1 && strcmp(argv[1], "--load") != 0) {
		fprintf(stderr, "Usage: %s [--load|--unload]\n", argv[0]);
		return 2;
	}

	void *hdi = dlopen("libhdi.z.so", RTLD_NOW);
	if (hdi == NULL)
		hdi = dlopen("/system/lib64/chipset-pub-sdk/libhdi.z.so", RTLD_NOW);
	if (hdi == NULL) {
		printf("RIL_HDI_ERR dlopen_libhdi=\"%s\"\n", dlerror() ? dlerror() : "unknown");
		return 1;
	}

	void *ipc = dlopen("libhdf_ipc_adapter.z.so", RTLD_NOW | RTLD_LOCAL);
	if (ipc == NULL)
		ipc = dlopen("/system/lib64/chipset-pub-sdk/libhdf_ipc_adapter.z.so", RTLD_NOW | RTLD_LOCAL);

	HDIDeviceManagerGetFn devmgr_get = (HDIDeviceManagerGetFn)dlsym(hdi, "HDIDeviceManagerGet");
	HDIDeviceManagerReleaseFn devmgr_rel = (HDIDeviceManagerReleaseFn)dlsym(hdi, "HDIDeviceManagerRelease");
	HDIServiceManagerGetFn servmgr_get = (HDIServiceManagerGetFn)dlsym(hdi, "HDIServiceManagerGet");
	HDIServiceManagerReleaseFn servmgr_rel = (HDIServiceManagerReleaseFn)dlsym(hdi, "HDIServiceManagerRelease");
	HdfRemoteServiceRecycleFn recycle = ipc ? (HdfRemoteServiceRecycleFn)dlsym(ipc, "HdfRemoteServiceRecycle") : NULL;

	if (devmgr_get == NULL || servmgr_get == NULL) {
		printf("RIL_HDI_ERR missing_symbols dev=%p svc=%p\n", (void *)devmgr_get, (void *)servmgr_get);
		return 1;
	}

	struct HDIDeviceManager *devmgr = devmgr_get();
	struct HDIServiceManager *servmgr = servmgr_get();
	if (devmgr == NULL || servmgr == NULL) {
		printf("RIL_HDI_ERR null_mgr devmgr=%p servmgr=%p\n", (void *)devmgr, (void *)servmgr);
		if (devmgr && devmgr_rel) devmgr_rel(devmgr);
		if (servmgr && servmgr_rel) servmgr_rel(servmgr);
		return 1;
	}

	int rc = 0;
	if (unload) {
		int32_t rc_ril = devmgr->UnloadDevice(devmgr, kRilService);
		printf("RIL_HDI_UNLOAD ril_rc=%d\n", rc_ril);
	} else {
		int32_t ril_rc = devmgr->LoadDevice(devmgr, kRilService);
		int svc_ril = 0;
		for (int i = 0; i < 100; i++) {
			struct HdfRemoteService *s1 = servmgr->GetService(servmgr, kRilService);
			svc_ril = (s1 != NULL) ? 1 : 0;
			if (s1 != NULL && recycle != NULL) recycle(s1);
			if (svc_ril)
				break;
			usleep(100000);
		}
		printf("RIL_HDI_LOAD ril_rc=%d svc_ril=%d\n", ril_rc, svc_ril);
		rc = svc_ril ? 0 : 1;
	}

	if (servmgr_rel) servmgr_rel(servmgr);
	if (devmgr_rel) devmgr_rel(devmgr);
	return rc;
}
