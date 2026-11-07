# Maple 桥接 DLL 构建

在 Visual Studio 2022 C++ 与 Windows 10 SDK 环境中，从干净且已提交的源码运行：

```powershell
./tools/build-agent-bridge.ps1
```

脚本构建解决方案的 `Release x86`（对应项目 `Release|Win32`），以命令行选择 `v143`。原项目的 `v142` 默认值保持不变，仓库自带的 x86 `detours/detours.lib` 直接参与链接，不下载替代版本。

上游文件混用 UTF-8 BOM 和无 BOM 的 GBK。编译器按 BOM 识别 Unicode 文件，其余源文件按 CP936 读取；窄字符串按 CP936 写入 DLL，保留客户端中文文本。新增含中文的 UTF-8 源文件应带 BOM，不能全局改成 `/utf-8`。

GitHub Actions 工作流 `agent-bridge.yml` 支持手动运行，以及 `main`、`feat/**` 分支相关源码推送和 PR。使用 `windows-2022` 的 VS2022 工具链；工件名为 `maple-agent-bridge-win32`。

输出目录 `out/agent-bridge/` 包含 DLL、可用的 PDB、许可证、普通构建日志、导出表、运行库依赖与 `manifest.json`。清单记录源码 commit/tree、目标 EXE 和 `2ijl15.dll` 固定 SHA256、本次 DLL SHA256、实际 MSVC/SDK 与 runner 身份。工件不包含原版客户端、WZ、配置或账号数据。

构建会核对 PE 文件为 x86 DLL，并保留原图片库的六个导出函数。这些检查证明产物结构与来源；游戏内桥接、中文显示、原 DLL 转发和生命周期行为仍需使用匹配客户端单独验收。
