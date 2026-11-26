/* Development adapter for explicitly listed, absent SceneBoard dependencies.
 * The donor performs the real CheckSystemAbility IPC. Only a null result for
 * an allowlisted optional SA terminates its enclosing synchronous retry loop.
 * Existing objects and every other process/SA retain their original behavior.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ABI carrier, not an owning replacement for OHOS::sptr. Both have one pointer
// and a nontrivial destructor, hence the AArch64 indirect-result x8 convention.
// The donor constructs/acquires the result; the original caller destroys it.
// Local carriers never acquire/release another reference, even without NRVO.
struct RemoteRefCarrier {
    void *pointer = nullptr;
    ~RemoteRefCarrier() {}
};
static bool listed_optional_sa(int id)
{
    static int cached_ids[128];
    static int cached_count = -1;
    int count = __atomic_load_n(&cached_count, __ATOMIC_ACQUIRE);
    if (count < 0) {
        char list[2048] = {};
        int n_ids = 0;
        int fd = open("/compat/etc/sceneboard-optional-sa.list", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, list, sizeof(list) - 1);
            close(fd);
            if (n > 0 && n < ssize_t(sizeof(list) - 1)) {
                char *p = list;
                while (*p && n_ids < 128) {
                    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
                    if (!*p) break;
                    char *end = nullptr;
                    long candidate = strtol(p, &end, 10);
                    if (end == p || (*end && *end != ' ' && *end != '\n' && *end != '\t' && *end != '\r')) {
                        n_ids = 0;
                        break;
                    }
                    cached_ids[n_ids++] = static_cast<int>(candidate);
                    p = end;
                }
            }
        }
        __atomic_store_n(&cached_count, n_ids, __ATOMIC_RELEASE);
        count = n_ids;
    }
    for (int i = 0; i < count; ++i) {
        if (cached_ids[i] == id) return true;
    }
    return false;
}
using NextCheck = RemoteRefCarrier (*)(void *, int, bool *, int *);
static void *next_check;
extern "C" RemoteRefCarrier hmos_optional_check(void *, int, bool *, int *)
    __asm__("_ZN4OHOS25SystemAbilityManagerProxy18CheckSystemAbilityEiRbRi");
extern "C" RemoteRefCarrier hmos_optional_check(void *self, int id, bool *exists, int *error)
{
    void *symbol = __atomic_load_n(&next_check, __ATOMIC_ACQUIRE);
    if (!symbol) {
        symbol = dlsym(RTLD_NEXT, "_ZN4OHOS25SystemAbilityManagerProxy18CheckSystemAbilityEiRbRi");
        if (symbol) __atomic_store_n(&next_check, symbol, __ATOMIC_RELEASE);
    }
    if (!symbol) {
        if (exists) *exists = false;
        if (error) *error = 1;
        return {};
    }
    RemoteRefCarrier result = reinterpret_cast<NextCheck>(symbol)(self, id, exists, error);
    if (!result.pointer && error && listed_optional_sa(id)) {
        // The donor wrapper's error==1 branch exits with a null object; no
        // connected/enabled state or successful operation is fabricated.
        *error = 1;
        auto print = reinterpret_cast<int (*)(int,int,unsigned,const char *,const char *,...)>(dlsym(RTLD_DEFAULT, "HiLogPrint"));
        if (print) print(3, 5, 0xD001810, "HMOS_OPTIONAL_SA", "absent optional SA %{public}d: stop app retry", id);
    }
    return result;
}
using NextLoadTimeout = RemoteRefCarrier (*)(void *, int, int);
static void *next_load_timeout;
extern "C" RemoteRefCarrier hmos_optional_load_timeout(void *, int, int)
    __asm__("_ZN4OHOS25SystemAbilityManagerProxy17LoadSystemAbilityEii");
extern "C" RemoteRefCarrier hmos_optional_load_timeout(void *self, int id, int timeout)
{
    if (listed_optional_sa(id)) {
        bool exists = false;
        int error = 0;
        RemoteRefCarrier existing = hmos_optional_check(self, id, &exists, &error);
        if (existing.pointer) return existing;
        auto print = reinterpret_cast<int (*)(int,int,unsigned,const char *,const char *,...)>(dlsym(RTLD_DEFAULT, "HiLogPrint"));
        if (print) print(3, 5, 0xD001810, "HMOS_OPTIONAL_SA", "absent optional SA %{public}d: skip LoadSystemAbility timeout=%{public}d", id, timeout);
        return {};
    }
    void *symbol = __atomic_load_n(&next_load_timeout, __ATOMIC_ACQUIRE);
    if (!symbol) {
        symbol = dlsym(RTLD_NEXT, "_ZN4OHOS25SystemAbilityManagerProxy17LoadSystemAbilityEii");
        if (symbol) __atomic_store_n(&next_load_timeout, symbol, __ATOMIC_RELEASE);
    }
    if (!symbol) return {};
    return reinterpret_cast<NextLoadTimeout>(symbol)(self, id, timeout);
}
using NextLoadCallback = int (*)(void *, int, const void *);
static void *next_load_callback;
extern "C" int hmos_optional_load_callback(void *, int, const void *)
    __asm__("_ZN4OHOS25SystemAbilityManagerProxy17LoadSystemAbilityEiRKNS_4sptrINS_26ISystemAbilityLoadCallbackEEE");
extern "C" int hmos_optional_load_callback(void *self, int id, const void *callback)
{
    if (listed_optional_sa(id)) {
        bool exists = false;
        int error = 0;
        RemoteRefCarrier existing = hmos_optional_check(self, id, &exists, &error);
        if (!existing.pointer) {
            auto print = reinterpret_cast<int (*)(int,int,unsigned,const char *,const char *,...)>(dlsym(RTLD_DEFAULT, "HiLogPrint"));
            if (print) print(3, 5, 0xD001810, "HMOS_OPTIONAL_SA", "absent optional SA %{public}d: reject LoadSystemAbility(cb)", id);
            return -1;
        }
    }
    void *symbol = __atomic_load_n(&next_load_callback, __ATOMIC_ACQUIRE);
    if (!symbol) {
        symbol = dlsym(RTLD_NEXT, "_ZN4OHOS25SystemAbilityManagerProxy17LoadSystemAbilityEiRKNS_4sptrINS_26ISystemAbilityLoadCallbackEEE");
        if (symbol) __atomic_store_n(&next_load_callback, symbol, __ATOMIC_RELEASE);
    }
    if (!symbol) return -1;
    return reinterpret_cast<NextLoadCallback>(symbol)(self, id, callback);
}
