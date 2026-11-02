#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADERS = ROOT / "include" / "libchiaking"
NAMESPACES = ROOT / "docs" / "doxygen" / "namespaces.dox"
INTERNAL_NAMESPACES = ROOT / "docs" / "doxygen" / "internal_namespaces.dox"
INTERNAL_HEADERS = [ROOT / "src" / "core", ROOT / "src" / "platform"]
INTERNAL_PATTERNS = ["*.hpp", "*.h"]
OUTPUT = ROOT / "build" / "docs"
CRASH_RETRIES = 8
LINT_XML = OUTPUT / "lint"
LINT_INTERNAL_XML = OUTPUT / "lint-internal"
SPECIFIERS = {"constexpr", "consteval", "inline", "static", "virtual", "explicit", "friend"}

FILE_BLOCK = re.compile(r"^\s*/// @file\s*$", re.MULTILINE)
LOCATED = re.compile(r"^((?:src|include)/[^:\s]+):(?:(\d+):)?")
DECLARED = re.compile(r"^namespace (libchiaking(?:::\w+)*)\s*\{", re.MULTILINE)
DESCRIBED = re.compile(r"^/// @namespace (\S+)\s*$", re.MULTILINE)
VERSION = re.compile(r"^project\(libchiaking\b[^)]*\bVERSION\s+([\d.]+)", re.MULTILINE)


def enclosing(namespace):
    parts = namespace.split("::")
    return {"::".join(parts[:end]) for end in range(1, len(parts) + 1)}


def headers_under(roots, patterns):
    found = set()
    for root in roots:
        for pattern in patterns:
            found.update(path for path in root.rglob(pattern)
                         if "generated" not in path.parts)
    return sorted(found)


def structural_problems(roots=(HEADERS,), patterns=("*.hpp",), descriptions=(NAMESPACES,)):
    problems = []
    described = set()
    for description in descriptions:
        described.update(DESCRIBED.findall(description.read_text()))
    declared = {}
    for header in headers_under(roots, patterns):
        text = header.read_text()
        name = header.relative_to(ROOT)
        if not FILE_BLOCK.search(text):
            problems.append(f"{name}: no '/// @file' block")
        for namespace in DECLARED.findall(text):
            for each in enclosing(namespace):
                declared.setdefault(each, name)
    for namespace in sorted(set(declared) - described):
        problems.append(
            f"{declared[namespace]}: namespace {namespace} is not described in "
            f"{descriptions[-1].relative_to(ROOT)}"
        )
    return problems


def library_version():
    found = VERSION.search((ROOT / "CMakeLists.txt").read_text())
    if found:
        return found.group(1)
    return ""


def doxygen_problems(doxyfile):
    doxygen = shutil.which("doxygen")
    if doxygen is None:
        return ["doxygen is not installed"]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, LIBCHIAKING_VERSION=library_version())
    for _ in range(CRASH_RETRIES):
        run = subprocess.run(
            [doxygen, doxyfile],
            cwd=ROOT,
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if run.returncode >= 0:
            break
    reported = run.stderr.replace(f"{ROOT}/", "").splitlines()
    problems = [line for line in reported if line.strip()]
    if run.returncode != 0:
        problems.append(f"doxygen exited with status {run.returncode}")
    return problems


def text_of(element):
    return "".join(element.itertext()).strip() if element is not None else ""


def returned_type(member):
    words = [word for word in text_of(member.find("type")).split() if word not in SPECIFIERS]
    declared = " ".join(words)
    arguments = text_of(member.find("argsstring"))
    if declared == "auto" and "->" in arguments:
        trailing = arguments.rsplit("->", 1)[1]
        return trailing.split("=", 1)[0].strip()
    return declared


def documented_names(member, kind):
    names = set()
    for listing in member.iter("parameterlist"):
        if listing.get("kind") != kind:
            continue
        for name in listing.iter("parametername"):
            names.add(text_of(name))
    return names


def returns_documented(member):
    return any(section.get("kind") == "return" for section in member.iter("simplesect"))


def signature_gaps(member):
    gaps = []
    documented = documented_names(member, "param")
    for parameter in member.findall("param"):
        name = text_of(parameter.find("declname"))
        if name and name not in documented:
            gaps.append(f"no @param for '{name}'")
    name = text_of(member.find("name"))
    returned = returned_type(member)
    constructs = not returned or name.startswith("~") or name.startswith("operator ")
    if not constructs and returned != "void" and not returns_documented(member):
        gaps.append("no @return")
    return gaps


def documented(member):
    return bool(text_of(member.find("briefdescription")) or text_of(member.find("detaileddescription")))


def signature_problems(xml=LINT_XML, prefix="include/", internal=False):
    problems = []
    seen = set()
    for compound in sorted(xml.glob("*.xml")):
        for member in ElementTree.parse(compound).getroot().iter("memberdef"):
            if member.get("kind") != "function":
                continue
            if not internal and member.get("prot") != "public":
                continue
            if member.get("id") in seen:
                continue
            seen.add(member.get("id"))
            location = member.find("location")
            header = location.get("file", "") if location is not None else ""
            if internal and not (header.startswith("src/") and documented(member)):
                continue
            if not header.endswith((".hpp", ".h")):
                continue
            if text_of(member.find("argsstring")).endswith(("=delete", "=default")):
                continue
            line = int(location.get("line", "0"))
            for gap in signature_gaps(member):
                problems.append(
                    (header, line, f"{prefix}{header}:{line}: "
                    f"{text_of(member.find('qualifiedname'))}: {gap}")
                )
    return [problem for _, _, problem in sorted(problems)]


def report_signatures(problems, strict):
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"{len(problems)} public function signature documentation gaps", file=sys.stderr)
    return 1 if strict and problems else 0


def report(problems):
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f"{len(problems)} documentation problems", file=sys.stderr)
        return 1
    return 0


def lint(strict_signatures):
    status = report(structural_problems() + doxygen_problems("docs/doxygen/Doxyfile.lint"))
    signatures = report_signatures(signature_problems(), strict_signatures)
    return status or signatures


def grouped(lines):
    problems = []
    for line in lines:
        if line.startswith(" ") and problems:
            problems[-1] = f"{problems[-1]}\n{line}"
        else:
            problems.append(line)
    return problems


def area_of(problem):
    found = LOCATED.match(problem)
    if found is None:
        return "doxygen"
    parts = Path(found.group(1)).parts
    if parts[:2] == ("src", "core") and len(parts) > 3:
        return parts[2]
    return parts[1] if parts[0] == "src" else parts[0]


def position_of(problem):
    found = LOCATED.match(problem)
    if found is None:
        return ("", 0)
    return (found.group(1), int(found.group(2) or 0))


def internal_areas():
    areas = {path.name: path for path in sorted(INTERNAL_HEADERS[0].iterdir()) if path.is_dir()}
    areas[INTERNAL_HEADERS[1].name] = INTERNAL_HEADERS[1]
    return areas


def selected_prefix(area):
    if area is None:
        return None
    areas = internal_areas()
    if area in areas:
        return f"{areas[area].relative_to(ROOT)}/"
    if (ROOT / area).is_dir():
        return f"{Path(area).as_posix().rstrip('/')}/"
    raise SystemExit(f"unknown area '{area}'; choose one of {', '.join(sorted(areas))} or a directory under src")


def internal_problems():
    structural = structural_problems(
        INTERNAL_HEADERS, INTERNAL_PATTERNS, (NAMESPACES, INTERNAL_NAMESPACES)
    )
    reported = grouped(doxygen_problems("docs/doxygen/Doxyfile.lint.internal"))
    located = [
        problem for problem in reported
        if "Incomplete input: scope for class" not in problem
        and (problem.startswith("src/") or LOCATED.match(problem) is None)
    ]
    signatures = signature_problems(LINT_INTERNAL_XML, "", internal=True)
    return sorted(structural + located + signatures, key=position_of)


def lint_internal(area):
    prefix = selected_prefix(area)
    problems = internal_problems()
    if prefix is not None:
        problems = [
            problem for problem in problems
            if problem.startswith(prefix) or LOCATED.match(problem) is None
        ]
    for problem in problems:
        print(problem, file=sys.stderr)
    counts = {}
    for problem in problems:
        counts[area_of(problem)] = counts.get(area_of(problem), 0) + 1
    for name in sorted(counts):
        print(f"{name:<16}{counts[name]:>6}")
    print(f"{'total':<16}{len(problems):>6}")
    return 1 if problems else 0


def build():
    problems = [
        problem for problem in doxygen_problems("docs/doxygen/Doxyfile")
        if "Incomplete input: scope for class" not in problem
    ]
    status = report(problems)
    if status == 0:
        print(OUTPUT / "html" / "index.html")
    return status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build", "lint", "lint-internal"])
    parser.add_argument("--strict-signatures", action="store_true")
    parser.add_argument("--area")
    arguments = parser.parse_args()
    if arguments.command == "build":
        return build()
    if arguments.command == "lint-internal":
        return lint_internal(arguments.area)
    return lint(arguments.strict_signatures)


if __name__ == "__main__":
    sys.exit(main())
