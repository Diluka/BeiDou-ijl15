# Lua 5.4.9

本目录使用 Lua 官方 **5.4.9** 稳定发布包（2026-08-10）。来源与归档 SHA256 见 [source.json](source.json)，版权与 MIT 许可原文见 [LICENSE](LICENSE)。Lua 保留自己的 MIT 许可，外层 ijl15 项目的 AGPL 许可不替换 Lua 的许可。

来源：<https://www.lua.org/ftp/lua-5.4.9.tar.gz>

SHA256：`2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`

`src/`、`README` 和 `doc/readme.html` 从校验通过的官方归档原样复制，没有修改 Lua 源码。`src/lua.c`、`src/luac.c` 为上游命令行程序入口，保留作来源材料，但明确排除在 DLL 的编译项之外。

VS 工程把其余 32 个库 `.c` 文件直接编译进 `ijl15.dll`，使用 C 编译（`CompileAsC`）并关闭该批文件的预编译头（`PrecompiledHeader=NotUsing`）。原项目其他 C++ 文件保持自己的 PCH。Lua 与宿主继承同一运行库设置，Release Win32 使用 `/MD`；C++ 宿主通过 `lua.hpp` 获得 `extern "C"` 声明。

不定义 `LUA_BUILD_AS_DLL`，不生成或加载独立的 `lua54.dll`。Lua 官方 `luaconf.h` 自动根据 `_WIN32` 选择 Windows 实现，无需改写源码或额外定义兼容宏。编译仅使用仓库内依赖，CI 不联网下载 Lua；构建工件同时附带本许可、声明和来源清单。
