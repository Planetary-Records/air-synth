"""Fail when the named routed test groups miss, repeat or invent a section of the default run; development tooling only.

RoutedSynthEngineTests.cpp's main() runs one group when AIRACK_ROUTED_TEST_GROUP names it, else its default run. CTest
registers groups, not the default run, so together they must call each of the default run's sections exactly once.
"""
import argparse
from pathlib import Path
import re
import sys

SELECTOR = 'if(const auto* group=std::getenv("AIRACK_ROUTED_TEST_GROUP")){'
CALL = re.compile(r"\b([A-Za-z_]\w*)(?:<[^<>()]*>)?\(\);")
GROUP = re.compile(r'name=="(\w+)"\)\{')
# A branch the default run takes only when a diagnostic flag is set; CTest sets none.
FLAGGED = re.compile(r'if\(std::getenv\("\w+"\)\)\{')


def _block(text: str, open_brace: int) -> str:
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:i + 1]
    raise ValueError("unbalanced braces")


def _calls(text: str) -> set[str]:
    return {m.group(1) for m in CALL.finditer(text)}


def _unflagged(text: str) -> str:
    while m := FLAGGED.search(text):
        text = text[:m.start()] + text[m.end() - 1 + len(_block(text, m.end() - 1)):]
    return text


def parse(source: str) -> tuple[dict[str, set[str]], set[str]]:
    """Each group's section calls, and the default run's: main()'s calls after the group selector, outside flagged
    branches."""
    main = source[source.index("int main()"):]
    main = _block(main, main.index("{"))
    start = main.index(SELECTOR)
    selector = _block(main, start + len(SELECTOR) - 1)
    groups = {m.group(1): _calls(_block(selector, m.end() - 1)) for m in GROUP.finditer(selector)}
    return groups, _calls(_unflagged(main[start + len(SELECTOR) - 1 + len(selector):]))


def check(groups: dict[str, set[str]], default: set[str], names: list[str]) -> list[str]:
    if not names:
        return ["no groups named"]
    problems = [f"unknown group: {name}" for name in names if name not in groups]
    named = [name for name in names if name in groups]
    holders: dict[str, list[str]] = {}
    for name in named:
        for call in sorted(groups[name]):
            holders.setdefault(call, []).append(name)
    problems += [f"missing: {call}" for call in sorted(default) if call not in holders]
    problems += [f"twice: {call} in {', '.join(held)}" for call, held in sorted(holders.items()) if len(held) > 1]
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("names", nargs="*")
    args = parser.parse_args(argv)
    groups, default = parse(args.source.read_text(encoding="utf-8"))
    print(f"{len(args.names)} groups named of {len(groups)} in the source; the default run calls {len(default)} sections")
    problems = check(groups, default, args.names)
    for problem in problems:
        print(problem)
    if problems:
        return 1
    print("The named groups call each of the default run's sections exactly once.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
