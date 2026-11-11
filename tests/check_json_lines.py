"""Runs lumen_json_sample and checks its Json output with Python's json module.

Usage: check_json_lines.py <path to lumen_json_sample>

Every line must be strict JSON (no NaN/Infinity literals, no raw control
characters, valid UTF-8), an object without duplicate keys, with every tag
value a string. A record's expect_* tags state what else its line must hold.
Exit status 0 means every check passed.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile


def reject_duplicates(pairs):
    keys = [key for key, _ in pairs]
    if len(keys) != len(set(keys)):
        raise ValueError(f"duplicate keys in {keys}")
    return dict(pairs)


def reject_constant(name):
    raise ValueError(f"non-standard JSON constant {name}")


def strict_parse(text):
    return json.loads(text, object_pairs_hook=reject_duplicates, parse_constant=reject_constant)


def raw_number(text, key):
    """The value of `key` as the exact text of its number (so -0 keeps its sign)."""
    raw = json.loads(text, parse_float=str, parse_int=str)
    return raw[key]


def check_line(text):
    record = strict_parse(text)
    tags = record["tags"]
    for key, value in tags.items():
        if not isinstance(value, str):
            raise ValueError(f"tag {key!r} is not a string: {value!r}")
    if "expect_msg_utf8_hex" in tags:
        actual = record["msg"].encode("utf-8").hex()
        if actual != tags["expect_msg_utf8_hex"]:
            raise ValueError(f"msg bytes {actual} != {tags['expect_msg_utf8_hex']}")
    if "expect_bits" in tags:
        bits = struct.pack(">d", float(raw_number(text, "value"))).hex()
        if bits != tags["expect_bits"]:
            raise ValueError(f"value bits {bits} != {tags['expect_bits']}")
        if "value_text" in record:
            raise ValueError("value_text present for a finite value")
    if "expect_value_text" in tags:
        if record["value"] is not None or record["value_text"] != tags["expect_value_text"]:
            raise ValueError(f"non-finite value written as {record['value']!r}")
    if "expect_tag_count" in tags:
        if len(tags) != int(tags["expect_tag_count"]):
            raise ValueError(f"{len(tags)} tags, expected {tags['expect_tag_count']}")
        if record["msg"] != "reserved" or record["type"] != "log":
            raise ValueError("a tag replaced a record field")


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "sample.jsonl")
        run = subprocess.run([sys.argv[1], path], check=True, capture_output=True, text=True)
        expected_lines = int(run.stdout.strip())
        with open(path, "rb") as file:
            data = file.read()
    if not data.endswith(b"\n"):
        print("the file does not end with a newline", file=sys.stderr)
        return 1
    lines = data[:-1].split(b"\n")
    if len(lines) != expected_lines:
        print(f"{len(lines)} lines, expected {expected_lines}", file=sys.stderr)
        return 1
    failures = 0
    for number, raw in enumerate(lines, start=1):
        try:
            check_line(raw.decode("utf-8"))
        except (ValueError, KeyError, UnicodeDecodeError) as error:
            failures += 1
            print(f"line {number}: {error}: {raw!r}", file=sys.stderr)
    print(f"{len(lines)} lines checked, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
