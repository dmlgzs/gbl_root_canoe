# GBL Root Canoe

[中文版](README_zh.md)

![Stone Badge](https://stone.professorlee.work/api/stone/superturtlee/gbl_root_canoe)

`gbl_root_canoe` is an EDK2-based workspace for patching the EFI applications within Qualcomm ABL (Android Bootloader) images. It leverages a GBL (Generic Bootloader Loader) vulnerability so the real ABL loads an embedded **superfastboot BDS** off the raw `efisp` partition. The BDS then scans a compatible partition (ext4/fat32) for boot entries and chains to the selected one - primarily to achieve a **Fake Locked Bootloader** state on Snapdragon 8 Gen 5 / 8 Elite (Gen 5) devices to bypass bootloader unlock detection.

`BDS.efi` is written raw to the `efisp` partition; the cracked ABL (`boot.efi`) and the boot entry list (`BOOTENTRIES`) live on the `persist` partition under its `efisp/` directory.

---

## Builder Guide

This section is for developers who want to compile the toolkits from source.

### Prerequisites
You must be on a **Linux** host to build the project:
- `gcc` / `clang`, `lld`, `make`, `zip`, `python3`
- `liblzma-dev` (for compiling `extractfv`)
- **Android NDK** (Required for `make target_magisk_module` to cross-compile tools for Android)
- **MinGW-w64**

### Build Targets

**Note:** You **do not** need to provide an `abl.img` to build the distributable toolkits or module.

- **`make target_toolkit_linux`**
  Builds the superfastboot BDS (`BDS.efi`) from the `uefi` submodule and compiles the patching utilities (`extractfv`, `patch_abl`) for Linux.

- **`make target_toolkit_windows`**
  Same as above, but cross-compiles the utilities into Windows `.exe` programs using MinGW-w64.

- **`make target_magisk_module`**
  Cross-compiles the patcher tools for Android using your NDK, builds the BDS, and packages everything as a KernelSU/Magisk module.

- **`make target_toolkit_android`**
  Produces a standalone Android arm64 toolkit (`toolkit_android.zip`) with Android-native binaries for on-device use outside of the module.

---

## User Guide

For more detailed instructions, please refer to the [Wiki](https://github.com/superturtlee/gbl_root_canoe/wiki).

### 1. Using the Module (On-Device)

The module is designed to run directly on your rooted Android device.

**Requirements:**
- Device must be Snapdragon 8 Gen 5 / 8 Elite (Gen 5).
- Bootloader must be unlocked.
- Kernel must NOT have Baseband Guard.
- The ABL on the `abl` partition must contain the GBL vulnerability. If it does not, flash an older ABL with the vulnerability first (the cracked `boot.efi` does not need to match the ABL on the `abl` partition).

**Installation & Usage:**
When flashing the module via a root manager (KernelSU, Magisk, or APatch), the script interacts with you using the volume keys:
- **Volume Up (First-time installation):** The script extracts the current-slot `.abl`, cracks it into `boot.efi`, places `boot.efi` / `LinuxLoader.efi` / `BOOTENTRIES` into `/mnt/vendor/persist/efisp/`, and flashes `BDS.efi` to `efisp`. After this, reboot into Recovery and **format Data**. Once booted, install this module again (Volume Down the second time) to complete the installation.
- **Volume Down (OTA retention or post-format):** Installs the OTA-update patch. After each OTA, open the module WebUI and flash again to retain the BL version.

### 2. Using the PC Toolkits (Linux / Windows)

If you downloaded the `target_toolkit_linux` or `target_toolkit_windows` zip files:
1. Extract the toolkit zip on your PC.
2. Place your device's stock `abl.img` inside the `images/` (or `images\`) directory of the toolkit.
3. **Linux:** Run `bash build.sh`. **Windows:** Run `build.bat`.
4. The script extracts and cracks the ABL, outputting `ABL.efi` (fake re-lock) and `ABL_original.efi` (original). `BDS.efi` is bundled. Check `patch_log.txt` - if it says "Warning: Failed to patch ABL GBL", the ABL lacks the vulnerability and the `abl` partition must be downgraded to an older ABL with it.

Then complete the install manually (see the [Wiki](https://github.com/superturtlee/gbl_root_canoe/wiki) for full steps): copy `ABL.efi` into `/mnt/vendor/persist/efisp/`, create `BOOTENTRIES`, `sync`, and flash `BDS.efi` to `efisp` (`dd if=BDS.efi of=/dev/block/by-name/efisp bs=4M`).

### 3. OTA Upgrade
Before rebooting for an OTA update, use the module WebUI to flash and retain the old ABL version. "Update efisp" is enabled by default; for a major version upgrade keep it on, otherwise the device may get stuck on the first boot screen.

### 4. Superfastboot Usage Instructions
The module WebUI's **Superfastboot Password Protection** sets a **6–64 digit password**, confirmed twice, effective on the next boot. Verification happens before entry to the entire interactive Superfastboot environment, including the boot menu, other operating systems, file browser, USB exports and Fastboot. `/mnt/vendor/persist/efisp/menu_password` sits beside the patched `boot.efi` and stores only a random 16-byte salt and SHA-256 digest with mode `0600`. Plaintext passwords are never written to files or module logs. Module/OTA updates preserve this file.

**No password disables password protection and preserves the existing saved-default behavior; setting a password enables it.** With password protection enabled, unattended boot launches the internal persist partition's `efisp/boot.efi` without a password. **Volume Up** requests verification before the boot menu. Use volume keys to choose **0–9, delete one digit, confirm password, or boot Android**, and power to confirm. Every nonempty incorrect confirmation counts as an error, including inputs shorter than six digits. Three failures accumulated in one boot, empty confirmation, invalid credentials, cancellation, or 30 seconds of input inactivity take the Android-only path. Clearing the password disables password protection on the next boot. Password I/O failures are not treated as an absent password.

With password protection enabled, saved defaults cannot boot another OS before verification and `DRIVER.LIST` is not loaded. The boot menu, EFI/driver loading, volume browser, USB exports, Fastboot USB startup and every BDS Fastboot command dispatch check authorization. A failed/returned launch follows the original menu path, with verification required before entry. If verification is unavailable and Android cannot launch, the loader follows the original cleanup and return to the calling firmware, without an added infinite loop.

This password protection controls this project's BDS, not the vendor ABL's independent Fastboot/Recovery, other firmware, or Root modifications to persist. The existing ABL patch still enables vendor Fastboot. A six-digit digest is susceptible to offline enumeration if the file is read; longer passwords increase that cost.

After verification, use **Advanced → Enter Fastboot** to enter Superfastboot.
Common commands include:
- **Temp-boot an EFI file (without flashing)**: `fastboot boot xxx.efi`
- **Lock and Unlock (BL related)**:
  - Lock BL, triggers a data wipe: `fastboot flashing lock`
  - Unlock BL, no data wipe: `fastboot flashing unlock` or `fastboot flashing unlock_critical`
  - *Note: If the TEE status is inconsistent, the device will refuse to provide the data key, rendering data inaccessible.*
- **Flashing and Erasing**:
  - `fastboot flash <partition> <file.img>`
  - `fastboot erase <partition>`
- **Rebooting**:
  - `fastboot reboot bootloader` (Next normal boot enters Official Fastboot)
  - `fastboot reboot recovery`
  - `fastboot reboot`

#### Custom authentication text

Use the module WebUI's custom authentication text field to save up to 80 characters below `Superfastboot Authentication`, effective next boot. Save an empty field to clear. The single-line UTF-8 file is `/mnt/vendor/persist/efisp/auth_message`, separate from credentials and preserved during module updates. Editing text does not change the password.

English and common Chinese text can be decoded; emoji, line breaks and control characters are unsupported. Chinese rendering depends on firmware fonts. This BDS bundles no Chinese font, so glyph support is not guaranteed. If the console reports unsupported glyphs, the original text is still rendered with firmware placeholders, and an English notice appears below it without affecting authentication.

### 5. File Reference
1. `BDS.efi`: The superfastboot BDS, flashed raw to the `efisp` partition.
2. `boot.efi` / `ABL.efi`: The cracked ABL with fake re-lock (the module names it `boot.efi`; the toolkit names it `ABL.efi`), placed on `persist` under `efisp/`.
3. `LinuxLoader.efi` / `ABL_original.efi`: The original unpatched ABL. For analysis; do not flash to `efisp`.
4. `BOOTENTRIES`: Boot entry list, format `<name>:<path relative to efisp/>`.
