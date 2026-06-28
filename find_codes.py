#!/usr/bin/env python3

import argparse
import re


CONTENT_ID_PATTERN = re.compile(
    rb"(?<![A-Z0-9])"
    rb"([A-Z]{2}[0-9]{4}-[A-Z0-9]{9}_[0-9]{2}-[A-Z0-9]{16})"
    rb"(?![A-Z0-9])"
)
LABEL_PATTERN = re.compile(rb"(?:^|\x00)([A-Z0-9]{16})(?=\x00|$)")
SKIP_LABEL = "0123456789ABCDEF"
SAMPLE_CONTENT_ID_PREFIX = "UP9000-PPSA01234_00-"


def find_identifiers(data):
    content_ids = []
    seen_content_ids = set()

    for match in CONTENT_ID_PATTERN.finditer(data):
        content_id = match.group(1).decode("ascii")
        if content_id in seen_content_ids:
            continue
        seen_content_ids.add(content_id)
        content_ids.append(content_id)

    known_labels = {content_id[-16:] for content_id in content_ids}
    labels = []
    seen_labels = set()

    for match in LABEL_PATTERN.finditer(data):
        label = match.group(1).decode("ascii")
        if label == SKIP_LABEL or label in known_labels or label in seen_labels:
            continue
        seen_labels.add(label)
        labels.append(label)

    return content_ids, labels


def make_entries(content_ids, labels):
    entries = [f"[PSAC]\ncontent_id={content_id}" for content_id in content_ids]
    entries.extend(
        f"[PSAC]\ncontent_id={SAMPLE_CONTENT_ID_PREFIX}{label}"
        for label in labels
    )
    return entries


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Find complete 36-character Content IDs and NUL-delimited "
            "16-character entitlement-label candidates in a binary file."
        )
    )
    parser.add_argument(
        "filename",
        help="Path to the binary file"
    )
    parser.add_argument(
        "-o",
        "--output",
        default="codes",
        help="Path to write the generated PSAC entries (default: codes)"
    )

    args = parser.parse_args()

    with open(args.filename, "rb") as file:
        data = file.read()

    content_ids, labels = find_identifiers(data)
    entries = make_entries(content_ids, labels)

    with open(args.output, "w", encoding="utf-8", newline="\n") as file:
        file.write("\n\n".join(entries))
        if entries:
            file.write("\n")

    print(
        f"Wrote {len(content_ids)} full Content IDs and "
        f"{len(labels)} label-only candidates "
        f"({len(entries)} total entries) to {args.output}"
    )


if __name__ == "__main__":
    main()
