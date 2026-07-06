# AYScript Design

> **命名来源**：Logia — λογία（逻辑 / 理据），与 Phoskia（φῶς + σκιά，光与影）成对：GPU 用 Phoskia 写材质，CPU 用 Logia 写玩法。

## 1. 概述

AYScript 是 AY Engine 的**游戏逻辑脚本子系统**。作者编写 **Logia** 源码（`.logia`），经编译器流水线生成 **Lua chunk** 并由 sol2 加载执行。**Lua 是实现细节，不暴露给内容作者**（体验目标类似 GDScript：引擎自有语法，隐藏宿主语言）。

### 1.1 设计目标

- **Logia DSL**：简化语法，只暴露玩法相关概念（entity、component、生命周期、引擎类型）
- **Phoskia 式编译器**：词法 → 语法 → 语义分析 → 后端；错误带文件/行号；可缓存编译结果
- **AYReflect 语义层**：组件/字段/类型在编译期校验，长期可维护、可接编辑器
- **脚本组件**：`ScriptComponent` 挂载到实体，生命周期 `on_start` / `on_update` / `on_destroy`
- **热更新**（后期）：监视 `.logia` 变更，重编译并重载，保留实例状态（可选）

### 1.2 与 Phoskia 的对称关系

| | Phoskia | Logia |
|--|---------|-------|
| 领域 | GPU 着色器 / 材质 | CPU 游戏逻辑 |
| 扩展名 | `.phoskia` | `.logia` |
| 编译器命名空间 | `ayt::shader::phoskia` | `ayt::script::logia` |
| 用户可见 | 材质块、uniform、vertex/fragment | component 块、生命周期、引擎 API |
| 隐藏的后端 | GLSL / bgfx `.sc` | **Lua 5.5** |
| 元数据 | 着色器语义、类型系统 | **AYReflect** TypeRegistry |
| 所属模块 | AYShader | AYScript |

### 1.3 在引擎中的位置

```
┌─────────────────────────────────────────────────────────────────┐
│                      Engine Modules                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐     │
│  │  AYGameLoop  │     │   AYScript   │     │   AYEntity   │     │
│  │  (主循环)    │     │  (脚本子系统) │     │  (ECS)       │     │
│  └──────┬───────┘     └──────┬───────┘     └──────┬───────┘     │
│         │                    │                      │            │
│         └────────────────────┼──────────────────────┘            │
│                              │                                   │
│         ┌────────────────────┼────────────────────┐              │
│         ▼                    ▼                    ▼              │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │              ayt::script::logia::Compiler                   │  │
│  │   .logia → Lexer → Parser → SemanticAnalyzer → LuaCodegen  │  │
│  └────────────────────────────┬─────────────────────────────┘  │
│                               │ Lua chunk (hidden)              │
│                               ▼                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │              RuntimeLoader (sol2 + 引擎 API 绑定)            │  │
│  └────────────────────────────┬─────────────────────────────┘  │
│                               │                                 │
│                               ▼                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │   ScriptComponent — on_start / on_update / on_destroy     │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 1.4 模块边界

| 模块 | 职责 |
|------|------|
| **AYScript** | Logia 编译器、运行时加载、ScriptSubSystem、引擎 API 绑定 |
| **AYEntity** | `ScriptComponent`、实体/组件查询；C++ 侧生命周期钩子 |
| **AYReflect** | 类型/字段/组件元数据；Logia 语义分析的数据源 |
| **AYGameLoop** | 驱动 `ScriptSubSystem::update` |
| **Lua 5.5 + sol2** | **仅实现层**；不出现在公开文档与示例中 |

### 1.5 明确不做（v1）

- 多宿主语言（Python / JavaScript）并行
- 源码扫描 + 多语言 codegen（旧 design 方案，已废弃）
- 自研字节码 VM（Logia 编译到 Lua，不自研运行时）
- 向作者暴露 `require`、元表、裸 Lua 协程 API（后期可用 `await` 语法糖封装）

---

## 2. Logia 语言

### 2.1 适用范围

**Logia 是游戏逻辑专用 DSL，不是通用语言。**

- 输入：`.logia` 源文件
- 输出：Lua chunk（内部）；作者不阅读、不手写 Lua
- 用户：玩法程序、关卡设计（配合编辑器）
- 错误容忍：编译失败 = 脚本不可用（与 Phoskia 一致，编译期拦住错误）

### 2.2 语法示例

```logia
component PlayerController {
    export var speed: float = 5.0
    export var jump_force: float = 8.0

    var transform: Transform

    on_start(entity: Entity) {
        transform = entity.get_component(Transform)
    }

    on_update(dt: float) {
        if input.is_pressed("jump") {
            transform.position.y += jump_force * dt
        }
        transform.position.x += speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
```

### 2.3 语法原则

1. **声明式块**：顶层以 `component Name { ... }` 为主（类比 Phoskia 的 `material`）
2. **生命周期是语言内置**：`on_start` / `on_update` / `on_destroy`，非约定函数名
3. **类型写引擎认识的**：`Entity`、`Transform`、`float` 等，由 Reflect 注册表解析
4. **snake_case**：关键字与 API 统一蛇形命名（`on_start`、`get_component`）
5. **`export`**：标记可在编辑器 Inspector 中编辑的字段（对接 AYEditor，后期）
6. **无 Lua 泄漏**：不提供 `local`/`nil`/`pcall`/`require` 等给用户

### 2.4 后期语法扩展（非 v1 阻塞）

| 特性 | 说明 | 后端策略 |
|------|------|----------|
| `signal` / `connect` | 事件 | 生成 Lua 表 + 回调注册 |
| `await delay(sec)` | 延时 | Lua 协程封装，用户只见 `await` |
| `extends` / 组件继承 | 复用逻辑 | 生成 Lua 元表链 |
| 静态类型提示 | 编译期检查 | SemanticAnalyzer + Reflect |

### 2.5 文法草案（BNF 子集，v1）

```bnf
<program>       ::= <component_decl>

<component_decl> ::= "component" <identifier> "{" <member>* "}"

<member>        ::= <var_decl>
                  | <lifecycle_func>

<var_decl>      ::= ["export"] "var" <identifier> ":" <type> ["=" <expression>] ";"

<lifecycle_func> ::= ("on_start" | "on_update" | "on_destroy")
                     "(" <param_list>? ")" <block>

<type>          ::= <identifier>    (* Entity, Transform, float, int, bool, ... *)

<block>         ::= "{" <statement>* "}"

<statement>     ::= <var_decl>
                  | <expr_stmt>
                  | "if" <expression> <block> ["else" <block>]
                  | "return" <expression>? ";"

<expression>    ::= <assignment> | <logic_or>

(* 标准表达式层级：or → and → equality → comparison → add → mul → unary → postfix → primary *)
```

完整文法随实现迭代补充至本文档 §附录。

---

## 3. 编译流程

```
┌────────────────────────────────────────────────────────────┐
│                      Logia 编译流程                          │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  1. Logia 源码 (.logia)                                     │
│                                                            │
│  2. 词法分析 (Lexer) → Token 流                             │
│                                                            │
│  3. 语法分析 (Parser) → AST                                 │
│                                                            │
│  4. 语义分析 (SemanticAnalyzer) + AYReflect TypeRegistry    │
│     — 类型解析、组件/字段存在性、export 合法性               │
│                                                            │
│  5. 后端 (LuaCodegen) → Lua 源码 / 可直接 load 的 chunk      │
│     — 附带 #line 或内部 source map 供运行时错误回映射        │
│                                                            │
│  6. RuntimeLoader (sol2) → 绑定引擎 API → 挂到 ScriptComponent │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### 3.1 编译缓存（后期）

与 Phoskia / ShaderCache 类似：

- 输入：`.logia` 路径 + 内容 hash + 编译器版本
- 输出：缓存的 Lua chunk 或字节码文件（如 `.logia.cache`）
- 热重载：文件变更 → 失效缓存 → 重编译

---

## 4. 命名空间分层

| 命名空间 | 职责 | 参考文件（规划） |
|----------|------|------------------|
| `ayt::script` | 引擎集成：ScriptSubSystem、RuntimeLoader、引擎 API 绑定 | `AYScript.h`、`AYScriptSubSystem.h` |
| `ayt::script::logia` | Logia 编译器核心：Token/Lexer/Parser/AST/Semantic/LuaCodegen | `AYLogia.h`、`AYLexer.h`、`AYParser.h`、`AYAst.h` |

**分层理由**（与 `ayt::shader` / `ayt::shader::phoskia` 同构）：

- Logia 是一种语言，`ayt::script::logia` 是其命名空间
- ScriptSubSystem、sol2 绑定属于引擎集成，不混入语言前端
- 未来若更换 Lua 版本或增加其他后端，只改 `LuaCodegen` + `RuntimeLoader`

---

## 5. 语义分析与 AYReflect

### 5.1 职责划分

| 层 | 做什么 |
|----|--------|
| **AYReflect** | 注册 C++ 类型、组件、字段；`FieldAttribute`（Serialize、NetReplicate、编辑器标志） |
| **Logia SemanticAnalyzer** | 读 TypeRegistry；解析 Logia 类型名 → `ITypeInfo*`；校验 `get_component(Transform)` |
| **LuaCodegen** | 不关心 C++ 布局，只生成调用已绑定 API 的 Lua |
| **RuntimeLoader** | sol2 注册 Entity、Transform 等；与 Reflect 字段偏移一致 |

### 5.2 编译期错误示例

```
error[logia/E001]: unknown type 'Tranform'
  --> player.logia:8:5
   |
 8 |     var transform: Tranform
   |     ^^^^^^^^^^^^^ did you mean 'Transform'?

error[logia/E002]: component 'Meshh' not registered
  --> enemy.logia:12:20
   |
12 |         entity.get_component(Meshh)
   |                    ^^^^^^^^^^^^^^^ 'Mesh' is registered
```

这是 Logia 相对「裸 Lua 绑定」的核心长期价值：**错误在编译期，不在运行时才 nil。**

### 5.3 与序列化的关系

- 组件字段若标 `Serialize`，存档/网络与 Logia `export` 可对齐（同一 Reflect 元数据）
- Logia 不替代 AYSerializer；场景 `.ayscene` 存组件数据，`.logia` 存行为

---

## 6. 运行时集成

### 6.1 ScriptComponent（已有，AYEntity）

`AYEntity` 已提供 `ScriptComponent` 与 `IScriptBridge` 桩。Logia 落地后：

1. `ScriptComponent::setScriptName("PlayerController")` → 加载 `PlayerController.logia`
2. 编译（或读缓存）→ 得到 module table
3. `on_start` / `on_update` / `on_destroy` 由 Bridge 调用

### 6.2 IScriptBridge

```cpp
namespace ayt::script {

class IScriptBridge {
public:
    virtual ~IScriptBridge() = default;

    // Load and compile .logia; cache by path + mtime
    virtual bool loadScript(const char* logiaPath) = 0;

    // Invoke lifecycle on a component instance
    virtual bool callLifecycle(const char* scriptName,
                               const char* method,   // "on_start", "on_update", "on_destroy"
                               void* instance,
                               void* arg = nullptr) = 0;

    virtual bool hasScript(const char* scriptName) const = 0;
};

} // namespace ayt::script
```

Lua/sol2 仅在 `LogiaRuntimeBridge` 实现类内部出现，不进入公开头文件注释示例。

### 6.3 ScriptSubSystem

```cpp
class ScriptSubSystem : public ISubSystem {
public:
    bool initialize() override;
    void update(float deltaTime) override;
    void shutdown() override;

    IScriptBridge* getBridge();
    void registerEngineApi();  // Entity, Transform, Input, Time, Log, Event
};
```

注册进 `AYGameLoop`，在 ECS `update` 之后或按文档约定顺序执行脚本 `on_update`。

### 6.4 引擎 API 面（分期暴露给 Logia）

| 阶段 | API |
|------|-----|
| S1 | `Entity`：create/destroy/get_component/add_component |
| S1 | `Transform`：position/rotation/scale |
| S1 | `log.info/warn/error`，`time.delta` |
| S2 | `input.is_pressed/is_just_pressed` |
| S2 | `event.emit/subscribe` |
| S3 | `Health` 等常用组件方法 |
| S3 | 资源：`spawn_prefab(path)` |

---

## 7. 目录结构（规划）

```
AYScript/
├── design.md                 # 本文档
├── CMakeLists.txt
├── include/
│   ├── AYScript.h            # 公开入口
│   ├── AYScriptSubSystem.h
│   ├── IScriptBridge.h
│   └── logia/
│       ├── AYLogia.h         # Compiler 入口
│       ├── AYToken.h
│       ├── AYLexer.h
│       ├── AYParser.h
│       ├── AYAst.h
│       ├── AYSemanticAnalyzer.h
│       ├── AYLuaCodegen.h
│       └── AYCompilerError.h
├── src/
│   ├── AYScriptSubSystem.cpp
│   ├── LogiaRuntimeBridge.cpp   # sol2 + Lua VM（实现文件，不暴露 Lua 头到 AYScript.h）
│   └── logia/
│       ├── AYLexer.cpp
│       ├── AYParser.cpp
│       ├── AYSemanticAnalyzer.cpp
│       ├── AYLuaCodegen.cpp
│       └── AYLogia.cpp
├── unittest/
│   ├── Test_LogiaLexer.cpp
│   ├── Test_LogiaParser.cpp
│   ├── Test_LogiaSemantic.cpp
│   ├── Test_LogiaCodegen.cpp
│   └── Test_LogiaRuntime.cpp
└── examples/
    └── player_controller.logia
```

编译器前端工程模式参考 `AYRuntime/AYShader/`（Lexer/Parser/错误聚合/测试分层），**不共享 shader 专用 AST/IR 代码**。

---

## 8. 实现优先级

按**长期好用**排序，不追求最早上线。

### Phase S0 — 语言骨架

- [x] `ayt::script::logia`：Token、Lexer、Parser
- [x] AST：`ComponentDecl`、`VarDeclStmt`、生命周期函数、表达式子集
- [x] 错误格式：`error[logia/...]`（`AYCompilerError`）
- [x] 单元测试：`Test_LogiaLexer`、`Test_LogiaParser`
- [x] 示例：`examples/player_controller.logia`

### Phase S1 — Lua 后端与运行时

- [x] `LuaCodegen`：AST → Lua 源码（`include/logia/AYLuaCodegen.h`）
- [x] `LogiaRuntimeBridge`：sol2 加载、调用生命周期（`AYScriptRuntimeBridge`）
- [x] `ScriptSubSystem`：ISubSystem 占位实现（首个 ISubSystem 样板）
- [x] 单元测试：`Test_LogiaCodegen`、`Test_LogiaRuntime`
- [ ] 对接 `ScriptComponent`（等 AYEntity 准备好，S2 末/S3）
- [ ] 端到端：`.logia` 修改真实 Transform（依赖 AYEntity ScriptComponent 接入）

### Phase S2 — Reflect 语义（核心）

- [ ] `SemanticAnalyzer` + `TypeRegistry`
- [ ] 类型/组件/字段编译期校验
- [ ] `export` 元数据写入 Reflect 或并行表供编辑器
- [ ] 单元测试：故意写错类型名/组件名应编译失败

### Phase S3 — 引擎 API 与工具

- [ ] Entity / Transform / Input / Time / Log / Event 绑定
- [ ] 编译缓存
- [ ] CLI：`ays-logia compile player.logia`（可选）
- [ ] 热重载（FileWatcher）

### Phase S4 — 语法扩展

- [ ] `signal` / `connect`
- [ ] `await delay`
- [ ] Source map 完善（运行时错误映射回 `.logia` 行号）

---

## 9. 依赖

| 依赖 | 用途 |
|------|------|
| Lua 5.5 | Logia 后端 VM（隐藏） |
| sol2 | C++ ↔ Lua 绑定 |
| AYReflect | 语义分析、类型/组件注册 |
| AYEntity | ScriptComponent |
| AYGameLoop | 子系统调度 |
| AYPlatform | FileWatcher（热重载） |

---

## 10. 与工业级引擎对比

| 功能 | AY Logia | Godot GDScript | Unity C# | O3DE Lua |
|------|----------|----------------|------------|----------|
| 自有表面语法 | ✅ | ✅ | ✅ | ❌（裸 Lua） |
| 隐藏宿主语言 | ✅（Lua） | N/A（自研 VM） | N/A（CLR） | ❌ |
| 编译期类型检查 | 规划（Reflect） | ✅（GDScript 2） | ✅ | ❌ |
| 组件生命周期 | ✅ | ✅ | ✅ | 约定 |
| 热更新 | 规划 | ✅ | 有限 | ✅ |
| 编辑器 export | 规划 | ✅ | ✅ | 部分 |

---

## 11. 变更记录

| 日期 | 变更 |
|------|------|
| 2026-07-06 | **路线 A 定型**：Logia DSL → Lua 后端；废弃多语言 codegen 方案 |
| 2026-07-06 | 语言命名：**Logia**（`.logia`），与 Phoskia 成对 |
| 2026-07-06 | **S0 完成**：Lexer/Parser/AST/错误聚合 + 6 个单测 |
| 2026-07-06 | **S1 完成**：LuaCodegen + LogiaRuntimeBridge（sol2 3.5.0 + Lua 5.5.0）；ScriptSubSystem 首个 ISubSystem 样板；Codegen 9 + Runtime 9 个单测。注意：实际后端是 Lua **5.5**（vcpkg 安装），原计划 5.4 |
| 2026-07-06 | Phase S2 待开始：AYReflect 类型校验接入 SemanticAnalyzer |

---

## 12. 参考

- [`AYShader/design.md`](../AYShader/design.md) — Phoskia 编译器模式
- [`AYFoundation/AYReflect/design.md`](../../AYFoundation/AYReflect/design.md) — 元数据系统
- [`AYEntity/design.md`](../AYEntity/design.md) — ScriptComponent
- [`ENGINE-SERIALIZER-REFLECT-STATUS.md`](../../ENGINE-SERIALIZER-REFLECT-STATUS.md) — Reflect 成熟度
- [sol2](https://sol2.readthedocs.io/) — Lua C++ 绑定（仅实现层）
