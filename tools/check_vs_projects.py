import io, os, re, sys, subprocess
import xml.etree.ElementTree as ET

NS = '{http://schemas.microsoft.com/developer/msbuild/2003}'
projs = ['netmech.vcxproj', 'netmech_ut.vcxproj', 'netmech_echo.vcxproj',
         'netmech_udp.vcxproj', 'serial_check.vcxproj']
errs = []

# 1) XML 合法性
trees = {}
for p in projs:
    try:
        trees[p] = ET.parse(p)
    except Exception as e:
        errs.append('%s XML 解析失败: %s' % (p, e))
print('1) XML 解析: %d/%d 通过' % (len(trees), len(projs)))

# 2) ProjectReference 目标存在
refs = 0
for p, t in trees.items():
    for r in t.iter(NS + 'ProjectReference'):
        inc = r.get('Include')
        refs += 1
        if not os.path.exists(inc):
            errs.append('%s -> ProjectReference 不存在: %s' % (p, inc))
print('2) ProjectReference: %d 条，检查完成' % refs)

# 3) 源文件引用是否存在
missing, checked = 0, 0
for p, t in trees.items():
    for tag in ('ClCompile', 'ClInclude', 'None'):
        for c in t.iter(NS + tag):
            inc = c.get('Include')
            if not inc or '$(' in inc:
                continue
            checked += 1
            if not os.path.exists(inc):
                missing += 1
                if missing <= 5:
                    errs.append('%s 引用不存在的文件: %s' % (p, inc))
print('3) 源文件引用: 检查 %d 条，缺失 %d' % (checked, missing))

# 4) sln 项目条目与 GUID 一致性
sln = io.open('netmech.sln', encoding='utf-8-sig').read()
guids = {}
for m in re.finditer(r'Project\("\{[^}]+\}"\) = "([^"]+)", "([^"]+)", "\{([^}]+)\}"', sln):
    name, path, guid = m.groups()
    if not os.path.exists(path):
        errs.append('sln 引用不存在的工程: %s (%s)' % (name, path))
    guids[name] = guid
print('4) sln 项目: %s' % ', '.join(sorted(guids)))
for p, t in trees.items():
    pg = (t.findtext('.//' + NS + 'ProjectGuid') or '').strip('{}')
    if pg and pg not in guids.values():
        errs.append('%s 的 ProjectGuid %s 不在 sln 里' % (p, pg))
print('4) ProjectGuid 一致性: 检查完成')

# 5) 库产物名 vs 各 exe 的 AdditionalDependencies
lib_tn = trees['netmech.vcxproj'].findtext('.//' + NS + 'TargetName')
expect = (lib_tn or '?') + '.lib'
for p, t in trees.items():
    if p == 'netmech.vcxproj':
        continue
    deps = [d.text or '' for d in t.iter(NS + 'AdditionalDependencies')]
    if not any(expect in d for d in deps):
        errs.append('%s 的 AdditionalDependencies 里没有 %s' % (p, expect))
print('5) 链接依赖: 库产物应为 %s' % expect)

# 6) 旧名残留（只看已跟踪文件）
left = subprocess.run(['git', 'grep', '-n', 'NetMechanism', '--', '.'],
                      capture_output=True, text=True).stdout.strip()
print('6) 残留 NetMechanism: %s' % ('无' if not left else '%d 处' % len(left.splitlines())))
if left:
    errs.append('仍有残留: ' + left.splitlines()[0])

print('')
print('=== 结果: ' + ('PASS' if not errs else 'FAIL (%d)' % len(errs)))
for e in errs:
    print('  - ' + e)
sys.exit(0 if not errs else 1)
