/* Bounded donor BMS client launched by native init with an explicit token.
 * No shell, credential changes, permission-check patches, or vendor writes. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef HMOS_BUNDLE_REQUEST
#define HMOS_BUNDLE_REQUEST "/data/local/tmp/hmos-bundle-request.txt"
#endif
#ifndef HMOS_BUNDLE_LOG
#define HMOS_BUNDLE_LOG "/data/local/tmp/hmos-bundle-last.log"
#endif
#ifndef HMOS_BM_PATH
#define HMOS_BM_PATH "/system/bin/bm"
#endif
#ifndef HMOS_FIXTURE_HAP
#define HMOS_FIXTURE_HAP "/compat/fixtures/marble-hap-fixture.hap"
#endif
#define HMOS_FIXTURE_BUNDLE "org.example.marble.hapfixture.a91e4f62"

static const char *const apps[] = {
    "ABCreadingHm", "CapCutHm", "DouyinHm", "IqiyiHm", "KuaiShouHm",
    "MeituxiuxiuHm", "MindmasterHm", "WPSHm", "XiaohongshuHm",
    "XiaoyuanAIHm", "XunleiHm", "ZhimuyunHm", "ZuoyebangHm", "ZwcadHm"
};

int main(void)
{
    alarm(240);
    FILE *f = fopen(HMOS_BUNDLE_REQUEST, "r");
    if (!f) return 2;
    char mode[24], name[160] = {0}, extra[2];
    int count = fscanf(f, "%23s %159s %1s", mode, name, extra);
    fclose(f);
    if (count < 1 || count > 2) return 3;
    char path[256];
    int system_core = !strcmp(mode, "install-system");
    int region_china = !strcmp(mode, "install-region");
    int fixture_install = !strcmp(mode, "install-fixture");
    int fixture_uninstall = !strcmp(mode, "uninstall-fixture");
    if ((fixture_install || fixture_uninstall) && count == 2) {
        if (strcmp(name, HMOS_FIXTURE_BUNDLE)) return 4;
        if (fixture_install) {
            struct stat st;
            if (lstat(HMOS_FIXTURE_HAP, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
                (st.st_mode & 022) || st.st_size <= 0) return 4;
        }
    } else if (!strcmp(mode, "install") && count == 2) {
        unsigned i;
        for (i = 0; i < sizeof(apps) / sizeof(apps[0]); ++i)
            if (!strcmp(name, apps[i])) break;
        if (i == sizeof(apps) / sizeof(apps[0])) return 4;
        snprintf(path, sizeof(path), "/preload/app/%s", name);
    } else if (system_core && count == 2) {
        if (strcmp(name, "ArkWebCore") && strcmp(name, "ArkWebCoreLegacy") &&
            strcmp(name, "SoundRecorder")) return 4;
        snprintf(path, sizeof(path), "/system/app/%s", name);
    } else if (region_china && count == 2) {
        if (strcmp(name, "DroiTong")) return 4;
        snprintf(path, sizeof(path), "/sys_prod/variant/region_comm/china/app/%s", name);
    } else if (!strcmp(mode, "dump") && count == 2) {
        for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
            if (!isalnum(*p) && *p != '.' && *p != '_') return 4;
    } else if (strcmp(mode, "list") || count != 1) return 4;
    if (!freopen(HMOS_BUNDLE_LOG, "w", stdout)) return 5;
    dup2(fileno(stdout), STDERR_FILENO);
    setbuf(stdout, NULL);
    printf("NATIVE_BUNDLE_CLI uid=%d gid=%d mode=%s name=%s\n", getuid(), getgid(), mode, name);
    if (fixture_install)
        execl(HMOS_BM_PATH, "bm", "install", "-p", HMOS_FIXTURE_HAP, "-u", "100", (char *)0);
    else if (fixture_uninstall)
        execl(HMOS_BM_PATH, "bm", "uninstall", "-n", HMOS_FIXTURE_BUNDLE, "-u", "100", (char *)0);
    else if (!strcmp(mode, "install") || system_core || region_china) {
        /* Donor preload directories also contain ag_config.json. Pass only
         * package files; install all modules of a bundle in one transaction. */
        DIR *dir = opendir(path);
        if (!dir) { perror("preload directory"); return 7; }
        char files[32][512];
        char *args[39] = {"bm", "install", "-p"};
        unsigned count = 0;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            const char *suffix = strrchr(entry->d_name, '.');
            if (!suffix || (strcmp(suffix, ".hap") && strcmp(suffix, ".hsp"))) continue;
            if (count == 32 || snprintf(files[count], sizeof(files[count]), "%s/%s", path,
                entry->d_name) >= (int)sizeof(files[count])) { closedir(dir); return 8; }
            printf("BUNDLE_MODULE=%s\n", files[count]);
            args[3 + count] = files[count];
            ++count;
        }
        closedir(dir);
        if (!count) return 9;
        args[3 + count] = "-u";
        /* Core singleton ArkWeb installs for user 0; the normal recorder app
         * belongs to the existing foreground user 100. Preserve BMS checks. */
        args[4 + count] = system_core && strcmp(name, "SoundRecorder") ? "0" : "100";
        args[5 + count] = NULL;
        execv(HMOS_BM_PATH, args);
    }
    else if (!strcmp(mode, "dump"))
        execl(HMOS_BM_PATH, "bm", "dump", "-n", name, "-u", "100", (char *)0);
    else
        execl(HMOS_BM_PATH, "bm", "dump", "-a", "-u", "100", (char *)0);
    perror("exec bm");
    return 6;
}
