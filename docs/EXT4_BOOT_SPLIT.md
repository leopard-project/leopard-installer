# 分离引导分区 + ext4 根：上游方案与 Leopard 用户态改法

> 依据：OpenXJ380 `origin/main`（93d76a7）的 `include/installer/disk_layout.h`、
> `include/efi/root_binding.h`、`boot/bootx64.c`、`driver/fs/partition.cpp`、
> `driver/fs/ext4/format.cpp`。本文只规划**非原语（用户态/引导）**部分；
> 内核 ext4 驱动属于原语，单列在 §4。

## 1. 上游新方案（已核对）

### 1.1 分区布局

GPT，两块：

| 分区 | 类型 GUID | 大小 | 文件系统 | 内容 |
|---|---|---|---|---|
| ESP（boot） | `c12a7328-f81f-11d2-ba4b-00a0c93ec93b` | 默认 64 MiB | FAT32 | `EFI/BOOT/BOOTX64.EFI`、`system/kernel.krl`、`system/boot.conf`、`EFI/XJ380/root.bin` |
| root | `0fc63daf-8483-4772-8e79-3d69d8477de4` | 剩余 | ext4 | 完整系统树（`/system`、`/apps`、`/users`、`/config` 都是目录） |

不再有独立的 `/apps`、`/users` FAT32 分区。

### 1.2 root.bin（96 字节，ESP 上）

`\EFI\XJ380\root.bin`，字段（偏移）：

| 偏移 | 内容 |
|---|---|
| 0 | magic `XJROOT01` |
| 8 | version（1=ext4，2=FAT32 legacy） |
| 12 | 固定 96 |
| 16 / 32 / 48 | GPT disk GUID / root 分区 GUID / ESP 分区 GUID |
| 64 | ext4 UUID（v1）或 FAT32 volume id（v2） |
| 80 | kernel.krl 字节数 |
| 88 | kernel CRC32C |
| 92 | 描述符 CRC32C（覆盖 0..91） |

启动顺序：BOOTX64 读 root.bin → 校验 ESP GUID 与 kernel CRC → 置
`BOOT_FLAG_BOUND_ROOT` 并交给内核 → 内核按 GUID 找 root 分区，挂 ext4 为 `/`。

### 1.3 上游的“格式化”在哪

没有独立 `mkfs-ext4` 工具：上游在**内核**里用 `driver/fs/ext4/format.cpp`
（纯逻辑 + 读写回调，Apache-2.0）给安装器格式化 ext4。这正是我们要搬到用户态的部分。

## 2. 目标：Leopard 的默认方案

- 默认 `scheme=ext4`：ESP 64 MiB FAT32 + ext4 root（两块）。
- 保留 `scheme=fat32`：当前三分区（root/apps/users 全 FAT32）作为兼容/调试路径。
- 引导相关文件只放 ESP；系统树只放 ext4 root。
- `/system/config/permissions.conf`、`initrc` 等仍在 root 的 `/system/config`（ext4）。

## 3. 用户态改动（本仓库与 leopard-utils / base / os）

### 3.1 `leopard-utils` 新增 `mkfs-ext4`

- 源码：移植 `driver/fs/ext4/format.cpp` + `format.h`，把 `read/write/flush` 回调换成
  `pread/pwrite/fsync`；工作区约 512 KiB 静态区。
- 用法：`mkfs-ext4 <node> [label]`，输出一行供安装器采集：
  `mkfs-ext4: uuid=<32hex> blocks=<n> journal=<mib>MiB`。
- UUID 由工具生成（`/dev/urandom` 或 RTC + 随机数），全零拒绝。
- 打包：`leopard-utils` 的 `TOOLS` 增加 `mkfs-ext4`（安装到 `/apps/system/mkfs-ext4`）。
- live root：`leopard-os/build_liveroot.sh` 增加拷贝 `mkfs-ext4.elf`。

### 3.2 `leopard-installer`

| 步骤 | 改动 |
|---|---|
| `10-disk` | 不变 |
| `20-partition` | 新默认 `scheme=ext4`：p0=ESP 64 MiB（ESP 类型 GUID，label `LP-ESP`），p1=root（Linux 类型 GUID，label `LEOPARD`，剩余空间）。保存 `disk.guid`、`part.0/1.guid`、`part.N.node/label/mount`。保留 `scheme=fat32` 旧三分区。 |
| `30-format` | p0 `mkfs-fat32`；p1 `mkfs-ext4`，把 `uuid` 写入 `part.root.uuid`。 |
| `40-mount` | root（ext4）挂 `/mnt`；ESP（fatfs）挂 `/mnt/boot`。不再有 `/mnt/apps`、`/mnt/users`。 |
| `50-packages` | 只向 ext4 root 装系统包：`OpenXSKernel`/`OpenXSBoot` 不再装进 root（见 70）；root 集合 = base/rc/getty/oobe/shell/mountd/xbps(+utils)。 |
| `60-config` | root：`/system/config/fstab`（只留注释/根）；ESP：`/system/boot.conf`（`init=/system/leopard/leopard-rc.elf`）。 |
| `70-bootloader` | 从 `/media` 的 `OpenXSBoot`/`OpenXSKernel` 包里取出 `BOOTX64.EFI` 与 `kernel.krl` 写入 ESP；计算 kernel 长度/CRC32C；写 `EFI/XJ380/root.bin`；校验 `EFI/BOOT/BOOTX64.EFI`、`system/kernel.krl`、root.bin。 |
| `80-verify` | 校验 ESP 三件套 + root 的 `system/leopard/leopard-rc.elf`、`system/config/initrc`、ext4 UUID 与 root.bin 一致。 |
| `90-finish` | 先 sync，再逆序卸载 ESP、root。 |

状态键新增：`disk.guid`、`part.N.guid`、`part.root.uuid`、`esp.mount`、`boot.marker`。

**红线调整**：installer 现在“不复制二进制”。分离 ESP 后，`OpenXSBoot`/`OpenXSKernel`
两个包的产物必须由 `70-bootloader` 发布到 ESP（xbps 不适合在 FAT32 ESP 上留元数据）。
建议把红线改为：“系统包不复制；**引导产物由 70 从 /media 包中提取并发布到 ESP**”。

### 3.3 `leopard-base`

- `fstab`：新方案只保留注释（root 由绑定挂载，无额外分区）；旧 FAT32 方案仍可用。
- `boot.conf`：保持默认（live ISO 用）；安装后的 ESP 版本由 `60-config` 生成。
- 目录骨架不变（`/system/config` 在 ext4 root 上）。

### 3.4 `leopard-os`

- `build_liveroot.sh`：live root 增加 `mkfs-ext4.elf`；其余不变（ISO 仍是 tmpfs 安装环境）。
- `img-tools/assemble_image.sh`（预装镜像）：若也切新方案，需要宿主机 `mkfs.ext4`
  + 无 root 挂载手段（`e2tools`/`debugfs -w`）写入 ext4；建议**先保留 FAT32 预装镜像**，
  新方案只在安装器路径验证，等 ext4 内核驱动稳定后再切换。`packages.conf` 不变。

## 4. 内核/引导原语前置（不属于用户态，但缺了跑不起来）

1. **ext4 驱动**：移植 `driver/fs/ext4/**`（约 26k 行，Apache-2.0）+ `driver/block_access`、
   `block_source`、AHCI/NVMe flush；挂载/读写/日志回放。
2. **bound root**：
   - `include/efi/root_binding.h`（96 字节 ABI，boot/kernel 共享）。
   - `BOOT_CONFIG` 增加 `root_binding`；`mount_root` 按 GUID 选 root 并挂 ext4/FAT32。
   - 分区层需要 disk/root/ESP GUID 库存（我们的 `partition_t` 已有 GUID 字段）。
3. **OpenXSBoot**：
   - 读 `\EFI\XJ380\root.bin`、校验 ESP GUID、校验 kernel CRC；
   - 置 bound-root 标志并把 binding 放进 `BOOT_CONFIG`；
   - **flag 冲突**：上游 `BOUND_ROOT=(1<<8)` 与我们现有 `LIVE_ROOT=(1<<8)` 冲突。
     两个仓库都是我们的，建议 `LIVE_ROOT` 保持 `1<<8`、新增 `BOUND_ROOT=(1<<9)`
     （上游的 `INSTALLER_HANDOFF` 我们没用到）。
4. **installer 的 GPT GUID 生成**：`20-partition` 现在会写 GPT，需要把 disk/partition
   GUID 持久化到 state，供 `70` 写 root.bin。

## 5. 建议实施顺序

**Stage A（先跑通“引导分区分离”，不依赖 ext4）**
- ESP 64 MiB FAT32 + root FAT32（binding v2，`filesystem=1`）。
- 改 boot/kernel 的 binding + `70-bootloader` + `60-config` + `40-mount`。
- 好处：不动 26k 行 ext4 驱动，先把新启动链和安装步骤验证通。

**Stage B（切 ext4 默认）**
- 移植 ext4 驱动（内核原语）。
- 新增 `mkfs-ext4`；`20/30/40/60/70/80` 切 ext4 默认；`50-packages` 调整。
- 预装镜像（`img-tools`）最后再切。

**Stage C（可选）**
- 预装镜像/ext4 宿主工具链；去掉 FAT32 三分区兼容路径。

## 6. 需要拍板的点

1. 是否按 Stage A → B 分两步（推荐），还是一次做完整链？
2. 预装镜像（`img-tools`）是否本轮就切 ext4，还是只改安装器路径？
3. `mkfs-ext4` 的 UUID 来源：`/dev/urandom` 优先，失败回退 RTC+计数器（可接受？）。
