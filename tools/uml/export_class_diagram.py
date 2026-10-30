#!/usr/bin/env python3
"""用 hpp2plantuml 导出一个 C++ 项目的类结构（.puml）。

为什么需要这层封装：hpp2plantuml 依赖的 CppHeaderParser 词法分析器
不认 UTF-8 BOM，而本项目所有头文件都带 BOM，直接跑会导致
"CppParseError: Lex error" 且报错位置显示为空，很难定位。

本脚本的做法是：把源码头文件复制到 build/uml/mirror 下并剥掉 BOM，
再对镜像目录做解析，源文件保持不动。

用法：
    python tools/uml/export_class_diagram.py --out docs/design/raw/netmech_raw.puml
    python tools/uml/export_class_diagram.py --src include/netmech --src src --deps -o out.puml
"""
from __future__ import annotations

import argparse
import io
import pathlib
import shutil
import sys

BOM = b"\xef\xbb\xbf"


def build_mirror(root: pathlib.Path, src_dirs: list[str], mirror: pathlib.Path) -> tuple[list[str], int]:
    """把 src_dirs 下的 *.hpp 去 BOM 复制到 mirror，返回 (glob 模式列表, 文件数)。"""
    if mirror.exists():
        shutil.rmtree(mirror)
    patterns: list[str] = []
    count = 0
    for rel in src_dirs:
        src = root / rel
        if not src.is_dir():
            print(f"[warn] 跳过不存在的目录: {src}")
            continue
        dst = mirror / rel
        dst.mkdir(parents=True, exist_ok=True)
        for f in sorted(src.glob("*.hpp")):
            data = f.read_bytes()
            stripped = data[len(BOM):] if data.startswith(BOM) else data
            (dst / f.name).write_bytes(stripped)
            count += 1
        patterns.append(f"{mirror.as_posix()}/{rel}/*.hpp")
    return patterns, count


def run_hpp2plantuml(patterns: list[str], out: pathlib.Path, deps: bool) -> int:
    from hpp2plantuml.hpp2plantuml import main as hpp_main

    argv = ["hpp2plantuml"]
    for p in patterns:
        argv += ["-i", p]
    argv += ["-o", str(out)]
    if deps:
        argv.append("-d")

    out.parent.mkdir(parents=True, exist_ok=True)
    saved = sys.argv
    sys.argv = argv
    try:
        hpp_main()
    finally:
        sys.argv = saved
    return 0


def summarize(out: pathlib.Path) -> None:
    text = io.open(out, encoding="utf-8").read()
    classes = sum(1 for ln in text.splitlines() if ln.strip().startswith(("class ", "abstract class ")))
    enums = sum(1 for ln in text.splitlines() if ln.strip().startswith("enum "))
    rels = sum(
        1
        for ln in text.splitlines()
        if any(t in ln for t in ("<|--", "<|..", "*--", "o--", "+--", "..>", "<.."))
    )
    print(f"[ok] {out}  类 {classes} 个 / 枚举 {enums} 个 / 关系 {rels} 条")


def main() -> int:
    ap = argparse.ArgumentParser(description="hpp2plantuml 封装（自动去 BOM）")
    ap.add_argument("--root", default=".", help="项目根目录，默认当前目录")
    ap.add_argument(
        "--src",
        action="append",
        help="相对 root 的头文件目录，可重复；默认 include/<name> 与 src",
    )
    ap.add_argument("-o", "--out", required=True, help="输出 .puml 路径")
    ap.add_argument("--deps", action="store_true", help="从方法参数提取依赖关系")
    ap.add_argument("--mirror", default="build/uml/mirror", help="去 BOM 镜像目录")
    args = ap.parse_args()

    root = pathlib.Path(args.root).resolve()
    mirror = root / args.mirror
    src_dirs = args.src or ["include", "src"]
    patterns, count = build_mirror(root, src_dirs, mirror)
    if not patterns:
        print("[error] 没有找到任何头文件目录")
        return 2
    print(f"[info] 镜像 {count} 个头文件 -> {mirror}")

    out = (root / args.out) if not pathlib.Path(args.out).is_absolute() else pathlib.Path(args.out)
    run_hpp2plantuml(patterns, out, args.deps)
    if not out.exists():
        print("[error] hpp2plantuml 未生成输出文件")
        return 1
    summarize(out)
    print(
        "[note] 工具产物需要人工复核：嵌套类归属、模板特化噪声、"
        "以及在 .cpp 内定义的实现类（工具只扫头文件）。"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
