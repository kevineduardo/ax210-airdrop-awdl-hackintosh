# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
"""Unit tests for tools/build/pbxinfo.py: the OpenStep plist parser on a synthetic
project, target resolution, and (on macOS) parser output equal to plutil's for the
driver projects."""
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "build"))
import pbxinfo  # noqa: E402

PROJECT = r"""// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {
	};
	objectVersion = 54;
	objects = {
/* Begin PBXBuildFile section */
		B1 /* a.cpp in Sources */ = {isa = PBXBuildFile; fileRef = F1 /* a.cpp */; };
		B2 /* b.cpp in Sources */ = {isa = PBXBuildFile; fileRef = F2; };
		B3 = {isa = PBXBuildFile; fileRef = F3; };
		B4 = {isa = PBXBuildFile; };
/* End PBXBuildFile section */
		F1 = {isa = PBXFileReference; path = a.cpp; sourceTree = "<group>"; };
		F2 = {isa = PBXFileReference; path = b.cpp; sourceTree = "<group>"; };
		F3 = {isa = PBXFileReference; name = IOKit.framework; path = System/Library/Frameworks/IOKit.framework; sourceTree = SDKROOT; };
		G0 = {isa = PBXGroup; children = (G1, F3, ); sourceTree = "<group>"; };
		G1 = {isa = PBXGroup; children = (F1, G2, ); path = src; sourceTree = "<group>"; };
		G2 = {isa = PBXGroup; children = (F2, ); path = sub; sourceTree = "<group>"; };
		P1 = {isa = PBXSourcesBuildPhase; files = (B1, B2, B4, ); };
		P2 = {isa = PBXFrameworksBuildPhase; files = (B3, ); };
		P3 = {isa = PBXShellScriptBuildPhase; shellScript = "scripts/gen.sh -P \"${PROJECT_DIR}/fw/\"\n"; };
		T1 = {isa = PBXNativeTarget; name = "Kext-Main"; buildConfigurationList = L1; buildPhases = (P1, P2, ); dependencies = (D1, ); };
		T2 = {isa = PBXAggregateTarget; name = gen; buildConfigurationList = L2; buildPhases = (P3, ); };
		D1 = {isa = PBXTargetDependency; target = T2; };
		L0 = {isa = XCConfigurationList; buildConfigurations = (C0r, C0d, ); };
		L1 = {isa = XCConfigurationList; buildConfigurations = (C1r, ); };
		L2 = {isa = XCConfigurationList; buildConfigurations = (C2r, ); };
		C0r = {isa = XCBuildConfiguration; name = Release; buildSettings = {
			MODULE_VERSION = 1.2.3;
			MACOSX_DEPLOYMENT_TARGET = 10.13;
			GCC_PREPROCESSOR_DEFINITIONS = ("VERSION=\\\"${MODULE_VERSION}\\\"", "COMMIT=\\\"$(GIT_COMMIT)\\\"", );
			TAB = "a\tb\\c\101\U00e9";
			DATA = <0aff 10>;
		}; };
		C0d = {isa = XCBuildConfiguration; name = Debug; buildSettings = { MODULE_VERSION = 9; }; };
		C1r = {isa = XCBuildConfiguration; name = Release; buildSettings = {
			GCC_PREPROCESSOR_DEFINITIONS = (FIRST, "$(inherited)", "LAST=1", );
			INFOPLIST_FILE = src/Info.plist;
			MACOSX_DEPLOYMENT_TARGET = 11.0;
		}; };
		C2r = {isa = XCBuildConfiguration; name = Release; buildSettings = { }; };
		R0 = {isa = PBXProject; buildConfigurationList = L0; mainGroup = G0; targets = (T1, T2, ); };
	};
	rootObject = R0 /* Project object */;
}
"""


class ParserTest(unittest.TestCase):
    def test_scalars_and_escapes(self):
        d = pbxinfo.parse_plist(PROJECT)
        s = d["objects"]["C0r"]["buildSettings"]
        self.assertEqual(d["archiveVersion"], "1")
        self.assertEqual(s["TAB"], "a\tb\\cAé")
        self.assertEqual(s["DATA"], b"\x0a\xff\x10")
        self.assertEqual(s["GCC_PREPROCESSOR_DEFINITIONS"],
                         ['VERSION=\\"${MODULE_VERSION}\\"', 'COMMIT=\\"$(GIT_COMMIT)\\"'])
        self.assertEqual(d["objects"]["P3"]["shellScript"], 'scripts/gen.sh -P "${PROJECT_DIR}/fw/"\n')

    def test_errors(self):
        for bad in ("{ a = b }", "{ a = (b c); }", '{ a = "x; }', "{ a = b; } junk", "{ /* x"):
            with self.assertRaises(pbxinfo.PlistError, msg=bad):
                pbxinfo.parse_plist(bad)


class ProjectTest(unittest.TestCase):
    def setUp(self):
        self.info = pbxinfo.Project(pbxinfo.parse_plist(PROJECT)).info("Kext-Main", "Release")

    def test_files(self):
        self.assertEqual(self.info["sources"], ["src/a.cpp", "src/sub/b.cpp"])
        self.assertEqual(self.info["frameworks"], ["System/Library/Frameworks/IOKit.framework"])
        self.assertEqual(self.info["resources"], [])

    def test_deps(self):
        self.assertEqual(self.info["deps"], [{"name": "gen", "settings": {},
                                              "scripts": ['scripts/gen.sh -P "${PROJECT_DIR}/fw/"\n']}])

    def test_resolve(self):
        self.assertEqual(pbxinfo.resolve(self.info, "MACOSX_DEPLOYMENT_TARGET"), "11.0")
        self.assertEqual(pbxinfo.resolve(self.info, "MODULE_VERSION"), "1.2.3")
        self.assertIsNone(pbxinfo.resolve(self.info, "NOPE"))
        self.assertEqual(pbxinfo.resolve(self.info, "GCC_PREPROCESSOR_DEFINITIONS"),
                         ["FIRST", 'VERSION=\\"${MODULE_VERSION}\\"', 'COMMIT=\\"$(GIT_COMMIT)\\"', "LAST=1"])

    def test_defines(self):
        self.assertEqual(pbxinfo.defines(self.info, {"MODULE_VERSION": "1.2.3", "GIT_COMMIT": "_abc"}),
                         ["-DFIRST", '-DVERSION="1.2.3"', '-DCOMMIT="_abc"', "-DLAST=1"])

    def test_unknown_target(self):
        with self.assertRaises(KeyError):
            pbxinfo.Project(pbxinfo.parse_plist(PROJECT)).info("Nope", "Release")


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.proj = os.path.join(self.tmp, "x.xcodeproj")
        os.mkdir(self.proj)
        with open(os.path.join(self.proj, "project.pbxproj"), "w") as f:
            f.write(PROJECT)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = pbxinfo.main([self.proj, "Kext-Main", "Release"] + list(args))
        return rc, out.getvalue(), err.getvalue()

    def test_json(self):
        rc, out, _ = self.run_main()
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out)["sources"], ["src/a.cpp", "src/sub/b.cpp"])

    def test_modes(self):
        self.assertEqual(self.run_main("--sources")[1], "src/a.cpp\nsrc/sub/b.cpp\n")
        self.assertEqual(self.run_main("--setting", "MODULE_VERSION")[1], "1.2.3\n")
        self.assertEqual(self.run_main("--setting", "INFOPLIST_FILE")[1], "src/Info.plist\n")
        rc, out, _ = self.run_main("--defines", "--var", "MODULE_VERSION=2", "--var", "GIT_COMMIT=_x")
        self.assertEqual(out.split("\n")[1], '-DVERSION="2"')
        rc, _, err = self.run_main("--setting", "NOPE")
        self.assertEqual(rc, 1)
        self.assertIn("NOPE is not set", err)


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("plutil"), "needs plutil (macOS)")
class PlutilParityTest(unittest.TestCase):
    def test_driver_projects(self):
        for rel in ("driver/AirportItlwm/itlwm.xcodeproj", "driver/BlueToolFixup/BrcmPatchRAM.xcodeproj"):
            path = os.path.join(ROOT, rel, "project.pbxproj")
            if not os.path.isfile(path):
                continue
            ref = json.loads(subprocess.run(["plutil", "-convert", "json", "-o", "-", path],
                                            capture_output=True, check=True).stdout)
            self.assertEqual(pbxinfo.load_project(os.path.join(ROOT, rel)), ref, rel)


if __name__ == "__main__":
    unittest.main()
