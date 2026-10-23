# GBL Root Canoe

[English](README.md)

> ⚠️ **本项目已归档。** 当前版本为最终版本——补丁引擎已在多个厂商、多个 ABL 版本上稳定运行，核心逻辑不再变动，因此停止主动维护。代码仍然可用，欢迎 Fork。详见 [ARCHIVE.md](ARCHIVE.md)。

`gbl_root_canoe` 是一个基于 EDK2 的工作区，用于修补高通 ABL 内的 EFI 程序。它利用 GBL (Generic Bootloader Loader) 漏洞，让真实 ABL 从原始 `efisp` 分区加载内嵌的 **superfastboot BDS**，BDS 再扫描兼容分区（ext4/fat32）获取启动项并链式启动——主要目的是在骁龙 8 Gen 5 / 8 Elite (Gen 5) 设备上实现**假回锁**（绕过 Bootloader 的解锁状态检测）。

`BDS.efi` 以原始方式刷入 `efisp` 分区；破解后的 ABL（`boot.efi`）和启动项列表（`BOOTENTRIES`）存放在 `persist` 分区的 `efisp/` 目录下。

---

## 开发者构建指南 (Builder Guide)

本节适用于希望从源码编译工具包的开发者。

### 编译依赖
构建各种发布包必须在 **Linux** 环境下进行：
- `gcc` / `clang`, `lld`, `make`, `zip`, `python3`
- `liblzma-dev` (用于编译 `extractfv` 解包工具)
- **Android NDK**（用于 `make target_magisk_module` 交叉编译 Android 平台的修补工具）
- **MinGW-w64**

### 核心构建目标

**注意**：在仅编译工具包或模块时，**不需要**事先提供 `abl.img`。

- **`make target_toolkit_linux`**
  从 `uefi` 子模块构建 superfastboot BDS（`BDS.efi`），并将修补工具（`extractfv`, `patch_abl`）编译为 Linux 原生程序。

- **`make target_toolkit_windows`**
  逻辑与 `target_toolkit_linux` 相同，但使用 MinGW-w64 将修补工具交叉编译为 Windows 原生的 `.exe` 格式文件。

- **`make target_magisk_module`**
  使用 NDK 将修补工具交叉编译至 Android 原生平台架构，构建 BDS，并封装为一个标准的 KernelSU/Magisk 模块。

- **`make target_toolkit_android`**
  构建独立的 Android arm64 工具包（`toolkit_android.zip`），包含 Android 原生二进制工具，可在设备上脱离模块独立使用。

---

## 普通用户使用指南 (User Guide)

更详细的使用说明请参考 [Wiki](https://github.com/superturtlee/gbl_root_canoe/wiki)。

### 1. 使用模块版本（手机端热修补）

模块可直接通过 Root 管理器在有 Root 权限的手机上刷入运行。

**设备要求：**
- 必须是骁龙 8 Gen 5 / 8 Elite (Gen 5) 芯片设备。
- 设备 BL 锁已经解锁。
- 内核**没有** Baseband Guard 拦截。
- `abl` 分区上的 ABL 必须包含 GBL 漏洞。若没有，请先刷写一个带有该漏洞的旧版本 ABL（破解后的 `boot.efi` 不必与 `abl` 分区上的 ABL 版本一致）。

**刷入及使用流程：**
在使用 Root 管理器（KernelSU/Magisk/APatch）刷入该压缩包时，脚本会通过音量键与您交互：
- **按音量上键 (首次全新安装)：** 脚本会提取当前槽位的 `.abl`，破解为 `boot.efi`，将 `boot.efi` / `LinuxLoader.efi` / `BOOTENTRIES` 放入 `/mnt/vendor/persist/efisp/`，并将 `BDS.efi` 刷入 `efisp`。完成后，请重启手机进入 Recovery 模式**格式化 Data**。开机后，请再次刷入本模块（第二次刷入时按音量下键）以走完完整安装流程。
- **按音量下键 (非首次安装的日常 OTA 保留)：** 安装 OTA 更新补丁。每次 OTA 后，打开模块 WebUI 重新刷写以保留 BL 版本。

### 2. 使用 PC 工具包 (Linux / Windows)

如果你下载的是 `target_toolkit_linux` 或 `target_toolkit_windows` 的发布压缩包：
1. 请先解压该 zip 并进入套件文件夹。
2. 提取出你所用机型的官方 `abl.img`，并将其直接拷贝至套件中的 `images/`（或 `images\`）目录下。
3. **Linux 平台：** 开启终端执行 `bash build.sh`。**Windows 平台：** 双击运行 `build.bat`。
4. 脚本会提取并破解 ABL，输出 `ABL.efi`（假回锁）和 `ABL_original.efi`（原版），`BDS.efi` 已附带。请查看 `patch_log.txt`，若显示 "Warning: Failed to patch ABL GBL"，则该 ABL 没有漏洞，需将 `abl` 分区降级为带有 GBL 漏洞的旧版本 ABL。

随后手动完成安装（完整步骤见 [Wiki](https://github.com/superturtlee/gbl_root_canoe/wiki)）：将 `ABL.efi` 复制到 `/mnt/vendor/persist/efisp/`，创建 `BOOTENTRIES`，`sync`，再将 `BDS.efi` 刷入 `efisp`（`dd if=BDS.efi of=/dev/block/by-name/efisp bs=4M`）。

### 3. OTA 升级
重启进行 OTA 更新前，使用模块 WebUI 刷写以保留旧版本 ABL。“更新 efisp”默认开启；跨版本升级时请保持开启，否则可能卡一屏。

### 4. superfastboot 使用方法
模块 WebUI 的“Superfastboot 密码保护”可设置 **6–64 位数字密码**，两次输入一致后保存，下次开机生效。验证位于进入整个 Superfastboot 交互环境之前，覆盖启动菜单、其他系统启动、文件浏览、USB 导出及 Fastboot。`/mnt/vendor/persist/efisp/menu_password` 与修补后的 `boot.efi` 同目录，只保存 16 字节随机盐和 SHA-256 校验值，权限 `0600`，不会将明文密码写入文件或模块日志。模块更新和 OTA 刷写保留此文件。

**未设置密码时密码保护关闭，高级操作无需验证，并保留原有默认启动项行为；设置密码后启用密码保护。** 密码保护启用时，正常开机直接启动内置 persist 分区的 `efisp/boot.efi`，无需输入密码。开机按 **音量上** 请求启动菜单时，先验证密码：音量键选择 **0–9、删除一位、确认密码或启动安卓**，电源键确认。非空错误输入均计错，包括不足 6 位的输入；本次开机累计三次失败、空密码确认、文件无效、取消或输入空闲 30 秒，均只启动安卓。清除密码会在下次开机关闭密码保护。无法读取密码文件的 I/O 错误不视为“未设置”。

密码保护启用且未验证时，不会根据保存的默认启动项启动其他系统，也不会加载 `DRIVER.LIST`；启动菜单、EFI/驱动加载、文件浏览、USB 导出及 BDS Fastboot 的 USB 启动和全部指令分发均检查授权。启动程序失败或返回后，沿用原版进入菜单的流程，进入前仍须通过密码验证；无法验证且安卓也无法启动时，沿用原版清理并返回上层固件的处理，不新增无限循环。

此密码保护仅控制本项目 BDS；厂商 ABL 的独立 Fastboot/Recovery、其他固件和 Root 修改 persist 不受其控制。已有 ABL 补丁仍包含启用厂商 Fastboot 的逻辑。六位数字密码的散列不能抵御读取文件后的离线枚举，较长密码可提高这一成本。

验证通过后，在 **Advanced → Enter Fastboot** 进入 Superfastboot。常用命令包括：
- **临时启动 EFI 文件（无需刷入）**：`fastboot boot xxx.efi`
- **锁定与解锁 (BL 锁相关)**：
  - 锁定 BL，触发数据清除：`fastboot flashing lock`
  - 解锁 BL，不触发数据清除：`fastboot flashing unlock` 或 `fastboot flashing unlock_critical`
  - 注意：如果遇到 TEE 状态不一致的情况，设备会拒绝下发 data key 导致数据无法访问。
- **刷写与擦除**：
  - `fastboot flash <partition> <file.img>`
  - `fastboot erase <partition>`
- **重启设备**：
  - `fastboot reboot bootloader` （下一次正常启动进入官方 Fastboot）
  - `fastboot reboot recovery`
  - `fastboot reboot`

#### 密码页自定义文字

在模块 WebUI 的“密码页自定义文字”中保存最多 80 个字符，下次开机会显示在 `Superfastboot Authentication` 标题下。留空保存可清除。配置以单行 UTF-8 存放在 `/mnt/vendor/persist/efisp/auth_message`，与密码文件分开，修改文字不会修改密码，模块更新会保留它。

支持英文和常见中文的编码，不支持表情符号、换行和控制字符。中文显示取决于设备固件字库，本 BDS 没有内置中文字库，不能保证所有设备能显示汉字；检测到缺字时仍显示原文字（缺字由固件显示为方块等占位符），并在下方提示改用英文，不影响密码验证。英文兼容性更好。

### 5. 文件说明
1. `BDS.efi`：superfastboot BDS，以原始方式刷入 `efisp` 分区。
2. `boot.efi` / `ABL.efi`：带假回锁的破解 ABL（模块中名为 `boot.efi`；toolkit 中名为 `ABL.efi`），存放在 `persist` 的 `efisp/` 下。
3. `LinuxLoader.efi` / `ABL_original.efi`：原始未破解 ABL。用于分析，**不要刷入 `efisp`**。
4. `BOOTENTRIES`：启动项列表，格式 `<名称>:<相对 efisp/ 的路径>`。
