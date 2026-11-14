# leopard-installer

LeopardOS 安装器（CLI，Debian 式）。**声明式 InstallPlan + 步骤执行器**：

```
main.c                                  编排器：读序列文件，按序跑步骤程序，全成功则重启
include/li_plan.h + lib/li_plan.c       InstallPlan：布局/挂载/模块/收尾的单一真源
steps/15-plan.c                         解析配置并持久化 InstallPlan（不写盘）
steps/20-partition.c                    只执行 plan 里的分区，不再重新计算布局
steps/*.c                               其余步骤消费 plan 产出的 part.* 状态
data/leopard-install-sequence.conf      步骤清单（全路径，行序=执行顺序）
data/leopard-install.conf               安装选项（auto / disk / scheme / modules …）
```

15-plan 先把配置解析成 `LiPlan`（disk / scheme / partitions / modules / init / finish），
写入 state；后续步骤只消费这个 plan，不再各自推导分区。步骤之间仍然只通过
`/config/installer/state` 交换数据，因此任何一步都能单独替换、重排、重跑。

## 步骤

| id | 程序 | 做什么 | 产出状态 |
|---|---|---|---|
| `10-disk` | `steps/10-disk.elf` | 列块设备、选目标盘 | `disk`、`disk.sectors` |
| `15-plan` | `steps/15-plan.elf` | 解析 scheme → `LiPlan`，打印并写入 state（不写盘） | `scheme`、`part.count`、`part.N.*`、`plan.modules` |
| `20-partition` | `steps/20-partition.elf` | 执行 plan：建 GPT、rescan、探测分区节点 | `part.N.node`、`esp/root/users.node` |
| `30-format` | `steps/30-format.elf` | `mkfs-fat32` 每个分区 | `part.N.formatted` |
| `40-mount` | `steps/40-mount.elf` | 挂到 `/mnt` 与 `/mnt/<mount>` | `mounted.count`、`mounted.N.*` |
| `50-packages` | `steps/50-packages.elf` | `xbps-install -r /mnt -R /media` | — |
| `60-config` | `steps/60-config.elf` | 写 `system/config/fstab`、`system/boot.conf` | — |
| `70-bootloader` | `steps/70-bootloader.elf` | 校验 `EFI/BOOT/BOOTX64.EFI`、`system/kernel.krl` | `boot.marker` |
| `80-verify` | `steps/80-verify.elf` | 整体校验（启动标记 / PID1 / initrc / 配置） | — |
| `90-finish` | `steps/90-finish.elf` | `sync` + 逆序 `umount`（重启由编排器做） | — |

## 分区方案

- `scheme=auto`（默认）：可用区三等分、1 MiB 对齐 → `ESP`(根) / `apps` / `users`；
- `scheme=custom`：逐分区给挂载点与大小（MiB），可挂任意目录。
  **第一个分区必须是 `/`**：内核 `mount_root` 只认全局索引最低、且同时含
  `EFI/BOOT/BOOTX64.EFI` 与 `system/kernel.krl` 的分区。

节点名不硬编码：`/dev/<disk>p<N>` 是内核的**全局分区索引**，所以 rescan 后按
「连续 N 个节点的扇区数序列」反查起始索引，fstab 里写的也是探测到的真名。

## 安装位置

- 编排器：`/apps/system/leopard-installer.elf`（live 的 `system/config/initrc` 尾端调用它）
- 步骤程序：`/apps/leopard/installer/steps/<id>.elf`
- 序列 / 选项：`/config/leopard-install-sequence.conf`、`/config/leopard-install.conf`

## 构建 / 打包

```bash
python3 gen_ninja.py && ninja build   # → out/leopard-installer.elf + out/steps/*.elf
ninja pack                            # → out/leopard-installer-1.2.0_1.x86_64.xbps
```

包内含编排器、9 个步骤程序与两份配置。`ninja pack` 需要 `xbps-create`（live 环境或 SDK 里自带）。

## 无人值守

在 live 的 `/config/leopard-install.conf` 里写：

```
auto=1
disk=sata0
scheme=auto
```

之后整个序列不再提问（步骤程序读不到 `auto=1` 就按交互模式走）。

> 代码尚未经过编译器验证：开发机原本没有 clang/ninja/qemu，也还没有 WSL 环境。
