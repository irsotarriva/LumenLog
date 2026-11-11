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
            requires=KEY       a bridge block, compiled only when the
                               framework is found (KEY: eigen, openmp, torch,
                               root); otherwise it is not built at all
            illustrative=WHY   not compiled; WHY is printed
python    run by ctest when LUMEN_ENABLE_PYTHON is ON. from=PATH and
          illustrative=WHY as above.
text, json
          must have output=PATH (the block equals the file, an expected
          output of an example), lines-in=PATH (every line of the block is a
          line of PATH, comment markers removed) or illustrative=WHY.
sh, cmake not executed. The names in them are checked (see below).

Anything else, or a block that breaks these rules, is an error.

Names. In the whole text of every document:
  - every LUMEN_* and LOG_* macro or CMake variable, and every -D variable,
    must be a word of the source tree;
  - every `--preset` must be a preset, every `--target` a target;
  - every lumen/... header must exist (or be generated from cmake/*.in);
  - every repository path and every relative link must exist;
  - every lumen::a::b name must be declared, as that qualified name, in the
    headers under include/ (an inherited member counts). lumen::lumen is the
    CMake target;
  - every Scope::member whose Scope is a class or enumeration of Lumen must
    name a member of one class or enumeration of that name;
  - no detail:: name may appear: those are internal. docs/design.md, which
    describes the internals, may name existing ones;
  - every lumen.<name> in Python must be a name the module registers;
  - every bridge metric name must appear in the bridge headers.
A table preceded by <!-- check: options-table --> must list exactly the
LUMEN_ cache variables of the build, with each literal default; one preceded
by <!-- check: header-table --> must list every public header.

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

# cpp_index sits next to this file; importing it must not write a bytecode
# cache into the source tree.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cpp_index  # noqa: E402

FENCE = re.compile(r"^(```+)\s*([\w+-]*)\s*$")
DIRECTIVE = re.compile(r"^<!--\s*check:(.*?)-->\s*$")
PAIR = re.compile(r'(\w[\w-]*)=(?:"([^"]*)"|(\S+))')

CPP_KEYS = {"from", "prelude", "wrap", "requires", "illustrative"}
PYTHON_KEYS = {"from", "illustrative"}
TEXT_KEYS = {"output", "lines-in", "illustrative"}
BRIDGE_KEYS = {"eigen", "openmp", "torch", "root"}
NAME_ONLY_LANGUAGES = {"sh", "cmake"}
# Documents whose subject is the implementation, so they may name internals.
INTERNALS_DOCUMENTS = {"docs/design.md"}

SUMMARY_LABELS = {
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


# Blocks.


def directive_before(lines: list[str], fence_index: int) -> dict:
    """The directive on the last non-blank line before the fence, or {}."""
    previous = fence_index - 1
    while previous >= 0 and not lines[previous].strip():
        previous -= 1
    return (parse_directive(lines[previous]) if previous >= 0 else None) or {}


def block_end(lines: list[str], start: int, fence: str) -> int:
    """The index of the line that closes the block opened at `start`, or
    len(lines) if none does."""
    end = start + 1
    while end < len(lines) and lines[end].strip() != fence:
        end += 1
    return end


def blocks_in(doc: Path, text: str, errors: list[str]) -> list[Block]:
    lines = text.splitlines()
    out = []
    i = 0
    while i < len(lines):
        m = FENCE.match(lines[i])
        if not m:
            i += 1
            continue
        end = block_end(lines, i, m.group(1))
        if end == len(lines):
            errors.append(f"{doc}:{i + 1}: unterminated code block")
            break
        body = "\n".join(lines[i + 1:end]) + "\n"
        out.append(Block(doc, i + 1, m.group(2), body, directive_before(lines, i)))
        i = end + 1
    return out


def blocks_of(root: Path, doc: Path, errors: list[str]) -> list[Block]:
    return blocks_in(doc, (root / doc).read_text(encoding="utf-8"), errors)


# Classification.


def same_text(a: str, b: str) -> bool:
    return a.rstrip("\n") == b.rstrip("\n")


def check_keys(block: Block, allowed: set[str], errors: list[str]) -> None:
    extra = set(block.directive) - allowed
    if extra:
        errors.append(f"{block.where}: unknown directive key(s) {sorted(extra)} for {block.language}")


def check_equal_to_file(root: Path, block: Block, key: str, errors: list[str]) -> None:
    """The block equals the file its directive `key` names."""
    name = block.directive[key]
    source = root / name
    if not source.is_file():
        errors.append(f"{block.where}: {key}={name} does not exist")
    elif not same_text(source.read_text(encoding="utf-8"), block.text):
        errors.append(f"{block.where}: the block differs from {name}")


def validate_cpp(root: Path, block: Block, errors: list[str]) -> str:
    d = block.directive
    check_keys(block, CPP_KEYS, errors)
    if "illustrative" in d:
        return "illustrative"
    if "from" in d:
        check_equal_to_file(root, block, "from", errors)
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


def validate_python(root: Path, block: Block, errors: list[str]) -> str:
    check_keys(block, PYTHON_KEYS, errors)
    if "illustrative" in block.directive:
        return "illustrative"
    if "from" in block.directive:
        check_equal_to_file(root, block, "from", errors)
        return "python-from"
    return "python-run"


def check_lines_in(root: Path, block: Block, errors: list[str]) -> str:
    """Every non-blank line of the block is a line of the file lines-in names
    (with // comment markers removed)."""
    name = block.directive["lines-in"]
    source = root / name
    if not source.is_file():
        errors.append(f"{block.where}: lines-in={name} does not exist")
        return "error"
    source_lines = {re.sub(r"^\s*//\s?", "", line).strip()
                    for line in source.read_text(encoding="utf-8").splitlines()}
    for line in block.text.splitlines():
        if line.strip() and line.strip() not in source_lines:
            errors.append(f"{block.where}: line not in {name}: {line.strip()}")
    return "lines-in"


def validate_text(root: Path, block: Block, errors: list[str]) -> str:
    check_keys(block, TEXT_KEYS, errors)
    if "illustrative" in block.directive:
        return "text-illustrative"
    if "lines-in" in block.directive:
        return check_lines_in(root, block, errors)
    if "output" not in block.directive:
        errors.append(f"{block.where}: a {block.language} block needs output=, lines-in= or illustrative=")
        return "error"
    check_equal_to_file(root, block, "output", errors)
    return "output"


def validate_names_only(_root: Path, block: Block, errors: list[str]) -> str:
    check_keys(block, set(), errors)
    return "names-only"


VALIDATORS = {
    "cpp": validate_cpp,
    "python": validate_python,
    "text": validate_text,
    "json": validate_text,
    **{language: validate_names_only for language in NAME_ONLY_LANGUAGES},
}


def validate(root: Path, block: Block, errors: list[str]) -> str:
    """Checks one block; returns its class for the summary."""
    if "illustrative" in block.directive and not block.directive["illustrative"].strip():
        errors.append(f"{block.where}: illustrative needs a reason")
    validator = VALIDATORS.get(block.language)
    if validator is None:
        errors.append(f"{block.where}: code block language '{block.language}' is not one the check knows")
        return "error"
    return validator(root, block, errors)


# Generated files.


def wrapped_body(block: Block) -> str:
    """The block as the body of a function; its leading #include lines stay
    outside the function."""
    lines = block.text.splitlines(keepends=True)
    n = 0
    while n < len(lines) and (lines[n].startswith("#include") or not lines[n].strip()):
        n += 1
    return ("".join(lines[:n]) + f"#line {block.line + 1 + n} \"{block.doc.as_posix()}\"\n"
            + f"void lumen_docs_{block.name}() {{\n{''.join(lines[n:])}}}\n")


def cpp_source(block: Block) -> str:
    d = block.directive
    source = f"// {block.where}\n"
    if "prelude" in d:
        source += f'#include "preludes/{d["prelude"]}.hpp"\n'
    source += f"#line {block.line + 1} \"{block.doc.as_posix()}\"\n"
    return source + (wrapped_body(block) if "wrap" in d else block.text)


def generated_files(blocks: list[tuple[Block, str]]) -> dict[str, str]:
    """The files `extract` writes, by name relative to OUT."""
    files = {}
    cmake = ["# Generated by tests/docs/check_docs.py extract; do not edit."]
    for block, kind in blocks:
        if kind in ("cpp-program", "cpp-compiled", "cpp-bridge"):
            files[f"{block.name}.cpp"] = cpp_source(block)
            mode = "program" if kind == "cpp-program" else "object"
            requires = block.directive.get("requires", "")
            cmake.append(f'lumen_docs_cpp_block({block.name} "{block.where}" {mode} "{requires}")')
        elif kind == "python-run":
            files[f"{block.name}.py"] = block.text
            cmake.append(f'lumen_docs_python_block({block.name} "{block.where}")')
    files["blocks.cmake"] = "\n".join(cmake) + "\n"
    return files


# The source tree, as the name checks see it.


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


class Tree:
    """What the name checks compare a document with; each part is read once."""

    def __init__(self, root: Path):
        self.root = root

    @functools.cached_property
    def code_words(self) -> set[str]:
        return words(tree_text(self.root, (
            "include/**/*.hpp", "src/**/*", "python/*", "CMakeLists.txt", "cmake/*", "tests/**/*",
            "examples/**/*", "CMakePresets.json", "vcpkg.json", ".github/workflows/*.yml")))

    @functools.cached_property
    def cmake_text(self) -> str:
        return tree_text(self.root, ("CMakeLists.txt", "cmake/*", "**/CMakeLists.txt", "**/*.cmake"))

    @functools.cached_property
    def presets(self) -> set[str]:
        data = json.loads((self.root / "CMakePresets.json").read_text())
        return {p["name"] for kind in ("configurePresets", "buildPresets", "testPresets")
                for p in data[kind]}

    @functools.cached_property
    def python_names(self) -> set[str]:
        """Strings in the module's sources other than argument names, and the
        top-level definitions of the logging handler module."""
        bindings = re.sub(r'\barg\(\s*"\w+"', "", tree_text(self.root, ("python/*.cpp",)))
        handler = (self.root / "python/logging_handler.py").read_text()
        definitions = re.findall(r"^(?:def|class)\s+(\w+)|^(\w+)\s*=", handler, re.MULTILINE)
        return set(re.findall(r'"(\w+)"', bindings)) | {a or b for a, b in definitions}

    @functools.cached_property
    def cpp(self) -> cpp_index.Index:
        index = cpp_index.Index()
        for path in sorted((self.root / "include").rglob("*.hpp")):
            cpp_index.index_text(path.read_text(encoding="utf-8"), index)
        return index

    @functools.cached_property
    def bridge_text(self) -> str:
        return tree_text(self.root, ("include/lumen/bridges/*.hpp",))


# Name checks. Each takes (tree, doc, text) and yields one problem for each
# name in the text that is wrong.


def absent(kind: str, name: str, detail: str = "") -> str:
    return f"{kind} `{name}` does not exist in the source tree{detail}"


def missing_macros(tree: Tree, _doc: Path, text: str):
    for name in sorted(set(re.findall(r"\b(?:LUMEN|LOG)_[A-Z0-9_]*[A-Z0-9]\b", text))):
        if name not in tree.code_words:
            yield absent("name", name)


def missing_presets(tree: Tree, _doc: Path, text: str):
    for name in sorted(set(re.findall(r"--preset[ =]([\w-]+)", text))):
        if name not in tree.presets:
            yield absent("preset", name)


def missing_targets(tree: Tree, _doc: Path, text: str):
    for name in sorted(set(re.findall(r"--target ([\w-]+)", text))):
        if not re.search(rf"add_(custom_target|executable|library)\(\s*{re.escape(name)}\b", tree.cmake_text):
            yield absent("build target", name)


def missing_cmake_variables(tree: Tree, _doc: Path, text: str):
    known = words(tree.cmake_text) | {"pybind11_DIR", "Python_EXECUTABLE"}
    for name in sorted(set(re.findall(r"-D(\w+)=", text))):
        if not name.startswith(("CMAKE_", "VCPKG_")) and name not in known:
            yield absent("CMake variable", name)


def header_exists(root: Path, name: str) -> bool:
    """A header under include/, or one CMake generates from cmake/<file>.in."""
    return (root / "include" / name).is_file() or (root / "cmake" / f"{Path(name).name}.in").is_file()


def missing_headers(tree: Tree, _doc: Path, text: str):
    for name in sorted(set(re.findall(r"\blumen/[\w/]+\.hpp\b", text))):
        if not header_exists(tree.root, name):
            yield absent("header", name)


PATH = re.compile(r"(?<![\w/<])((?:examples|tests|src|cmake|python|docs|include|\.github)/[\w./-]*[\w/])")


def missing_paths(tree: Tree, _doc: Path, text: str):
    for name in sorted(set(PATH.findall(text))):
        # `src/line_writer.*` names the .hpp and the .cpp.
        if not (tree.root / name).exists() and not any(tree.root.glob(name + ".*")):
            yield absent("path", name)


def missing_links(tree: Tree, doc: Path, text: str):
    for target in sorted(set(re.findall(r"\]\(([^)#\s]+)(?:#[^)]*)?\)", text))):
        if "://" not in target and not (tree.root / doc.parent / target).exists():
            yield absent("link target", target)


def is_cmake_target(tree: Tree, name: str) -> bool:
    return bool(re.search(rf"add_library\(\s*{re.escape(name)}\s+ALIAS\b", tree.cmake_text))


def missing_qualified_names(tree: Tree, doc: Path, text: str):
    """lumen::a::b, resolved as that qualified name in the headers."""
    for name in sorted(set(re.findall(r"\blumen::[\w:]*\w", text))):
        if "::detail::" in name:
            continue  # reported by internal_names
        if not is_cmake_target(tree, name) and not cpp_index.resolves(tree.cpp, name):
            yield absent("C++ name", name)


def missing_scoped_names(tree: Tree, _doc: Path, text: str):
    """Scope::member, where Scope is the last part of a Lumen class name."""
    for scope, member in sorted(set(re.findall(r"(?<![\w:])([A-Z]\w+)::(\w+)\b", text))):
        candidates = [c for c in cpp_index.classes_named(tree.cpp, scope) if c.startswith("lumen::")]
        if candidates and not any(cpp_index.resolves(tree.cpp, f"{c}::{member}") for c in candidates):
            yield absent("C++ name", f"{scope}::{member}", f" (looked in {', '.join(candidates)})")


def internal_names(tree: Tree, doc: Path, text: str):
    """detail:: names are not part of the interface a document describes."""
    for name in sorted(set(re.findall(r"(?<![\w:])(?:lumen::)?detail::\w+", text))):
        qualified = name if name.startswith("lumen::") else f"lumen::{name}"
        if doc.as_posix() not in INTERNALS_DOCUMENTS:
            yield f"`{name}` is an internal (detail::) name, which only {sorted(INTERNALS_DOCUMENTS)} may use"
        elif not cpp_index.resolves(tree.cpp, qualified):
            yield absent("C++ name", name)


def missing_python_names(tree: Tree, _doc: Path, text: str):
    for name in sorted(set(re.findall(r"(?<![/\w])lumen\.(?!hpp\b)(\w+)\b", text))):
        if name not in tree.python_names:
            yield absent("Python name", f"lumen.{name}")


def missing_metrics(tree: Tree, _doc: Path, text: str):
    pattern = r"(?<![/\w])(?:eigen|openmp|root|torch)\.(?!hpp\b)[a-z_]+(?:\.[a-z_]+)*\b"
    for name in sorted(set(re.findall(pattern, text))):
        if name not in tree.bridge_text:
            yield absent("metric", name)


NAME_CHECKS = (
    missing_macros, missing_presets, missing_targets, missing_cmake_variables, missing_headers,
    missing_paths, missing_links, missing_qualified_names, missing_scoped_names, internal_names,
    missing_python_names, missing_metrics,
)


def check_names(tree: Tree, doc: Path, text: str, errors: list[str]) -> None:
    for check in NAME_CHECKS:
        errors.extend(f"{doc}: {problem}" for problem in check(tree, doc, text))


# Tables.


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


def tables_after(lines: list[str], marker: str):
    """The index of each line that holds the directive <!-- check: marker -->."""
    return [i for i, line in enumerate(lines) if parse_directive(line) == {marker: ""}]


def table_rows(lines: list[str]) -> list[list[str]]:
    """The cells of the first Markdown table in `lines`."""
    rows = []
    for row in lines:
        if row.startswith("|"):
            rows.append([c.strip() for c in row.strip("|").split("|")])
        elif rows:
            break
    return rows


def options_in_table(lines: list[str]) -> dict[str, str]:
    """Option name -> its Default cell."""
    out = {}
    for cells in table_rows(lines):
        for name in re.findall(r"`(LUMEN_\w+)`", cells[0]):
            out[name] = cells[1] if len(cells) > 1 else ""
    return out


def option_problems(rows: dict[str, str], expected: dict[str, str]) -> list[str]:
    problems = [f"the options table lacks {name}" for name in sorted(set(expected) - set(rows))]
    problems += [f"the options table lists {name}, which the build lacks"
                 for name in sorted(set(rows) - set(expected))]
    for name, default in expected.items():
        shown = f"`{default}`" if default else "empty"
        if name in rows and "${" not in default and shown not in rows[name]:
            problems.append(f"{name} defaults to `{default}`, the table says {rows[name]}")
    return problems


def check_options_table(root: Path, doc: Path, errors: list[str]) -> None:
    lines = (root / doc).read_text(encoding="utf-8").splitlines()
    for i in tables_after(lines, "options-table"):
        for problem in option_problems(options_in_table(lines[i + 1:]), cache_variables(root)):
            errors.append(f"{doc}:{i + 1}: {problem}")


def headers_in_table(lines: list[str]) -> set[str]:
    """The headers named in the first column, up to the next heading."""
    listed = set()
    for row in lines:
        if row.startswith("#"):
            break
        if row.startswith("|"):
            listed |= set(re.findall(r"`(lumen/[\w/]+\.hpp)`", row.split("|")[1]))
    return listed


def check_header_table(root: Path, doc: Path, errors: list[str]) -> None:
    """A table after <!-- check: header-table --> lists every public header."""
    lines = (root / doc).read_text(encoding="utf-8").splitlines()
    public = {p.relative_to(root / "include").as_posix()
              for p in (root / "include/lumen").rglob("*.hpp") if "detail" not in p.parts}
    for i in tables_after(lines, "header-table"):
        for name in sorted(public - headers_in_table(lines[i + 1:])):
            errors.append(f"{doc}:{i + 1}: the header table lacks {name}")


# Commands.


def collect(root: Path, errors: list[str]) -> list[tuple[Block, str]]:
    classified = []
    for doc in documents(root):
        for block in blocks_of(root, doc, errors):
            classified.append((block, validate(root, block, errors)))
    return classified


def write_changed(out: Path, files: dict[str, str]) -> None:
    """Writes only the files whose content changed, so that the build does
    not recompile the blocks that stayed the same."""
    out.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        path = out / name
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            path.write_text(content, encoding="utf-8")


def stale_files(out: Path, files: dict[str, str]) -> list[str]:
    return [f"{out / name} is out of date: configure again (cmake --build re-runs it)"
            for name, content in files.items()
            if not (out / name).exists() or (out / name).read_text(encoding="utf-8") != content]


def check_documents(root: Path, errors: list[str]) -> None:
    tree = Tree(root)
    for doc in documents(root):
        check_names(tree, doc, (root / doc).read_text(encoding="utf-8"), errors)
        check_options_table(root, doc, errors)
        check_header_table(root, doc, errors)


def print_summary(classified: list[tuple[Block, str]]) -> None:
    counts: dict[str, int] = {}
    for _, kind in classified:
        counts[kind] = counts.get(kind, 0) + 1
    print("Code blocks in README.md, AGENTS.md and docs/*.md:")
    for kind, label in SUMMARY_LABELS.items():
        if counts.get(kind):
            print(f"  {counts[kind]:3d}  {label}")
    for block, kind in classified:
        if kind in ("illustrative", "text-illustrative"):
            print(f"  illustrative: {block.where}: {block.directive['illustrative']}")


def run(mode: str, root: Path, out: Path) -> int:
    errors: list[str] = []
    classified = collect(root, errors)
    files = generated_files(classified)
    if mode == "extract":
        write_changed(out, files)
        return 0
    errors += stale_files(out, files)
    check_documents(root, errors)
    print_summary(classified)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


def main(argv: list[str]) -> int:
    if len(argv) != 4 or argv[1] not in ("extract", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    return run(argv[1], Path(argv[2]), Path(argv[3]))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
