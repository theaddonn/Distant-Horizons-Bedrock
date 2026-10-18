"""Lenient JSON loader for Mojang pack files (// and /* */ comments, trailing commas)."""
import json
import re


def strip_comments(text: str) -> str:
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
        elif c == '"':
            in_str = True
            out.append(c)
            i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def loads(text: str):
    text = text.lstrip("﻿")
    text = strip_comments(text)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return json.loads(text)


def load(path):
    with open(path, encoding="utf-8") as f:
        return loads(f.read())
