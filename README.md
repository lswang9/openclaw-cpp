# openclaw-cpp

OpenClaw 的 **C++ 全量重写**（非 Node 包装、非纯协议 shim）。

## 对齐目标

- 行为对齐官方 **OpenClaw 2026.10.5** 能力（分阶段落地）
- 本仓库为系列第一刀：**核心 CLI** — stdin/参数 → DeepSeek Chat Completions → 打印回复
- 依赖管理：**Conan**（不用 vcpkg）
- 标准：**C++20**

后续模块占位见 `include/openclaw/{gateway,agent,tools,session}/`。系列规划见 [SERIES.md](SERIES.md)。

## 依赖

- CMake ≥ 3.20
- Conan ≥ 2.0
- C++20 编译器（g++ / clang++ / MSVC）
- 网络（构建时拉取 Conan 包；运行时访问 DeepSeek API）

Conan 拉取：

- `nlohmann_json`
- `libcurl`

## 构建

```bash
cp .env.example .env
# 编辑 .env，填入 DEEPSEEK_API_KEY

conan install . --output-folder=build --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build --preset conan-release -j
```

可执行文件：`build/build/Release/openclaw-cpp`。

## 运行

```bash
export DEEPSEEK_API_KEY=...   # 或 source .env
./build/build/Release/openclaw-cpp "你好，介绍一下你自己"
# 或
echo "用一句话解释什么是 OpenClaw" | ./build/build/Release/openclaw-cpp
```

可选环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `DEEPSEEK_API_KEY` | （必填） | API Key |
| `DEEPSEEK_BASE_URL` | `https://api.deepseek.com` | API 根地址 |
| `DEEPSEEK_MODEL` | `deepseek-chat` | 模型名 |

## 许可证

待定。与上游 OpenClaw 官方许可及本系列策略对齐后再定。
