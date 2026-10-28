# PROJECT KNOWLEDGE BASE — leopard-installer

LeopardOS 安装器（CLI，Debian 式），在 live 环境运行（initrc 尾端代替 getty），编排现成工具：
列磁盘 → 选盘 → lpdisk 分区 → mkfs-fat32 格式化 → SYS_MOUNT 挂载 → …

## 构建
    python3 gen_ninja.py && ninja build   # → out/leopard-installer.elf
    ninja pack

- 静态 ELF，依赖 `../leopard-sdk`。
- 不新增内核 syscall：只复用分区/格式化/挂载等现成工具与内核原语。
