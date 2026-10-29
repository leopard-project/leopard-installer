/*
 * steps/20-partition.c — 步骤 20：建立 GPT 分区表
 *
 * 由 main.c（序列编排器）以全路径、无参数执行。
 *
 * 两种方案（选项/状态键 scheme）：
 *   auto   —— 自动三分区：ESP(根) / apps / users，可用区三等分、1 MiB 对齐
 *   custom —— 用户自定义：逐分区给挂载点 + 大小（MiB），第一个必须是 "/"
 *             （内核 mount_root 只认"全局索引最低、且带启动标记"的分区）
 *
 * 输入：disk、scheme（可选，缺省 auto）
 * 输出：part.scheme、part.count、part.N.{start,end,name,label,mount,node}
 */
#include "li.h"

#define LI_SECTOR    512ULL
#define LI_ALIGN     2048ULL                            /* 1 MiB */
#define LI_GPT_TAIL  34ULL                              /* 备份 GPT 尾部 */
#define LI_MIN_SECT  (16ULL * 1024 * 1024 / LI_SECTOR)   /* mkfs-fat32 下限 16 MiB */
#define LI_MAX_PARTS 8

typedef struct {
        char     name[24];
        char     label[16];
        char     mount[32];
        uint64_t start;
        uint64_t end;
} part_t;

static part_t   s_parts[LI_MAX_PARTS];
static int      s_count;
static char     s_disk[32];
static uint64_t s_total;

static uint64_t part_sectors(const part_t *p)
{
        return p->end - p->start + 1;
}

static void node_name(int base, int index, char *out, size_t cap)
{
        snprintf(out, cap, "%sp%d", s_disk, base + index);
}

static void default_labels(void)
{
        static const char *names[]  = { "ESP", "apps", "users" };
        static const char *labels[] = { "LEOPARD", "LP-APPS", "LP-USERS" };
        static const char *mounts[] = { "/", "/apps", "/users" };

        for (int i = 0; i < 3; i++) {
                snprintf(s_parts[i].name, sizeof(s_parts[i].name), "%s", names[i]);
                snprintf(s_parts[i].label, sizeof(s_parts[i].label), "%s", labels[i]);
                snprintf(s_parts[i].mount, sizeof(s_parts[i].mount), "%s", mounts[i]);
        }
}

static int auto_layout(void)
{
        uint64_t first;
        uint64_t last;
        uint64_t usable;
        uint64_t chunk;

        if (s_total <= LI_ALIGN + LI_GPT_TAIL) return -1;

        first  = LI_ALIGN;
        last   = s_total - LI_GPT_TAIL;
        if (last <= first) return -1;

        usable = last - first + 1;
        chunk  = (usable / 3) & ~(LI_ALIGN - 1);
        if (chunk < LI_MIN_SECT) return -1;
        if (usable - 2 * chunk < LI_MIN_SECT) return -1;

        default_labels();
        s_parts[0].start = first;
        s_parts[0].end   = first + chunk - 1;
        s_parts[1].start = s_parts[0].end + 1;
        s_parts[1].end   = s_parts[1].start + chunk - 1;
        s_parts[2].start = s_parts[1].end + 1;
        s_parts[2].end   = last;
        s_count          = 3;
        return 0;
}

/* 从状态里读回用户给定的分区（选项文件或上次运行留下的） */
static int custom_layout(void)
{
        int      count = li_state_get_int("part.count", 0);
        uint64_t cursor;
        uint64_t last;

        if (count <= 0 || count > LI_MAX_PARTS) return -1;

        cursor = LI_ALIGN;
        last   = s_total - LI_GPT_TAIL;

        for (int i = 0; i < count; i++) {
                char        key[40];
                char        buf[LI_VAL_LEN];
                const char *mb;
                const char *mount;
                const char *pname;
                const char *plabel;
                uint64_t    size_sect = 0;

                snprintf(key, sizeof(key), "part.%d.size_mb", i);
                mb = li_state_get(key);
                if (mb != NULL && mb[0] != '\0')
                        size_sect = (uint64_t)atoi(mb) * 1024 * 1024 / LI_SECTOR;

                snprintf(key, sizeof(key), "part.%d.mount", i);
                mount = li_state_get(key);
                snprintf(buf, sizeof(buf), "%s", (mount != NULL && mount[0] != '\0') ? mount : "");
                snprintf(s_parts[i].mount, sizeof(s_parts[i].mount), "%s", buf);

                snprintf(key, sizeof(key), "part.%d.name", i);
                pname = li_state_get(key);
                snprintf(s_parts[i].name, sizeof(s_parts[i].name), "%s",
                         (pname != NULL && pname[0] != '\0') ? pname : "leopard");

                snprintf(key, sizeof(key), "part.%d.label", i);
                plabel = li_state_get(key);
                snprintf(s_parts[i].label, sizeof(s_parts[i].label), "%s",
                         (plabel != NULL && plabel[0] != '\0') ? plabel : "LEOPARD");

                if (size_sect < LI_MIN_SECT) return -1;
                if (cursor + size_sect - 1 > last) return -1;

                s_parts[i].start = cursor;
                s_parts[i].end   = cursor + size_sect - 1;
                cursor           = s_parts[i].end + 1;
        }
        s_count = count;
        return 0;
}

/* 交互式自定义：逐分区问挂载点与大小 */
static int ask_custom_layout(void)
{
        int      count = li_state_get_int("part.count", 0);
        uint64_t cursor;
        uint64_t last;

        if (count <= 0) {
                char answer[16];

                if (li_ask("分区数量（1-8）：", answer, sizeof(answer)) != 0) return -1;
                count = atoi(answer);
                if (count <= 0 || count > LI_MAX_PARTS) return -1;
        }

        cursor = LI_ALIGN;
        last   = s_total - LI_GPT_TAIL;

        for (int i = 0; i < count; i++) {
                char     answer[32];
                uint64_t size_sect;

                xapi_Printf("\n分区 %d / %d（剩余 %llu MiB）\n", i + 1, count,
                            (unsigned long long)((last - cursor + 1) * LI_SECTOR / 1024 / 1024));
                if (i == 0) {
                        snprintf(s_parts[0].mount, sizeof(s_parts[0].mount), "/");
                        xapi_Printf("  挂载点固定为 /（根分区，必须带启动标记）\n");
                } else {
                        if (li_ask("  挂载点（如 /apps、/users，留空结束）：", answer, sizeof(answer)) != 0)
                                return -1;
                        snprintf(s_parts[i].mount, sizeof(s_parts[i].mount), "%s", answer);
                        if (answer[0] == '\0') break;
                }

                if (li_ask("  大小（MiB）：", answer, sizeof(answer)) != 0) return -1;
                size_sect = (uint64_t)atoi(answer) * 1024 * 1024 / LI_SECTOR;
                if (size_sect < LI_MIN_SECT || cursor + size_sect - 1 > last) {
                        xapi_Printf("  大小非法或超出剩余空间\n");
                        return -1;
                }

                snprintf(s_parts[i].name, sizeof(s_parts[i].name), "%s", (i == 0) ? "ESP" : "data");
                snprintf(s_parts[i].label, sizeof(s_parts[i].label), "%s", (i == 0) ? "LEOPARD" : "LP-DATA");
                s_parts[i].start = cursor;
                s_parts[i].end   = cursor + size_sect - 1;
                cursor           = s_parts[i].end + 1;
                s_count          = i + 1;

                char key[40], value[32];
                snprintf(key, sizeof(key), "part.%d.size_mb", i);
                snprintf(value, sizeof(value), "%llu",
                         (unsigned long long)(size_sect * LI_SECTOR / 1024 / 1024));
                li_state_set(key, value);
                snprintf(key, sizeof(key), "part.%d.mount", i);
                li_state_set(key, s_parts[i].mount);
        }
        if (s_count > 0) {
                char value[16];

                snprintf(value, sizeof(value), "%d", s_count);
                li_state_set("part.count", value);
        }
        return (s_count > 0) ? 0 : -1;
}

static int write_table(void)
{
        char *create_argv[] = { (char *)"lpdisk", (char *)"create", s_disk, NULL };

        if (li_run_tool("lpdisk.elf", create_argv) != 0) return li_fail("lpdisk create", 0);

        for (int i = 0; i < s_count; i++) {
                char start_s[24], end_s[24];
                char *add_argv[] = { (char *)"lpdisk", (char *)"add", s_disk, start_s, end_s,
                                     (char *)"--type", (char *)"ef00",
                                     (char *)"--name", (char *)s_parts[i].name, NULL };

                snprintf(start_s, sizeof(start_s), "%llu", (unsigned long long)s_parts[i].start);
                snprintf(end_s, sizeof(end_s), "%llu", (unsigned long long)s_parts[i].end);
                if (li_run_tool("lpdisk.elf", add_argv) != 0) {
                        li_log("lpdisk add failed at partition %d", i);
                        return 1;
                }
        }

        char *rescan_argv[] = { (char *)"lpdisk", (char *)"rescan", s_disk, NULL };

        if (li_run_tool("lpdisk.elf", rescan_argv) != 0) return li_fail("lpdisk rescan", 0);
        return 0;
}

/*
 * 分区节点名是内核的**全局分区索引**（扫描顺序决定），所以 rescan 后按
 * "连续 N 个节点的扇区数序列"定位本盘分区的起始索引，而不是硬编码 p0。
 */
static int discover_nodes(int *base_out)
{
        for (int base = 0; base + s_count <= 16; base++) {
                int ok = 1;

                for (int i = 0; i < s_count; i++) {
                        char node[40];

                        node_name(base, i, node, sizeof(node));
                        if (li_dev_sectors(node) != part_sectors(&s_parts[i])) {
                                ok = 0;
                                break;
                        }
                }
                if (ok) {
                        *base_out = base;
                        return 0;
                }
        }
        return -1;
}

static int store_state(int base)
{
        char value[32], key[40];

        snprintf(value, sizeof(value), "%d", s_count);
        li_state_set("part.count", value);

        for (int i = 0; i < s_count; i++) {
                char node[40];

                node_name(base, i, node, sizeof(node));
                snprintf(key, sizeof(key), "part.%d.start", i);
                snprintf(value, sizeof(value), "%llu", (unsigned long long)s_parts[i].start);
                li_state_set(key, value);
                snprintf(key, sizeof(key), "part.%d.end", i);
                snprintf(value, sizeof(value), "%llu", (unsigned long long)s_parts[i].end);
                li_state_set(key, value);
                snprintf(key, sizeof(key), "part.%d.name", i);
                li_state_set(key, s_parts[i].name);
                snprintf(key, sizeof(key), "part.%d.label", i);
                li_state_set(key, s_parts[i].label);
                snprintf(key, sizeof(key), "part.%d.mount", i);
                li_state_set(key, s_parts[i].mount);
                snprintf(key, sizeof(key), "part.%d.node", i);
                li_state_set(key, node);

                li_log("partition %d: %s LBA %llu..%llu label=%s mount=%s node=/dev/%s", i,
                       s_parts[i].name, (unsigned long long)s_parts[i].start,
                       (unsigned long long)s_parts[i].end, s_parts[i].label, s_parts[i].mount, node);
        }
        return 0;
}

int main(int argc, char *argv[], char *envp[])
{
        const char *disk;
        const char *scheme;
        int         rc;
        int         base = 0;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("建立分区表");

        disk = li_state_get("disk");
        if (disk == NULL || disk[0] == '\0')
                return li_step_end(li_fail("no disk selected (run 10-disk first)", 0));

        snprintf(s_disk, sizeof(s_disk), "%s", disk);
        s_total = li_dev_sectors(s_disk);
        if (s_total == 0) return li_step_end(li_fail("cannot read disk size", 0));

        scheme = li_state_get("scheme");
        if (scheme == NULL || scheme[0] == '\0') scheme = "auto";

        if (!li_is_auto() && !li_state_is("scheme", "custom")) {
                char answer[8];

                xapi_Printf("分区方案：1) 自动三分区（ESP/apps/users）  2) 自定义\n");
                if (li_ask("选择 [1]：", answer, sizeof(answer)) != 0) return li_step_end(1);
                if (answer[0] == '2') scheme = "custom";
        }

        if (strcmp(scheme, "custom") == 0) {
                rc = custom_layout();
                if (rc != 0) rc = ask_custom_layout();
                if (rc != 0) return li_step_end(li_fail("custom layout invalid", 0));
                li_state_set("scheme", "custom");
        } else {
                if (auto_layout() != 0)
                        return li_step_end(li_fail("disk too small for 3 partitions", 0));
                li_state_set("scheme", "auto");
        }

        /* 根分区必须是第 0 个：mount_root 选全局索引最低、且带启动标记的分区 */
        if (s_parts[0].mount[0] != '/') {
                for (int i = 1; i < s_count; i++) {
                        if (strcmp(s_parts[i].mount, "/") == 0) {
                                part_t tmp = s_parts[0];

                                s_parts[0] = s_parts[i];
                                s_parts[i] = tmp;
                                break;
                        }
                }
        }

        for (int i = 0; i < s_count; i++)
                xapi_Printf("  p%d  %-8s LBA %llu..%llu  %llu MiB  -> %s\n", i, s_parts[i].name,
                            (unsigned long long)s_parts[i].start, (unsigned long long)s_parts[i].end,
                            (unsigned long long)(part_sectors(&s_parts[i]) * LI_SECTOR / 1024 / 1024),
                            s_parts[i].mount);

        if (!li_confirm("Write this partition table?", 1))
                return li_step_end(li_fail("aborted by user", 0));

        if (write_table() != 0) return li_step_end(1);

        if (discover_nodes(&base) != 0) {
                li_log("WARNING: partition nodes not found by size, assuming p0..p%d", s_count - 1);
                base = 0;
        } else {
                li_log("partition nodes start at %sp%d", s_disk, base);
        }

        store_state(base);
        return li_step_end(0);
}
