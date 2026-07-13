#!/usr/bin/env python3
"""
Generate a C++ header file containing an HTML file as a string constant.
Usage: generate_header.py <input_html> <output_hpp>
"""
import sys, os

def main():
    if len(sys.argv) != 3:
        print("Usage: generate_header.py <input> <output>", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    # Generate C array
    output = "#pragma once\n"
    output += "static const unsigned char embedded_html[] = {\n  "

    col = 2
    for i, byte in enumerate(data):
        output += f"0x{byte:02x}, "
        col += 6
        if col > 76 and i < len(data) - 1:
            output += "\n  "
            col = 2

    output += "\n};\n"
    output += f"static const unsigned int embedded_html_len = {len(data)};\n"

    with open(sys.argv[2], 'w') as f:
        f.write(output)

    print(f"Generated {sys.argv[2]} ({len(data)} bytes)")

if __name__ == '__main__':
    main()
