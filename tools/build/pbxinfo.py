#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""pbxinfo: show what xcodebuild would use for one target of an Xcode project.

    pbxinfo.py <project.xcodeproj> <target> <configuration>              JSON summary
    pbxinfo.py <project.xcodeproj> <target> <configuration> --sources    source files, one per line
    pbxinfo.py <project.xcodeproj> <target> <configuration> --setting NAME [--var K=V ...]
    pbxinfo.py <project.xcodeproj> <target> <configuration> --defines [--var K=V ...]

The JSON summary holds the project and target build settings of that
configuration, the source/framework/resource files of the target (paths
relative to the project directory) and its target dependencies with their
shell scripts.

--setting prints the target's value, falling back to the project's; a list is
printed one item per line, with "$(inherited)" replaced by the project's list.
--defines prints GCC_PREPROCESSOR_DEFINITIONS resolved the same way, as -D flags.
--var K=V substitutes ${K} and $(K) in the printed values.

Example (used by tools/build/build-airportitlwm-clt.sh):
    python3 -I tools/build/pbxinfo.py driver/AirportItlwm/itlwm.xcodeproj AirportItlwm-Tahoe Release --sources

The project file is read with a built-in OpenStep property-list parser, so no
Xcode or plutil is needed. Python 3.8+, standard library only.
"""
import argparse
import json
import os
import sys

UNQUOTED = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$/:.-+")
ESCAPES = {"a": "\a", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t", "v": "\v",
           '"': '"', "\\": "\\", "'": "'", "\n": "\n"}
PHASE_KEYS = {"PBXSourcesBuildPhase": "sources", "PBXFrameworksBuildPhase": "frameworks",
              "PBXResourcesBuildPhase": "resources"}


class PlistError(Exception):
    pass


class _Parser:
    """Recursive-descent parser for old-style (OpenStep) ASCII property lists."""

    def __init__(self, text):
        self.s = text
        self.i = 0

    def error(self, msg):
        line = self.s.count("\n", 0, self.i) + 1
        raise PlistError("%s at line %d" % (msg, line))

    def skip(self):
        s, n = self.s, len(self.s)
        while self.i < n:
            c = s[self.i]
            if c in " \t\r\n":
                self.i += 1
            elif s.startswith("//", self.i):
                j = s.find("\n", self.i)
                self.i = n if j < 0 else j + 1
            elif s.startswith("/*", self.i):
                j = s.find("*/", self.i + 2)
                if j < 0:
                    self.error("unterminated comment")
                self.i = j + 2
            else:
                break

    def expect(self, ch):
        self.skip()
        if self.i >= len(self.s) or self.s[self.i] != ch:
            self.error("expected %r" % ch)
        self.i += 1

    def peek(self):
        self.skip()
        return self.s[self.i] if self.i < len(self.s) else ""

    def value(self):
        c = self.peek()
        if c == "{":
            return self.dictionary()
        if c == "(":
            return self.array()
        if c == '"' or c == "'":
            return self.quoted(c)
        if c == "<":
            return self.data()
        return self.unquoted()

    def dictionary(self):
        self.expect("{")
        d = {}
        while self.peek() != "}":
            if not self.peek():
                self.error("unterminated dictionary")
            k = self.value()
            if not isinstance(k, str):
                self.error("dictionary key is not a string")
            self.expect("=")
            d[k] = self.value()
            self.expect(";")
        self.i += 1
        return d

    def array(self):
        self.expect("(")
        a = []
        while self.peek() != ")":
            if not self.peek():
                self.error("unterminated array")
            a.append(self.value())
            if self.peek() == ",":
                self.i += 1
            elif self.peek() != ")":
                self.error("expected ',' or ')'")
        self.i += 1
        return a

    def quoted(self, q):
        self.i += 1
        s, out = self.s, []
        while True:
            if self.i >= len(s):
                self.error("unterminated string")
            c = s[self.i]
            if c == q:
                self.i += 1
                return "".join(out)
            if c != "\\":
                out.append(c)
                self.i += 1
                continue
            e = s[self.i + 1:self.i + 2]
            if e in ("U", "u"):
                out.append(chr(int(s[self.i + 2:self.i + 6], 16)))
                self.i += 6
            elif e and e in "01234567":
                j = self.i + 1
                while j < self.i + 4 and j < len(s) and s[j] in "01234567":
                    j += 1
                out.append(chr(int(s[self.i + 1:j], 8)))
                self.i = j
            else:
                out.append(ESCAPES.get(e, e))
                self.i += 2

    def data(self):
        j = self.s.find(">", self.i)
        if j < 0:
            self.error("unterminated data")
        hexdigits = "".join(self.s[self.i + 1:j].split())
        self.i = j + 1
        try:
            return bytes.fromhex(hexdigits)
        except ValueError:
            self.error("bad data")

    def unquoted(self):
        j = self.i
        while j < len(self.s) and self.s[j] in UNQUOTED:
            j += 1
        if j == self.i:
            self.error("unexpected character %r" % self.s[self.i:self.i + 1])
        tok, self.i = self.s[self.i:j], j
        return tok


def parse_plist(text):
    p = _Parser(text)
    v = p.value()
    if p.peek():
        p.error("trailing data")
    return v


def load_project(xcodeproj):
    path = xcodeproj if xcodeproj.endswith(".pbxproj") else os.path.join(xcodeproj, "project.pbxproj")
    with open(path, encoding="utf-8") as f:
        return parse_plist(f.read())


class Project:
    def __init__(self, data):
        self.objects = data["objects"]
        self.root = self.objects[data["rootObject"]]
        self.parent = {}
        for k, v in self.objects.items():
            for c in v.get("children", []):
                self.parent[c] = k

    def path_of(self, ref):
        """Path of a file reference, relative to the project directory."""
        parts, r = [], ref
        while r in self.objects:
            o = self.objects[r]
            if o.get("path"):
                parts.append(o["path"])
            st = o.get("sourceTree")
            if st and st != "<group>":
                break
            r = self.parent.get(r)
        return "/".join(reversed(parts))

    def settings(self, listref, conf):
        for c in self.objects[listref]["buildConfigurations"]:
            if self.objects[c]["name"] == conf:
                return self.objects[c]["buildSettings"]
        return {}

    def target(self, name):
        for t in self.root["targets"]:
            if self.objects[t]["name"] == name:
                return self.objects[t]
        names = ", ".join(sorted(self.objects[t]["name"] for t in self.root["targets"]))
        raise KeyError("no target %r (targets: %s)" % (name, names))

    def info(self, target, conf):
        tgt = self.target(target)
        out = {"project_settings": self.settings(self.root["buildConfigurationList"], conf),
               "target_settings": self.settings(tgt["buildConfigurationList"], conf),
               "sources": [], "frameworks": [], "resources": [], "deps": []}
        for ph in tgt.get("buildPhases", []):
            p = self.objects[ph]
            key = PHASE_KEYS.get(p["isa"])
            for bf in p.get("files", []):
                fr = self.objects[bf].get("fileRef")
                if key and fr:
                    out[key].append(self.path_of(fr))
        for dep in tgt.get("dependencies", []):
            t = self.objects[self.objects[dep]["target"]]
            scripts = [self.objects[ph].get("shellScript") for ph in t.get("buildPhases", [])
                       if self.objects[ph]["isa"] == "PBXShellScriptBuildPhase"]
            out["deps"].append({"name": t["name"], "scripts": scripts,
                                "settings": self.settings(t["buildConfigurationList"], conf)})
        return out


def resolve(info, name):
    """Target value of a setting, falling back to the project; lists get $(inherited) expanded."""
    proj = info["project_settings"].get(name)
    tgt = info["target_settings"].get(name)
    if tgt is None:
        return proj
    if isinstance(tgt, list):
        inherited = proj if isinstance(proj, list) else ([proj] if proj else [])
        out = []
        for x in tgt:
            out += inherited if x == "$(inherited)" else [x]
        return out
    return tgt


def substitute(value, variables):
    for k, v in variables.items():
        value = value.replace("${%s}" % k, v).replace("$(%s)" % k, v)
    return value


def defines(info, variables):
    vals = resolve(info, "GCC_PREPROCESSOR_DEFINITIONS") or []
    if isinstance(vals, str):
        vals = vals.split()
    return ["-D" + substitute(x, variables).replace('\\"', '"') for x in vals]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project")
    ap.add_argument("target")
    ap.add_argument("configuration")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--sources", action="store_true")
    mode.add_argument("--setting", metavar="NAME")
    mode.add_argument("--defines", action="store_true")
    ap.add_argument("--var", action="append", default=[], metavar="K=V")
    a = ap.parse_args(argv)
    variables = {}
    for kv in a.var:
        if "=" not in kv:
            ap.error("--var needs K=V, got %r" % kv)
        k, v = kv.split("=", 1)
        variables[k] = v
    try:
        info = Project(load_project(a.project)).info(a.target, a.configuration)
    except (OSError, PlistError, KeyError) as e:
        print("pbxinfo: %s" % (e.args[0] if isinstance(e, KeyError) else e), file=sys.stderr)
        return 1
    if a.sources:
        lines = info["sources"]
    elif a.defines:
        lines = defines(info, variables)
    elif a.setting:
        v = resolve(info, a.setting)
        if v is None:
            print("pbxinfo: %s is not set" % a.setting, file=sys.stderr)
            return 1
        lines = [substitute(x, variables) for x in (v if isinstance(v, list) else [v])]
    else:
        json.dump(info, sys.stdout, indent=1, default=lambda b: b.hex())  # <data> as hex
        sys.stdout.write("\n")
        return 0
    for line in lines:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
