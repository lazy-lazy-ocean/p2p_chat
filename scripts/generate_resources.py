#!/usr/bin/env python3
"""Combine HTML, CSS, JS into a single C++ header with inline resources."""
import os, sys

def main():
    if len(sys.argv) != 4:
        print("Usage: generate_resources.py <html> <css> <js>", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1]) as f: html = f.read()
    with open(sys.argv[2]) as f: css = f.read()
    with open(sys.argv[3]) as f: js = f.read()

    # Inline CSS
    html = html.replace(
        '<link rel="stylesheet" href="style.css">',
        f'<style>{css}</style>'
    )
    # Inline JS
    html = html.replace(
        '<script src="app.js"></script>',
        f'<script>{js}</script>'
    )

    # Write combined HTML to stdout
    sys.stdout.write(html)

if __name__ == '__main__':
    main()
