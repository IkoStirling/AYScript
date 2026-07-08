# AYScript Design

> **命名来源**：Logia — λογία（逻辑 / 理据），与 Phoskia（φῶς + σκιά，光与影）成对：GPU 用 Phoskia 写材质，CPU 用 Logia 写玩法。

## 1. 概述

AYScript 是 AY Engine 的**游戏行为脚本子系统**。**作者编写 Logia 源码（`.logia`）**，经编译器流水线生成 **Lua chunk** 并由 sol2 加载执行。

**核心模型（S2.5 锁定，2026-07-07）**：

- **Logia 是 host-bound behavior DSL**——`script Foo { ... }` 声明一段绑定到 **C++ host 类型** 的行为，不是「只能给 ScriptComponent 用的语法糖」。
- **S2.5 唯一落地的 host**：`ScriptComponent` 子类（如 `PlayerController`）。S3 再扩展 System / Tool 等 host，**不改 Logia 表面语法**。
- **Logia 不定义数据，只消费 AYReflect 元数据**：C++ host 用 `AY_PROPERTY(float, speed, ...)` 注册字段；Logia 通过 `self.speed` 读写这些字段。
- **`script` 块**给出**行为**（生命周期 + 自定义逻辑）；C++ host 给出**数据**（`AY_PROPERTY` 字段）。
- **`var` 块** = 纯 Lua local（脚本内部状态），C++ 不可见。
- **`self`**（S2.5）：lightuserdata 指向绑定的 `ScriptComponent*`。S3 其他 host 时 `self` 类型由 `LogiaHostContext` 决定；无 receiver 的 host（如 CLI tool）可省略 `self`。
- **生命周期 `on_start` / `on_update` / `on_destroy`** 是**约定方法名**，不是 Logia 内置 magic——host 调度方决定调用哪些、何时调用。S2.5 由 `ScriptComponent` + `IScriptBridge` 驱动。

**关键字 `script` 保留**——不改为 `behavior` / `attach` 等。泛化的是 **host 绑定**，不是 DSL 名字。

**Lua 是实现细节**，不暴露给内容作者（体验目标类似 GDScript：引擎自有语法，隐藏宿主语言）。

### 1.1 设计目标

- **Logia DSL**：简化语法，只暴露玩法相关概念（self、生命周期、引擎类型、ambient 引擎 API）
- **Phoskia 式编译器**：词法 → 语法 → 语义分析 → 后端；错误带文件/行号；可缓存编译结果
- **AYReflect 语义层**：C++ 字段/类型在编译期校验，长期可维护、可接编辑器
- **Host 绑定（S2.5）**：`script Name` 与 Reflect 中注册的 C++ 类型同名；**S2.5 运行时**只通过 `ScriptComponent` 实例化并调度
- **Host 多元化（S3）**：同一套 `script` 语法，编译/加载时由调用方传入 `LogiaHostContext`（见 §1.6）
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

### 1.6 Host 绑定模型（S2.5 锁定 / S3 扩展）

Logia **语法层**与 **host 类型**解耦：作者始终写 `script Name { ... }`；**谁加载、以什么 C++ 类型绑定 `self`** 由引擎集成层决定。

```
┌─────────────────────────────────────────────────────────────┐
│  .logia 源码（作者可见）                                      │
│    script PlayerController { on_update(dt) { self.speed } } │
└───────────────────────────┬─────────────────────────────────┘
                            │ Compiler::compile(source, ctx)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  LogiaHostContext（调用方传入，作者不可见）                    │
│    hostKind     = Component | System | Tool | ...  (S3+)    │
│    hostType     = Reflect TypeInfo*（决定 self 类型校验）    │
│    expectSelf   = true/false（Tool 可能无 self）               │
└───────────────────────────┬─────────────────────────────────┘
                            │ SemanticAnalyzer + LuaCodegen
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  Lua module + 运行时调度                                      │
│    S2.5: ScriptComponent → IScriptBridge → callLifecycle    │
│    S3+:  SystemSubSystem / EditorToolRunner / ...           │
└─────────────────────────────────────────────────────────────┘
```

**S2.5 锁定的 `LogiaHostContext` 默认值**（隐式，尚未暴露为公开 API）：

| 字段 | S2.5 值 |
|------|---------|
| `hostKind` | `Component` |
| `hostType` | S3.0 起由调用方传入期望 host 基类型；S2.5 隐式等价于 Component。`nullptr` = 不做 host 兼容性校验 |
| `expectSelf` | `true` |

**S3 计划支持的其他 host（语法不变，只改 ctx + 调度）**：

| 场景 | C++ host | Logia 侧 | `self` | 调度方 |
|------|----------|----------|--------|--------|
| 实体组件脚本（**S2.5 已落地**） | `class PlayerController : ScriptComponent` | `script PlayerController { on_update(dt) {...} }` | `ScriptComponent*` | `ScriptComponent` + `IScriptBridge` |
| ECS System（S3 首选扩展） | `class MovementSystem : ISystem` | `script MovementSystem { on_update(dt) {...} }` | `ISystem*` 或无 | `AnimationSystem` 同类 tick |
| 编辑器/CLI 工具（S3+） | `class BuildTool` | `script BuildTool { run() {...} }` | 无 | 一次性 `call("run")` |
| 事件回调（S3+） | 已注册 Reflect 类型 | `script DamageHandler { on_damage(amount) {...} }` | host 实例 | EventBus 订阅 |
| 纯数据配置 | — | **不用 Logia** | — | 用 JSON / AYConfig |

**语义分析规则（分阶段）**：

| 规则 | S2.5 / S3.0 | S3.1+ | S3.2（可选 B-min） |
|------|-------------|-------|---------------------|
| `script Name` 在 AYReflect 注册 | soft warning（未知名仍生成 Lua） | 同左 | 同左 |
| `Name` 与 `ctx.hostType` 继承兼容 | **不做**（S2.5 从未做子类检查） | **不做**；按 `hostKind` + registry 存在性 + 运行时注册表 | `isDerivedFrom(type, hostType)` 沿单链 parent walk |
| `self.field` 字段存在 | 检查 AY_PROPERTY | 同左 | 同左 |
| 生命周期名 | `on_start/on_update/on_destroy` | 各 host 文档化合法方法集 | 同左 |

**AYReflect 现状（2026-07-08）**：`ITypeInfo` 无 `isSubclassOf` / 无派生图；`TypeRegistry` 只有 name/id 映射。`resolveScriptName` 仅 `findType(name)`——warning 文案中的 “ScriptComponent subclass” 是**目标语义**，不是 S2.5 已实现行为；S3.0 应修正文案为 “no matching registered type”。

**明确不做**：为每种 host 新增 Logia 关键字（如 `system Foo`、`tool Foo`）。Host 种类是 **C++ 集成概念**，不是语法概念。

### 1.7 明确不做（v1）

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

1. **`script Name { ... }`**：顶层行为块；`Name` 应对应 AYReflect 中已注册的 C++ host 类型（**S2.5 运行时**通过 `ScriptComponent::setScriptName("Name")` 绑定到实体组件实例）。
2. **`var x: T = ...`**：纯 Lua local 状态（self 不可见）。仅用于脚本内部计数器、缓存等。
3. **`self`**：指向当前 host 实例的 lightuserdata。**S2.5** = `ScriptComponent*`；`self.field` 走 AYReflect（须已在 C++ 用 `AY_PROPERTY` 注册）。
4. **生命周期是约定方法名**：S2.5 标准集为 `on_start` / `on_update` / `on_destroy`；源码可写 `on_update(dt: float)` 作文档性参数，codegen 仍 emit `function M.on_update(self)`，Lua 侧由 bridge 传入 `dt`。**S3** 其他 host 可定义 `run()`、`on_damage()` 等，由调度方调用。
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
│     + LogiaHostContext（S3 公开；S2.5 隐式 = Component）   │
│     — `script Name` 校验 Reflect 中是否存在（S2.5/S3.0: soft warning）│
│     — `ctx.hostType` 传入 analyzer（S3.0: 存储不用；S3.1+: hostKind 规则）│
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
| **AYEntity** | `ScriptComponent` 基类、子类用 `AY_PROPERTY` 注册字段、`IScriptBridge` 接口（**S2.5 唯一 runtime host**） |
| **AYReflect** | 类型/字段元数据；Logia 语义分析的数据源 |
| **Logia SemanticAnalyzer** | 校验 `script Name` 在 registry；`var` 类型合法；`self.field` 存在；**S3.0** 接收 `LogiaHostContext`（校验不变）；**S3.1+** 按 hostKind 启用规则 |
| **LuaCodegen** | 对 `self.field` 生成反射访问代码（详见 §5.3）；对 ambient API 生成直接调用 |
| **LogiaRuntimeBridge (sol2)** | 注册 ambient API + helper 用于 `self.field` 反射；**S2.5** 只服务 ScriptComponent 调度 |

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

### 5.6 LogiaHostContext 与 S3 分阶段路线

S2.5：`SemanticAnalyzer` 行为等价于隐式 `LogiaHostContext{ Component, nullptr, true }`，但 **API 尚未公开**。

#### API（S3.0 锁定）

```cpp
namespace ayt::script::logia {

enum class LogiaHostKind {
    Component,   // S2.5 — ScriptComponent on Entity
    System,      // S3.1 — ISystem tick
    Tool,        // S3+ — editor/CLI one-shot
    EventHandler // S3+ — callback object
};

struct LogiaHostContext {
    LogiaHostKind kind = LogiaHostKind::Component;
    // Expected host base type for future strict checks.
    // S3.0: stored, not used for validation. nullptr = skip.
    const ayt::reflect::ITypeInfo* hostType = nullptr;
    bool expectSelf = true;   // false for Tool.run()-only scripts
};

} // namespace
```

公开入口：

```cpp
CompileResult Compiler::compile(const std::string& source,
                                const LogiaHostContext& ctx = LogiaHostContext{});
```

默认参数保持 S2.5 行为；现有 169 测试零修改。

#### S3.0 — LG-03：API 穿线（**不改 AYReflect**）

| 做 | 不做 |
|----|------|
| `LogiaHostContext` + `Compiler::compile(source, ctx)` | `ITypeInfo::isSubclassOf` |
| `SemanticAnalyzer` 构造/分析时存 `_ctx` | TypeRegistry 派生图 |
| 校验逻辑与 S2.5 相同（registry lookup + soft warning） | 启用 `hostType` 子类 hard check |
| 修正 unknown-script warning 文案 | 重命名 `ScriptDecl` → `BehaviorDecl` |
| +2~3 个 ctx 传递单测 | 改 LuaCodegen 输出形状 |

#### S3.1 — LG-04：System host 落地（**仍可不碰 Reflect 继承**）

| 项 | 策略 |
|----|------|
| 校验 | `ctx.kind == System` + `findType(scriptName)` + `World` 已注册该系统 |
| `self` | `ISystem*` 或 `expectSelf=false`（实现时二选一锁死） |
| 调度 | GameLoop tick 调 `callLifecycle("MovementSystem", "on_update", system, &dt)` |
| 继承 | **不需要** `isSubclassOf`——类型合法性由 ECS 注册 + Reflect 名字存在性保证 |

**S3.1 (LG-04) 锁定决策**（2026-07-08 实现完成后补）：

- **`self` 语义 = `ISystem*` lightuserdata**（`ctx.expectSelf = true`）。理由：与 Component host 的 `self` 模型保持对称；`self.field` 的 reflection 路径后续 S3.x 接入 `sol::usertype<ISystem>` 时无需重写语义层。
- **未知 `script Name` = soft warning**（与 S2.5 / LG-03 保持一致）。host-kind-aware hint：System host 提示 `AY_SYSTEM`；Component host 提示 `AY_FINALIZE_REGISTRATION_METADATA`。
- **System host lifecycle 白名单**：`on_start`、`on_update` 调；`on_destroy` 编译期软警告"not invoked on System host scripts"（ISystem 不销毁）。
- **调度入口**：`ScriptSubSystem::update(float dt)` / `fixedUpdate(float dt)` 遍历 `World::instance().systemCount()`，对 `getSystemNameAt(i) == bridge.hasScript(name)` 的 system 调 `callLifecycle(name, "on_update", systemPtr, &dt)`。
- **新增 `World::findSystemByName(name)`**（`AYRuntime/AYEntity/include/AYWorld.h`）——S3.1 之前 World 没有按名查 system 的公共入口。
- **测试 fixture**：`unittest/AYTestMovementSystem.h`（ISystem 子类 + `AY_PROPERTY(moveSpeed, ...)` + `AY_FINALIZE_REGISTRATION_METADATA`），`unittest/Test_LogiaSystemHost.cpp`（4 用例）；`examples/movement_system.logia`（文档示例）。
- **暂未做**：`self.field` 真正的 usertype 读写绑定（仍是 bare Lua member access，self 是 lightuserdata，访问会运行期 nil error）——这是 S3.x 任务，对应 Phase S3 backlog 的 "sol2 usertype 注册"。

#### S3.2 — LG-04b（可选 B-min）：Reflect 单链 parent

**触发条件**（任一成立才做）：Component host 需 hard 拒绝「已注册但不是 Component 子类」的类型；Serializer/Editor 也要同一套 `isDerivedFrom`。

**范围**（刻意小于完整派生图）：

```cpp
// ITypeInfo — optional virtual, default 0
virtual size_t getBaseTypeId() const { return 0; }
virtual void setBaseTypeId(size_t /*baseId*/) {}  // default no-op

// TypeRegistry — walk base chain only
bool isDerivedFrom(const ITypeInfo* type, const ITypeInfo* base);
void setBaseTypeByName(const char* childName, const char* baseName);
```

注册宏（`AY_INHERITS` + `AY_FINALIZE_REGISTRATION_METADATA`）写入 `getBaseTypeId()`；**不做** `getAllDerived()`、多继承图。

**Logia 集成**：新增 `LogiaHostContext::strictInheritance`（默认 false）。当 `kind == Component && hostType != nullptr && strictInheritance == true` 时，analyzer 调 `isDerivedFrom(scriptNameInfo, hostType)`：true → 通过；false → **hard error**（`TypeMismatch`）；script name 不在 registry → 维持 S2.5/LG-03 的 soft warning（strict 不适用）。

#### 明确拒绝的方案

| 方案 | 结论 |
|------|------|
| S3.0 上完整 B（派生图 + 改 FINALIZE 宏） | ❌ 跨模块爆炸，非 LG-03 范围 |
| C：child→base 映射散在 AYScript 调用方 | ❌ 破坏 host 模型集中在校验层 |

**迁移原则**：

1. AST 仍叫 `ScriptDecl`。
2. **S3.0** 只穿 `ctx`，**不**把「ScriptComponent 子类检查」当作迁移项（S2.5 从未实现）。
3. **S3.1** 第一个新 host = System；合法性 = hostKind + registry + 运行时注册。
4. **S3.2** 按需 B-min；完整派生图留 Reflect 独立 backlog。

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

### Phase S2 — Reflect 语义（核心） ✅

- [x] `SemanticAnalyzer` + `TypeRegistry` —— 已接入 AYReflect
- [x] 类型/字段编译期校验
- [x] 单元测试 13 个

### Phase S2.5 — ScriptComponent host 重设计 ✅

- [x] `component` → `script` 重命名
- [x] 删除 `export` 关键字
- [x] `ComponentDecl` → `ScriptDecl`
- [x] 删除 `entity.get_component(...)` 路径（改由 self + World 查询）
- [x] 生命周期参数移除（codegen emit `(self)`；bridge 仍可传 dt）
- [x] `self.field` 语义检查走 AYReflect
- [x] 未知 `script Name` → **soft warning**（非 hard error，支持离线编辑）
- [x] 重写所有测试 + example（169/169 通过）
- [x] **design §1.6**：Logia = host-bound behavior DSL；ScriptComponent = 第一 host

### Phase S3 — 引擎 API、多 host 与工具

#### S3.0 — LG-03：LogiaHostContext API 穿线 ✅ 目标

- [x] 公开 `LogiaHostContext` + `Compiler::compile(source, ctx)`（§5.6）
- [x] `SemanticAnalyzer` 存 `_ctx`；**校验行为与 S2.5 相同**
- [x] 修正 unknown-script warning 文案（去掉虚假的 subclass 暗示）
- [x] +2~3 单元测试：ctx 传递 + 默认 ctx 等价旧路径
- [x] **不动 AYReflect**

#### S3.1 — LG-04：System host

- [x] `script MovementSystem { on_update(dt) }` + GameLoop tick 调度
- [x] 校验：`hostKind` + Reflect 存在 + World 系统注册（**不用 isSubclassOf**）
- [x] `self` 决策 = `ISystem*` lightuserdata（`expectSelf=true`）
- [x] `World::findSystemByName(name)` 公共入口
- [x] System host lifecycle 白名单：`on_destroy` 软警告
- [x] `examples/movement_system.logia` + `unittest/Test_LogiaSystemHost.cpp`（4 用例）

#### S3.2 — LG-04b（可选）：Reflect B-min

- [x] `ITypeInfo::getBaseTypeId()` + `TypeRegistry::isDerivedFrom()`（单链 parent only）
- [x] Component strict 模式：拒绝「已注册非 Component 派生」的 script 名

#### S3.3 — LG-05：`self.field` 走 AYReflect 真实读写

**S3.3 (LG-05) 锁定决策**（2026-07-08 实现完成后补）：

- **范围**：单跳 `self.<primitiveField>`（resolvedField 非空 + resolvedType 字段数为 0）走 reflect 调用；**chained struct 链 `self.position.x` 保留 S2.5 行为**（裸成员访问、运行时 no-op）— 链式 struct 需要递归 reflect、暂不实现。
- **bridge 入口**：`ayt_reflect_get_field(self, "<type>", "<f>")` / `ayt_reflect_set_field(self, "<type>", "<f>", <value>)`。注册为 `lua_CFunction`（`lua_register`），不走 sol2 variadic-template 路径，避免与未来 sol2 类型签名推导交叉。
- **codegen 注入点**：single-hop primitive 时，`emitMemberExpr` 直接输出 `ayt_reflect_get_field(...)`；`emitExprStmt` 处理 `self.field = <r>` 和 `self.field <op>= <r>`（compound 拆分为「读 + 算 + 写」三步）。
- **host type name 来源**：`ScriptDecl::hostTypeName` 由 SemanticAnalyzer 在 resolveScriptName 成功后 stamp，codegen 用作字符串字面量。
- **类型白名单**：int / Int32 / float / Float32 / bool / Bool / double / Float64 / Int64 / UInt32；其他类型 set 失败打印 ayt::log::error 但不崩；get 返回 nil。
- **测试**：`unittest/Test_LogiaReflectRuntime.cpp`（5 用例：helpers 可见、单 hop 读、单 hop 写、codegen 字符串包含 reflect call、未知 type 安全）+ `Test_LogiaSemantic` 中既有 `self.field` 用例保持绿。

#### S3.x — 其余（与 host 正交）

- [x] sol2 usertype：`self.field` 真正走 AYReflect 读写（LG-05 / S3.3）— **见上**
- [ ] 真实 AYInput / AYTime 绑定（替换 mock）
- [ ] 编译缓存、热重载（LG-06）
- [ ] ScriptSubSystem 接入 GameLoop（遍历 ScriptComponent）
- [ ] CLI：`ays-logia compile`（可选）
- [ ] Editor / CLI Tool host（LG-07）

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
| 2026-07-08 | **§1.6 Host 绑定模型**：Logia = host-bound behavior DSL；S2.5 锁 Component host |
| 2026-07-08 | **§5.6 S3 分阶段**：S3.0 API 穿线（不改 Reflect）；S3.1 System host；S3.2 可选 B-min `getBaseTypeId` |
| 2026-07-08 | **S3.1 (LG-04) 完成**：第一个非 Component host——ECS `ISystem`（`script MovementSystem { on_update() { ... } }`）。校验 = `hostKind + Reflect name lookup + World registration`（无 `isSubclassOf`）。`self` = `ISystem*` lightuserdata。`ScriptSubSystem::update/fixedUpdate` 调度。新增 `World::findSystemByName`。`unittest/Test_LogiaSystemHost.cpp`（4 用例）+ `examples/movement_system.logia` |
| 2026-07-08 | **S3.2 (LG-04b) B-min 完成**：Reflect 单链 parent 指针 + `isDerivedFrom()` + Component host strict 模式（`LogiaHostContext::strictInheritance`）。`ITypeInfo::getBaseTypeId()` 默认 0；`AY_FINALIZE_REGISTRATION_METADATA` 写入 `AY_INHERITS` 解析出的 base；strict 模式仅在 `kind==Component && hostType!=nullptr && strictInheritance==true` 时启用，对「已注册但非 hostType 派生」的 script 名产生 hard error。`unittest/ReflectBminTests`（6 用例）+ `Test_LogiaSemantic`（5 用例）。**不**做完整派生图 / `getAllDerived()` / 多继承。 |
| 2026-07-08 | **S3.3 (LG-05) 完成**：`self.<primitiveField>` 单跳走 AYReflect 真实读写。Bridge 注册 `ayt_reflect_get_field` / `ayt_reflect_set_field`（`lua_CFunction`，lua_register）；codegen 单跳 primitive leaf 改发 `ayt_reflect_*_field(self, "<type>", "<f>", ...)`；`ScriptDecl::hostTypeName` 由 SemanticAnalyzer stamp；compound assignment 拆分「读 + 算 + 写」。`unittest/Test_LogiaReflectRuntime.cpp`（5 用例：helpers 可见 / 单跳读 / 单跳写 int+float+bool / codegen 字符串包含 reflect 调用 / 未知 type 安全）。`AYScript_Test` 237/237 PASS。**不**做 struct subfield 链 (`self.position.x`)、non-primitive 字段。 |


---

## 12. 参考

- [`AYShader/design.md`](../AYShader/design.md) — Phoskia 编译器模式
- [`AYFoundation/AYReflect/design.md`](../../AYFoundation/AYReflect/design.md) — 元数据系统
- [`AYEntity/design.md`](../AYEntity/design.md) — ScriptComponent
- [sol2](https://sol2.readthedocs.io/) — Lua C++ 绑定（仅实现层）