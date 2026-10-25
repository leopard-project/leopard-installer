/*
 * leopard-installer — LeopardOS 安装器（CLI，Debian 式）
 * 在 live 环境运行（initrc 尾端代替 getty）。纯用户态，编排现成工具：
 *   lpdisk 分区 → mkfs-fat32 格式化 → SYS_MOUNT 挂载 → xbps-install 装包 → 重启。
 */
#include "xtuiapi.h"

#define LINE_CAP    256
#define O_RDONLY    0
#define SEEK_SET    0
#define SEEK_END    2
#define SECTOR_SIZE 512
#define MOUNT_POINT "/mnt"
#define MEDIA_REPO  "/media"

static const char *CANDIDATES[] = {
    "sata0","sata1","sata2","sata3","sata4","sata5","sata6","sata7",
    "nvme0","nvme1","nvme2","nvme3",
    "ide0","ide1","ide2","ide3", NULL
};

/* ---- POSIX 薄封装（只读磁盘大小用）---- */
static int fd_open(const char *path, uint64_t flags) {
    return (int)(int64_t)enter_syscall(SYS_OPEN, (uint64_t)path, flags, 0644, 0, 0, 0);
}
static int64_t fd_lseek(int fd, int64_t off, uint64_t whence) {
    return (int64_t)enter_syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)off, whence, 0, 0, 0);
}
static int64_t fd_read(int fd, void *buf, size_t size) {
    return (int64_t)enter_syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)size, 0, 0, 0);
}

static int fd_close(int fd) {
    return (int)enter_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
}

static uint64_t disk_sectors(const char *dev) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", dev);
    int fd = fd_open(path, O_RDONLY);
    if (fd < 0) return 0;
    int64_t sz = fd_lseek(fd, 0, SEEK_END);
    fd_close(fd);
    return (sz < 0) ? 0 : (uint64_t)sz / SECTOR_SIZE;
}

/* ---- 输入 / 运行子程序 ---- */
static void read_line(char *buf, size_t cap) {
    buf[0] = '\0';
    enter_syscall(XAPI_INPUT, (uint64_t)buf, (uint64_t)cap, 0, 0, 0, 0);
}

static int run_cmd(const char *path, char *argv[]) {
    int64_t pid = (int64_t)xapi_RunArgs((WSTR)path, (WSTR *)argv);
    if (pid < 0) return -1;
    int status = 0;
    enter_syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)&status, 0, 0, 0, 0);
    return status;
}

static void list_disks(void) {
    xapi_Printf("Available disks:\n");
    for (int i = 0; CANDIDATES[i] != NULL; i++) {
        uint64_t sectors = disk_sectors(CANDIDATES[i]);
        if (sectors == 0) continue;
        xapi_Printf("  /dev/%-8s  %llu MiB  (%llu sectors)\n",
                    CANDIDATES[i],
                    (unsigned long long)(sectors * SECTOR_SIZE / 1024 / 1024),
                    (unsigned long long)sectors);
    }
}

static int confirm(const char *prompt) {
    xapi_Printf("%s [y/N]: ", prompt);
    char line[8];
    read_line(line, sizeof(line));
    return (line[0] == 'y' || line[0] == 'Y');
}

/* ---- 步骤 ---- */
static int do_partition(const char *dev) {
    uint64_t total = disk_sectors(dev);
    if (total < 4096) { xapi_Printf("disk too small\n"); return -1; }
    uint64_t start = 2048;
    uint64_t end   = total - 34;
    char start_s[32], end_s[32];
    snprintf(start_s, sizeof(start_s), "%llu", (unsigned long long)start);
    snprintf(end_s, sizeof(end_s), "%llu", (unsigned long long)end);

    xapi_Printf("Partitioning /dev/%s: 1 GPT partition (ef00) LBA %llu..%llu\n",
                dev, (unsigned long long)start, (unsigned long long)end);

    char *a1[] = { (char*)"lpdisk", (char*)"create", (char*)dev, NULL };
    if (run_cmd("/apps/system/lpdisk.elf", a1) != 0) { xapi_Printf("lpdisk create failed\n"); return -1; }
    char *a2[] = { (char*)"lpdisk", (char*)"add", (char*)dev, start_s, end_s,
                   (char*)"--type", (char*)"ef00", (char*)"--name", (char*)"LEOPARD", NULL };
    if (run_cmd("/apps/system/lpdisk.elf", a2) != 0) { xapi_Printf("lpdisk add failed\n"); return -1; }
    char *a3[] = { (char*)"lpdisk", (char*)"rescan", (char*)dev, NULL };
    if (run_cmd("/apps/system/lpdisk.elf", a3) != 0) { xapi_Printf("lpdisk rescan failed\n"); return -1; }
    return 0;
}

static int do_format(const char *dev) {
    char part[64];
    snprintf(part, sizeof(part), "%sp0", dev); /* 分区节点名（全局索引 0 的单分区场景） */
    xapi_Printf("Formatting /dev/%s as FAT32...\n", part);
    char *argv[] = { (char*)"mkfs-fat32", part, (char*)"LEOPARD", NULL };
    if (run_cmd("/apps/system/mkfs-fat32.elf", argv) != 0) { xapi_Printf("mkfs-fat32 failed\n"); return -1; }
    return 0;
}

static int do_mount(const char *dev) {
    char part[64];
    snprintf(part, sizeof(part), "/dev/%sp0", dev);
    xapi_Mkdir((WSTR)MOUNT_POINT);
    /* 写缓存可能未 flush，失败则等待后重试 */
    for (int attempt = 0; attempt < 6; attempt++) {
        int64_t r = (int64_t)enter_syscall(SYS_MOUNT, (uint64_t)part, (uint64_t)MOUNT_POINT,
                                           (uint64_t)"fatfs", 0, 0, 0);
        if (r == 0) { xapi_Printf("Mounted %s at %s\n", part, MOUNT_POINT); return 0; }
        xapi_Sleep(1000);
    }
    xapi_Printf("mount %s -> %s failed\n", part, MOUNT_POINT);
    return -1;
}

static int do_install(void) {
    xapi_Printf("Installing base system from %s...\n", MEDIA_REPO);
    /* 调试：用 Linux read syscall 读 repodata 实际大小 + 前 8 字节 */
    {
        int rfd = fd_open("/media/x86_64-repodata", O_RDONLY);
        if (rfd >= 0) {
            int64_t sz = fd_lseek(rfd, 0, SEEK_END);
            fd_lseek(rfd, 0, SEEK_SET);
            unsigned char head[8] = {0};
            int64_t n = fd_read(rfd, head, sizeof(head));
            xapi_Printf("[dbg-repodata] size=%lld read=%lld head=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        (long long)sz, (long long)n, head[0],head[1],head[2],head[3],head[4],head[5],head[6],head[7]);
            fd_close(rfd);
        } else {
            xapi_Printf("[dbg-repodata] open failed fd=%d\n", rfd);
        }
    }
    /* 调试3：直接测 fstat(SYS_fstat=5) + fstatat(SYS_newfstatat=262, AT_EMPTY_PATH) */
    {
        int ffd = fd_open("/media/x86_64-repodata", 0);
        if (ffd >= 0) {
            unsigned char sbuf[160]; memset(sbuf, 0, sizeof(sbuf));
            int fr = (int)enter_syscall(5, (uint64_t)ffd, (uint64_t)sbuf, 0, 0, 0, 0);
            long long fsz = *(long long *)(sbuf + 48);
            unsigned int fmode = *(unsigned int *)(sbuf + 24);
            xapi_Printf("[dbg-fstat] ret=%d st_size=%lld st_mode=0%o\n", fr, fsz, fmode);

            unsigned char sbuf2[160]; memset(sbuf2, 0, sizeof(sbuf2));
            int fr2 = (int)enter_syscall(262, (uint64_t)ffd, (uint64_t)"", (uint64_t)sbuf2, 0x1000, 0, 0);
            long long fsz2 = *(long long *)(sbuf2 + 48);
            xapi_Printf("[dbg-fstatat] ret=%d st_size=%lld\n", fr2, fsz2);
            /* 全量校验：读 1590 字节求和 + 采样 */
            {
                fd_lseek(ffd, 0, SEEK_SET);
                unsigned char full[1600]; memset(full, 0, sizeof(full));
                int64_t fn = fd_read(ffd, full, sizeof(full));
                unsigned long fsum = 0;
                for (int i = 0; i < (int)fn; i++) fsum += full[i];
                xapi_Printf("[dbg-sum] read=%lld sum=%lu b8=%02x%02x b16=%02x%02x b512=%02x%02x\n",
                            (long long)fn, fsum, full[8],full[9], full[16],full[17], full[512],full[513]);
            }
            fd_close(ffd);
        }
    }
    /* 调试2：模拟 libarchive 的 O_CLOEXEC 打开 + 4096 字节读 */
    {
        int fd2 = fd_open("/media/x86_64-repodata", 0x80000);
        xapi_Printf("[dbg-cloexec] open(O_CLOEXEC=0x80000) fd=%d\n", fd2);
        if (fd2 >= 0) {
            unsigned char big[4096];
            int64_t bn = fd_read(fd2, big, sizeof(big));
            xapi_Printf("[dbg-read4096] read=%lld head=%02x%02x%02x%02x\n", (long long)bn, big[0],big[1],big[2],big[3]);
            fd_close(fd2);
        }
    }
    char *argv[] = { (char*)"xbps-install", (char*)"-y", (char*)"-r", (char*)MOUNT_POINT,
                     (char*)"-R", (char*)MEDIA_REPO,
                     (char*)"OpenXSBoot", (char*)"OpenXSKernel", (char*)"leopard-base",
                     (char*)"leopard-rc", (char*)"leopard-getty", (char*)"leopard-oobe",
                     (char*)"leopard-shell", (char*)"poweroff", (char*)"initctl",
                     (char*)"leopard-mountd", (char*)"leopard-disk", (char*)"mkfs-fat32",
                     (char*)"leopard-ime", (char*)"xbps", NULL };
    if (run_cmd("/apps/system/xbps-install", argv) != 0) {
        xapi_Printf("xbps-install failed (需 xbps 内部路径重编后才可用)\n");
        return -1;
    }
    xapi_Printf("Base system installed.\n");
    return 0;
}

static void do_reboot(void) {
    xapi_Printf("Installation complete. Rebooting...\n");
    xapi_Sleep(2000);
    xapi_PowerAction(XPOWER_REBOOT);
}

int main(int argc, char *argv[], char *envp[]) {
    (void)envp;

    xapi_Printf("\nLeopardOS Installer\n");
    xapi_Printf("==================\n\n");

    char dev[64];
    int  auto_yes = 0;

    if (argc >= 2) {
        /* 非交互：argv[1] = 目标盘；argv[2] == --yes 跳过确认 */
        strncpy(dev, argv[1], sizeof(dev) - 1);
        dev[sizeof(dev) - 1] = '\0';
        if (argc >= 3 && strcmp(argv[2], "--yes") == 0) auto_yes = 1;
        if (disk_sectors(dev) == 0) {
            xapi_Printf("Disk /dev/%s not found or has no size\n", dev);
            return 1;
        }
        if (!auto_yes) {
            char prompt[160];
            snprintf(prompt, sizeof(prompt),
                     "Install LeopardOS to /dev/%s? THIS DESTROYS ALL DATA ON IT", dev);
            if (!confirm(prompt)) { xapi_Printf("Aborted.\n"); return 1; }
        }
    } else {
        for (;;) {
            list_disks();
            xapi_Printf("Enter target disk (e.g. sata1): ");
            read_line(dev, sizeof(dev));
            if (dev[0] == '\0') continue;
            if (disk_sectors(dev) == 0) {
                xapi_Printf("Disk /dev/%s not found or has no size\n\n", dev);
                continue;
            }
            char prompt[160];
            snprintf(prompt, sizeof(prompt),
                     "Install LeopardOS to /dev/%s? THIS DESTROYS ALL DATA ON IT", dev);
            if (confirm(prompt)) break;
            xapi_Printf("Aborted.\n\n");
        }
    }

    if (do_partition(dev) != 0) return 1;
    if (do_format(dev) != 0) return 1;
    if (do_mount(dev) != 0) return 1;
    if (do_install() != 0) return 1;
    do_reboot();
    return 0;
}
