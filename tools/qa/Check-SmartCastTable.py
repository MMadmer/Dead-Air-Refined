"""Check that the smart_cast specialisation table still tells the truth.

smart_cast (src/xrServerEntities/smart_cast.h) answers a declared pair (target, source, method)
by calling source->method() instead of walking RTTI. That is only the same answer dynamic_cast
would give while EVERY class that derives from the target resolves method() to an unconditional
`return this`. A class that inherits the target through some other base and never overrides the
method gets a `return NULL` from further up and the cast silently fails - which is a wrong answer,
not a slow one.

The table spent years compiled out (xrGame defined PURE_DYNAMIC_CAST) and decayed while nobody
was looking. This is what says whether it holds:

    python tools\\qa\\Check-SmartCastTable.py

It prints nothing and exits 0 when every declared pair agrees with dynamic_cast. Pairs that do
not are expected to be absent from the table - smart_cast.h marks the three that are, with the
reason - and a new one showing up here means a class was added without its cast method.

The reading is textual and therefore conservative about what it can see: an override written as
anything other than a one-expression body is reported rather than assumed correct.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2] / 'src'
TABLE = ROOT / 'xrServerEntities' / 'smart_cast.h'

THIS_ANSWERS = ('return this;', 'return (this);')

# A cast method is any of these shapes; `visual`, `shape` and `motion` are the server entities'
# own spelling of the same idea.
METHOD = r'(\w*cast_\w+|visual|shape|motion)'


def read(path):
    return path.read_text('utf-8', errors='replace')


def main():
    headers = list(ROOT.rglob('*.h')) + list(ROOT.rglob('*.hpp'))
    sources = list(ROOT.rglob('*.cpp'))
    text = {f: read(f) for f in headers + sources}

    table = [m.groups() for m in
             re.finditer(r'DECLARE_SPECIALIZATION\((\w+),\s*(\w+),\s*(\w+)\)', read(TABLE))]
    if not table:
        print('no DECLARE_SPECIALIZATION found - has smart_cast.h moved?')
        return 2

    # class -> direct bases
    bases = {}
    for f in headers:
        for m in re.finditer(r'class\s+(?:[A-Z_0-9]+_API\s+)?(\w+)\s*(?::\s*([^{;]+))?\{', text[f]):
            bases.setdefault(m.group(1), [])
            if m.group(2):
                bases[m.group(1)] = [b.split('::')[-1] for b in re.findall(
                    r'(?:public|protected|private)\s+(?:virtual\s+)?([\w:]+)', m.group(2))]

    # (class, method) -> body, from inline definitions and from out-of-line ones
    owned = {}
    for f in headers:
        src = text[f]
        for m in re.finditer(r'class\s+(?:[A-Z_0-9]+_API\s+)?(\w+)\s*(?::[^{;]+)?\{', src):
            cls, rest = m.group(1), src[m.end():]
            depth, i = 1, 0
            while i < len(rest) and depth:
                depth += (rest[i] == '{') - (rest[i] == '}')
                i += 1
            for fm in re.finditer(r'(\w+)\s*\*\s*' + METHOD + r'\s*\(\s*\)[^;{]*\{([^}]*)\}', rest[:i]):
                owned[(cls, fm.group(2))] = ' '.join(fm.group(3).split())
    for f in sources:
        for m in re.finditer(r'(\w+)\s*\*\s*(\w+)\s*::\s*' + METHOD + r'\s*\(\s*\)[^;{]*\{([^}]*)\}', text[f]):
            owned.setdefault((m.group(2), m.group(3)), ' '.join(m.group(4).split()))

    def derives(cls, target, seen=None):
        seen = seen or set()
        if cls in seen:
            return False
        seen.add(cls)
        return any(b == target or derives(b, target, seen) for b in bases.get(cls, []))

    def resolve(cls, method, seen=None):
        seen = seen or set()
        if cls in seen:
            return None
        seen.add(cls)
        if (cls, method) in owned:
            return cls, owned[(cls, method)]
        for b in bases.get(cls, []):
            found = resolve(b, method, seen)
            if found:
                return found
        return None

    problems = 0
    for target, source, method in table:
        for cls in bases:
            # only classes the cast can actually be handed: they are the source type too
            if not derives(cls, source) or not (cls == target or derives(cls, target)):
                continue
            answer = resolve(cls, method)
            if answer is None:
                print(f'{cls} is a {target} but {method}() resolves to nothing')
                problems += 1
            elif answer[1] not in THIS_ANSWERS and 'static_cast' not in answer[1]:
                print(f'{cls} is a {target} but {method}() resolves to '
                      f'{answer[0]}::{method} -> {answer[1][:60]}')
                problems += 1

    if problems:
        print(f'\n{problems} class(es) would get a wrong answer from the table '
              f'({len(table)} pairs checked)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
