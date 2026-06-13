#!/usr/bin/env python3

import argparse
import re


def main():
    parser = argparse.ArgumentParser(
        description="Find 16-character uppercase alphanumeric strings surrounded by null bytes in a binary file."
    )
    parser.add_argument(
        "filename",
        help="Path to the binary file"
    )
    parser.add_argument(
        "-o",
        "--output",
        default="codes",
        help="Path to write the generated PSAC entries"
    )

    args = parser.parse_args()

    pattern = re.compile(rb"(?:^|\x00)([A-Z0-9]{16})(?=\x00|$)")
    skip_code = "0123456789ABCDEF"

    with open(args.filename, "rb") as file:
        data = file.read()

    seen = set()
    entries = []

    for match in pattern.finditer(data):
        code = match.group(1).decode("ascii")
        if code == skip_code or code in seen:
            continue

        seen.add(code)
        entries.append(f"[PSAC]\ncontent_id=UP9000-PPSA01234_00-{code}")

    with open(args.output, "w", encoding="utf-8", newline="\n") as file:
        file.write("\n\n".join(entries))
        if entries:
            file.write("\n")

    print(f"Wrote {len(entries)} unique codes to {args.output}")


if __name__ == "__main__":
    main()
