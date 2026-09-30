"""Read C/C++ sources with their implementation parts expanded.

A few large translation units (the renderer, SPURS, the game's import
overrides) include ordered implementation parts, `*.inc` files in their own
directory. Tools that scan source text for definitions read each translation
unit through read_source() so that code in its parts is attributed to it, and
skip the `.inc` files themselves.
"""

import os
import re

_PART = re.compile(r'^#include "([\w.-]+\.inc)"[ \t]*$', re.M)


def read_source(path):
    """Return the text of path with each `#include "x.inc"` replaced by x.inc.

    An include whose part is not in the file's directory is left as it is."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    base = os.path.dirname(path)

    def expand(match):
        part = os.path.join(base, match.group(1))
        return read_source(part) if os.path.isfile(part) else match.group(0)

    return _PART.sub(expand, text)
