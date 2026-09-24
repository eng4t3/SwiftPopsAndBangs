#!/usr/bin/env python3
"""
Structural sync check between swift_show_tuning/engine_control.cpp and tools/sim/engine_core.js.

There is no host C++ compiler on the development machine, so the simulator runs the JavaScript
mirror of the control core. This script guards the mirror: for every control function present
in both files, the bodies are reduced to a stream of identifiers / numbers / operators after
removing what legitimately differs between the languages (types, casts, U suffixes, the JS
helpers u32() / s32() / udiv() / sdiv(), const / let, compound assignments, hw.* accessors and
sim-only logging). Any remaining difference is printed. The values of all shared constants are
compared too.

    python tools/sim/check_sync.py      (exit code 1 on mismatch)
"""
import difflib
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = (ROOT / 'swift_show_tuning' / 'engine_control.cpp').read_text(encoding='utf-8')
JS = (ROOT / 'tools' / 'sim' / 'engine_core.js').read_text(encoding='utf-8')
HDR = (ROOT / 'swift_show_tuning' / 'engine_control.h').read_text(encoding='utf-8')

FUNCS = ['predMargin', 'absDiff', 'trRec', 'trReject', 'clampSet', 'modelRpmAt', 'ringPush', 'floodCheck', 'slotDrop', 'minCutOk', 'hardCutSlots',
         'startSeq', 'seqStillWanted', 'decideNext', 'onTachEdge', 'predictedEvent', 'launchStep',
         'decelStep', 'ghostStep', 'benchStep', 'slowStep', 'onTick']

CPP_WORDS = r'\b(?:static|inline|const|IRAM_ATTR|uint8_t|uint16_t|uint32_t|int32_t|int|bool|void|unsigned)\b'
CPP_CAST = r'\((?:uint8_t|uint16_t|uint32_t|int32_t|int|bool)\)'
TOKEN = r'[A-Za-z_]\w*|\d+|==|!=|<=|>=|&&|\|\||<<|>>|[-+*/%<>=!?:&|^~]'


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def body(src, name, js):
    pat = (r'function\s+' + name + r'\s*\([^)]*\)\s*\{') if js else (r'\b' + name + r'\s*\([^)]*\)\s*\{')
    m = re.search(pat, src)
    if not m:
        return None
    i, depth = m.end(), 1
    while depth:
        depth += (src[i] == '{') - (src[i] == '}')
        i += 1
    return src[m.end():i - 1]


def unwrap_call(s, fname, op):
    """fname(a, b) -> ((a) op (b)), or fname(a) -> (a) when op is None; nesting-aware."""
    while True:
        m = re.search(r'\b' + fname + r'\(', s)
        if not m:
            return s
        i, depth, comma = m.end(), 1, None
        while depth:
            c = s[i]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif c == ',' and depth == 1 and comma is None:
                comma = i
            i += 1
        if op is None:
            s = s[:m.start()] + '(' + s[m.end():i - 1] + ')' + s[i:]
        else:
            s = s[:m.start()] + '((' + s[m.end():comma] + ') ' + op + ' (' + s[comma + 1:i - 1] + '))' + s[i:]


def canon(s):
    # x += y; -> x = x + y;   x++; -> x = x + 1;   (both languages)
    s = re.sub(r'\b([A-Za-z_][\w.]*)\s*([-+])=\s*([^;]+);', lambda m: f'{m[1]} = {m[1]} {m[2]} {m[3]};', s)
    s = re.sub(r'(?<![+\w.])([A-Za-z_][\w.]*)\+\+\s*;', lambda m: f'{m[1]} = {m[1]} + 1;', s)
    return s


def norm(s, js):
    s = strip_comments(s)
    if js:
        s = re.sub(r'if \(hw\.log\)[^;]*;', '', s)
        s = re.sub(r'\b(?:const|let)\b', '', s)
        s = s.replace('===', '==').replace('!==', '!=')
        s = s.replace('hw.clamp', 'hwClamp').replace('hw.switchActive', 'hwSwitchActive').replace('hw.tachLowNow', 'hwTachLow')
        s = unwrap_call(s, 'u32', None)
        s = unwrap_call(s, 's32', None)
        s = unwrap_call(s, 'udiv', '/')
        s = unwrap_call(s, 'sdiv', '/')
    else:
        s = re.sub(CPP_CAST, '', s)
        s = re.sub(CPP_WORDS, '', s)
        s = re.sub(r'(\d)(?:UL|U)\b', r'\1', s)
    s = canon(s)
    return re.findall(TOKEN, s)


def consts(src, js):
    if js:
        pairs = []
        for stmt in re.findall(r'^const ([A-Z][^;]*);', src, flags=re.M):
            for part in stmt.split(','):
                m = re.match(r'\s*([A-Z][A-Z0-9_]+)\s*=\s*(.+?)\s*$', part, flags=re.S)
                if m:
                    pairs.append((m[1], m[2]))
    else:
        pairs = re.findall(r'^static const \w+\s+([A-Z][A-Z0-9_]+)\s*=\s*([^;]+);', src, flags=re.M)
    return {k: v.strip() for k, v in pairs}


def evalc(expr, table):
    e = expr
    for _ in range(6):
        e = re.sub(r'\b([A-Z][A-Z0-9_]+)\b', lambda m: '(' + table.get(m[1], m[1]) + ')', e)
    e = re.sub(r'(\d)(?:UL|U)\b', r'\1', e)
    try:
        return eval(e.replace('/', '//'))
    except Exception:
        return e


def main():
    header = dict(re.findall(r'#define\s+(\w+)\s+(\d+)', HDR))
    # enum values declared in the header (CutReason, LaunchState, LaunchEnd, TraceKind)
    header.update(dict(re.findall(r'^\s*((?:CUT|LAUNCH|TR)_[A-Z0-9_]+)\s*=\s*(\d+)', HDR, flags=re.M)))
    bad = 0
    cc, jc = consts(CPP, False), consts(JS, True)
    tc = dict(header); tc.update(cc)
    tj = dict(header); tj.update(jc)
    for k in sorted(set(cc) | set(jc)):
        if k not in cc or k not in jc:
            if k not in header and not re.match(r'(CUT|LAUNCH|SEQ)_', k):
                print(f'constant only in {"C++" if k in cc else "JS"}: {k}')
                bad += 1
            continue
        a, b = evalc(cc[k], tc), evalc(jc[k], tj)
        if a != b:
            print(f'constant {k}: C++ {a} != JS {b}')
            bad += 1
    for k, v in header.items():
        if k in jc and str(evalc(jc[k], tj)) != v:
            print(f'header constant {k}: header {v} != JS {jc[k]}')
            bad += 1
    for f in FUNCS:
        bc, bj = body(CPP, f, False), body(JS, f, True)
        if bc is None or bj is None:
            print(f'function {f}: missing in {"C++" if bc is None else "JS"}')
            bad += 1
            continue
        a, b = norm(bc, False), norm(bj, True)
        if f == 'launchStep':
            # JS uses switch/case, C++ an if-chain: compare the token multisets instead
            skip = {'switch', 'case', 'break', 'default', 'if', 'else', 'return', '==', 'launchState', ':'}
            ca, cb = Counter(t for t in a if t not in skip), Counter(t for t in b if t not in skip)
            if ca != cb:
                print(f'function {f}: C++-only {dict(ca - cb)} JS-only {dict(cb - ca)}')
                bad += 1
            continue
        if a != b:
            print(f'function {f}: token streams differ')
            for line in difflib.unified_diff(a, b, 'C++', 'JS', n=4, lineterm=''):
                print('   ', line)
            bad += 1
    print(f'sync check: {len(FUNCS)} functions, {len(set(cc) & set(jc))} constants:',
          'OK' if not bad else f'{bad} difference(s)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
