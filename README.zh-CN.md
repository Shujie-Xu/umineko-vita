# 简体中文语言包准备

本分支准备日不落简体中文脚本与独立语言资源。采用 Vita 项目现有语言插件的目录布局和 0.5 图片缩放方式，保留原译文、配置与字体。引擎、其他语言、通用底座资源均未改动。**中文语言注册和真机验证留待第三步。**

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

工具只读取 PC 原料和第一步的脚本包，在新的输出目录中生成增量文件、来源说明、脚本许可证和 `SHA256SUMS`；字体保持逐字节不变。输出内的 `umineko/` 对应 `ux0:data/umineko/`。本阶段只准备数据，待中文语言接入后再启用。

游戏素材沿用原权利人的权利；脚本的 BSD 许可证不扩展到图片或字体。仓库只保存准备工具和说明，生成包与 PC 原料放在仓库外或已忽略的 `build/` 内。

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
