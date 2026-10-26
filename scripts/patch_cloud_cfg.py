#!/usr/bin/env python3
"""把云端配置写进 pet_config.h

用法：
    python scripts/patch_cloud_cfg.py main/boards/dfrobot/df-k10/pet_config.h \
        "https://xxx.ap-guangzhou.tencentscf.com/" "qiaoli-2026" 300

为什么要有这个脚本而不是在 workflow 里直接 sed：
    URL 里可能有 `/`、`&`、`?`，sed 的分隔符和转义很容易出错；
    而且 KEY 为空时要能原样保留（不能把已有的口令误清空）。
    用 Python 的正则替换更可控，还能打印注入结果供 CI 日志核对。

返回码：
    0 = 全部按预期写入
    1 = 文件里找不到对应 #define（说明 pet_config.h 被改过，宁可失败也不静默）
"""

import re
import sys


def patch(path, url, key, beat):
    src = open(path, encoding="utf-8").read()

    def sub(pattern, repl, label):
        new, n = re.subn(pattern, repl, src, count=1)
        if n == 0:
            print(f"[FAIL] 找不到 {label} 的定义，pet_config.h 改动过？", file=sys.stderr)
            sys.exit(1)
        return new

    src = sub(
        r'#define\s+PET_CLOUD_URL\s+"[^"]*"',
        '#define PET_CLOUD_URL "%s"' % url.replace('\\', '\\\\').replace('"', '\\"'),
        "PET_CLOUD_URL",
    )
    if key:
        src = sub(
            r'#define\s+PET_CLOUD_KEY\s+"[^"]*"',
            '#define PET_CLOUD_KEY "%s"' % key.replace('\\', '\\\\').replace('"', '\\"'),
            "PET_CLOUD_KEY",
        )
    if beat:
        src = sub(
            r"#define\s+PET_HEARTBEAT_SEC\s+\d+",
            "#define PET_HEARTBEAT_SEC %s" % int(beat),
            "PET_HEARTBEAT_SEC",
        )

    open(path, "w", encoding="utf-8", newline="\n").write(src)

    for line in src.splitlines():
        s = line.strip()
        if s.startswith("#define PET_CLOUD_URL") or s.startswith("#define PET_CLOUD_KEY") \
                or s.startswith("#define PET_HEARTBEAT_SEC"):
            if s.startswith("#define PET_CLOUD_KEY") and key:
                print("  #define PET_CLOUD_KEY \"***\"")
            else:
                print("  " + s)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    patch(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "",
          sys.argv[4] if len(sys.argv) > 4 else "")
