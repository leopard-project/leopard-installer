# PROJECT KNOWLEDGE BASE — leopard-installer

LeopardOS 安装器：**声明式 InstallPlan + 序列执行器**。

启动方式：live 环境的 initrc 是

    respawn /apps/leopard/leopard-conhost.elf /apps/system/leopard-installer.elf

即 conhost 拥有 console/VT，在 PTY 上运行 installer；installer 继续使用
`XAPI_PRINTF` / `XAPI_INPUT`，由 conhost 的 X3TP terminal view 提供输入输出。
不再直接 `respawn installer`，也不再依赖裸串口 TTY。

```
main.c                              编排器：读 /config/leopard-install-sequence.conf，
                                    按行序以「全路径、无参数」执行步骤程序；失败问重试/关机
include/li_plan.h + lib/li_plan.c    InstallPlan：布局/挂载/模块/收尾的单一真源
steps/15-plan.c                     解析 profile 成 LiPlan，打印并写入 state（不写盘）
steps/20-partition.c                只执行 plan 里的分区；不再自己计算布局
steps/NN-*.c                        其余步骤消费 plan 产出的 part.* 状态
include/li.h + lib/li_core.c        步骤公共库（状态/选项/日志/交互/子进程/块设备）
data/leopard-install-sequence.conf  步骤清单（行序=执行顺序）
data/leopard-install.conf           安装选项（auto / disk / profile / package_profile / modules …）
data/install-profiles.conf          安装方案（只描述分区布局）
data/format-profiles.conf           格式化方案（fstype → tool/args）
data/package-profiles.conf          软件选择（核心环境 + 组 + minimal/standard/full/custom）
```

## TUI 架构（1.6.0 起）

```
main.c                               TUI 唯一 owner：lpcurses 屏幕 + 步骤序列 + 日志面板
include/li_ui.h + lib/li_ui.c        步骤侧协议客户端；line/auto 回退
include/li_tui.h + lib/li_tui.c      lpcurses 控件：框架、菜单、输入、确认、日志环
include/li_ui_host.h + lib/li_ui_host.c
                                     编排器侧：UI 管道、子进程、请求路由、日志捕获
```

- **只有编排器调用 curses**。步骤程序绝不 `initscr()`；TUI 模式下它们通过
  `LI_UI_FD` / `LI_UI_OUT_FD` 发 `MENU` / `INPUT` / `CONFIRM`，行模式才走
  `xapi_Input`。
- **auto=1 永不初始化 curses**；`LEOPARD_INSTALL_TUI=0` 或非 tty 走 line 模式；
  `initscr()` 失败必须回退 line，不能中止安装。
- 子进程的 stdout/stderr 接到日志管道，stdin 关闭；编排器在模态循环里
  用 poll 排空日志，**不要先 read 再 poll**（空管道会阻塞，菜单就再也读不到键）。
- 模态边界镜像到串口：`installer: ui menu|input|confirm|error|finish ...`，
  无头 TUI 测试靠它同步按键。
- `lpcurses` 是构建期静态依赖（MIT），链接编排器，不进入运行期包依赖。

## 架构红线（改动前必读）

- **main.c 是编排器，不许把安装逻辑写回去**：它只负责"读序列 → 逐个起进程 → 等退出 →
  失败问重试/关机 → 全成功重启"。新增/调整步骤只动 `steps/` 和序列文件。
- **布局只在 15-plan / li_plan.c 计算**：20-partition 只执行 plan；30/40/60 只读
  plan 写入的 `part.*`。不要在步骤里重新推导 scheme 或分区尺寸。
- **安装方案只描述布局**：`data/install-profiles.conf` 定义 modern / upstream /
  leopard-classic / custom 的分区；`profile=` 选择，旧 `scheme=` 兼容。
  不要在 C 里新增硬编码 scheme。
- **格式化也来自配置**：`data/format-profiles.conf` 定义 `format.<fstype>.tool/args`；
  30-format 只按 `part.N.fs` 查表，新增 fstype 不应改 C 分支。
- **软件选择也来自配置**：`data/package-profiles.conf` 用声明式
  `core.packages` + `group.<id>` + `package.<id>` 模型；`45-packages-select`
  解析成 `plan.core_packages` / `plan.optional_packages` / `plan.modules`，
  `50-packages` 只负责执行。新增软件组合只改配置，不要在步骤里写死列表。
- **两个清单不许混用**：分区布局看 `install-profiles.conf`，软件看
  `package-profiles.conf`；install-profiles 不再携带 packages/modules。
- **包安装事务**：核心 + 可选包在**一次** xbps 事务里装（并集去重），模块一次事务；
  合并事务失败自动重试 core-only 并告警，只有核心失败才让步骤失败。
  `optional_packages=` / `modules=` 是显式用户覆盖；`minimal` 是真正的空集
  （`plan.packages_selected` 标记，不回退到内置 EXTRA）。
- **计划失败**：交互模式 15-plan 提供 `[r]etry / [c]hange profile / [s]hutdown`；
  auto 模式 fail-fast，不阻塞键盘。
- **步骤程序之间不许互相调用**：只通过 `/config/installer/state`（key=value）交换数据。
  这样任何一步都能单独重跑、替换、删掉。
- **步骤程序无参数**：不要设计 `--flag`；可配置项一律走 `/config/leopard-install.conf`
  （选项文件先加载，state 覆盖它）。
- **不复制二进制**：`50-packages` 用 xbps 把内核/引导器/用户态装到目标盘；安装器只编排。
- **分区方案**：`modern`（默认，ESP+ext4 root+ext4 /users）、`upstream`（ESP+ext4 root）、
  `leopard-classic`（3×FAT32）、`custom`。旧名 `ext4`→upstream、`fat32`/`auto`→leopard-classic。
  `upstream`/`modern` 的 ESP 都挂到 `/boot`；`leopard-classic` 没有 `/boot`。
- **挂载 fstype**：fstab 第三列原样交给内核（`fatfs`/`ext4`）；旧格式缺省回退 `fatfs`。
- **分区节点名不硬编码**：`/dev/<disk>p<N>` 是内核全局分区索引，按「连续 N 个节点扇区数序列」
  反查起始索引；根分区必须排第 0（`mount_root` 只认索引最低且带启动标记的分区）。
- **写文件先删后写**：`xapi_WriteFile` 不截断；探测存在性用 `xapi_ReadFile` 读 1 字节，
  不要用 `xapi_OpenFile/CloseFile`（整文件读-改-写）。
- **包名用实名**：`leopard-utils`（单包，含 poweroff/initctl/lpdisk/mkfs-fat32）、`xbps`（与仓库目录名 `leopard-xbps` 不同）。
- **安装完成后**：交互模式询问「1) 重启 / 2) 关机」（实体机可在重启前拔掉安装介质）；
  auto=1 按 `finish=reboot|shutdown` 自动执行，缺省重启，仍然零输入。
- **auto=1 必须零输入**：任何代码路径都不许在 auto 下阻塞等键盘。`li_ask` 在 auto 返回 -1
  （调用方须回退到 conf 里的 state 值）；`li_confirm` 返回默认值；`main.c` 的失败重试在 auto
  下直接关机（fail-fast），不打印 `[r/s]`。破坏性确认（如 10-disk 的「清盘」）在 auto 下跳过。
  无头测试 = 写全 conf（`auto=1 disk=sata0 profile=modern package_profile=standard`）+ 断言串口日志，不敲任何键。
- **串口可观测性**：安装器跑在 conhost PTY 下，`xapi_Printf` 只进 VT；
  `li_log` / `li_header` / `li_step_*` / `li_fail` 与 `main.c` 顶层消息额外用
  `xapi_OutputSerial` 镜像到串口。无头测试依赖这条通道，不要删。
- **子进程用 fork/execve**：`li_run`（步骤公共库）与 `main.c::run_step` 用
  `fork()` + `execve()` + `waitpid()` 启动步骤/工具，子进程直接继承 fd 0/1/2
  （conhost 的 PTY slave）。不要改回 `xapi_RunArgs`：它可能回退到 `/dev/stdio`，
  导致步骤读不到 PTY 输入、也看不到终端输出。
- 不新增内核 syscall、不改 `OpenXSKernel`；只用既有原语（`SYS_MOUNT`/`SYS_UMOUNT2`/`SYS_WAIT4`/`SYS_SYNC`…）。

## 状态键（步骤间契约）

见 `README.md` 表格：`disk`、`profile`、`package_profile`、`part.*`、`mounted.*`、
`plan.core_packages`、`plan.optional_packages`、`plan.modules`、`plan.packages_selected`、
`plan.init`、`init`、`boot.marker`、`auto`。
新增键请同时更新 README。

## 构建 / 打包

    (cd ../lpcurses && python3 gen_ninja.py && ninja build)   # 首次构建期依赖
    python3 gen_ninja.py && ninja build   # → out/leopard-installer.elf + out/steps/*.elf
    ninja pack                            # → 一个 xbps 包含编排器 + 全部步骤 + 五份配置

- 编排器链接 `../lpcurses/out/liblpcurses.a`（静态、MIT）；步骤程序不链接 curses。
- 静态 ELF，依赖 `../XJ380-SDK/out/crt`（`include/` 头 + `lib/x86_64/` 启动对象、`libxact.a`、`liballoc.a`）。
- 链接固定 `-Ttext=0x200000`。
