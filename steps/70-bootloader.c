/*
 * steps/70-bootloader.c — Step 70: publish boot artifacts and write the root binding
 *
 * ext4 scheme (scheme=ext4):
 *   ESP = /mnt/boot (FAT32), root = /mnt (ext4)
 *   1. Copy EFI/BOOT/BOOTX64.EFI and system/kernel.krl from root to the ESP
 *   2. Compute the kernel size/CRC32C
 *   3. Use the GPT GUID saved by 20-partition plus the ext4 UUID saved by 30-format
 *      to write the ESP's EFI/XJ380/root.bin (96 bytes)
 * Old FAT32 scheme: only verify the EFI + kernel markers on root.
 */
#include "li.h"
#include "root_binding.h"

#define LI_ESP_ROOT "/mnt/boot"

static int file_size(const char *path, uint64_t *out)
{
        XFILE *fp = xapi_OpenFile((WSTR)path);

        if (fp == NULL) return -1;
        *out = fp->length;
        xapi_CloseFile(fp);
        return 0;
}

static int make_parent(const char *path)
{
        char   dir[LI_PATH_LEN];
        size_t len = strlen(path);

        if (len >= sizeof(dir)) return -1;
        memcpy(dir, path, len + 1);
        for (size_t i = len; i > 0; i--) {
                if (dir[i] == '/') {
                        dir[i] = '\0';
                        return li_make_dirs(dir);
                }
        }
        return 0;
}

static int publish(const char *src, const char *dst)
{
        if (make_parent(dst) != 0) return -1;
        return li_copy_file(src, dst);
}

static int check_file(const char *path, const char *why)
{
        if (li_file_exists(path) != 0) {
                xapi_Printf("  OK      %s\n", path);
                return 0;
        }
        xapi_Printf("  MISSING %s  (%s)\n", path, why);
        li_log("missing %s (%s)", path, why);
        return 1;
}

static int parse_hex(const char *text, uint8_t *out, size_t bytes)
{
        if (text == NULL || strlen(text) < bytes * 2) return -1;
        for (size_t i = 0; i < bytes; i++) {
                int value = 0;
                for (int half = 0; half < 2; half++) {
                        char c = text[i * 2 + half];
                        int  nibble = (c >= '0' && c <= '9') ? c - '0' :
                                      (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                                      (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                        if (nibble < 0) return -1;
                        value = (value << 4) | nibble;
                }
                out[i] = (uint8_t)value;
        }
        return 0;
}

static uint32_t crc32c(const char *path, uint64_t *size_out)
{
        char    *buffer = (char *)malloc(65536);
        uint32_t state = 0xffffffffU;
        uint64_t offset = 0;

        if (buffer == NULL) return 0;
        for (;;) {
                uint64_t got = xapi_ReadFile((WSTR)path, buffer, 65536, offset);

                if (got == 0) break;
                if (got == (uint64_t)-1) { free(buffer); return 0; }
                state = xj_root_binding_crc32c_update(state, buffer, got);
                offset += got;
        }
        free(buffer);
        *size_out = offset;
        return state ^ 0xffffffffU;
}

static void put_le32(uint8_t *p, uint32_t v)
{
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
        for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int write_binding(void)
{
        static const uint8_t magic[8] = {'X', 'J', 'R', 'O', 'O', 'T', '0', '1'};
        const char *disk_hex = li_state_get("disk.guid");
        const char *root_hex = li_state_get("part.1.guid");
        const char *esp_hex  = li_state_get("part.0.guid");
        const char *uuid_hex = li_state_get("part.root.uuid");
        XJ_ROOT_BINDING binding;
        uint64_t kernel_bytes = 0;
        uint32_t kernel_crc;

        if (disk_hex == NULL || root_hex == NULL || esp_hex == NULL || uuid_hex == NULL)
                return li_fail("missing GUID/UUID state (run 20/30 first)", 0);

        memset(&binding, 0, sizeof(binding));
        memcpy(binding.bytes, magic, sizeof(magic));
        put_le32(binding.bytes + 8, 1);
        put_le32(binding.bytes + 12, XJ_ROOT_BINDING_SIZE);
        if (parse_hex(disk_hex, binding.bytes + 16, 16) != 0 ||
            parse_hex(root_hex, binding.bytes + 32, 16) != 0 ||
            parse_hex(esp_hex, binding.bytes + 48, 16) != 0 ||
            parse_hex(uuid_hex, binding.bytes + 64, 16) != 0)
                return li_fail("invalid GUID/UUID state", 0);

        kernel_crc = crc32c(LI_TARGET "/system/kernel.krl", &kernel_bytes);
        if (kernel_bytes == 0) return li_fail("cannot read kernel for binding", 0);
        put_le64(binding.bytes + 80, kernel_bytes);
        put_le32(binding.bytes + 88, kernel_crc);
        put_le32(binding.bytes + 92, xj_root_binding_crc32c(binding.bytes, 92));

        li_make_dirs(LI_ESP_ROOT "/EFI/XJ380");
        if (li_write_binary(LI_ESP_ROOT "/EFI/XJ380/root.bin", binding.bytes, sizeof(binding)) != 0)
                return li_fail("write root.bin", 0);
        li_log("root.bin written (kernel=%llu bytes crc=%08x)",
               (unsigned long long)kernel_bytes, kernel_crc);
        return 0;
}

int main(int argc, char *argv[], char *envp[])
{
        const char *scheme;
        int missing = 0;

        (void)argc;
        (void)argv;
        (void)envp;

        li_step_begin("Verify boot files");

        scheme = li_state_get("scheme");
        if (scheme != NULL && (strcmp(scheme, "upstream") == 0 || strcmp(scheme, "modern") == 0)) {
                if (publish(LI_TARGET "/EFI/BOOT/BOOTX64.EFI",
                            LI_ESP_ROOT "/EFI/BOOT/BOOTX64.EFI") != 0)
                        return li_step_end(li_fail("publish BOOTX64.EFI to ESP", 0));
                if (publish(LI_TARGET "/system/kernel.krl",
                            LI_ESP_ROOT "/system/kernel.krl") != 0)
                        return li_step_end(li_fail("publish kernel to ESP", 0));

                missing += check_file(LI_ESP_ROOT "/EFI/BOOT/BOOTX64.EFI", "leopard-boot on ESP");
                missing += check_file(LI_ESP_ROOT "/system/kernel.krl", "kernel on ESP");
                missing += check_file(LI_ESP_ROOT "/system/boot.conf", "boot.conf on ESP (60-config)");
                if (missing != 0) return li_step_end(li_fail("ESP boot files incomplete", missing));

                if (write_binding() != 0) return li_step_end(1);
                missing += check_file(LI_ESP_ROOT "/EFI/XJ380/root.bin", "root binding (70)");
                if (missing != 0) return li_step_end(li_fail("root binding missing", missing));
        } else {
                missing += check_file(LI_TARGET "/EFI/BOOT/BOOTX64.EFI", "leopard-boot");
                missing += check_file(LI_TARGET "/system/kernel.krl", "leopard-xsk");
                if (missing != 0)
                        return li_step_end(li_fail("boot markers incomplete", missing));
        }

        li_state_set("boot.marker", "1");
        return li_step_end(0);
}
