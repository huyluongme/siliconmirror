import os, re, sys
A = open(os.path.join(os.environ.get('SMIRROR_SPECS', os.path.expanduser('~/specs')), 'armarm.txt')).read().split('\n')
def section(reg):
    # find "Accessing REG" in D24 (first occurrence after line 600000)
    for i, l in enumerate(A):
        if i > 600000 and l.strip() == 'Accessing ' + reg:
            return i
    return None
def compact(reg, which):
    i = section(reg)
    if i is None:
        print(reg, 'not found'); return
    out = []
    started = False
    for l in A[i:i+400]:
        s = l.strip()
        if s.startswith(which + ' ') and ('<Xt>' in s or '<X' in s):
            started = True; out.append('## ' + s); continue
        if started and (s.startswith('MSR ') or s.startswith('MRS ')) and not s.startswith(which):
            break
        if not started: continue
        if not s or s.startswith(('ARM DDI','M.d','AArch64 System','D24.','op0','0b')) or re.match(r'^D\d', s):
            continue
        s = s.replace('IsFeatureImplemented', 'Impl').replace('AArch64_SystemAccessTrap', 'Trap').replace(',→', '')
        out.append(s)
    print('\n'.join(out))
for arg in sys.argv[1:]:
    reg, which = arg.split(':')
    compact(reg, which)
    print('-----')
