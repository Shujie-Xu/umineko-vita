# 简体中文语言包准备

本分支将日不落简体中文接到 Vita 项目已有的语言机制。中文脚本与资源使用现有语言插件布局，译文、配置、字体和通用底座资源保持原样。**已实现中文识别、语言切换及独立存档/缓存，并补齐完整 VPK 构建；中文真机验证尚未完成。**

## 固定来源

- 脚本：[Umineko-Project-CN/umineko-scripting](https://github.com/Umineko-Project-CN/umineko-scripting/tree/162a77eb764f181236b8044d1f75a93458ea4937)，提交 `162a77eb764f181236b8044d1f75a93458ea4937`（`cn_staging`）。
- 内容：简体中文 `cn`，EP1–EP8 和 omake。章节参数使用上游 `current/last_cn` 对应的 `8`。
- 编码核验：[Vita v1.4 的脚本读取实现](https://github.com/stoicpingu/umineko-vita/blob/6ce2284e3457e4e51eba90511a6bc1cfd4da0bd7/upstream/onscripter-ru/Engine/Handlers/Script.cpp)。

## 下载产物

在 [Actions 构建记录](https://github.com/Shujie-Xu/umineko-vita/actions/workflows/chinese-script.yml)中选择成功运行，下载 `umineko-cn-script`：

| 文件 | 用途 |
| --- | --- |
| `cn.file` | 使用上游格式编码的简体中文脚本 |
| `cn.txt` | 生成的可读脚本，便于后续核对兼容性 |
| `cn.cfg` | 日不落原有的中文语言目录和字体目录配置 |
| `LICENSE.txt` | 脚本源码的原始 BSD-3-Clause 许可证 |
| `SOURCE.txt` | 固定来源、生成命令和 PHP 版本 |
| `SHA256SUMS` | 文件校验值 |

工作流检查中文标识、八章起止标签和未展开的语言导入指令，再按 Vita 引擎的解码表还原 `cn.file`，逐字节核对编码前的脚本。通过这些检查不代表 Vita 运行兼容性已验证。

## 语言资源增量包

与上游 [PT-BR 语言包](https://drive.google.com/file/d/1iQNFmuhpnWUwzsGJfr58sutvoaR72x6j/view)使用同样的松散文件布局：

```text
umineko/
├── cn.file
├── cn.cfg
└── language_cn/
    ├── fonts_cn/
    ├── graphics/
    ├── native-image-sizes.tsv
    └── render_scale.txt
```

中文专用字体目录直接使用日不落原 `cn.cfg` 的 `font-dir=fonts_cn` 机制。图片按原宽高的一半取整，保留透明度；尺寸表记录原 PC 尺寸，`render_scale.txt` 为 `0.5000000`。不另加加载器、全局配置或安装脚本。

原料使用[日不落官方 PC 完整包](https://snsteam.club/download/port)，先覆盖官网的 `resource.zip` 资源更新。仅整理 `language_cn` 的 PNG 和十个引擎字体槽；背景、立绘、语音、视频继续使用现有 Vita 底座与 Hotfix v2。不包含歌曲字幕、字幕专用字体或预生成存档缓存。

使用 Python 3.11 或更新版本、Pillow 和 fontTools，在仓库根目录运行：

```sh
python tools/prepare_cn_assets.py /path/to/updated-pc-game /path/to/umineko-cn-script build/cn-addon
```

工具只读取 PC 原料和第一步的脚本包，在新的输出目录中生成增量文件、来源说明、脚本许可证和 `SHA256SUMS`；字体保持逐字节不变。输出内的 `umineko/` 对应 `ux0:data/umineko/`。启用中文需要包含本分支语言接入改动的引擎；上游 v1.4 VPK 不会自动识别中文。

游戏素材沿用原权利人的权利；脚本的 BSD 许可证不扩展到图片或字体。仓库只保存准备工具和说明，生成包与 PC 原料放在仓库外或已忽略的 `build/` 内。

## 语言切换、存档与缓存

中文完整性检查沿用日文插件的方式：`cn.file` 是有效脚本，`cn.cfg` 存在，且 `language_cn/fonts_cn/default.ttf` 存在。只有一个语言插件、且没有原生偏好中保存的语言选择时，自动选中该插件；多个插件时沿用英文默认值。已保存的选择优先。

设置菜单中的顺序为 `English / JP / PT-BR / Español / ZH-CN`，只列出已安装的语言。选择语言后重启应用生效，与现有语言一致。`ZH-CN` 使用 ASCII 标签，可在英文和其他语言的字体下正常显示。

| 语言 | 存档与缓存根目录 |
| --- | --- |
| 英文 | `ux0:data/umineko/save/` |
| 日文 | `ux0:data/umineko/save/jp/` |
| 葡语 | `ux0:data/umineko/save/pt/` |
| 西语 | `ux0:data/umineko/save/es/` |
| 简体中文 | `ux0:data/umineko/save/cn/` |

中文的已读记录、进度与渲染缓存走同一个原有保存路径机制，其中缓存位于 `save/cn/caches/`。不迁移或混用其他语言的存档。

日不落脚本通过 `language japanese` 使用 CJK 排版。菜单适配依据脚本是否定义歌曲字幕菜单判断布局，不再把 CJK 排版声明等同于日文菜单。适配只在内存中进行，保留原脚本字节长度、换行和标签位置；未改译文或脚本文件。

## 构建与真机测试

需要 Python 3.11+、CMake 3.20+、Make、curl 和主机 C 编译器，支持 Apple Silicon macOS 与 x86_64 Linux。在仓库根目录执行：

```sh
python3 tools/build_vita.py
```

构建工具会校验固定官方 VitaSDK softfp 编译器和依赖包的 SHA-256，将 SDK、下载与依赖源码保存在已忽略的 `build/` 中。若设置了 `VITASDK`，则使用该目录并向其中安装固定依赖包。FFmpeg 6.0、libpng、SDL_mixer 和与现有适配代码匹配的 vitaGL 从固定源码构建；SDL_mixer 只应用上游原有的两处离线解码锁调整。上游资源编译器按原提交恢复，继续使用原生 CMake/SELF/VPK 打包流程。

产物为 `native-build/umineko-native.vpk`，也可从 [VPK Actions 构建记录](https://github.com/Shujie-Xu/umineko-vita/actions/workflows/vita.yml)下载 `umineko-cn-vpk`。23 个 GXP 着色器保留上游原始二进制，针对可复现的旧版 vitaGL 重建依赖指纹；构建时检查完整 GLES 源码对应关系、文件 SHA-256、头部和 CRC，未启用真机动态着色器导出。

1. 按[上游安装说明](https://github.com/stoicpingu/umineko-vita#setup-instructions-for-end-users)准备 Vita 底座和 Hotfix v2。
2. 将中文增量包内 `umineko/` 的内容合并到 `ux0:data/umineko/`。
3. 用 VitaShell 安装本分支 VPK，应用 ID 继续使用 `UMNK00001`，可覆盖原应用。
4. 启动后在语言菜单选择 `ZH-CN` 并重启。只有中文插件且没有已保存语言偏好时会自动选中中文。

真机首轮检查标题、开篇中文与语音、设置中语言切换，以及中文保存/读取后重启恢复。中文数据位于 `save/cn/`，其他语言仍使用各自原有目录。编译与打包通过不代表已经完成真机运行验证。

## 主机回归验证

主机回归测试编译生产代码中的语言识别、脚本列表、保存路径及菜单适配逻辑，配合实际 ONS2 脚本检查 16 种插件组合，并用 AddressSanitizer/UndefinedBehaviorSanitizer 检查内存访问。原有语言在未安装中文时，除恢复语言选择的初始化区外，其余菜单输出与 v1.4 逐字节比较。初始化区通过原有 `getscriptpath` 遍历脚本列表恢复选择，避免新增语言后超出原脚本固定行槽。

```sh
python tools/tests/test_native_languages.py /path/to/cn.file /path/to/en.file /path/to/jp.file /path/to/pt.file /path/to/es.file
```

Actions 会先生成固定版本的中文脚本，再执行同一测试的中文用例。以上是主机代码检查，不代表已在 Vita 上运行。

## 本地生成脚本

需要 Git 和 PHP CLI。在仓库根目录执行：

```sh
mkdir -p build
git clone https://github.com/Umineko-Project-CN/umineko-scripting.git build/cn-source
git -C build/cn-source checkout 162a77eb764f181236b8044d1f75a93458ea4937
mkdir -p build/cn-script
php build/cn-source/update-manager/update-manager.php dscript build/cn-script/cn.txt build/cn-source cn
php build/cn-source/update-manager/update-manager.php script build/cn-script/cn.txt build/cn-script/cn.file 8
cp build/cn-source/misc/cn.cfg build/cn-source/LICENSE.txt build/cn-script/
```

生成器会写入构建时间，同一源码重复构建的文件校验值可能不同。`build/` 已由上游忽略，无需把生成文件或整份中文源码重复提交到本仓库。

感谢日不落汉化组与 Umineko Project。分发脚本时须随附原始 `LICENSE.txt`；原仓库的其他许可证保持不变。
