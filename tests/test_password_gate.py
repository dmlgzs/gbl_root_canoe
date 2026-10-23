"""Run on Windows (MinGW + Git Bash) or Linux (gcc + bash), without a device.

python tests/test_password_gate.py
"""
import ctypes
import hashlib
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EDK = ROOT / "submodules/uefi/edk2"
APP = EDK / "QcomModulePkg/Application/LinuxLoader"
SCRIPT = ROOT / "targets/magisk_module/module/bin/bl_flasher.sh"


def run(*args, **kwargs):
    result = subprocess.run(args, text=True, encoding="utf-8", errors="replace", capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    cc = shutil.which("gcc")
    bash = shutil.which("bash")
    if os.name == "nt":
        git_bash = Path("C:/Program Files/Git/bin/bash.exe")
        if git_bash.exists():
            bash = str(git_bash)
    if not cc or not bash:
        raise RuntimeError("gcc and bash are required")
    includes = ["MdePkg/Include", "MdePkg/Include/X64", "QcomModulePkg/Include",
                "EmbeddedPkg/Include", "ArmPkg/Include"]
    flags = ["-fshort-wchar", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-unused-parameter"] + [f"-I{EDK / item}" for item in includes]
    with tempfile.TemporaryDirectory(prefix="canoe-password-") as directory:
        tmp = Path(directory)
        library = tmp / ("password.dll" if os.name == "nt" else "password.so")
        run(cc, *flags, "-shared", "-fPIC", str(APP / "SuperFbPassword.c"), "-o", str(library))
        lib = ctypes.CDLL(str(library))
        digest_type = ctypes.c_uint8 * 32
        lib.SfbPasswordHash.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t,
                                      ctypes.POINTER(ctypes.c_uint8)]
        lib.SfbPasswordHash.restype = ctypes.c_uint8
        lib.SfbPasswordParse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p,
                                       ctypes.POINTER(ctypes.c_uint8)]
        lib.SfbPasswordParse.restype = ctypes.c_uint8
        rng = random.Random(192)
        count = 0
        for length in range(6, 65):
            for _ in range(5):
                salt = bytes(rng.choice(b"0123456789abcdef") for _ in range(32))
                pin = bytes(rng.choice(b"0123456789") for _ in range(length))
                actual = digest_type()
                assert lib.SfbPasswordHash(salt, pin, len(pin), actual)
                expected = hashlib.sha256(salt + pin).digest()
                assert bytes(actual) == expected, length
                record = b"SFBPW1:" + salt + b":" + expected.hex().encode()
                for suffix in (b"", b"\n"):
                    decoded_salt = ctypes.create_string_buffer(32)
                    decoded_digest = digest_type()
                    value = record + suffix
                    assert lib.SfbPasswordParse(value, len(value), decoded_salt, decoded_digest)
                    assert decoded_salt.raw == salt and bytes(decoded_digest) == expected
                count += 1
        for pin in (b"", b"12345", b"1" * 65, b"12345a", b"12345\n"):
            assert not lib.SfbPasswordHash(b"a" * 32, pin, len(pin), digest_type())
        valid = b"SFBPW1:" + b"a" * 32 + b":" + b"0" * 64
        for bad in (b"123456", valid[:-1], valid + b"x", valid + b"\r\n",
                    b"X" + valid[1:], valid[:39] + b"x" + valid[40:],
                    valid[:7] + b"G" + valid[8:], valid[:40] + b"G" + valid[41:]):
            assert not lib.SfbPasswordParse(bad, len(bad), ctypes.create_string_buffer(32), digest_type())
        print(f"SHA-256 cross-check with hashlib: {count} salted passwords, 6-64 digits; malformed records rejected")
        # Release before later assertions can raise during temporary cleanup.
        if os.name == "nt":
            import _ctypes
            _ctypes.FreeLibrary(lib._handle)

        harness = tmp / ("gate.exe" if os.name == "nt" else "gate")
        run(cc, *flags, str(ROOT / "tests/password_gate_harness.c"),
            str(APP / "SuperFbPassword.c"), "-o", str(harness))
        print(run(str(harness)).strip())

        # LinuxLoader includes its generated header; none of its used symbols
        # require generated values in these host-side control-flow tests.
        (tmp / "AutoGen.h").write_bytes(b"#include <Uefi.h>\n")
        loader_harness = tmp / ("loader.exe" if os.name == "nt" else "loader")
        run(cc, *flags, "-Wno-attributes", "-DMDEPKG_NDEBUG", f"-I{tmp}",
            f"-I{EDK / 'QcomModulePkg/Library'}",
            f"-I{EDK / 'QcomModulePkg/Include/Library'}",
            str(ROOT / "tests/loader_return_harness.c"), "-o", str(loader_harness))
        print(run(str(loader_harness)).strip())

        menu_source = (APP / "SuperFbMenu.c").read_text(encoding="utf-8")
        key_function = menu_source.split("SFB_KEY\nSfbWaitForKey", 1)[1].split("/* ---- drawing", 1)[0]
        key_header = '#include "SuperFbMenu.h"\n#include <Library/DebugLib.h>\n#include <Library/UefiBootServicesTableLib.h>\n#include <Protocol/SimpleTextIn.h>\nSFB_KEY\nSfbWaitForKey' + key_function
        (tmp / "SfbWaitForKeyUnderTest.h").write_bytes(key_header.encode())
        key_harness = tmp / ("key.exe" if os.name == "nt" else "key")
        run(cc, *flags, "-DMDEPKG_NDEBUG", f"-I{tmp}", f"-I{APP}",
            str(ROOT / "tests/key_timeout_harness.c"), "-o", str(key_harness))
        print(run(str(key_harness)).strip())

        # Exercise the real module actions in a fixture, with only the mount
        # probe/path redirected. No real phone or block device is touched.
        module = tmp / "module"
        persist = tmp / "persist"
        boot_root = persist / "efisp"
        module.mkdir()
        boot_root.mkdir(parents=True)
        (boot_root / "boot.efi").write_bytes(b"test Android loader")
        shell_file = tmp / "bl_flasher.sh"
        source = SCRIPT.read_text(encoding="utf-8")
        source = source.replace('PERSIST_MNT="/mnt/vendor/persist"', f'PERSIST_MNT="{persist.as_posix()}"')
        source = source.replace('persist_mounted() { grep -q " $PERSIST_MNT " /proc/mounts; }',
                                'persist_mounted() { return 0; }')
        shell_file.write_bytes(source.encode("utf-8"))
        shell_path = shell_file.as_posix()
        env = dict(os.environ, MODDIR=module.as_posix())
        password_file = boot_root / "menu_password"
        for pin in ("001234", "123456789", "0123456789012345" * 4):
            assert "PASSWORD_SAVED=1" in run(bash, shell_path, "set-password", pin, env=env)
            value = password_file.read_bytes()
            tag, salt, digest = value.strip().split(b":")
            assert tag == b"SFBPW1" and len(salt) == 32
            assert digest == hashlib.sha256(salt + pin.encode()).hexdigest().encode()
            assert pin.encode() not in value
            assert "PASSWORD_STATE=configured" in run(bash, shell_path, "status", env=env)
            logs = b"".join(p.read_bytes() for p in (module / "tmp").iterdir() if p.is_file())
            assert pin.encode() not in logs
        before = password_file.read_bytes()
        for pin in ("", "12345", "1" * 65, "12345a"):
            output = subprocess.run([bash, shell_path, "set-password", pin],
                                    text=True, encoding="utf-8", capture_output=True, env=env)
            assert output.returncode != 0 and "PASSWORD_ERROR=invalid" in output.stdout, (repr(pin), output.returncode, output.stdout, output.stderr)
            assert password_file.read_bytes() == before
        # Windows/MSYS strips newlines from native argv. Construct this one
        # argument inside Bash so the production validator actually sees it.
        newline_driver = tmp / "newline-test.sh"
        newline_driver.write_bytes(("set -- set-password '123456\n'\n. '" + shell_path + "'\n").encode())
        output = subprocess.run([bash, newline_driver.as_posix()], text=True,
                                encoding="utf-8", capture_output=True, env=env)
        assert output.returncode != 0 and "PASSWORD_ERROR=invalid" in output.stdout
        assert password_file.read_bytes() == before
        lock = module / "tmp/flash.lock"
        lock.mkdir()
        output = subprocess.run([bash, shell_path, "set-password", "123456"],
                                text=True, encoding="utf-8", capture_output=True, env=env)
        assert "PASSWORD_ERROR=busy" in output.stdout and password_file.read_bytes() == before
        lock.rmdir()
        assert "PASSWORD_CLEARED=1" in run(bash, shell_path, "clear-password", env=env)
        assert not password_file.exists()
        assert "PASSWORD_STATE=missing" in run(bash, shell_path, "status", env=env)
        password_file.write_bytes(b"123456")
        assert "PASSWORD_STATE=invalid" in run(bash, shell_path, "status", env=env)
        message_file = boot_root / "auth_message"
        for message in ("Owner: test@example.com", "Chinese: \u4f60\u597d", "\u4e2d" * 100, "literal $() `cmd` ' %s"):
            assert "AUTH_MESSAGE_SAVED=1" in run(bash, shell_path, "set-auth-message", message, env=env)
            assert message_file.read_bytes() == message.encode("utf-8")
            assert run(bash, shell_path, "get-auth-message", env=env) == message
        old_message = message_file.read_bytes()
        for message in ("x" * 101, "bad\ttext", "\u4e2d" * 101):
            output = subprocess.run([bash, shell_path, "set-auth-message", message], text=True,
                                    encoding="utf-8", capture_output=True, env=env)
            assert output.returncode != 0 and "AUTH_MESSAGE_ERROR=invalid" in output.stdout
            assert message_file.read_bytes() == old_message
        assert "AUTH_MESSAGE_SAVED=1" in run(bash, shell_path, "set-auth-message", "", env=env)
        assert not message_file.exists()
        assert password_file.read_bytes() == b"123456"  # Text editing is independent of credentials.
        print("Custom text: UTF-8 round trip, size/control validation, literal shell characters and clearing passed")
        print("Module actions: salted storage, leading zeros, variable lengths, validation, flash lock, status, clear and no plaintext logs passed")


if __name__ == "__main__":
    main()
