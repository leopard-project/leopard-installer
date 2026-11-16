#!/usr/bin/env python3
"""leopard-installer 构建 + xbps 打包（序列编排器 + 多可执行文件步骤程序）。

架构：
  main.c    序列编排器：读 /config/leopard-install-sequence.conf，按行序以「全路径、
            无参数」执行步骤程序，等它退出；失败问重试/关机；全成功则重启。
  steps/*.c 步骤程序：每个安装步骤一个独立可执行文件，只通过
            /config/installer/state（key=value）交换数据，可单独替换/重排/重跑。

产物：
  out/leopard-installer.elf                 编排器 → apps/system/leopard-installer.elf
  out/steps/<id>.elf                        步骤程序 → apps/leopard/installer/steps/<id>.elf
  out/leopard-installer-<ver>.x86_64.xbps   一个包，含编排器 + 全部步骤 + 序列/选项配置

用法：python3 gen_ninja.py && ninja build / ninja pack
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT  = ROOT / "out"

# 步骤程序共享的库（状态 / 选项 / 日志 / 交互 / 子进程 / 块设备）
COMMON_SOURCES = ["lib/li_core.c", "lib/li_plan.c", "lib/li_format.c", "lib/li_ui.c"]

# 编排器（main.c）链接 TUI host + lpcurses；步骤程序不链接 curses。
ORCH_SOURCES = ["main.c", "lib/li_ui_host.c", "lib/li_tui.c"]

# 步骤程序：(id, 源文件)。id 同时是产物名和 sequence.conf 里的文件名。
STEPS = [
    ("10-disk",       "steps/10-disk.c"),
    ("15-plan",       "steps/15-plan.c"),
    ("20-partition",  "steps/20-partition.c"),
    ("30-format",     "steps/30-format.c"),
    ("40-mount",      "steps/40-mount.c"),
    ("45-packages-select", "steps/45-packages-select.c"),
    ("50-packages",   "steps/50-packages.c"),
    ("60-config",     "steps/60-config.c"),
    ("70-bootloader", "steps/70-bootloader.c"),
    ("80-verify",     "steps/80-verify.c"),
    ("90-finish",     "steps/90-finish.c"),
]

# 纯数据文件（相对本仓 → 安装路径）
DATA = [
    ("data/leopard-install-sequence.conf", "config/leopard-install-sequence.conf"),
    ("data/leopard-install.conf",          "config/leopard-install.conf"),
    ("data/install-profiles.conf",         "config/install-profiles.conf"),
    ("data/format-profiles.conf",          "config/format-profiles.conf"),
    ("data/package-profiles.conf",         "config/package-profiles.conf"),
]

ORCH_INSTALL = "apps/system/leopard-installer.elf"
STEP_INSTALL = "apps/leopard/installer/steps"

# XACT CRT（../leopard-sdk/out/crt）：启动对象 + 静态运行时 + 分配器
CRT_ROOT = ROOT / ".." / "leopard-sdk"
INC      = CRT_ROOT / "out" / "crt" / "include"
LIB      = CRT_ROOT / "out" / "crt" / "lib" / "x86_64"
CRT_OBJS = [LIB / "crt0.o", LIB / "start.o"]
CRT_LIBS = [LIB / "libxact.a", LIB / "liballoc.a"]

# lpcurses（../lpcurses）：只链接编排器，运行期无依赖。
LPCURSES_ROOT = ROOT / ".." / "lpcurses"
LPCURSES_INC  = LPCURSES_ROOT / "include"
LPCURSES_LIB  = LPCURSES_ROOT / "out" / "liblpcurses.a"

PKG = {
    "pkgname": "leopard-installer",
    "pkgver":  "1.6.0_1",
    "arch":    "x86_64",
    "desc":    "Leopard installer (sequencer + step programs)",
    "deps":    "",
}


def rel(p):
    p = Path(p)
    if p.is_absolute():
        try:
            return str(p.relative_to(ROOT))
        except ValueError:
            return str(p)
    return str(p)


def esc(p):
    return rel(p).replace("$", "$$").replace(":", "$:").replace(" ", "$ ")


def main():
    cc   = os.environ.get("CC",  "clang")
    cxx  = os.environ.get("CXX", "clang++")
    ld   = os.environ.get("LD",  "ld")
    opt  = "-O" + os.environ.get("OPT_LEVEL", "2")
    col  = os.environ.get("DIAGNOSTICS_COLOR", "-fdiagnostics-color=always")

    common = (f"{col} {opt} -Wall -g -ffreestanding -fno-builtin -m64 -mstackrealign "
              "-fno-stack-protector -fno-exceptions -fno-strict-aliasing -fshort-wchar -nostdinc")
    c_flags   = f"{common} -std=c11   -I {rel(INC)} -I {rel(ROOT / 'include')} -I {rel(LPCURSES_INC)} -I . -MMD -MP"
    cpp_flags = f"{common} -std=c++11 -I {rel(INC)} -I {rel(ROOT / 'include')} -I {rel(LPCURSES_INC)} -I . -Wno-write-strings -MMD -MP"

    L = ["# leopard-installer 构建 + xbps 打包（编排器 + 步骤程序）", "ninja_required_version = 1.10",
         f"cc = {cc}", f"cxx = {cxx}", f"ld = {ld}",
         f"c_flags = {c_flags}", f"cpp_flags = {cpp_flags}", ""]
    L += [
        "rule cc",
        "  command = mkdir -p $$(dirname $out) && $cc $c_flags -MF $out.d -c $in -o $out",
        "  depfile = $out.d", "  deps = gcc", "",
        "rule cxx",
        "  command = mkdir -p $$(dirname $out) && $cxx $cpp_flags -MF $out.d -c $in -o $out",
        "  depfile = $out.d", "  deps = gcc", "",
        "rule link",
        "  command = $ld -Ttext=0x200000 $in -o $out", "",
    ]

    def obj_for(src):
        rule = "cc" if src.endswith(".c") else "cxx"
        obj = OUT / (src + ".o")
        L.append(f"build {esc(obj)}: {rule} {esc(ROOT / src)}")
        return obj

    common_objs = [obj_for(src) for src in COMMON_SOURCES]
    L.append("")

    # 编排器：main.c + TUI host（不链接步骤公共库，避免把步骤逻辑带进来）
    orch_target = OUT / "leopard-installer.elf"
    orch_objs   = [obj_for(src) for src in ORCH_SOURCES]
    ins = CRT_OBJS + orch_objs + [LPCURSES_LIB] + CRT_LIBS
    L += [f"build {esc(orch_target)}: link {' '.join(esc(o) for o in ins)}", ""]

    # 步骤程序：每个都链接公共库
    step_targets = []
    for step_id, src in STEPS:
        target = OUT / "steps" / f"{step_id}.elf"
        objs   = [obj_for(src)]
        ins = CRT_OBJS + common_objs + objs + CRT_LIBS
        L += [f"build {esc(target)}: link {' '.join(esc(o) for o in ins)}", ""]
        step_targets.append(target)

    # ---- xbps 包：编排器 + 全部步骤 + 序列/选项配置 ----
    out_pkg = OUT / f"{PKG['pkgname']}-{PKG['pkgver']}.{PKG['arch']}.xbps"
    files = [(orch_target, ORCH_INSTALL)]
    for step_id, _src in STEPS:
        files.append((OUT / "steps" / f"{step_id}.elf", f"{STEP_INSTALL}/{step_id}.elf"))
    files += [(ROOT / src, dst) for src, dst in DATA]

    args = f"-A {PKG['arch']} -n {PKG['pkgname']}-{PKG['pkgver']} -s \"{PKG['desc']}\""
    cmd = "rm -rf .pkg"
    for src, dst in files:
        cmd += f" && mkdir -p .pkg/$$(dirname {dst}) && cp {esc(src)} .pkg/{dst}"
    cmd += f" && cd out && xbps-create --compression gzip {args} ../.pkg"

    L += ["rule package", f"  command = {cmd}", ""]
    L += [f"build {esc(out_pkg)}: package {' '.join(esc(s) for s, _ in files)}"]
    L += [f"build build: phony {' '.join([esc(orch_target)] + [esc(t) for t in step_targets])}"]
    L += [f"build pack: phony {esc(out_pkg)}"]
    L += ["default build"]
    L += ["rule clean", "  command = rm -rf out .pkg", "build clean: clean"]

    (ROOT / "build.ninja").write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"已生成 build.ninja（leopard-installer：编排器 + {len(STEPS)} 个步骤程序）。"
          "用法：ninja build / ninja pack")


if __name__ == "__main__":
    main()
