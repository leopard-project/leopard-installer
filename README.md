# leopard-installer

LeopardOS 安装器（CLI，Debian 式）。在 live 环境运行（initrc 尾端代替 getty），编排现成工具：

1. 列磁盘 + 选目标盘
2. lpdisk 分区（GPT，单分区 ef00）
3. mkfs-fat32 格式化
4. SYS_MOUNT 挂载到 /mnt
5. xbps-install -r /mnt -R /media 装基础系统
6. 重启

## 安装

- 二进制：/apps/system/leopard-installer.elf
- 打包：ninja pack -> leopard-installer-1.0_1.x86_64.xbps
