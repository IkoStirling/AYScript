# AYScript Design

> **命名来源**：Logia — λογία（逻辑 / 理据），与 Phoskia（φῶς + σκιά，光与影）成对：GPU 用 Phoskia 写材质，CPU 用 Logia 写玩法。

## 1. 概述

AYScript 是 AY Engine 的**游戏行为脚本子系统**。**作者编写 Logia 源码（`.logia`）**，经编译器流水线生成 **Lua chunk** 并由 sol2 加载执行。

**核心模型（2026-07-07 重设计）**：

- **Logia 脚本不是定义数据的地方，是消费 AYReflect 元数据的地方**。
- C++ 端的 `ScriptComponent` 子类（如 `PlayerController`）已经用 `AY_PROPERTY(float, speed, ...)` 注册了字段；Logia 脚本通过 `self.speed` 读写这些**已经在 C++ 端声明**的字段。
- **`script` 块**对应一个 C++ `ScriptComponent` 子类——Logia 给出**行为**（生命周期函数），C++ 给出**数据**（`AY_PROPERTY` 字段）。
- **`var` 块** = 纯 Lua local（脚本内部状态），C++ 不可见。
- **`self`** = lightuserdata 指向绑定的 `ScriptComponent*` 实例。字段访问 `self.speed` 走 AYReflect 反射到 C++ 端 `PlayerController::speed`。
- **生命周期函数 `on_start` / `on_update` / `on_destroy` 不收参数**——实体上下文通过 `self` 隐式可达，需要时通过 `ayt::entity::World::instance()` 显式查询。

**Lua 是实现细节**，不暴露给内容作者（体验目标类似 GDScript：引擎自有语法，隐藏宿主语言）。

### 1.1 设计目标

- **Logia DSL**：简化语法，只暴露玩法相关概念（self、生命周期、引擎类型、ambient 引擎 API）
- **Phoskia 式编译器**：词法 → 语法 → 语义分析 → 后端；错误带文件/行号；可缓存编译结果
- **AYReflect 语义层**：C++ 字段/类型在编译期校验，长期可维护、可接编辑器
- **绑定**：Logia script 与 C++ `ScriptComponent` 子类一对一（同名），运行时通过 `ScriptComponent` 实例化
- **热更新**（S3+）：监视 `.logia` 变更，重编译并重载

### 1.2 与 Phoskia 的对称关系

| | Phoskia | Logia |
||--|---------|-------|
| 领域 | GPU 着色器 / 材质 | CPU 游戏行为 |
| 扩展名 | `.phoskia` | `.logia` |
| 编译器命名空间 | `ayt::shader::phoskia` | `ayt::script::logia` |
| 用户可见 | 材质块、uniform、vertex/fragment | `script` 块、生命周期、ambient API |
| 隐藏的后端 | GLSL / bgfx `.sc` | **Lua 5.5** |
| 元数据 | 着色器语义、类型系统 | **AYReflect** TypeRegistry |
| 所属模块 | AYShader | AYScript |
| 与 C++ 端耦合 | 编译期 ubo/cbuffer 布局 | 运行时通过 `AY_PROPERTY` 读写字段 |

### 1.3 在引擎中的位置

```
┌─────────────────────────────────────────────────────────────────┐
│                      Engine Modules                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  AYEntity                                                         │
│  ├─ class ScriptComponent { virtual void onAttach(Entity*); ... } │
│  ├─ class PlayerController : ScriptComponent {                    │
│  │      AY_PROPERTY(float, speed, kAttrSerialize)                 │
│  │      AY_PROPERTY(float, jump_force, kAttrSerialize)            │
│  │  }                                                              │
│                                                                  │
│  AYScript                                                         │
│  ├─ Compiler (.logia → Lua chunk)                                 │
│  ├─ LogiaRuntimeBridge (sol2 + Lua 5.5)                          │
│  │     loadScript(name, source) → cached module table              │
│  │     callLifecycle(name, method, receiver) → invoke              │
│  ├─ LogiaScriptBridgeAdapter (AYEntity IScriptBridge shim)        │
│  └─ ScriptSubSystem (drives bridge update() each tick)            │
│                                                                  │
│  AYReflect                                                        │
│  └─ TypeRegistry, ITypeInfo, IFieldInfo, FieldAttribute          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

数据流（运行时）：
  ScriptComponent::onUpdate(dt)
    → IScriptBridge::call("onUpdate", this, &dt)
    → LogiaScriptBridgeAdapter::call
    → LogiaRuntimeBridge::callLifecycle("PlayerController", "on_update", this, &dt)
    → sol2 fn.call(receiver, dt)         // Lua `self` = receiver
    → M.on_update(self, dt)              // Logia script 内 `self.position` 走 AYReflect
    → position.x = position.x + self.speed * dt   // self.speed 反射到 C++ PlayerController::speed
```

### 1.4 模块边界

| 模块 | 职责 |
|------|------|
| **AYEntity** | `ScriptComponent` 基类、子类用 `AY_PROPERTY` 注册字段、`IScriptBridge` 接口 |
| **AYScript** | Logia 编译器、运行时加载、adapter、子系统、ambient 引擎 API 绑定 |
| **AYReflect** | 类型/字段元数据；Logia 语义分析的数据源 |
| **AYGameLoop** | 驱动 `ScriptSubSystem::update` |
| **Lua 5.5 + sol2** | **仅实现层**；不出现在公开文档与示例中 |

### 1.5 明确不做（v1）

- 多宿主语言（Python / JavaScript）并行
- 源码扫描 + 多语言 codegen
- 自研字节码 VM（Logia 编译到 Lua，不自研运行时）
- 向作者暴露 `require`、元表、裸 Lua 协程 API（后期可用 `await` 语法糖封装）

---

## 2. Logia 语言

### 2.1 适用范围

**Logia 是游戏行为专用 DSL，不是通用语言。**

- 输入：`.logia` 源文件
- 输出：Lua chunk（内部）；作者不阅读、不手写 Lua
- 用户：玩法程序、关卡设计（配合编辑器）
- 错误容忍：编译失败 = 脚本不可用（与 Phoskia 一致，编译期拦住错误）

### 2.2 语法示例

```logia
script PlayerController {
    var tick_counter: int = 0        // 纯 Lua local（self 看不到）

    on_start() {
        tick_counter = 0             // 写 local
        // self.speed / self.jump_force 直接来自 C++ PlayerController
        // 的 AY_PROPERTY 注册字段
    }

    on_update(dt: float) {
        tick_counter = tick_counter + 1
        if input.is_pressed("jump") {
            self.position.y = self.position.y + self.jump_force * dt
        }
        self.position.x = self.position.x + self.speed * dt
    }

    on_destroy() {
        log.info("PlayerController destroyed")
    }
}
```

### 2.3 语法原则

1. **`script Name { ... }`**：顶层声明；`Name` 必须对应一个 C++ 端 `ScriptComponent` 子类（同名），运行时通过 `setScriptName("Name")` 绑定。
2. **`var x: T = ...`**：纯 Lua local 状态（self 不可见）。仅用于脚本内部计数器、缓存等。
3. **`self`**：lightuserdata 指向绑定的 `ScriptComponent*`。`self.field` 走 AYReflect 反射到 C++ 字段（必须已在 C++ 端用 `AY_PROPERTY` 注册）。
4. **生命周期是语言内置**：`on_start` / `on_update` / `on_destroy`，**无参数**——实体上下文通过 `World::instance()` 显式查询。
5. **类型写引擎认识的**：`Entity`、`Transform`、`float` 等，由 Reflect 注册表解析。
6. **snake_case**：关键字与 API 统一蛇形命名。
7. **无 Lua 泄漏**：不提供 `local` / `nil` / `pcall` / `require` 等给用户。

### 2.4 后期语法扩展（非 v1 阻塞）

| 特性 | 说明 | 后端策略 |
|------|------|----------|
| `signal` / `connect` | 事件 | 生成 Lua 表 + 回调注册 |
| `await delay(sec)` | 延时 | Lua 协程封装，用户只见 `await` |
| 静态类型提示 | 编译期检查 | SemanticAnalyzer + Reflect |

### 2.5 文法草案（BNF 子集，v1）

```bnf
<program>      ::= <script_decl>+

<script_decl>  ::= "script" <identifier> "{" <member>* "}"

<member>       ::= <var_decl>
                 | <lifecycle_func>

<var_decl>     ::= "var" <identifier> ":" <type> ["=" <expression>] ";"

<lifecycle_func> ::= ("on_start" | "on_update" | "on_destroy")
                     "(" ")" <block>            (* v1: 无参数 *)

<type>         ::= <identifier>    (* Entity, Transform, float, int, bool, ... *)

<block>        ::= "{" <statement>* "}"

<statement>    ::= <var_decl>
                 | <expr_stmt>
                 | "if" <expression> <block> ["else" <block>]
                 | "return" <expression>? ";"

<expression>   ::= <assignment> | <logic_or>

(* 标准表达式层级：or → and → equality → comparison → add → mul → unary → postfix → primary *)
(* postfix 包含 member access：`self.position`、`tick_counter` *)
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
│  3. 语法分析 (Parser) → AST  (ScriptDecl, VarDeclStmt, ...)  │
│                                                            │
│  4. 语义分析 (SemanticAnalyzer) + AYReflect TypeRegistry    │
│     — `script Name` 校验对应 C++ ScriptComponent 子类存在    │
│     — `var x: T` 校验 T 是已知类型（builtin 或 registry）     │
│     — `self.field` 校验 field 是 AY_PROPERTY 注册的字段      │
│     — Ambient API（log.*, input.*, time.*）— 名称白名单     │
│                                                            │
│  5. 后端 (LuaCodegen) → Lua 源码 / 可直接 load 的 chunk      │
│     — `self.field` 生成反射访问（详见 §5.3）                 │
│                                                            │
│  6. RuntimeLoader (sol2) → 绑定 ambient 引擎 API →          │
│     → 缓存到 LogiaRuntimeBridge 内的 module table           │
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

| 命名空间 | 职责 | 参考文件 |
|----------|------|----------|
| `ayt::script` | 引擎集成：ScriptSubSystem、LogiaRuntimeBridge、LogiaScriptBridgeAdapter、ambient API 绑定 | `AYScript.h`、`AYScriptSubSystem.h`、`AYScriptRuntimeBridge.h`、`AYScriptBridgeAdapter.h` |
| `ayt::script::logia` | Logia 编译器核心：Token/Lexer/Parser/AST/Semantic/LuaCodegen | `AYLogia.h`、`AYLexer.h`、`AYParser.h`、`AYAst.h`、`AYSemanticAnalyzer.h`、`AYLuaCodegen.h`、`AYCompilerError.h` |

**分层理由**：

- Logia 是一种语言，`ayt::script::logia` 是其命名空间
- 运行时、sol2 绑定属于引擎集成，不混入语言前端
- 未来若更换 Lua 版本或增加其他后端，只改 `LuaCodegen` + `LogiaRuntimeBridge`

---

## 5. 语义分析与 AYReflect

### 5.1 职责划分

| 层 | 做什么 |
|----|--------|
| **AYEntity** | `ScriptComponent` 子类 + `AY_PROPERTY` 字段注册（**数据来源**） |
| **AYReflect** | 读 AY_PROPERTY 注册的字段元数据，暴露 `ITypeInfo` / `IFieldInfo` |
| **Logia SemanticAnalyzer** | 校验 `script Name` 对应的 C++ 类存在；`var` 类型合法；`self.field` 字段在 registry 中存在 |
| **LuaCodegen** | 对 `self.field` 生成反射访问代码（详见 §5.3）；对 ambient API 生成直接调用 |
| **LogiaRuntimeBridge (sol2)** | 注册 ambient API + LogiaRuntimeBridge 内的 helper 用于 `self.field` 反射 |

### 5.2 编译期错误示例

```
error[logia/E001]: unknown script 'PlayerControllerr'
  --> player.logia:1:1
   |
 1 | script PlayerControllerr {
   |        ^^^^^^^^^^^^^^^^^^^ did you mean 'PlayerController'?

error[logia/E002]: unknown field 'speeed' on type 'PlayerController'
  --> player.logia:7:13
   |
 7 |         self.speeed = 5.0
   |              ^^^^^^ 'speed' is registered (AY_PROPERTY on C++ side)

error[logia/E003]: unknown type 'Vectorr3' for var declaration
  --> player.logia:3:5
   |
 3 |     var tick: Vectorr3 = 0
   |     ^^^^^^^^^^^^^^^^^^^^^^ builtin types: int, float, bool, string, Entity
```

这是 Logia 相对「裸 Lua 绑定」的核心长期价值：**错误在编译期，不在运行时才 nil。**

### 5.3 `self.field` 反射访问（生成策略）

S2 阶段：仅做语义检查（字段是否在 AYReflect 注册），codegen 暂时生成 stub（仍可桥接但不全功能）。

S3 完整 codegen：
```lua
-- `self.position` 生成：
local __reflect_pos = ayt_reflect_get_field(self, "position")
__reflect_pos.x = __reflect_pos.x + self.speed * dt
-- `self.speed` 生成：
local __tmp_speed = ayt_reflect_get_field(self, "speed")
-- ... 计算用 __tmp_speed ...
-- 写回（如有赋值）：
ayt_reflect_set_field(self, "speed", new_value)
```

实际生成路径（**S3 实现**，S2 仅语义检查）：
- `self.field` 读 → `ayt_reflect_get_<type>(self, "field")`
- `self.field = expr` → `ayt_reflect_set_<type>(self, "field", expr)`

sol2 usertype 注册（**S3**）：`ScriptComponent` 子类用 `sol::usertype<T>` 暴露字段访问方法。

### 5.4 Ambient API 白名单

以下名称不需要 C++ 端 `AY_PROPERTY` 注册；由 `LogiaRuntimeBridge::registerEngineApi()` 在 sol2 端注册：

| Ambient 名称 | 来源 | 阶段 |
|---|---|---|
| `log.info/warn/error/debug` | `ayt::log::*` | S1 |
| `input.is_pressed/is_just_pressed` | AYInput mock | S1 |
| `time.delta` | `ayt::time::delta()` | S3 |
| `event.emit/subscribe` | AYEvent | S3+ |

`log` / `input` / `time` / `event` 是 ambient identifier——在 `self` 上下文外可以直接使用，**不**走 AYReflect。

### 5.5 与序列化的关系

- C++ 组件字段标 `Serialize` 的，存档 / 网络与 Logia `self.field` 访问**共享同一份 AYReflect 元数据**——数据布局只定义一次。
- Logia **不替代** AYSerializer；场景 `.ayscene` 存组件数据，`.logia` 存行为。
- 因此 Logia **不**再有 `export` 关键字——数据可见性由 C++ 端的 `AY_PROPERTY` + `FieldAttribute` 决定。

---

## 6. 运行时集成

### 6.1 ScriptComponent（AYEntity 提供）

```cpp
class IScriptBridge {
public:
    virtual ~IScriptBridge() = default;
    virtual bool call(const char* method, void* arg1, void* arg2) = 0;
    virtual bool hasScript(const char* scriptName) const = 0;
};

class ScriptComponent : public IComponent {
    void onAttach(Entity* entity) override;   // → bridge.call("onStart", this, entity)
    void onUpdate(float dt) override;        // → bridge.call("onUpdate", this, &dt)
    void onDetach() override;                // → bridge.call("onDestroy", this)
    // ...
};
```

### 6.2 IScriptBridge 调用约定（AYEntity 已固定）

```cpp
namespace ayt::entity {
class IScriptBridge {
public:
    virtual ~IScriptBridge() = default;
    virtual bool call(const char* method, void* arg1, void* arg2) = 0;
    virtual bool hasScript(const char* scriptName) const = 0;
};
}
```

`method` 是 camelCase：`"onStart"` / `"onUpdate"` / `"onDestroy"`。`arg1` 是 `ScriptComponent* this`；`arg2` 是 `Entity*`（onStart）或 `float*`（onUpdate）或 `nullptr`（onDestroy）。

Logia 生成的 Lua 模块函数是 snake_case (`M.on_start`)。`LogiaScriptBridgeAdapter` 做 camelCase → snake_case 转换。

### 6.3 LogiaRuntimeBridge（AYScript 实现）

```cpp
namespace ayt::script {

class LogiaRuntimeBridge {
public:
    bool initialize();
    void shutdown();

    // Compile and load a Logia source string under a logical script
    // name. The compiled module table is cached; re-loads with the
    // same name replace the cache entry.
    bool loadScript(const std::string& scriptName,
                    const std::string& logiaSource,
                    std::vector<logia::CompilerError>& errors);

    [[nodiscard]] bool hasScript(const std::string& scriptName) const;

    // Invoke a lifecycle method. `receiver` is the ScriptComponent*
    // (forwarded to Lua as lightuserdata so the script's `self`
    // parameter receives it). `arg2` is passed verbatim (Entity* or
    // float* depending on the method). Lua silently discards extra
    // args, so passing `entity` to `on_start()` (which takes no
    // args in Logia) is harmless.
    bool callLifecycle(const std::string& scriptName,
                       const std::string& methodName,   // snake_case
                       void* receiver = nullptr,
                       void* arg2 = nullptr);
};

}
```

### 6.4 ScriptSubSystem

```cpp
class ScriptSubSystem : public ISubSystem {
public:
    bool initialize() override;
    void update(float deltaTime) override;       // S3: iterate all ScriptComponent, call onUpdate
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    LogiaRuntimeBridge& bridge();
};
```

### 6.5 引擎 API 暴露给 Logia 的 ambient 面（分期）

| 阶段 | API |
|------|-----|
| S1 | `log.info/warn/error/debug`，`input.is_pressed/is_just_pressed`（mock） |
| S3 | `time.delta`、`event.emit/subscribe` |
| S3 | 真实 `AYInput` / `AYTime` 绑定（替换 mock） |
| S3+ | 资源：`spawn_prefab(path)` |

---

## 7. 目录结构

```
AYScript/
├── design.md                       # 本文档
├── CMakeLists.txt
├── include/
│   ├── AYScript.h                  # 公开入口
│   ├── AYScriptSubSystem.h
│   ├── AYScriptRuntimeBridge.h     # sol2 + 引擎 API 绑定
│   ├── AYScriptBridgeAdapter.h     # AYEntity IScriptBridge 适配
│   └── logia/
│       ├── AYLogia.h               # Compiler 入口
│       ├── AYToken.h
│       ├── AYLexer.h
│       ├── AYParser.h
│       ├── AYAst.h
│       ├── AYSemanticAnalyzer.h
│       ├── AYLuaCodegen.h
│       └── AYCompilerError.h
├── src/
│   ├── AYScriptSubSystem.cpp
│   ├── AYScriptRuntimeBridge.cpp
│   ├── AYScriptBridgeAdapter.cpp
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
│   ├── Test_LogiaRuntime.cpp
│   └── Test_LogiaAdapter.cpp
└── examples/
    └── player_controller.logia
```

---

## 8. 实现优先级

按**长期好用**排序。

### Phase S0 — 语言骨架 ✅

- [x] `ayt::script::logia`：Token、Lexer、Parser
- [x] AST：`ComponentDecl`（**注**：S2.5 改名 `ScriptDecl`）、`VarDeclStmt`、生命周期函数、表达式子集
- [x] 错误格式：`error[logia/...]`（`AYCompilerError`）
- [x] 单元测试：`Test_LogiaLexer`、`Test_LogiaParser`
- [x] 示例：`examples/player_controller.logia`

### Phase S1 — Lua 后端与运行时 ✅

- [x] `LuaCodegen`：AST → Lua 源码
- [x] `LogiaRuntimeBridge`：sol2 3.5.0 + Lua 5.5.0 加载、调用生命周期
- [x] `ScriptSubSystem`：ISubSystem 占位实现
- [x] 单元测试：`Test_LogiaCodegen`、`Test_LogiaRuntime`
- [x] 端到端：Logia 脚本能修改 mock 状态

### Phase S2 — Reflect 语义（核心） 🟡 重设计中

- [x] `SemanticAnalyzer` + `TypeRegistry` —— 已接入 AYReflect
- [x] 类型/字段编译期校验
- [x] 单元测试 13 个
- [ ] **S2.5 重设计（2026-07-07 决定）**：
  - [ ] `component` → `script` 重命名
  - [ ] 删除 `export` 关键字
  - [ ] 删除 `ComponentDecl` → `ScriptDecl`
  - [ ] 删除 `entity.get_component(...)` 路径（改由 self + World 查询）
  - [ ] 生命周期参数移除（on_start() / on_update(dt) / on_destroy()）
  - [ ] `self.field` 语义检查走 AYReflect
  - [ ] 重写所有测试 + example

### Phase S3 — 引擎 API 与工具

- [ ] sol2 usertype 注册：`self.field` 真正走 AYReflect 读写
- [ ] 真实 AYInput / AYTime 绑定（替换 mock）
- [ ] 编译缓存
- [ ] ScriptSubSystem 接入 GameLoop（update 遍历所有 ScriptComponent）
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
| sol2 3.5.0 | C++ ↔ Lua 绑定 |
| AYReflect | 语义分析、类型/字段注册 |
| AYEntity | ScriptComponent 基类、子类（带 AY_PROPERTY 字段） |
| AYGameLoop | 子系统调度 |
| AYPlatform | FileWatcher（热重载） |

---

## 10. 与工业级引擎对比

| 功能 | AY Logia | Godot GDScript | Unity C# | O3DE Lua |
|------|----------|----------------|------------|----------|
| 自有表面语法 | ✅ | ✅ | ✅ | ❌（裸 Lua） |
| 隐藏宿主语言 | ✅（Lua） | N/A（自研 VM） | N/A（CLR） | ❌ |
| 编译期类型检查 | ✅（S2） | ✅（GDScript 2） | ✅ | ❌ |
| 组件生命周期 | ✅ | ✅ | ✅ | 约定 |
| 热更新 | 规划 | ✅ | 有限 | ✅ |
| 共享 C++ 数据 | ✅（AY_PROPERTY） | ❌（GDScript 自管） | ❌ | 部分 |
| 编辑器 inspector | 规划（S3+） | ✅ | ✅ | 部分 |

---

## 11. 变更记录

| 日期 | 变更 |
|------|------|
| 2026-07-06 | **路线 A 定型**：Logia DSL → Lua 后端 |
| 2026-07-06 | S0 完成：Lexer/Parser/AST + 6 个单测 |
| 2026-07-06 | S1 完成：LuaCodegen + LogiaRuntimeBridge（sol2 3.5.0 + Lua 5.5.0）；注意后端是 Lua **5.5**（vcpkg） |
| 2026-07-07 | S2 第一版完成（168/168 测试通过）：SemanticAnalyzer + IScriptBridge adapter |
| 2026-07-07 | **S2.5 重设计**：删除 `component`/`export`/`entity` 参数；引入 `script`/`self` 模型；Logia 改为消费 AYReflect 元数据而非定义数据。详见 §1 核心模型 |

---

## 12. 参考

- [`AYShader/design.md`](../AYShader/design.md) — Phoskia 编译器模式
- [`AYFoundation/AYReflect/design.md`](../../AYFoundation/AYReflect/design.md) — 元数据系统
- [`AYEntity/design.md`](../AYEntity/design.md) — ScriptComponent
- [sol2](https://sol2.readthedocs.io/) — Lua C++ 绑定（仅实现层）