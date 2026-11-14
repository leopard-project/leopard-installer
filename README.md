# leopard-installer

LeopardOS 安装器（CLI，Debian 式）。**声明式 InstallPlan + 步骤执行器**：

```
main.c                                  编排器：读序列文件，按序跑步骤程序，全成功则重启
include/li_plan.h + lib/li_plan.c       InstallPlan：布局/挂载/模块/收尾的单一真源
steps/15-plan.c                         解析配置并持久化 InstallPlan（不写盘）
steps/20-partition.c                    只执行 plan 里的分区，不再重新计算布局
steps/*.c                               其余步骤消费 plan 产出的 part.* 状态
data/leopard-install-sequence.conf      步骤清单（全路径，行序=执行顺序）
data/leopard-install.conf               安装选项（auto / disk / profile / modules …）
data/install-profiles.conf              安装方案：modern / upstream / classic / custom
data/format-profiles.conf               格式化方案：fatfs / ext4 的工具与参数
```

15-plan 先把配置解析成 `LiPlan`（disk / profile / partitions / modules / init / finish），
写入 state；20-partition 只执行 plan，30-format 按 `format-profiles.conf` 选
formatter，后续步骤只消费 plan 产出的状态。步骤之间仍然只通过
`/config/installer/state` 交换数据，因此任何一步都能单独替换、重排、重跑。

## 步骤

| id | 程序 | 做什么 | 产出状态 |
|---|---|---|---|
| `10-disk` | `steps/10-disk.elf` | 列块设备、选目标盘 | `disk`、`disk.sectors` |
| `15-plan` | `steps/15-plan.elf` | 解析 profile → `LiPlan`，打印并写入 state（不写盘） | `profile`、`part.count`、`part.N.*`、`plan.modules` |
| `20-partition` | `steps/20-partition.elf` | 执行 plan：建 GPT、rescan、探测分区节点 | `part.N.node`、`esp/root/users.node` |
| `30-format` | `steps/30-format.elf` | 按 `format-profiles.conf` 格式化每个分区 | `part.N.formatted`、`part.root.uuid` |
| `40-mount` | `steps/40-mount.elf` | 挂到 `/mnt` 与 `/mnt/<mount>` | `mounted.count`、`mounted.N.*` |
| `45-packages-select` | `steps/45-packages-select.elf` | 选可选软件方案，写 `plan.optional_packages` / `plan.modules` | `package_profile` |
| `50-packages` | `steps/50-packages.elf` | 先装核心包，再装方案里的可选包/模块 | — |
| `60-config` | `steps/60-config.elf` | 写 `system/config/fstab`、`system/boot.conf` | — |
| `70-bootloader` | `steps/70-bootloader.elf` | 校验 `EFI/BOOT/BOOTX64.EFI`、`system/kernel.krl` | `boot.marker` |
| `80-verify` | `steps/80-verify.elf` | 整体校验（启动标记 / PID1 / initrc / 配置） | — |
| `90-finish` | `steps/90-finish.elf` | `sync` + 逆序 `umount`（重启由编排器做） | — |

## 安装方案（profile）

方案定义在 `/config/install-profiles.conf`，每行一个分区：

```
profile.modern.title=Modern: ESP + ext4 root + ext4 /users
profile.modern.part.0=ESP|LP-ESP|64M|fatfs|/boot|ef00
profile.modern.part.1=root|LEOPARD|30%|ext4|/|8300
profile.modern.part.2=users|LP-USERS|rest|ext4|/users|8300
profile.modern.packages=leopard-utils,leopard-coreutils
profile.modern.modules=
```

- 支持 `64M`、`30%`、`third`、`rest` 尺寸；
- `profile=` 选择方案，旧 `scheme=` 仍兼容；
- `custom` 继续使用 `part.N.mount` + `part.N.size_mb`；
- **第一个分区必须是 `/`**：内核 `mount_root` 只认全局索引最低、且同时含
  `EFI/BOOT/BOOTX64.EFI` 与 `system/kernel.krl` 的分区。

## 可选软件方案（package profile）

`/config/package-profiles.conf`：

```
package.list=minimal,standard,full,custom

package.minimal.title=Minimal: base system only
package.minimal.packages=
package.minimal.modules=

package.standard.title=Standard: utilities and core commands (recommended)
package.standard.packages=leopard-utils,leopard-coreutils
package.standard.modules=

package.full.title=Full: utilities + all kernel modules
package.full.packages=leopard-utils,leopard-coreutils
package.full.modules=e1000-kmod,netserver-kmod,xhci-kmod
```

- 核心包（boot/kernel/base/rc/getty/oobe/shell/mountd/xbps）总是安装；
- `45-packages-select` 选方案，`50-packages` 执行；
- `package_profile=` 选择方案（缺省 `standard`）；
- `optional_packages=` / `modules=` 可覆盖方案内容；
- `minimal` 是真正的空集，不会回退去装 `leopard-utils`。

## 格式化方案（format profile）

`/config/format-profiles.conf`：

```
format.fatfs.tool=mkfs-fat32.elf
format.fatfs.args={node} {label}
format.ext4.tool=mkfs-ext4.elf
format.ext4.args={node} --label {label} --uuid-out {uuid_out}
```

`30-format` 只按 `part.N.fs` 查表执行；新增文件系统只需加一段配置。

## 计划失败

15-plan 在交互模式下提供：

```
[r]etry / [c]hange profile / [s]hutdown
```

auto 模式仍然 fail-fast，不阻塞等键盘。

节点名不硬编码：`/dev/<disk>p<N>` 是内核的**全局分区索引**，所以 rescan 后按
「连续 N 个节点的扇区数序列」反查起始索引，fstab 里写的也是探测到的真名。

## 安装位置

- 编排器：`/apps/system/leopard-installer.elf`（live 的 `system/config/initrc` 尾端调用它）
- 步骤程序：`/apps/leopard/installer/steps/<id>.elf`
- 序列 / 选项：`/config/leopard-install-sequence.conf`、`/config/leopard-install.conf`

## 构建 / 打包

```bash
python3 gen_ninja.py && ninja build   # → out/leopard-installer.elf + out/steps/*.elf
ninja pack                            # → out/leopard-installer-1.4.0_1.x86_64.xbps
```

包内含编排器、10 个步骤程序与四份配置。`ninja pack` 需要 `xbps-create`（live 环境或 SDK 里自带）。

## 无人值守

在 live 的 `/config/leopard-install.conf` 里写：

```
auto=1
disk=sata0
profile=modern
package_profile=standard
# optional_packages=leopard-utils,leopard-coreutils
# modules=e1000-kmod,xhci-kmod
```

之后整个序列不再提问（步骤程序读不到 `auto=1` 就按交互模式走）。

> 代码尚未经过编译器验证：开发机原本没有 clang/ninja/qemu，也还没有 WSL 环境。
