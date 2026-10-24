# 简体中文脚本准备

本分支完成中文接入的第一步：用日不落原有工具生成简体中文脚本，保留原译文与配置。引擎、字体、图片、音频、视频及其他语言均未改动。**产物还不是可安装的 Vita 中文补丁。**

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

## 本地生成

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
