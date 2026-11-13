#!/usr/bin/env python3
"""Configure-only smoke test for the ps4-kit compatibility CMake surface."""
from __future__ import annotations
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
COMPAT = ROOT / "toolchain" / "ps4-kit-compat"

class CMakeCompatibilitySmokeTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cmake") and shutil.which("python3"), "CMake and Python 3 are required")
    def test_configure_generates_linker_script_and_eboot_target(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wineps4-kit-smoke-") as tmp:
            work = Path(tmp)
            openorbis = work / "openorbis"
            (openorbis / "bin").mkdir(parents=True)
            (openorbis / "bin" / "create-fself").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            (openorbis / "link.x").write_text(
                "SECTIONS {\n"
                "  .init_array : { *(.init_array); }\n"
                "  .data : { *(.data) }\n"
                "  .tdata : { *(.tdata) }\n"
                "  .tbss : { *(.tbss) }\n"
                "}\n", encoding="utf-8")
            project = work / "project"
            project.mkdir()
            (project / "main.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
                "project(compat_smoke C)\n"
                "include(\"" + str(COMPAT / "ps4-kit.cmake") + "\")\n"
                "add_executable(sample main.c)\n"
                "ps4_kit_app(sample)\n", encoding="utf-8")
            build = work / "build"
            result = subprocess.run(
                ["cmake", "-S", str(project), "-B", str(build),
                 f"-DOPENORBIS={openorbis}"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            generated = build / "ps4-link.x"
            self.assertTrue(generated.is_file())
            script = generated.read_text(encoding="utf-8")
            self.assertIn("SORT_BY_INIT_PRIORITY(.init_array.* .ctors.*)", script)
            self.assertIn("SORT_BY_INIT_PRIORITY(.fini_array.* .dtors.*)", script)
            for section in (".data", ".tdata", ".tbss"):
                self.assertIn(f"*({section} {section}.*)", script)
            link_files = list((build / "CMakeFiles").glob("sample.dir/link.txt"))
            self.assertEqual(len(link_files), 1, result.stdout)
            self.assertIn(str(generated), link_files[0].read_text(encoding="utf-8"))
            help_result = subprocess.run(
                ["cmake", "--build", str(build), "--target", "help"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(help_result.returncode, 0, help_result.stdout)
            self.assertIn("sample_eboot", help_result.stdout)

if __name__ == "__main__":
    unittest.main()
