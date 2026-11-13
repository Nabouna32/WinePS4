from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "ps4-kit" / "mesa" / "archives.cmake"


class MesaArchiveManifestTests(unittest.TestCase):
    def test_resolves_complete_candidate_closure_and_fails_on_missing_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mesa_build = root / "mesa"
            cmake_src = root / "cmake-src"
            cmake_src.mkdir()
            (cmake_src / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(mesa_manifest NONE)\n"
                f'set(PS4_MESA_BUILD "{mesa_build.as_posix()}")\n'
                f'include("{MANIFEST.as_posix()}")\n'
                "ps4_kit_resolve_mesa_archives(archives)\n"
                "list(LENGTH archives archive_count)\n"
                "if(NOT archive_count EQUAL 16)\n"
                "  message(FATAL_ERROR \"unexpected archive count: ${archive_count}\")\n"
                "endif()\n"
            )

            relatives = [
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
            for relative in relatives:
                archive = mesa_build / relative
                archive.parent.mkdir(parents=True, exist_ok=True)
                archive.write_bytes(b"fake archive")

            configure = subprocess.run(
                ["cmake", "-S", str(cmake_src), "-B", str(root / "build-ok")],
                text=True,
                capture_output=True,
            )
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)

            (mesa_build / relatives[0]).unlink()
            missing = subprocess.run(
                ["cmake", "-S", str(cmake_src), "-B", str(root / "build-missing")],
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("libvulkan_radeon.a", missing.stdout + missing.stderr)


if __name__ == "__main__":
    unittest.main()
