#!/usr/bin/env python3
"""
Split a Chinese novel txt into small chapter files for spiffs_data.

Usage:
    python tools/split_novel.py 斗罗大陆.txt [output_dir] [max_chapters]

Example:
    python tools/split_novel.py douluo.txt spiffs_data

The splitter looks for chapter headings like:
    第一章 xxx
    第1章 xxx
    第123章 xxx
"""
import os
import re
import sys

CHAPTER_RE = re.compile(r'^\s*(第\s*[0-9零一二三四五六七八九十百千两]+\s*章[^\n]{0,40})\s*$', re.MULTILINE)

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else 'spiffs_data'
    max_chapters = int(sys.argv[3]) if len(sys.argv) > 3 else 100000

    with open(src, 'r', encoding='utf-8') as f:
        text = f.read()

    # Normalize line endings
    text = text.replace('\r\n', '\n').replace('\r', '\n')

    matches = list(CHAPTER_RE.finditer(text))
    if not matches:
        print('No chapter headings found; writing whole file as 001.txt')
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, '001.txt'), 'w', encoding='utf-8') as f:
            f.write(text)
        return

    os.makedirs(out, exist_ok=True)
    count = 0
    for i, m in enumerate(matches[:max_chapters]):
        start = m.start()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        title = re.sub(r'[\/:*?"<>|]', '_', m.group(1).strip())
        if len(title) > 40:
            title = title[:40]
        filename = f'{i+1:03d}.txt'
        chapter_text = text[start:end].strip() + '\n'
        with open(os.path.join(out, filename), 'w', encoding='utf-8') as f:
            f.write(chapter_text)
        count += 1

    print(f'Wrote {count} chapters to {out}')

if __name__ == '__main__':
    main()
