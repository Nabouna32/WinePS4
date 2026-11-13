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

MESA_ARCHIVES = [
    "src/amd/vulkan/libvulkan_radeon.a",
    "src/amd/common/libamd_common.a",
    "src/amd/compiler/libaco.a",
    "src/amd/addrlib/libaddrlib.a",
    "src/vulkan/util/libvulkan_util.a",
    "src/compiler/spirv/libvtn.a",
    "src/compiler/nir/libnir.a",
    "src/compiler/libcompiler.a",
    "src/util/libxmlconfig.a",
    "src/util/libmesa_util.a",
    "src/util/libmesa_util_simd.a",
    "src/util/libmesa_util_clflush.a",
    "src/util/libmesa_util_clflushopt.a",
    "src/util/libparson.a",
    "src/util/blake3/libblake3.a",
    "src/c11/impl/libmesa_util_c11.a",
]


class CMakeCompatibilitySmokeTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cmake") and shutil.which("python3"), "CMake and Python 3 are required")
    def test_configure_generates_linker_eboot_and_mesa_link_contract(self) -> None:
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

            mesa_source = work / "mesa-source"
            (mesa_source / "include").mkdir(parents=True)
            vulkan_headers = work / "vulkan-headers"
            (vulkan_headers / "vulkan").mkdir(parents=True)
            (vulkan_headers / "vulkan" / "vulkan.h").write_text("/* fake Vulkan header */\n", encoding="utf-8")
            mesa_build = work / "mesa-build"
            for relative in MESA_ARCHIVES:
                archive = mesa_build / relative
                archive.parent.mkdir(parents=True, exist_ok=True)
                archive.write_bytes(b"fake archive")

            wsi_source = work / "ps4_vk_wsi_window.cpp"
            wsi_source.write_text("int fake_wsi_translation_unit() { return 0; }\n", encoding="utf-8")

            project = work / "project"
            project.mkdir()
            (project / "main.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
                "project(compat_smoke C CXX)\n"
                "set(PS4_MESA_BUILD \"" + mesa_build.as_posix() + "\" CACHE PATH \"\")\n"
                "set(PS4_MESA_SOURCE \"" + mesa_source.as_posix() + "\" CACHE PATH \"\")\n"
                "set(PS4_VULKAN_HEADERS \"" + vulkan_headers.as_posix() + "\" CACHE PATH \"\")\n"
                "set(PS4_KIT_MESA_WSI_SOURCE \"" + wsi_source.as_posix() + "\" CACHE FILEPATH \"\")\n"
                "include(\"" + str(COMPAT / "ps4-kit.cmake") + "\")\n"
                "add_executable(sample main.c)\n"
                "ps4_kit_app(sample)\n"
                "ps4_kit_link_mesa(sample)\n", encoding="utf-8")
            build = work / "build"
            result = subprocess.run(
                ["cmake", "-S", str(project), "-B", str(build), f"-DOPENORBIS={openorbis}"],
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
            link_line = link_files[0].read_text(encoding="utf-8")
            self.assertIn(str(generated), link_line)
            self.assertIn("--whole-archive", link_line)
            self.assertIn("--no-whole-archive", link_line)
            self.assertIn(str(mesa_build / MESA_ARCHIVES[0]), link_line)
            self.assertIn(str(wsi_source), link_line)

            help_result = subprocess.run(
                ["cmake", "--build", str(build), "--target", "help"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertEqual(help_result.returncode, 0, help_result.stdout)
            self.assertIn("sample_eboot", help_result.stdout)

            # The real tree has no WSI source yet; the kit must stop explicitly
            # rather than silently substituting a full-screen VideoOut WSI.
            wsi_source.unlink()
            missing_wsi = subprocess.run(
                ["cmake", "-S", str(project), "-B", str(work / "build-missing-wsi"),
                 f"-DOPENORBIS={openorbis}"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertNotEqual(missing_wsi.returncode, 0)
            self.assertIn("Wine window-mode Vulkan WSI is not reconstructed yet", missing_wsi.stdout)


if __name__ == "__main__":
    unittest.main()
