"""patch helper: exact-string replacements that keep a file's bytes (latin-1) and its line endings"""
import os

NEO = 'C:/Source/DOOM-3/neo/'


def patch(path, pairs, base=NEO):
    full = path if os.path.isabs(path) or ':' in path else base + path
    raw = open(full, 'rb').read()
    crlf = b'\r\n' in raw
    s = raw.decode('latin-1').replace('\r\n', '\n')
    for item in pairs:
        old, new = item[0], item[1]
        count = item[2] if len(item) > 2 else 1
        found = s.count(old)
        assert found == count, '%s: expected %d, found %d of: %s' % (path, count, found, old[:80])
        s = s.replace(old, new)
    if crlf:
        s = s.replace('\n', '\r\n')
    open(full, 'wb').write(s.encode('latin-1'))
    print('patched', path)


def read(path, base=NEO):
    return open(base + path, 'rb').read().decode('latin-1').replace('\r\n', '\n')
