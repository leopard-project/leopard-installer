# PROJECT KNOWLEDGE BASE — leopard-installer

LeopardOS 安装器：**序列编排器（main.c）+ 多可执行文件步骤程序（steps/）**。

```
main.c                              编排器：读 /config/leopard-install-sequence.conf，
                                    按行序以「全路径、无参数」执行步骤程序；失败问重试/关机
steps/NN-*.c                        每个安装步骤一个独立可执行文件
include/li.h + lib/li_core.c        步骤程序公共库（状态/选项/日志/交互/子进程/块设备）
data/leopard-install-sequence.conf  步骤清单（行序=执行顺序）
data/leopard-install.conf           安装选项（auto / disk / scheme / init …）
```

## 架构红线（改动前必读）

- **main.c 是编排器，不许把安装逻辑写回去**：它只负责"读序列 → 逐个起进程 → 等退出 →
  失败问重试/关机 → 全成功重启"。新增/调整步骤只动 `steps/` 和序列文件。
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
  无头测试 = 写全 conf（`auto=1 disk=sata0 scheme=auto`）+ 断言串口日志，不敲任何键。
- 不新增内核 syscall、不改 `OpenXSKernel`；只用既有原语（`SYS_MOUNT`/`SYS_UMOUNT2`/`SYS_WAIT4`/`SYS_SYNC`…）。

## 状态键（步骤间契约）

见 `README.md` 表格：`disk`、`scheme`、`part.*`、`mounted.*`、`init`、`boot.marker`、`auto`。
新增键请同时更新 README。

## 构建 / 打包

    python3 gen_ninja.py && ninja build   # → out/leopard-installer.elf + out/steps/*.elf
    ninja pack                            # → 一个 xbps 包含编排器 + 全部步骤 + 两份配置

- 静态 ELF，依赖 `../leopard-sdk`（`include/` 头 + `obj-tui/` 预编译运行时对象 + `liballoc`）。
- 链接固定 `-Ttext=0x200000`。
