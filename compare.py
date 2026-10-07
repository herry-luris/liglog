#!/usr/bin/env python3
import json, sys

def load(path):
    with open(path) as f:
        return json.load(f)

def index(bench):
    out = {}
    for b in bench["benchmarks"]:
        name = b["name"]
        # 只取聚合结果，没有的话取单项
        if b.get("aggregate_name"):
            continue
        out[name] = b
    return out

a = index(load(sys.argv[1]))
b = index(load(sys.argv[2]))

# 把名字里的前缀去掉，便于匹配
def norm(name):
    for pre in ("BM_Spdlog_", "BM_"):
        if name.startswith(pre):
            name = name[len(pre):]
            break
    # 处理多线程后缀
    return name

rows = []
for k in sorted(a):
    ka = norm(k)
    kb = "BM_Spdlog_" + ka
    if kb not in b:
        continue
    ta = a[k]["real_time"]
    tb = b[kb]["real_time"]
    ratio = tb / ta
    rows.append((ka, ta, tb, ratio))

print(f"{'scenario':<40} {'liglog(ns)':>12} {'spdlog(ns)':>12} {'spdlog/liglog':>14}")
print("-" * 80)
for name, ta, tb, r in rows:
    print(f"{name:<40} {ta:>12.2f} {tb:>12.2f} {r:>14.2f}x")
