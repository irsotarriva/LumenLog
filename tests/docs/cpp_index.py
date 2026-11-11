"""An index of the names that C++ headers declare, by fully qualified name.

The documentation check resolves a name such as `lumen::Core::flush` against
it. This is not a C++ parser: it reads declarations the way Lumen's headers
are written (one declaration per statement, scopes opened by `namespace`,
`class`, `struct`, `union` and `enum`), and treats every other brace as the
body of a function or an initialiser, whose contents it skips.

Recorded in each namespace, class and enumeration scope: classes, structs,
unions, enumerations, concepts, type aliases, functions (constructors
included), variables and data members, enumerators, and the direct bases of
each class (to resolve inherited members).
"""

import re
from dataclasses import dataclass, field

_TOKEN = re.compile(r"[A-Za-z_]\w*|::|\S")
_COMMENT_OR_LITERAL = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"(?P<delim>[^(\s]*)\(.*?\)(?P=delim)"|"(?:\\.|[^"\\\n])*"|'
    r"'(?:\\.|[^'\\\n])*'",
    re.DOTALL,
)
_CLASS_KEYS = {"class", "struct", "union"}
_NOT_A_NAME = {
    "alignas", "alignof", "decltype", "noexcept", "requires", "sizeof", "static_assert",
    "operator", "explicit", "return", "if", "for", "while", "switch", "throw",
    "static_cast", "const_cast", "reinterpret_cast", "dynamic_cast",
}
_ACCESS = {"public", "protected", "private"}


@dataclass
class Index:
    names: set[str] = field(default_factory=set)        # every qualified name
    classes: set[str] = field(default_factory=set)      # qualified class, struct, union names
    bases: dict[str, list[str]] = field(default_factory=dict)  # class -> base names as written

    def add(self, scope: str, name: str) -> str:
        qualified = f"{scope}::{name}" if scope else name
        self.names.add(qualified)
        return qualified


@dataclass
class _Frame:
    kind: str  # "namespace", "class", "enum" or "body"
    name: str  # qualified name; "" for the global namespace and for bodies


def strip_code(text: str) -> str:
    """The text without comments, string and character literals, and
    preprocessor lines (with their continuation lines)."""
    text = _COMMENT_OR_LITERAL.sub(lambda m: " " if m.group(0)[0] != '"' else '""', text)
    text = re.sub(r"\\\n", " ", text)
    return "\n".join("" if line.lstrip().startswith("#") else line for line in text.splitlines())


def tokens(text: str) -> list[str]:
    return _TOKEN.findall(strip_code(text))


def _without_attributes(statement: list[str]) -> list[str]:
    """`statement` without [[...]] attributes and alignas(...)."""
    out: list[str] = []
    i = 0
    while i < len(statement):
        if statement[i:i + 2] == ["[", "["]:
            i = _skip_balanced(statement, i + 1, "[", "]") + 1
        elif statement[i] == "alignas" and statement[i + 1:i + 2] == ["("]:
            i = _skip_balanced(statement, i + 1, "(", ")")
        else:
            out.append(statement[i])
            i += 1
    return out


def _skip_balanced(items: list[str], start: int, opening: str, closing: str) -> int:
    """The index just after the bracket that closes the one at `start`."""
    depth = 0
    for i in range(start, len(items)):
        if items[i] == opening:
            depth += 1
        elif items[i] == closing:
            depth -= 1
            if depth == 0:
                return i + 1
    return len(items)


def _without_template_header(statement: list[str]) -> list[str]:
    while statement[:2] == ["template", "<"]:
        statement = statement[_skip_balanced(statement, 1, "<", ">"):]
    return statement


class _Nesting:
    """Depth in parentheses, brackets and template argument lists."""

    def __init__(self):
        self.parens = 0
        self.angles = 0

    @property
    def top(self) -> bool:
        return self.parens == 0 and self.angles == 0

    def step(self, token: str) -> None:
        if token in ("(", "["):
            self.parens += 1
        elif token in (")", "]"):
            self.parens -= 1
        elif self.parens == 0 and token == "<":
            self.angles += 1
        elif self.parens == 0 and token == ">" and self.angles > 0:
            self.angles -= 1


def _top_level(statement: list[str]):
    """(index, token) for each token that is not inside parentheses,
    brackets or a template argument list (opening brackets included)."""
    nesting = _Nesting()
    for i, token in enumerate(statement):
        if nesting.top:
            yield i, token
        nesting.step(token)


def _is_identifier(token: str) -> bool:
    return bool(re.fullmatch(r"[A-Za-z_]\w*", token))


def _is_name(token: str) -> bool:
    return _is_identifier(token) and token not in _NOT_A_NAME


def _call_position(statement: list[str]) -> int | None:
    """The index of the first top-level `(` that follows a name, before any
    top-level `=` (after it, a call is a variable's initialiser)."""
    for i, token in _top_level(statement):
        if token == "=":
            return None
        if token == "(" and i > 0 and _is_name(statement[i - 1]):
            return i
    return None


def _variable_names(statement: list[str]) -> list[str]:
    """The declarator names of a variable declaration: the last name before
    each top-level `=`, `{`, `:` (bit-field), `,` and the end."""
    names = []
    last = None
    taking = True
    for _, token in _top_level(statement + [","]):
        if token in ("=", "{", ":", ","):
            if taking and last:
                names.append(last)
            last = None
            taking = token == ","
        elif taking and _is_identifier(token) and token not in _NOT_A_NAME:
            last = token
    return names


def _using_names(statement: list[str]) -> list[str]:
    if len(statement) > 2 and statement[2] == "=":
        return [statement[1]]  # using Alias = ...;
    if statement[1:2] == ["namespace"]:
        return []
    return [statement[-1].rpartition("::")[2]]  # using Base::member;


_SPECIAL_DECLARATIONS = {
    "using": _using_names,
    "concept": lambda statement: statement[1:2],
    "typedef": lambda statement: [t for t in statement if _is_identifier(t)][-1:],
    "friend": lambda statement: [],
    "static_assert": lambda statement: [],
    "namespace": lambda statement: [],
}


def declared_names(statement: list[str]) -> list[str]:
    """The names one statement (without its `;` or body) declares."""
    statement = _without_template_header(_without_attributes(statement))
    if not statement:
        return []
    special = _SPECIAL_DECLARATIONS.get(statement[0])
    if special:
        return special(statement)
    call = _call_position(statement)
    return [statement[call - 1]] if call is not None else _variable_names(statement)


def _class_head(statement: list[str]) -> tuple[str, str, list[str]] | None:
    """(key, name, bases) if the statement opens or declares a class or an
    enumeration; key is "class" or "enum"."""
    statement = _without_requires_clause(_without_template_header(_without_attributes(statement)))
    if statement[:1] == ["enum"]:
        return _enum_head(statement)
    if statement[:1] and statement[0] in _CLASS_KEYS:
        return _struct_head(statement)
    return None


def _enum_head(statement: list[str]) -> tuple[str, str, list[str]] | None:
    rest = [t for t in statement[1:] if t not in ("class", "struct")]
    return ("enum", rest[0], []) if rest and _is_identifier(rest[0]) else None


def _struct_head(statement: list[str]) -> tuple[str, str, list[str]] | None:
    has_bases = ":" in statement
    head = statement[:statement.index(":")] if has_bases else statement
    if len(statement) < 2 or not _is_identifier(statement[1]) or "(" in head:
        return None
    bases = _base_names(statement[len(head) + 1:]) if has_bases else []
    return "class", statement[1], bases


def _without_requires_clause(statement: list[str]) -> list[str]:
    """A class head without the requires-clause in front of it."""
    if statement[:1] != ["requires"]:
        return statement
    for i, token in _top_level(statement):
        if token in _CLASS_KEYS:
            return statement[i:]
    return statement


def _base_names(tail: list[str]) -> list[str]:
    """Base class names (qualified as written) of a base clause."""
    out = []
    current: list[str] = []
    for _, token in _top_level(tail + [","]):
        if token == ",":
            if current:
                out.append("".join(current))
            current = []
        elif token not in _ACCESS and token not in ("virtual", "final"):
            current.append(token)
    return out


def _namespace_names(statement: list[str]) -> list[str] | None:
    """The names of a `namespace a::b {` head; [] for an anonymous one."""
    statement = [t for t in statement if t != "inline"]
    if statement[:1] != ["namespace"]:
        return None
    return [t for t in statement[1:] if t != "::"]


class _Builder:
    def __init__(self, index: Index):
        self.index = index
        self.stack = [_Frame("namespace", "")]
        self.statement: list[str] = []

    @property
    def frame(self) -> _Frame:
        return self.stack[-1]

    def feed(self, token: str) -> None:
        if self.frame.kind == "body":
            self._in_body(token)
        elif token == "{":
            self._open()
        elif token == "}":
            self._close()
        elif token == ";":
            self._end_statement()
        elif token == ":" and len(self.statement) == 1 and self.statement[0] in _ACCESS:
            self.statement = []
        else:
            self.statement.append(token)

    def _in_body(self, token: str) -> None:
        if token == "{":
            self.stack.append(_Frame("body", ""))
        elif token == "}":
            self.stack.pop()

    def _open(self) -> None:
        statement, self.statement = self.statement, []
        namespaces = _namespace_names(statement)
        if namespaces is not None:
            self._open_namespaces(namespaces)
            return
        head = _class_head(statement)
        if head is not None:
            self._open_class(*head)
            return
        if self.frame.kind == "enum":
            self.statement = statement  # an enumerator initialised with braces
        else:
            self._record(statement)
        self.stack.append(_Frame("body", ""))

    def _open_namespaces(self, names: list[str]) -> None:
        """`namespace a::b {` is one scope, closed by one brace."""
        scope = self.frame.name
        for name in names or ["(anonymous)"]:
            scope = self.index.add(scope, name)
        self.stack.append(_Frame("namespace", scope))

    def _open_class(self, key: str, name: str, bases: list[str]) -> None:
        qualified = self.index.add(self.frame.name, name)
        if key == "class":
            self.index.classes.add(qualified)
            self.index.bases.setdefault(qualified, []).extend(bases)
        self.stack.append(_Frame("enum" if key == "enum" else "class", qualified))

    def _close(self) -> None:
        if self.frame.kind == "enum":
            self._record_enumerators(self.statement)
        self.statement = []
        if len(self.stack) > 1:
            self.stack.pop()

    def _end_statement(self) -> None:
        statement, self.statement = self.statement, []
        head = _class_head(statement)
        if head is not None and not any(t in ("=", "(") for t in statement):
            qualified = self.index.add(self.frame.name, head[1])
            if head[0] == "class":
                self.index.classes.add(qualified)
            return
        self._record(statement)

    def _record(self, statement: list[str]) -> None:
        if self.frame.kind == "enum":
            return
        for name in declared_names(statement):
            self.index.add(self.frame.name, name)

    def _record_enumerators(self, statement: list[str]) -> None:
        for i, token in _top_level(statement):
            if _is_identifier(token) and (i == 0 or statement[i - 1] == ","):
                self.index.add(self.frame.name, token)


def index_text(text: str, index: Index | None = None) -> Index:
    """Adds the declarations of one header's text to `index` (a new one if
    None) and returns it."""
    index = index if index is not None else Index()
    builder = _Builder(index)
    for token in _merge_scopes(tokens(text)):
        builder.feed(token)
    return index


def _merge_scopes(items: list[str]) -> list[str]:
    """Joins `a :: b` into one token `a::b` everywhere except in namespace
    heads, so that a qualified name reads as one name."""
    out: list[str] = []
    for token in items:
        if out and (token == "::" or out[-1].endswith("::")) and not _in_namespace_head(out):
            out[-1] += token
        else:
            out.append(token)
    return out


def _in_namespace_head(out: list[str]) -> bool:
    for token in reversed(out):
        if token in ("{", ";", "}"):
            return False
        if token == "namespace":
            return True
    return False


def resolves(index: Index, qualified: str) -> bool:
    """True if `qualified` (a::b::c) names a declaration, directly or as a
    member that a class inherits."""
    if qualified in index.names:
        return True
    scope, _, member = qualified.rpartition("::")
    return bool(scope) and any(
        resolves(index, f"{base}::{member}") for base in qualified_bases(index, scope))


def qualified_bases(index: Index, cls: str) -> list[str]:
    """The direct bases of class `cls`, resolved from the namespaces that
    enclose it."""
    out = []
    for base in index.bases.get(cls, []):
        base = re.sub(r"<.*", "", base)
        enclosing = cls.split("::")[:-1]
        for depth in range(len(enclosing), -1, -1):
            candidate = "::".join(enclosing[:depth] + [base])
            if candidate in index.classes:
                out.append(candidate)
                break
    return out


def classes_named(index: Index, name: str) -> list[str]:
    """Every indexed class or enumeration whose last name component is `name`."""
    return sorted(q for q in index.names if q.rsplit("::", 1)[-1] == name and _is_scope(index, q))


def _is_scope(index: Index, qualified: str) -> bool:
    prefix = qualified + "::"
    return qualified in index.classes or any(n.startswith(prefix) for n in index.names)
