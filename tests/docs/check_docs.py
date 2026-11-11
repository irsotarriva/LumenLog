"""Checks the code blocks and the names in Lumen's Markdown documents.

Documents: README.md, AGENTS.md and docs/*.md.

Every fenced block is classified by its info string and by an optional
directive, an HTML comment on the line just before the fence:

    <!-- check: key=value key="value with spaces" -->

cpp       compiled, as one translation unit. A block with `int main(` is
          linked and run by ctest (exit status 0). Directives:
            from=PATH          the block is the whole file PATH (an example,
                               which is built and run on its own); checked
                               for equality instead of compiled again
            prelude=NAME       tests/docs/preludes/NAME.hpp goes first
            wrap=body          the block is the body of a function
            requires=KEY       a bridge block, built only when the framework
                               is found (KEY: eigen, openmp, torch, root);
                               otherwise ctest reports it as skipped
            illustrative=WHY   not compiled; WHY is printed
python    run by ctest when LUMEN_ENABLE_PYTHON is ON. from=PATH and
          illustrative=WHY as above.
text, json
          must have output=PATH (the block equals the file, an expected
          output of an example) or illustrative=WHY.
sh, cmake not executed. The names in them are checked (see below).

Anything else, or a block that breaks these rules, is an error.

Names. In the whole text of every document: every LUMEN_* and LOG_* macro or
CMake variable, every `--preset` and `--target`, every -D variable, every
lumen/... header, every repository path, every relative link, every C++
qualified name (lumen::X::Y, Enum::Value), every lumen.<name> in Python, and
every bridge metric name must exist in the source tree. A table preceded by
<!-- check: options-table --> must list exactly the LUMEN_ cache variables of
the build, with each literal default.

Usage:
    check_docs.py extract ROOT OUT   write the compiled blocks to OUT and
                                     OUT/blocks.cmake (at configure time)
    check_docs.py check ROOT OUT     everything above, and that OUT is
                                     up to date (a ctest test)
"""

import functools
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

FENCE = re.compile(r"^(```+)\s*([\w+-]*)\s*$")
DIRECTIVE = re.compile(r"^<!--\s*check:(.*?)-->\s*$")
PAIR = re.compile(r'(\w[\w-]*)=(?:"([^"]*)"|(\S+))')

CPP_KEYS = {"from", "prelude", "wrap", "requires", "illustrative"}
PYTHON_KEYS = {"from", "illustrative"}
TEXT_KEYS = {"output", "lines-in", "illustrative"}
BRIDGE_KEYS = {"eigen", "openmp", "torch", "root"}
NAME_ONLY_LANGUAGES = {"sh", "cmake"}


@dataclass
class Block:
    doc: Path
    line: int  # 1-based line of the opening fence
    language: str
    text: str
    directive: dict = field(default_factory=dict)

    @property
    def where(self) -> str:
        return f"{self.doc.as_posix()}:{self.line}"

    @property
    def name(self) -> str:
        return re.sub(r"\W", "_", f"{self.doc.with_suffix('').as_posix()}_{self.line}")


def documents(root: Path) -> list[Path]:
    docs = [Path("README.md"), Path("AGENTS.md")]
    docs += sorted(p.relative_to(root) for p in (root / "docs").glob("*.md"))
    return docs


def parse_directive(line: str) -> dict | None:
    """The key/value pairs of a directive line; a bare word maps to ""."""
    m = DIRECTIVE.match(line.strip())
    if not m:
        return None
    content = m.group(1).strip()
    pairs = PAIR.findall(content)
    if not pairs:
        return {content: ""}
    return {key: quoted or plain for key, quoted, plain in pairs}


def blocks_of(root: Path, doc: Path, errors: list[str]) -> list[Block]:
    lines = (root / doc).read_text(encoding="utf-8").splitlines()
    out = []
    i = 0
    while i < len(lines):
        m = FENCE.match(lines[i])
        if not m:
            i += 1
            continue
        fence, language = m.group(1), m.group(2)
        start = i
        i += 1
        body = []
        while i < len(lines) and lines[i].strip() != fence:
            body.append(lines[i])
            i += 1
        if i == len(lines):
            errors.append(f"{doc}:{start + 1}: unterminated code block")
            break
        i += 1
        previous = start - 1
        while previous >= 0 and not lines[previous].strip():
            previous -= 1
        directive = parse_directive(lines[previous]) if previous >= 0 else None
        out.append(Block(doc, start + 1, language, "\n".join(body) + "\n", directive or {}))
    return out


def same_text(a: str, b: str) -> bool:
    return a.rstrip("\n") == b.rstrip("\n")


def validate(root: Path, block: Block, errors: list[str]) -> str:
    """Checks one block; returns its class for the summary."""
    d = block.directive
    lang = block.language

    def unknown(allowed):
        extra = set(d) - allowed
        if extra:
            errors.append(f"{block.where}: unknown directive key(s) {sorted(extra)} for {lang}")

    def check_from() -> bool:
        source = root / d["from"]
        if not source.is_file():
            errors.append(f"{block.where}: from={d['from']} does not exist")
        elif not same_text(source.read_text(encoding="utf-8"), block.text):
            errors.append(f"{block.where}: the block differs from {d['from']}")
        return True

    if "illustrative" in d and not d["illustrative"].strip():
        errors.append(f"{block.where}: illustrative needs a reason")
    if lang == "cpp":
        unknown(CPP_KEYS)
        if "illustrative" in d:
            return "illustrative"
        if "from" in d:
            check_from()
            return "cpp-from"
        if "prelude" in d and not (root / "tests/docs/preludes" / f"{d['prelude']}.hpp").is_file():
            errors.append(f"{block.where}: prelude {d['prelude']} does not exist")
        if d.get("wrap", "body") != "body":
            errors.append(f"{block.where}: wrap must be body")
        if "requires" in d:
            if d["requires"] not in BRIDGE_KEYS:
                errors.append(f"{block.where}: requires must be one of {sorted(BRIDGE_KEYS)}")
            return "cpp-bridge"
        return "cpp-program" if "int main(" in block.text else "cpp-compiled"
    if lang == "python":
        unknown(PYTHON_KEYS)
        if "illustrative" in d:
            return "illustrative"
        if "from" in d:
            check_from()
            return "python-from"
        return "python-run"
    if lang in ("text", "json"):
        unknown(TEXT_KEYS)
        if "illustrative" in d:
            return "text-illustrative"
        if "lines-in" in d:
            source = root / d["lines-in"]
            if not source.is_file():
                errors.append(f"{block.where}: lines-in={d['lines-in']} does not exist")
                return "error"
            source_lines = {re.sub(r"^\s*//\s?", "", line).strip()
                            for line in source.read_text(encoding="utf-8").splitlines()}
            for line in block.text.splitlines():
                if line.strip() and line.strip() not in source_lines:
                    errors.append(f"{block.where}: line not in {d['lines-in']}: {line.strip()}")
            return "lines-in"
        if "output" not in d:
            errors.append(f"{block.where}: a {lang} block needs output=, lines-in= or illustrative=")
            return "error"
        expected = root / d["output"]
        if not expected.is_file():
            errors.append(f"{block.where}: output={d['output']} does not exist")
        elif not same_text(expected.read_text(encoding="utf-8"), block.text):
            errors.append(f"{block.where}: the block differs from {d['output']}")
        return "output"
    if lang in NAME_ONLY_LANGUAGES:
        unknown(set())
        return "names-only"
    errors.append(f"{block.where}: code block language '{lang}' is not one the check knows")
    return "error"


def generated_files(root: Path, blocks: list[tuple[Block, str]]) -> dict[str, str]:
    """The files `extract` writes, by name relative to OUT."""
    files = {}
    cmake = ["# Generated by tests/docs/check_docs.py extract; do not edit."]
    for block, kind in blocks:
        if kind in ("cpp-program", "cpp-compiled", "cpp-bridge"):
            d = block.directive
            source = f"// {block.where}\n"
            if "prelude" in d:
                source += f'#include "preludes/{d["prelude"]}.hpp"\n'
            source += f"#line {block.line + 1} \"{block.doc.as_posix()}\"\n"
            if "wrap" in d:
                # Leading #include lines stay outside the function.
                lines = block.text.splitlines(keepends=True)
                n = 0
                while n < len(lines) and (lines[n].startswith("#include") or not lines[n].strip()):
                    n += 1
                source += "".join(lines[:n])
                source += f"#line {block.line + 1 + n} \"{block.doc.as_posix()}\"\n"
                source += f"void lumen_docs_{block.name}() {{\n{''.join(lines[n:])}}}\n"
            else:
                source += block.text
            files[f"{block.name}.cpp"] = source
            mode = "program" if kind == "cpp-program" else "object"
            cmake.append(
                f'lumen_docs_cpp_block({block.name} "{block.where}" {mode} "{d.get("requires", "")}")'
            )
        elif kind == "python-run":
            files[f"{block.name}.py"] = block.text
            cmake.append(f'lumen_docs_python_block({block.name} "{block.where}")')
    files["blocks.cmake"] = "\n".join(cmake) + "\n"
    return files


@functools.cache
def tree_text(root: Path, patterns: tuple[str, ...]) -> str:
    """The text of the files matching `patterns`, outside build/ and this
    check's own directory (a name must exist in the code, not in the check)."""
    parts = []
    for pattern in patterns:
        for path in sorted(root.glob(pattern)):
            relative = path.relative_to(root).as_posix()
            if path.is_file() and not relative.startswith(("build/", "tests/docs/")):
                parts.append(path.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)


def words(text: str) -> set[str]:
    return set(re.findall(r"\w+", text))


def check_names(root: Path, doc: Path, text: str, errors: list[str]) -> None:
    code = tree_text(root, ("include/**/*.hpp", "src/**/*", "python/*", "CMakeLists.txt",
                            "cmake/*", "tests/**/*", "examples/**/*", "CMakePresets.json",
                            "vcpkg.json", ".github/workflows/*.yml"))
    code_words = words(code)
    headers = words(tree_text(root, ("include/**/*.hpp",)))
    cmake_text = tree_text(root, ("CMakeLists.txt", "cmake/*", "**/CMakeLists.txt", "**/*.cmake"))
    presets = {p["name"] for kind in ("configurePresets", "buildPresets", "testPresets")
               for p in json.loads((root / "CMakePresets.json").read_text())[kind]}
    python_names = set(re.findall(r'"(\w+)"', tree_text(root, ("python/*.cpp",)))) | words(
        (root / "python/logging_handler.py").read_text())

    def missing(kind: str, name: str) -> None:
        errors.append(f"{doc}: {kind} `{name}` does not exist in the source tree")

    for name in set(re.findall(r"\b(?:LUMEN|LOG)_[A-Z0-9_]*[A-Z0-9]\b", text)):
        if name not in code_words:
            missing("name", name)
    for name in set(re.findall(r"--preset[ =]([\w-]+)", text)):
        if name not in presets:
            missing("preset", name)
    for name in set(re.findall(r"--target ([\w-]+)", text)):
        if not re.search(rf"add_(custom_target|executable|library)\(\s*{re.escape(name)}\b", cmake_text):
            missing("build target", name)
    for name in set(re.findall(r"-D(\w+)=", text)):
        if not name.startswith(("CMAKE_", "VCPKG_")) and name not in words(cmake_text) | {
                "pybind11_DIR", "Python_EXECUTABLE"}:
            missing("CMake variable", name)
    for name in set(re.findall(r"\blumen/[\w/]+\.hpp\b", text)):
        if not (root / "include" / name).is_file():
            missing("header", name)
    path_pattern = r"(?<![\w/<])((?:examples|tests|src|cmake|python|docs|include|\.github)/[\w./-]*[\w/])"
    for name in set(re.findall(path_pattern, text)):
        # `src/line_writer.*` names the .hpp and the .cpp.
        if not (root / name).exists() and not any(root.glob(name + ".*")):
            missing("path", name)
    for target in set(re.findall(r"\]\(([^)#\s]+)(?:#[^)]*)?\)", text)):
        if "://" not in target and not (root / doc.parent / target).exists():
            missing("link target", target)
    for name in set(re.findall(r"\blumen::[\w:]*\w", text)):
        for part in name.split("::")[1:]:
            if part not in headers:
                missing("C++ name", name)
    lumen_types = set(re.findall(r"\b(?:class|struct|enum class)\s+(\w+)",
                                 tree_text(root, ("include/lumen/**/*.hpp",))))
    for scope, member in set(re.findall(r"\b([A-Z]\w+)::(\w+)\b", text)):
        if scope in lumen_types and member not in headers:
            missing("C++ name", f"{scope}::{member}")
    for name in set(re.findall(r"(?<![/\w])lumen\.(?!hpp\b)(\w+)\b", text)):
        if name not in python_names:
            missing("Python name", f"lumen.{name}")
    bridges = tree_text(root, ("include/lumen/bridges/*.hpp",))
    for name in set(re.findall(r"(?<![/\w])(?:eigen|openmp|root|torch)\.(?!hpp\b)[a-z_]+(?:\.[a-z_]+)*\b",
                               text)):
        if name not in bridges:
            missing("metric", name)


def cache_variables(root: Path) -> dict[str, str]:
    """LUMEN_ options and cache variables of the build, with their default text."""
    out = {}
    files = [root / "CMakeLists.txt", *sorted((root / "cmake").glob("*.cmake")),
             root / "tests/bridges/frameworks.cmake"]
    for path in files:
        text = path.read_text()
        for name, default in re.findall(r'option\((LUMEN_\w+)\s+"[^"]*"\s+(\S+?)\)', text):
            out[name] = default
        for name, default in re.findall(r'set\((LUMEN_\w+)\s+"([^"]*)"\s+CACHE', text):
            out[name] = default
    return out


def check_options_table(root: Path, doc: Path, errors: list[str]) -> None:
    lines = (root / doc).read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if parse_directive(line) != {"options-table": ""}:
            continue
        rows = {}
        for row in lines[i + 1:]:
            if not row.startswith("|"):
                if rows:
                    break
                continue
            cells = [c.strip() for c in row.strip("|").split("|")]
            names = re.findall(r"`(LUMEN_\w+)`", cells[0])
            for name in names:
                rows[name] = cells[1] if len(cells) > 1 else ""
        expected = cache_variables(root)
        for name in sorted(set(expected) - set(rows)):
            errors.append(f"{doc}:{i + 1}: the options table lacks {name}")
        for name in sorted(set(rows) - set(expected)):
            errors.append(f"{doc}:{i + 1}: the options table lists {name}, which the build lacks")
        for name, default in expected.items():
            literal = "${" not in default
            shown = f"`{default}`" if default else "empty"
            if name in rows and literal and shown not in rows[name]:
                errors.append(f"{doc}:{i + 1}: {name} defaults to `{default}`, the table says {rows[name]}")


def check_header_table(root: Path, doc: Path, errors: list[str]) -> None:
    """A table after <!-- check: header-table --> lists every public header."""
    lines = (root / doc).read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if parse_directive(line) != {"header-table": ""}:
            continue
        listed = set()
        for row in lines[i + 1:]:
            if row.startswith("#"):
                break
            if not row.startswith("|"):
                continue
            listed |= set(re.findall(r"`(lumen/[\w/]+\.hpp)`", row.split("|")[1]))
        public = {p.relative_to(root / "include").as_posix()
                  for p in (root / "include/lumen").rglob("*.hpp") if "detail" not in p.parts}
        for name in sorted(public - listed):
            errors.append(f"{doc}:{i + 1}: the header table lacks {name}")


def collect(root: Path, errors: list[str]) -> list[tuple[Block, str]]:
    classified = []
    for doc in documents(root):
        for block in blocks_of(root, doc, errors):
            classified.append((block, validate(root, block, errors)))
    return classified


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[1] not in ("extract", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    mode, root, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    errors: list[str] = []
    classified = collect(root, errors)
    files = generated_files(root, classified)

    if mode == "extract":
        out.mkdir(parents=True, exist_ok=True)
        for name, content in files.items():
            path = out / name
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                path.write_text(content, encoding="utf-8")
        return 0

    for name, content in files.items():
        path = out / name
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            errors.append(f"{path} is out of date: configure again (cmake --build re-runs it)")
    for doc in documents(root):
        text = (root / doc).read_text(encoding="utf-8")
        check_names(root, doc, text, errors)
        check_options_table(root, doc, errors)
        check_header_table(root, doc, errors)

    counts: dict[str, int] = {}
    for _, kind in classified:
        counts[kind] = counts.get(kind, 0) + 1
    print("Code blocks in README.md, AGENTS.md and docs/*.md:")
    labels = {
        "cpp-program": "C++, compiled and run by ctest",
        "cpp-compiled": "C++, compiled",
        "cpp-from": "C++, equal to an example that is compiled and run",
        "cpp-bridge": "C++, compiled when the bridge's framework is found",
        "python-run": "Python, run when LUMEN_ENABLE_PYTHON=ON",
        "python-from": "Python, equal to an example run when LUMEN_ENABLE_PYTHON=ON",
        "output": "output, equal to an example's checked output",
        "lines-in": "text, every line found in the header it quotes",
        "names-only": "shell/CMake, names checked, not executed",
        "illustrative": "C++/Python, illustrative (not compiled)",
        "text-illustrative": "text, illustrative",
        "error": "invalid",
    }
    for kind, label in labels.items():
        if counts.get(kind):
            print(f"  {counts[kind]:3d}  {label}")
    for block, kind in classified:
        if kind in ("illustrative", "text-illustrative"):
            print(f"  illustrative: {block.where}: {block.directive['illustrative']}")
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
