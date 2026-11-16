# leopard-installer

LeopardOS 安装器（全屏 TUI + 行模式回退）。**声明式 InstallPlan + 步骤执行器**：

```
main.c                                  编排器：读序列文件，按序跑步骤程序；TUI 由它独占
include/li_ui.h + lib/li_ui.c           步骤侧 UI 协议（TUI/line/auto 三种模式）
include/li_tui.h + lib/li_tui.c         lpcurses 全屏控件（框架/菜单/输入/确认/日志）
include/li_ui_host.h + lib/li_ui_host.c 编排器侧 TUI host：UI 管道、日志捕获、请求路由
include/li_plan.h + lib/li_plan.c       InstallPlan：布局/挂载/模块/收尾的单一真源
steps/15-plan.c                         解析配置并持久化 InstallPlan（不写盘）
steps/20-partition.c                    只执行 plan 里的分区，不再重新计算布局
steps/*.c                               其余步骤消费 plan 产出的 part.* 状态
data/leopard-install-sequence.conf      步骤清单（全路径，行序=执行顺序）
data/leopard-install.conf               安装选项（auto / disk / profile / package_profile …）
data/install-profiles.conf              安装方案：分区布局（modern / upstream / classic / custom）
data/format-profiles.conf               格式化方案：fatfs / ext4 的工具与参数
data/package-profiles.conf              软件选择：核心环境 + 组 + 软件方案
```

步骤程序仍然是独立可执行文件，彼此只通过 `/config/installer/state` 交换数据。
TUI 模式只改变「用户如何选择」：步骤不直接调用 curses，而是通过继承的 UI 管道
向编排器发 `MENU` / `INPUT` / `CONFIRM` 请求；auto/line 模式完全不初始化 curses。

## UI 模式

| 模式 | 触发条件 | 行为 |
|---|---|---|
| `auto` | `auto=1` | 零输入；步骤立即返回默认值，不初始化 curses |
| `line` | 非 tty、`LEOPARD_INSTALL_TUI=0`，或 lpcurses 初始化失败 | 原有 `xapi_Input` 行提示 |
| `tui` | 交互式 tty（conhost PTY slave） | 编排器独占 lpcurses 全屏界面 |

`main.c` 只选择一次模式。`initscr()` 失败会回退到 `line`，不会中止安装。
TUI 模式下编排器给每个步骤设置：

```
LI_UI_MODE=tui
LI_UI_FD=<host -> step pipe>
LI_UI_OUT_FD=<step -> host pipe>
LI_STATE_DIR=/config/installer
```

步骤的标准输入被关闭，标准输出/错误被接到日志管道；步骤永远不碰真实终端。
协议按行、字段用 TAB 分隔：

```
step -> host:  STEP <title>
               MENU <id>\t<title>\t<selected>
               ITEM <index>\t<title>\t<description>
               END
               INPUT <id>\t<prompt>\t<default>
               CONFIRM <id>\t<prompt>\t<y|n>
               PROGRESS <0..100>\t<text>
               LOG <text>
               DONE <rc>
host -> step:  ANSWER <id>\t<value>
               CANCEL <id>
```

菜单/输入/确认会阻塞步骤直到收到对应回答；进度与日志是异步事件。编排器在
等待键盘时持续排空日志管道，因此长时间步骤不会因为管道写满而卡住。

当前由 TUI 控件接管的选择：

- `10-disk`：磁盘列表菜单 + 清盘二次确认；
- `15-plan`：安装方案菜单 + 计划失败后的 Retry / Change profile / Shut down；
- `20-partition`：写分区表确认（沿用 `li_confirm`，自动走 TUI 确认框）；
- `45-packages-select`：软件方案菜单 + 软件选择确认；
- 收尾：Reboot / Shut down 菜单。

`50-packages` 及其余非交互步骤只发 `PROGRESS` / `LOG`，进度条与日志面板实时更新。

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
| `45-packages-select` | `steps/45-packages-select.elf` | 解析软件方案 → 核心包 + 组 + 可选包/模块，写入 `plan.*` | `package_profile`、`plan.core_packages`、`plan.optional_packages`、`plan.modules`、`plan.packages_selected` |
| `50-packages` | `steps/50-packages.elf` | 核心 + 可选包一次事务；模块一次事务 | — |
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
```

- 只描述**磁盘布局**；软件选择在 `package-profiles.conf`（避免两处真源）；
- 支持 `64M`、`30%`、`third`、`rest` 尺寸；
- `profile=` 选择方案，旧 `scheme=` 仍兼容；
- `custom` 继续使用 `part.N.mount` + `part.N.size_mb`；
- **第一个分区必须是 `/`**：内核 `mount_root` 只认全局索引最低、且同时含
  `EFI/BOOT/BOOTX64.EFI` 与 `system/kernel.krl` 的分区。

## 软件选择（package profiles）

`/config/package-profiles.conf` 采用声明式「核心环境 + 组 + 方案」模型，
类似 Debian tasksel / Fedora comps / Arch package groups：

```
core.packages=leopard-boot,leopard-xsk,leopard-base,leopard-rc,leopard-getty,leopard-conhost,leopard-doas,leopard-oobe,leopard-shell,leopard-mountd,xbps

group.list=console,input,network,usb
group.console.title=Console tools
group.console.description=Core utilities, disk tools and shell commands
group.console.packages=leopard-utils,leopard-coreutils
group.console.modules=
group.input.title=Input and IME
group.input.packages=leopard-ime
group.network.title=Network drivers
group.network.modules=e1000-kmod,netserver-kmod
group.usb.title=USB support
group.usb.modules=xhci-kmod

package.list=minimal,standard,full,custom
package.minimal.title=Minimal: core system only
package.minimal.groups=
package.standard.title=Standard: console tools and IME (recommended)
package.standard.groups=console,input
package.full.title=Full: standard plus all kernel modules
package.full.groups=console,input,network,usb
package.custom.title=Custom: explicit lists
package.custom.groups=
```

- `core.packages` 是必装基础环境；
- `group.<id>` 只提供 `title` / `description` / `packages` / `modules`；
- `package.<id>` 用 `groups=` 聚合组，也可直接写 `packages=` / `modules=`；
- 解析时对组和方案的包/模块取并集并去重，未知组名只告警、不中止；
- `45-packages-select` 解析并写入 `plan.core_packages`、
  `plan.optional_packages`、`plan.modules`、`plan.packages_selected`；
- `50-packages` 把核心 + 可选包放进**一次 xbps 事务**，模块放进一次事务；
  合并事务失败时自动重试「仅核心包」，并告警降级（系统仍可启动）；
- `package_profile=` 选择方案，缺省 `standard`（含控制台工具与 IME）；
- `optional_packages=` / `modules=` 是显式用户覆盖，优先于方案解析结果；
- 旧格式的 `package.<id>.packages` / `.modules` 仍可直接使用；
- 没有 `core.packages` 时，`50-packages` 回退到内置核心包列表（旧镜像兼容）。

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

交互模式（TUI）会弹出：

```
Retry          Try the same profile again
Change profile Pick a different layout
Shut down      Stop the installer
```

line 模式保留 `[r]etry / [c]hange profile / [s]hutdown`。auto 模式仍然
fail-fast，不阻塞等键盘。

节点名不硬编码：`/dev/<disk>p<N>` 是内核的**全局分区索引**，所以 rescan 后按
「连续 N 个节点的扇区数序列」反查起始索引，fstab 里写的也是探测到的真名。

## 串口可观测性

安装器现在由 `leopard-conhost` 在 PTY 上运行，`xapi_Printf` 的常规输出只进 VT。
为无头测试与现场诊断，安装器会把这些内容再镜像到串口（不改 VT 路径）：

- `li_log` / `li_header` / `li_step_begin` / `li_step_end` / `li_fail`；
- `main.c` 的顶层消息：序列缺失、步骤缺失、步骤失败、全部完成、关机/重启、
  auto 模式 fail-fast；
- TUI 模态边界：`installer: ui welcome|menu|input|confirm|error|finish <text>`。
  这些标记让无头测试可以在正确的时机注入按键，也方便现场判断安装器停在哪个对话框。

测试断言（`tests/test_install.sh`）因此可以直接从串口日志看到每一步的
`== <步骤标题> ==` 与结果。安装器不读取密码，镜像内容不含敏感数据。

步骤程序与外部工具由 `li_run` / `main.c::run_step` 以 `fork()` + `execve()` 启动，
直接继承安装器的 fd 0/1/2（conhost PTY slave），不再走可能回退到 `/dev/stdio`
的 `xapi_RunArgs`；子进程的 `xapi_Printf` / `xapi_Input` 因此与安装器同一个终端。

## 安装位置

- 编排器：`/apps/system/leopard-installer.elf`（live 的 `system/config/initrc` 尾端调用它）
- 步骤程序：`/apps/leopard/installer/steps/<id>.elf`
- 序列 / 选项：`/config/leopard-install-sequence.conf`、`/config/leopard-install.conf`

## 构建 / 打包

```bash
# lpcurses 是构建期依赖，只需先构建一次
(cd ../lpcurses && python3 gen_ninja.py && ninja build)

python3 gen_ninja.py && ninja build   # → out/leopard-installer.elf + out/steps/*.elf
ninja pack                            # → out/leopard-installer-1.6.0_1.x86_64.xbps
```

包内含编排器、11 个步骤程序与五份配置。编排器静态链接
`../lpcurses/out/liblpcurses.a`；lpcurses 不是运行期依赖。`ninja pack` 需要
`xbps-create`（live 环境或 SDK 里自带）。

## 测试

```bash
# 无头 auto=1 安装 + 启动
sh ../leopard-os/tests/test_install.sh
sh ../leopard-os/tests/test_boot.sh

# 真正 TUI：QEMU monitor 注入按键，串口断言
sh ../leopard-os/tests/test_install_tui.sh
```

`test_install_tui.sh` 不需要图形显示：conhost 的 VT 在 framebuffer 上渲染，
测试通过 QEMU HMP `sendkey` 走欢迎 → 磁盘 → 安装方案 → 分区确认 → 软件方案 →
进度 → 收尾，并用串口日志断言每一步。

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
`standard` 现在是缺省值，包含控制台工具与 IME；需要网络/USB 模块时用 `full`
或显式写 `modules=`。
