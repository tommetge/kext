/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2012 Benjamin Fleischer
 * All rights reserved.
 */

#include <err.h>
#include <libgen.h>
#include <sysexits.h>
#include <paths.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <libgen.h>
#include <signal.h>
#include <mach/mach.h>

#include "mntopts.h"
#include <fuse_ioctl.h>
#include <fuse_mount.h>
#include <fuse_param.h>
#include <fuse_version.h>

#include <fsproperties.h>
#include <CoreFoundation/CoreFoundation.h>

#define PROGNAME "mount_" OSXFUSE_FS_TYPE

static int signal_idx = -1;
static int signal_fd  = -1;

void  showhelp(void);
void  showversion(int doexit);

struct mntopt mopts[] = {
    MOPT_STDOPTS,
    MOPT_UPDATE,
    { "allow_other",         0, FUSE_MOPT_ALLOW_OTHER,            1 }, // kused
    { "allow_recursion",     0, FUSE_MOPT_ALLOW_RECURSION,        1 }, // uused
    { "allow_root",          0, FUSE_MOPT_ALLOW_ROOT,             1 }, // kused
    { "auto_cache",          0, FUSE_MOPT_AUTO_CACHE,             1 }, // kused
    { "auto_xattr",          0, FUSE_MOPT_AUTO_XATTR,             1 }, // kused
    { "blocksize=",          0, FUSE_MOPT_BLOCKSIZE,              1 }, // kused
    { "daemon_timeout=",     0, FUSE_MOPT_DAEMON_TIMEOUT,         1 }, // kused
    { "debug",               0, FUSE_MOPT_DEBUG,                  1 }, // kused
    { "default_permissions", 0, FUSE_MOPT_DEFAULT_PERMISSIONS,    1 }, // kused
    { "defer_permissions",   0, FUSE_MOPT_DEFER_PERMISSIONS,      1 }, // kused
    { "direct_io",           0, FUSE_MOPT_DIRECT_IO,              1 }, // kused
    { "extended_security",   0, FUSE_MOPT_EXTENDED_SECURITY,      1 }, // kused
    { "fsid=" ,              0, FUSE_MOPT_FSID,                   1 }, // kused
    { "fsname=",             0, FUSE_MOPT_FSNAME,                 1 }, // kused
    { "fssubtype=",          0, FUSE_MOPT_FSSUBTYPE,              1 }, // kused
    { "fstypename=",         0, FUSE_MOPT_FSTYPENAME,             1 }, // kused
    { "init_timeout=",       0, FUSE_MOPT_INIT_TIMEOUT,           1 }, // kused
    { "iosize=",             0, FUSE_MOPT_IOSIZE,                 1 }, // kused
    { "jail_symlinks",       0, FUSE_MOPT_JAIL_SYMLINKS,          1 }, // kused
    { "local",               0, FUSE_MOPT_LOCALVOL,               1 }, // kused
    { "native_xattr",        0, FUSE_MOPT_NATIVE_XATTR,           1 }, // kused
    { "negative_vncache",    0, FUSE_MOPT_NEGATIVE_VNCACHE,       1 }, // kused
    { "sparse",              0, FUSE_MOPT_SPARSE,                 1 }, // kused
    { "slow_statfs",         0, FUSE_MOPT_SLOW_STATFS,            1 }, // kused
    { "use_ino",             0, FUSE_MOPT_USE_INO,                1 },
    { "volname=",            0, FUSE_MOPT_VOLNAME,                1 }, // kused

    /* negative ones */

    { "appledouble",         1, FUSE_MOPT_NO_APPLEDOUBLE,         1 }, // kused
    { "applexattr",          1, FUSE_MOPT_NO_APPLEXATTR,          1 }, // kused
    { "attrcache",           1, FUSE_MOPT_NO_ATTRCACHE,           1 }, // kused
    { "browse",              1, FUSE_MOPT_NO_BROWSE,              1 }, // kused
    { "localcaches",         1, FUSE_MOPT_NO_LOCALCACHES,         1 }, // kused
    { "readahead",           1, FUSE_MOPT_NO_READAHEAD,           1 }, // kused
    { "synconclose",         1, FUSE_MOPT_NO_SYNCONCLOSE,         1 }, // kused
    { "syncwrites",          1, FUSE_MOPT_NO_SYNCWRITES,          1 }, // kused
    { "ubc",                 1, FUSE_MOPT_NO_UBC,                 1 }, // kused
    { "vncache",             1, FUSE_MOPT_NO_VNCACHE,             1 }, // kused

    { NULL }
};

typedef int (* converter_t)(void **target, void *value, void *fallback);

struct mntval {
    uint64_t    mv_mntflag;
    void       *mv_value;
    size_t      mv_len;
    converter_t mv_converter;
    void       *mv_fallback;
    void      **mv_target;
    char       *mv_errstr;
};

static __inline__ int
fuse_to_string(void **target, void *value, void *fallback)
{
    if (!value) {
        // think about what to do if we want to set a NULL value when the
        // fallback value is non-NULL
        value = fallback;
    }

    *target = value;

    return 0;
}

static __inline__ int
fuse_to_uint32(void **target, void *value, void *fallback)
{
    unsigned long u;

    if (!value) {
        *target = fallback;
        return 0;
    }

    errno = 0;
    u = strtoul((char *)value, NULL, 10);
    if ((errno == ERANGE) || (errno == EINVAL)) {
        *target = fallback;
        return errno;
    }

    *target = (void *)u;

    return 0;
}

static __inline__ int
fuse_to_fsid(void **target, void *value, void *fallback)
{
    int ret;
    uint32_t u;

    if (!value) {
        *target = fallback;
        return 0;
    }

    ret = fuse_to_uint32(target, value, fallback);

    if (ret) {
        return ret;
    }

    u = *(uint32_t *)target;

    if ((u & ~FUSE_MINOR_MASK) || (u == 0)) {
        return EINVAL;
    }

    return 0;
}

static uint32_t
fsbundle_find_fssubtype(const char *bundle_path_C,
                        const char *claimed_name_C,
                        uint32_t    claimed_fssubtype)
{
    uint32_t result = FUSE_FSSUBTYPE_UNKNOWN;

    CFStringRef bundle_path_string  = NULL;
    CFStringRef claimed_name_string = NULL;

    CFURLRef    bundleURL = NULL;
    CFBundleRef bundleRef = NULL;

    CFDictionaryRef fspersonalities = NULL;

    CFIndex idx   = 0;
    CFIndex count = 0;
    Boolean found = false;

    CFStringRef     *keys     = NULL;
    CFDictionaryRef *subdicts = NULL;

    bundle_path_string = CFStringCreateWithCString(kCFAllocatorDefault,
                                                   bundle_path_C,
                                                   kCFStringEncodingUTF8);
    if (!bundle_path_string) {
        goto out;
    }

    bundleURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                              bundle_path_string,
                                              kCFURLPOSIXPathStyle,
                                              true);
    if (!bundleURL) {
        goto out;
    }

    bundleRef = CFBundleCreate(kCFAllocatorDefault, bundleURL);
    if (!bundleRef) {
        goto out;
    }

    fspersonalities = CFBundleGetValueForInfoDictionaryKey(
                          bundleRef, CFSTR(kFSPersonalitiesKey));
    if (!fspersonalities) {
        goto out;
    }

    count = CFDictionaryGetCount(fspersonalities);
    if (count <= 0) {
        goto out;
    }

    keys = (CFStringRef *)malloc(count * sizeof(CFStringRef));
    subdicts = (CFDictionaryRef *)malloc(count * sizeof(CFDictionaryRef));

    if (!keys || !subdicts) {
        goto out;
    }

    CFDictionaryGetKeysAndValues(fspersonalities,
                                 (const void **)keys,
                                 (const void **)subdicts);

    if (claimed_fssubtype == (uint32_t)FUSE_FSSUBTYPE_INVALID) {
        goto lookupbyfsname;
    }

    for (idx = 0; idx < count; idx++) {
        CFNumberRef n = NULL;
        uint32_t candidate_fssubtype = (uint32_t)FUSE_FSSUBTYPE_INVALID;
        if (CFDictionaryGetValueIfPresent(subdicts[idx],
                                          (const void *)CFSTR(kFSSubTypeKey),
                                          (const void **)&n)) {
            if (CFNumberGetValue(n, kCFNumberIntType, &candidate_fssubtype)) {
                if (candidate_fssubtype == claimed_fssubtype) {
                    found = true;
                    result = candidate_fssubtype;
                    break;
                }
            }
        }
    }

    if (found) {
        goto out;
    }

lookupbyfsname:

    claimed_name_string = CFStringCreateWithCString(kCFAllocatorDefault,
                                                    claimed_name_C,
                                                    kCFStringEncodingUTF8);
    if (!claimed_name_string) {
        goto out;
    }

    for (idx = 0; idx < count; idx++) {
        CFRange where = CFStringFind(claimed_name_string, keys[idx],
                                     kCFCompareCaseInsensitive);
        if (where.location != kCFNotFound) {
            found = true;
        }
        if (found) {
            CFNumberRef n = NULL;
            uint32_t candidate_fssubtype = (uint32_t)FUSE_FSSUBTYPE_INVALID;
            if (CFDictionaryGetValueIfPresent(
                    subdicts[idx], (const void *)CFSTR(kFSSubTypeKey),
                    (const void **)&n)) {
                if (CFNumberGetValue(n, kCFNumberIntType,
                                     &candidate_fssubtype)) {
                    result = candidate_fssubtype;
                }
            }
            break;
        }
    }

out:
    if (keys) {
        free(keys);
    }

    if (subdicts) {
        free(subdicts);
    }

    if (bundle_path_string) {
        CFRelease(bundle_path_string);
    }

    if (bundleURL) {
        CFRelease(bundleURL);
    }

    if (claimed_name_string) {
        CFRelease(claimed_name_string);
    }

    if (bundleRef) {
        CFRelease(bundleRef);
    }

    return result;
}

static __inline__ int
fuse_to_fssubtype(void **target, void *value, void *fallback)
{
    char *name = getenv("MOUNT_FUSEFS_DAEMON_PATH");

    *(uint32_t *)target = (uint32_t)FUSE_FSSUBTYPE_INVALID;

    if (value) {
        int ret = fuse_to_uint32(target, value, fallback);
        if (ret) {
            *(uint32_t *)target = (uint32_t)FUSE_FSSUBTYPE_INVALID;
        }
    }

    *(uint32_t *)target = fsbundle_find_fssubtype(OSXFUSE_BUNDLE_PATH,
                                                  name, *(uint32_t *)target);

    return 0;
}

static uintptr_t blocksize      = FUSE_DEFAULT_BLOCKSIZE;
static uintptr_t daemon_timeout = FUSE_DEFAULT_DAEMON_TIMEOUT;
static uintptr_t fsid           = 0;
static char     *fsname         = NULL;
static uintptr_t fssubtype      = 0;
static char     *fstypename     = NULL;
static uintptr_t init_timeout   = FUSE_DEFAULT_INIT_TIMEOUT;
static uintptr_t iosize         = FUSE_DEFAULT_IOSIZE;
static uint32_t  drandom        = 0;
static char     *volname        = NULL;

struct mntval mvals[] = {
    {
        FUSE_MOPT_BLOCKSIZE,
        NULL,
        0,
        fuse_to_uint32,
        (void *)FUSE_DEFAULT_BLOCKSIZE,
        (void **)&blocksize,
        "invalid value for argument blocksize"
    },
    {
        FUSE_MOPT_DAEMON_TIMEOUT,
        NULL,
        0,
        fuse_to_uint32,
        (void *)FUSE_DEFAULT_DAEMON_TIMEOUT,
        (void **)&daemon_timeout,
        "invalid value for argument daemon_timeout"
    },
    {
        FUSE_MOPT_FSID,
        NULL,
        0,
        fuse_to_fsid,
        0,
        (void **)&fsid,
        "invalid value for argument fsid (must be 0 < fsid < 0xFFFFFF)"
    },
    {
        FUSE_MOPT_FSNAME,
        NULL,
        0,
        fuse_to_string,
        NULL,
        (void **)&fsname,
        "invalid value for argument fsname"
    },
    {
        FUSE_MOPT_INIT_TIMEOUT,
        NULL,
        0,
        fuse_to_uint32,
        (void *)FUSE_DEFAULT_INIT_TIMEOUT,
        (void **)&init_timeout,
        "invalid value for argument init_timeout"
    },
    {
        FUSE_MOPT_IOSIZE,
        NULL,
        0,
        fuse_to_uint32,
        (void *)FUSE_DEFAULT_IOSIZE,
        (void **)&iosize,
        "invalid value for argument iosize"
    },
    {
        FUSE_MOPT_FSSUBTYPE,
        NULL,
        0,
        fuse_to_fssubtype,
        NULL,
        (void **)&fssubtype,
        "invalid value for argument fssubtype"
    },
    {
        FUSE_MOPT_FSTYPENAME,
        NULL,
        0,
        fuse_to_string,
        NULL,
        (void **)&fstypename,
        "invalid value for argument fstypename"
    },
    {
        FUSE_MOPT_VOLNAME,
        NULL,
        0,
        fuse_to_string,
        NULL,
        (void **)&volname,
        "invalid value for argument volname"
    },
    {
        0, NULL, 0, NULL, (void *)NULL, (void **)NULL, (char *)NULL
    },
};

static void
fuse_process_mvals(void)
{
    int ret;
    struct mntval *mv;

    for (mv = mvals; mv->mv_mntflag; mv++) {
        ret = mv->mv_converter(mv->mv_target, mv->mv_value, mv->mv_fallback);
        if (ret) {
            errx(EX_USAGE, "%s", mv->mv_errstr);
        }
    }
}

/* OSXFUSE notifications */

enum osxfuse_notification {
    NOTIFICATION_INIT_COMPLETED,
    NOTIFICATION_INIT_TIMED_OUT,
    NOTIFICATION_MOUNT
};
typedef enum osxfuse_notification osxfuse_notification_t;

const char * const osxfuse_notification_names[] = {
    "kOSXFUSEInitCompleted", // NOTIFICATION_INIT_COMPLETED
    "kOSXFUSEInitTimedOut",  // NOTIFICATION_INIT_TIMED_OUT
    "kOSXFUSEMount"          // NOTIFICATION_MOUNT
};

const char * const osxfuse_notification_object = OSXFUSE_IDENTIFIER;

#if OSXFUSE_ENABLE_MACFUSE_MODE
#define OSXFUSE_MACFUSE_MODE_ENV "OSXFUSE_MACFUSE_MODE"

#define MACFUSE_NOTIFICATION_OBJECT \
"com.google.filesystems.fusefs.unotifications"

const char * const macfuse_notification_names[] = {
    MACFUSE_NOTIFICATION_OBJECT ".inited",       // NOTIFICATION_INIT_COMPLETED
    MACFUSE_NOTIFICATION_OBJECT ".inittimedout", // NOTIFICATION_INIT_TIMED_OUT
    MACFUSE_NOTIFICATION_OBJECT ".mounted"       // NOTIFICATION_MOUNT
};

const char * const macfuse_notification_object = MACFUSE_NOTIFICATION_OBJECT;
#endif /* OSXFUSE_ENABLE_MACFUSE_MODE */

/* User info keys */

#define kFUSEDevicePathKey "kFUSEDevicePath"
#define kFUSEMountPathKey  "kFUSEMountPath"

static void
post_notification(const osxfuse_notification_t  notification,
                  const char                   *dict[][2],
                  const int                     dict_count)
{
    CFNotificationCenterRef notification_center =
            CFNotificationCenterGetDistributedCenter();

    CFStringRef            name      = NULL;
    CFStringRef            object    = NULL;
    CFMutableDictionaryRef user_info = NULL;

#if OSXFUSE_ENABLE_MACFUSE_MODE
    char *env_value = getenv(OSXFUSE_MACFUSE_MODE_ENV);
    if (env_value != NULL && strcmp(env_value, "1") == 0) {
        name   = CFStringCreateWithCString(kCFAllocatorDefault,
                                           macfuse_notification_names[notification],
                                           kCFStringEncodingUTF8);
        object = CFStringCreateWithCString(kCFAllocatorDefault,
                                           macfuse_notification_object,
                                           kCFStringEncodingUTF8);
    } else {
#endif
        name   = CFStringCreateWithCString(kCFAllocatorDefault,
                                           osxfuse_notification_names[notification],
                                           kCFStringEncodingUTF8);
        object = CFStringCreateWithCString(kCFAllocatorDefault,
                                           osxfuse_notification_object,
                                           kCFStringEncodingUTF8);
#if OSXFUSE_ENABLE_MACFUSE_MODE
    }
#endif

    if (!name || !object) goto out;
    if (dict_count == 0)  goto post;

    user_info = CFDictionaryCreateMutable(kCFAllocatorDefault, dict_count,
                                          &kCFCopyStringDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);

    CFStringRef key;
    CFStringRef value;
    int         i;
    for (i = 0; i < dict_count; i++) {
        key   = CFStringCreateWithCString(kCFAllocatorDefault, dict[i][0],
                                          kCFStringEncodingUTF8);
        value = CFStringCreateWithCString(kCFAllocatorDefault, dict[i][1],
                                          kCFStringEncodingUTF8);

        if (!key || !value) {
            if (key)   CFRelease(key);
            if (value) CFRelease(value);
            goto out;
        }

        CFDictionarySetValue(user_info, key, value);
        CFRelease(key); key = NULL;
        CFRelease(value); value = NULL;
    }

post:
    CFNotificationCenterPostNotification(notification_center, name, object,
                                         user_info, false);
out:
    if (name)      CFRelease(name);
    if (object)    CFRelease(object);
    if (user_info) CFRelease(user_info);
}

static int
check_kext_status(void)
{
    int    result = -1;
    char   version[MAXHOSTNAMELEN + 1] = { 0 };
    size_t version_len = MAXHOSTNAMELEN;
    size_t version_len_desired = 0;
    struct vfsconf vfc = { 0 };

    result = getvfsbyname(OSXFUSE_FS_TYPE, &vfc);
    if (result) { /* OSXFUSE is not already loaded */
        return ESRCH;
    }

    /* some version of OSXFUSE is already loaded; let us check it out */

    result = sysctlbyname(SYSCTL_OSXFUSE_VERSION_NUMBER, version,
                          &version_len, (void *)NULL, (size_t)0);
    if (result) {
        return result;
    }

    /* sysctlbyname() includes the trailing '\0' in version_len */
    version_len_desired = strlen(OSXFUSE_VERSION) + 1;

    if ((version_len != version_len_desired) ||
        strncmp(OSXFUSE_VERSION, version, version_len)) {
        return EINVAL;
    }

    /* What's currently loaded is good */

    return 0;
}

static void
signal_idx_atexit_handler(void)
{
    if (signal_idx != -1) {

        (void)ioctl(signal_fd, FUSEDEVIOCSETDAEMONDEAD, &signal_fd);

        /*
         * Originally, I did kill_fs from here.
         *
         * int32_t kill_fs_old = 0;
         * int32_t kill_fs_new = signal_idx;
         * size_t oldlen = sizeof(kill_fs_old);
         * size_t newlen = sizeof(kill_fs_new);
         *
         * (void)sysctlbyname("osxfuse.control.kill_fs", (void *)&kill_fs_old,
         *                    &oldlen, (void *)&kill_fs_new, newlen);
         */
    }
}

// We will be called as follows by the FUSE library:
//
//   mount_<OSXFUSE_FS_TYPE> -o OPTIONS... <fdnam> <mountpoint>

int
main(int argc, char **argv)
{
    int       result    = -1;
    int       mntflags  = 0;
    int       fd        = -1;
    int32_t   dindex    = -1;
    char     *fdnam     = NULL;
    uint64_t  altflags  = 0ULL;
    char     *mntpath   = NULL;

    int i, ch = '\0', done = 0;
    struct mntopt *mo;
    struct mntval *mv;
    struct statfs statfsb;
    fuse_mount_args args;

    if (!getenv("MOUNT_FUSEFS_CALL_BY_LIB")) {
        showhelp();
        /* NOTREACHED */
    }

    /* Kludge to make "<fsdaemon> --version" happy. */
    if ((argc == 2) &&
        ((!strncmp(argv[1], "--version", strlen("--version"))) ||
         (!strncmp(argv[1], "-v", strlen("-v"))))) {
        showversion(1);
    }

    /* Kludge to make "<fsdaemon> --help" happy. */
    if ((argc == 2) &&
        ((!strncmp(argv[1], "--help", strlen("--help"))) ||
         (!strncmp(argv[1], "-h", strlen("-h"))))) {
        showhelp();
    }

    memset((void *)&args, 0, sizeof(args));

    do {
        for (i = 0; i < 3; i++) {
            if (optind < argc && argv[optind][0] != '-') {
                if (mntpath) {
                    done = 1;
                    break;
                }
                if (fdnam)
                    mntpath = argv[optind];
                else
                    fdnam = argv[optind];
                optind++;
            }
        }

        switch(ch) {
        case 'o':
            getmntopts(optarg, mopts, &mntflags, &altflags);
            for (mv = mvals; mv->mv_mntflag; ++mv) {
                if (!(altflags & mv->mv_mntflag)) {
                    continue;
                }
                for (mo = mopts; mo->m_option; ++mo) {
                    char *p, *q;
                    if (mo->m_flag != mv->mv_mntflag) {
                        continue;
                    }
                    p = strstr(optarg, mo->m_option);
                    if (p) {
                        p += strlen(mo->m_option);
                        q = p;
                        while (*q != '\0' && *q != ',') {
                            q++;
                        }
                        mv->mv_len = q - p + 1;
                        mv->mv_value = malloc(mv->mv_len);
                        memcpy(mv->mv_value, p, mv->mv_len - 1);
                        ((char *)mv->mv_value)[mv->mv_len - 1] = '\0';
                        break;
                    }
                }
            }
            break;

        case '\0':
            break;

        case 'v':
            showversion(1);
            break;

        case '?':
        case 'h':
        default:
            showhelp();
            break;
        }

        if (done) {
            break;
        }

    } while ((ch = getopt(argc, argv, "ho:v")) != -1);

    argc -= optind;
    argv += optind;

    if ((!fdnam) && argc > 0) {
        fdnam = *argv++;
        argc--;
    }

    if (!fdnam) {
        errx(EX_USAGE, "missing OSXFUSE device file descriptor");
    }

    errno = 0;
    fd = (int)strtol(fdnam, NULL, 10);
    if ((errno == EINVAL) || (errno == ERANGE)) {
        errx(EX_USAGE,
             "invalid name (%s) for OSXFUSE device file descriptor", fdnam);
    }

    signal_fd = fd;

    {
        char  ndev[MAXPATHLEN];
        char *ndevbas;
        struct stat sb;

        if (fstat(fd, &sb) == -1) {
            err(EX_OSERR, "fstat failed for OSXFUSE device file descriptor");
        }
        args.rdev = sb.st_rdev;
        (void)strlcpy(ndev, _PATH_DEV, sizeof(ndev));
        ndevbas = ndev + strlen(_PATH_DEV);
        devname_r(sb.st_rdev, S_IFCHR, ndevbas,
                  (int)(sizeof(ndev) - strlen(_PATH_DEV)));

        if (strncmp(ndevbas, OSXFUSE_DEVICE_BASENAME,
                    strlen(OSXFUSE_DEVICE_BASENAME))) {
            errx(EX_USAGE, "mounting inappropriate device");
        }

        errno = 0;
        dindex = (int)strtol(ndevbas + strlen(OSXFUSE_DEVICE_BASENAME),
                             NULL, 10);
        if ((errno == EINVAL) || (errno == ERANGE) ||
            (dindex < 0) || (dindex > OSXFUSE_NDEVICES)) {
            errx(EX_USAGE, "invalid OSXFUSE device unit (#%d)\n", dindex);
        }
    }

    signal_idx = dindex;

    atexit(signal_idx_atexit_handler);

    result = check_kext_status();

    switch (result) {

    case 0:
        break;

    case ESRCH:
        errx(EX_UNAVAILABLE, "the OSXFUSE kernel extension is not loaded");
        break;

    case EINVAL:
        errx(EX_UNAVAILABLE,
             "the loaded OSXFUSE kernel extension has a mismatched version");
        break;

    default:
        errx(EX_UNAVAILABLE,
             "failed to query the loaded OSXFUSE kernel extension (%d)",
             result);
        break;
    }

    if ((!mntpath) && argc > 0) {
        mntpath = *argv++;
        argc--;
    }

    if (!mntpath) {
        errx(EX_USAGE, "missing mount point");
    }

    (void)checkpath(mntpath, args.mntpath);

    mntpath = args.mntpath;

    fuse_process_mvals();

    if (statfs(mntpath, &statfsb)) {
        errx(EX_OSFILE, "cannot stat the mount point %s", mntpath);
    }

    if ((strlen(statfsb.f_fstypename) == strlen(OSXFUSE_FS_TYPE)) &&
        (strcmp(statfsb.f_fstypename, OSXFUSE_FS_TYPE) == 0)) {
        if (!(altflags & FUSE_MOPT_ALLOW_RECURSION)) {
            errx(EX_USAGE,
                 "mount point %s is itself on a OSXFUSE volume", mntpath);
        }
    } if (strncmp(statfsb.f_fstypename, FUSE_FSTYPENAME_PREFIX,
                  strlen(FUSE_FSTYPENAME_PREFIX)) == 0) {
        if (!(altflags & FUSE_MOPT_ALLOW_RECURSION)) {
            errx(EX_USAGE,
                 "mount point %s is itself on a OSXFUSE volume", mntpath);
        }
    }

    /* allow_root and allow_other checks are done in the kernel. */

    if (altflags & FUSE_MOPT_NO_LOCALCACHES) {
        altflags |= FUSE_MOPT_NO_ATTRCACHE;
        altflags |= FUSE_MOPT_NO_READAHEAD;
        altflags |= FUSE_MOPT_NO_UBC;
        altflags |= FUSE_MOPT_NO_VNCACHE;
    }

    if ((altflags & FUSE_MOPT_NEGATIVE_VNCACHE) &&
        (altflags & FUSE_MOPT_NO_VNCACHE)) {
        errx(EX_USAGE, "'negative_vncache' can't be used with 'novncache'");
    }

    /*
     * 'nosyncwrites' must not appear with either 'noubc' or 'noreadahead'.
     */
    if ((altflags & FUSE_MOPT_NO_SYNCWRITES) &&
        (altflags & (FUSE_MOPT_NO_UBC | FUSE_MOPT_NO_READAHEAD))) {
        errx(EX_USAGE,
             "disabling local caching can't be used with 'nosyncwrites'");
    }

    /*
     * 'nosynconclose' only allowed if 'nosyncwrites' is also there.
     */
    if ((altflags & FUSE_MOPT_NO_SYNCONCLOSE) &&
        !(altflags & FUSE_MOPT_NO_SYNCWRITES)) {
        errx(EX_USAGE, "the 'nosynconclose' option requires 'nosyncwrites'");
    }

    if ((altflags & FUSE_MOPT_DEFAULT_PERMISSIONS) &&
        (altflags & FUSE_MOPT_DEFER_PERMISSIONS)) {
        errx(EX_USAGE,
             "'default_permissions' can't be used with 'defer_permissions'");
    }

    if ((altflags & FUSE_MOPT_AUTO_XATTR) &&
        (altflags & FUSE_MOPT_NATIVE_XATTR)) {
        errx(EX_USAGE,
             "'auto_xattr' can't be used with 'native_xattr'");
    }

    if (daemon_timeout < FUSE_MIN_DAEMON_TIMEOUT) {
        daemon_timeout = FUSE_MIN_DAEMON_TIMEOUT;
    }

    if (daemon_timeout > FUSE_MAX_DAEMON_TIMEOUT) {
        daemon_timeout = FUSE_MAX_DAEMON_TIMEOUT;
    }

    if (init_timeout < FUSE_MIN_INIT_TIMEOUT) {
        init_timeout = FUSE_MIN_INIT_TIMEOUT;
    }

    if (init_timeout > FUSE_MAX_INIT_TIMEOUT) {
        init_timeout = FUSE_MAX_INIT_TIMEOUT;
    }

    result = ioctl(fd, FUSEDEVIOCGETRANDOM, &drandom);
    if (result) {
        errx(EX_UNAVAILABLE, "failed to negotiate with /dev/osxfuse%d", dindex);
    }

    args.altflags       = altflags;
    args.blocksize      = (uint32_t) blocksize;
    args.daemon_timeout = (uint32_t) daemon_timeout;
    args.fsid           = (uint32_t) fsid;
    args.fssubtype      = (uint32_t) fssubtype;
    args.init_timeout   = (uint32_t) init_timeout;
    args.iosize         = (uint32_t) iosize;
    args.random         = drandom;

    char *daemon_name = NULL;
    char *daemon_path = getenv("MOUNT_FUSEFS_DAEMON_PATH");
    if (daemon_path) {
        daemon_name = basename(daemon_path);
    }

    if (!fsname) {
        if (daemon_name) {
            snprintf(args.fsname, MAXPATHLEN, "%s@osxfuse%d", daemon_name, dindex);
        } else {
            snprintf(args.fsname, MAXPATHLEN, "instance@osxfuse%d", dindex);
        }
    } else {
        snprintf(args.fsname, MAXPATHLEN, "%s", fsname);
    }

    if (fstypename) {
        if (strlen(fstypename) > FUSE_FSTYPENAME_MAXLEN) {
            errx(EX_USAGE, "fstypename can be at most %lu characters",
                 (long unsigned int) FUSE_FSTYPENAME_MAXLEN);
        } else {
            snprintf(args.fstypename, MFSTYPENAMELEN, "%s", fstypename);
        }
    }

    if (!volname) {
        if (daemon_name) {
            snprintf(args.volname, MAXPATHLEN, "OSXFUSE Volume %d (%s)",
                     dindex, daemon_name);
        } else {
            snprintf(args.volname, MAXPATHLEN, "OSXFUSE Volume %d", dindex);
        }
    } else {
        snprintf(args.volname, MAXPATHLEN, "%s", volname);
    }

    /* Finally! */
    result = mount(OSXFUSE_FS_TYPE, mntpath, mntflags, (void *)&args);

    if (result < 0) {
        err(EX_OSERR, "failed to mount %s@/dev/osxfuse%d", mntpath, dindex);
    } else {
        const char *dict[][2] = { { kFUSEMountPathKey, mntpath } };
        post_notification(NOTIFICATION_MOUNT, dict, 1);
    }

    signal_idx = -1;

    exit(0);
}

void
showhelp()
{
    if (!getenv("MOUNT_FUSEFS_CALL_BY_LIB")) {
        showversion(0);
        fprintf(stderr, "\nThis program is not meant to be called directly. The OSXFUSE library calls it.\n");
    }
    fprintf(stderr, "\nAvailable mount options:\n");
    fprintf(stderr,
      "    -o allow_other         allow access to others besides the user who mounted\n"
      "                           the file system\n"
      "    -o allow_recursion     allow a mount point that itself resides on a OSXFUSE\n"
      "                           volume (by default, such mounting is disallowed)\n"
      "    -o allow_root          allow access to root (can't be used with allow_other)\n"
      "    -o auto_xattr          handle extended attributes entirely through ._ files\n"
      "    -o blocksize=<size>    specify block size in bytes of \"storage\"\n"
      "    -o daemon_timeout=<s>  timeout in seconds for kernel calls to daemon\n"
      "    -o debug               turn on debug information printing\n"
      "    -o default_permissions let the kernel handle permission checks locally\n"
      "    -o defer_permissions   defer permission checks to file operations themselves\n"
      "    -o direct_io           use alternative (direct) path for kernel-user I/O\n"
      "    -o extended_security   turn on Mac OS X extended security (ACLs)\n"
      "    -o fsid=<fsid>         set the second 32-bit component of the fsid\n"
      "    -o fsname=<name>       set the file system's name\n"
      "    -o fssubtype=<num>     set the file system's fssubtype identifier\n"
      "    -o fstypename=<name>   set the file system's type name\n"
      "    -o iosize=<size>       specify maximum I/O size in bytes\n"
      "    -o jail_symlinks       contain symbolic links within the mount\n"
      "    -o local               mark the volume as \"local\" (default is \"nonlocal\")\n"
      "    -o negative_vncache    enable vnode name caching of non-existent objects\n"
      "    -o sparse              enable support for sparse files\n"
      "    -o volname=<name>      set the file system's volume name\n"
      "\nAvailable negative mount options:\n"
      "    -o noalerts            disable all graphical alerts (if any) in OSXFUSE Core\n"
      "    -o noappledouble       ignore Apple Double (._) and .DS_Store files entirely\n"
      "    -o noapplexattr        ignore all \"com.apple.*\" extended attributes\n"
      "    -o nobrowse            mark the volume as non-browsable by the Finder\n"
      "    -o nolocalcaches       meta option equivalent to noreadahead,noubc,novncache\n"
      "    -o noreadahead         disable I/O read-ahead behavior for this file system\n"
      "    -o nosynconclose       disable sync-on-close behavior (enabled by default)\n"
      "    -o nosyncwrites        disable synchronous-writes behavior (dangerous)\n"
      "    -o noubc               disable the unified buffer cache for this file system\n"
      "    -o novncache           disable the vnode name cache for this file system\n"
    );
    exit(EX_USAGE);
}

void
showversion(int doexit)
{
    fprintf(stderr, "OSXFUSE mount version %s\n", OSXFUSE_VERSION);
    if (doexit) {
        exit(EX_USAGE);
    }
}
