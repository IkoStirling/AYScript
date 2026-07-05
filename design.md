# AYScript Design

## 1. 概述

AYScript 是 AY Engine 的**脚本层**，负责：
- 多脚本语言支持（Lua/Python/JavaScript 等）
- C++ 引擎 API 绑定到脚本
- 脚本组件系统
- 热更新支持
- 统一元数据框架（避免为每种语言单独写绑定）

### 1.1 设计目标

- **统一框架**：通过元数据驱动 + 代码生成，支持多种脚本语言
- **一次声明，多语言可用**：类型只需声明一次，自动生成各语言绑定
- **脚本组件**：游戏逻辑可用脚本编写，作为组件附加到实体
- **热更新**：运行时重载脚本，保持游戏状态
- **与引擎解耦**：脚本子系统通过统一接口与引擎交互

### 1.2 在引擎中的位置

```
┌─────────────────────────────────────────────────────────────────┐
│                      Engine Modules                             │
├──────────────────────────────────────────────────────────────  │
│                                                                  │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐   │
│  │   AYGameLoop │     │   AYScript   │     │   AYEntity   │   │
│  │  (主循环)     │     │  (脚本层)    │     │ (游戏对象)   │   │
│  └──────┬───────┘     └──────┬───────┘     └──────┬───────┘   │
│         │                    │                      │              │
│         │    onUpdate()     │                      │              │
│         └──────────────────┼──────────────────────┘              │
│                            │                                     │
│         ┌──────────────────┼──────────────────┐                  │
│         ▼                  ▼                  ▼                  │
│  ┌──────────────────────────────────────────────────────┐     │
│  │                   ScriptBridge                         │     │
│  │               (统一绑定接口)                          │     │
│  └──────────────────────────────────────────────────────┘     │
│                            │                                   │
│         ┌──────────────────┼──────────────────┐               │
│         ▼                  ▼                  ▼               │
│  ┌───────────┐      ┌───────────┐      ┌───────────┐        │
│  │  LuaBridge │      │PyBridge   │      │  JSBridge  │        │
│  └───────────┘      └───────────┘      └───────────┘        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 模块边界

| 模块 | 职责 | 与其他模块关系 |
|------|------|---------------|
| **AYScript** | 脚本生命周期、VM 管理 | 持有 `IAYScriptBridge`，调用引擎 API |
| **AYEntity** | 游戏对象管理 | 脚本组件附加到实体，被脚本操作 |
| **AYReflect** | 元数据系统 | 提供类型信息，供绑定层使用 |
| **AYNetwork** | 网络通信 | 脚本可发送网络消息 |
| **AYGameLoop** | 主循环 | 脚本子系统在 GameLoop 中 update |

---

## 2. 元数据系统 (AYReflect)

### 2.1 核心概念

```
┌─────────────────────────────────────────────────────────────┐
│                 C++ 类型声明 (单一来源)                        │
│                                                             │
│  struct Transform {                                         │
│      Vector3 position;                                     │
│      Quaternion rotation;                                   │
│      Vector3 scale;                                         │
│                                                             │
│      AYTYPE_DECLARE(Transform);  // ← 声明元数据            │
│  };                                                        │
└─────────────────────────────────────────────────────────────┘
                          │
                          ▼ 代码生成
┌─────────────────────────────────────────────────────────────┐
│                 自动生成的绑定代码                           │
│                                                             │
│  Transform_lua.cpp    → Lua 绑定                            │
│  Transform_python.cpp → Python 绑定                        │
│  Transform_js.cpp    → JS 绑定                            │
│  Transform_reflection.cpp → 运行时反射                     │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 类型信息基类

```cpp
namespace ayt::reflect
{

// 类型信息基类
struct TypeInfo {
    const char* name;
    size_t id;              // 唯一标识
    size_t size;            // 实例大小
    size_t fieldCount;      // 字段数量
    
    // 基础操作
    virtual void* create() = 0;
    virtual void destroy(void* obj) = 0;
    virtual void copy(void* dest, const void* src) = 0;
    
    // 序列化（用于网络复制、存档）
    virtual void serialize(BitStream& stream, const void* obj) = 0;
    virtual void deserialize(void* obj, BitStream& stream) = 0;
    
    // 字段迭代
    virtual FieldInfo* getField(size_t index) = 0;
    virtual FieldInfo* findField(const char* name) = 0;
};

// 字段信息
struct FieldInfo {
    const char* name;           // 字段名
    TypeInfo* type;             // 字段类型
    size_t offset;              // 内存偏移
    
    // 通用访问
    void* get(void* obj) const { return (uint8_t*)obj + offset; }
    const void* get(const void* obj) const { return (uint8_t*)obj + offset; }
    
    // 类型转换
    template<typename T>
    T* as() { return static_cast<T*>(get(nullptr)); }
};
```

### 2.3 类型注册表

```cpp
class TypeRegistry {
public:
    static TypeRegistry& instance();
    
    // 注册类型
    template<typename T>
    TypeInfo* registerType(const char* name) {
        auto* info = new TypeInfoImpl<T>(name);
        _types[name] = info;
        _typesById[info->id] = info;
        return info;
    }
    
    // 查询
    TypeInfo* findType(const char* name) const;
    TypeInfo* findType(size_t id) const;
    const std::vector<TypeInfo*>& getAllTypes() const;
    
private:
    std::unordered_map<std::string, TypeInfo*> _types;
    std::unordered_map<size_t, TypeInfo*> _typesById;
};
```

### 2.4 类型声明宏

```cpp
// 声明类型（放在头文件）
#define AYTYPE_DECLARE(T) \
    static ayt::reflect::TypeInfo* getTypeInfo() { \
        static ayt::reflect::TypeInfoImpl<T> _info{#T}; \
        return &_info; \
    } \
    static ayt::reflect::TypeInfo* _getTypeInfo() { return getTypeInfo(); }

// 声明字段（放在实现文件）
#define AYTYPE_FIELD(name, field) \
    { #name, offsetof(T, field), sizeof(((T*)nullptr)->field) }

#define AYTYPE_FIELD_VEC3(name, field) \
    { #name, offsetof(T, field), sizeof(((T*)nullptr)->field), &Vec3::getTypeInfo() }

// 字段块
#define AYTYPE_BEGIN_FIELDS(T) \
    static std::vector<FieldInfo> _getFields() { \
        return std::vector<FieldInfo>{ 

#define AYTYPE_END_FIELDS(T) \
        }; \
    }

// 使用示例
struct Transform {
    Vector3 position;
    Quaternion rotation;
    Vector3 scale;
    
    AYTYPE_DECLARE(Transform);
};

// 在 .cpp 中
AYTYPE_BEGIN_FIELDS(Transform)
    AYTYPE_FIELD_VEC3("position", position),
    AYTYPE_FIELD_VEC3("rotation", rotation),
    AYTYPE_FIELD_VEC3("scale", scale),
AYTYPE_END_FIELDS(Transform)
```

### 2.5 内置类型支持

```cpp
// 自动为内置类型注册元数据
AYTYPE_REGISTER(Vector3);      // 位置、缩放
AYTYPE_REGISTER(Quaternion);  // 旋转
AYTYPE_REGISTER(Matrix4x4);    // 变换矩阵
AYTYPE_REGISTER(Color);        // 颜色
AYTYPE_REGISTER(int8_t);
AYTYPE_REGISTER(int16_t);
AYTYPE_REGISTER(int32_t);
AYTYPE_REGISTER(int64_t);
AYTYPE_REGISTER(uint8_t);
AYTYPE_REGISTER(uint16_t);
AYTYPE_REGISTER(uint32_t);
AYTYPE_REGISTER(uint64_t);
AYTYPE_REGISTER(float);
AYTYPE_REGISTER(double);
AYTYPE_REGISTER(bool);
AYTYPE_REGISTER(std::string);
```

---

## 3. 统一绑定接口

### 3.1 IAYScriptBridge

```cpp
class IAYScriptBridge {
public:
    virtual ~IAYScriptBridge() = default;
    
    // ===== 脚本执行 =====
    virtual void call(const char* function, ...) = 0;
    virtual void exec(const char* script) = 0;
    virtual void execFile(const char* path) = 0;
    
    // ===== 类型注册 =====
    virtual void registerType(ayt::reflect::TypeInfo* type) = 0;
    virtual void registerGlobalFunction(const char* name, auto&& func) = 0;
    virtual void registerGlobalConstant(const char* name, auto&& value) = 0;
    
    // ===== 对象生命周期 =====
    virtual void* createObject(const char* typeName) = 0;
    virtual void destroyObject(void* obj) = 0;
    virtual bool isValid(void* obj) const = 0;
    
    // ===== 字段访问 =====
    virtual void setField(void* obj, const char* field, auto&& value) = 0;
    virtual auto getField(void* obj, const char* field) = 0;
    
    // ===== 表/对象操作 =====
    virtual void setGlobal(const char* name, void* value) = 0;
    virtual void* getGlobal(const char* name) = 0;
    
    // ===== 环境 =====
    virtual void collectGarbage() = 0;
    virtual const char* getLanguageName() const = 0;
};
```

### 3.2 各语言实现

```cpp
// Lua 绑定 (sol2)
class LuaBridge : public IAYScriptBridge {
public:
    const char* getLanguageName() const override { return "Lua"; }
    void registerType(ayt::reflect::TypeInfo* type) override;
    void call(const char* func, ...) override;
    // ...
private:
    sol::state _lua;
};

// Python 绑定 (pybind11)
class PythonBridge : public IAYScriptBridge {
public:
    const char* getLanguageName() const override { return "Python"; }
    void registerType(ayt::reflect::TypeInfo* type) override;
    void call(const char* func, ...) override;
    // ...
private:
    py::object _mainModule;
};

// JavaScript 绑定 (QuickJS/Goja)
class JSBridge : public IAYScriptBridge {
public:
    const char* getLanguageName() const override { return "JavaScript"; }
    void registerType(ayt::reflect::TypeInfo* type) override;
    void call(const char* func, ...) override;
    // ...
private:
    JSContext* _ctx;
};
```

---

## 4. 代码生成器

### 4.1 生成器架构

```
┌─────────────────────────────────────────────────────────────┐
│                    TypeScanner                              │
│           扫描 C++ 源码，提取 AYTYPE_* 声明                   │
└─────────────────────────────┬───────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    TypeDatabase                             │
│              JSON/YAML 存储所有类型的元数据                   │
└─────────────────────────────┬───────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│ LuaGenerator     │ │ PythonGenerator │ │ JSGenerator     │
│                 │ │                 │ │                 │
│ 生成 *_lua.cpp  │ │ 生成 *_py.cpp   │ │ 生成 *_js.cpp   │
└─────────────────┘ └─────────────────┘ └─────────────────┘
```

### 4.2 生成器输入

```yaml
# generated/types.yaml
types:
  - name: Transform
    fields:
      - { name: position, type: Vector3 }
      - { name: rotation, type: Quaternion }
      - { name: scale, type: Vector3 }
    
  - name: HealthComponent
    fields:
      - { name: hp, type: int32 }
      - { name: maxHp, type: int32 }
      - { name: regenRate, type: float }

  - name: PlayerController
    fields:
      - { name: speed, type: float }
      - { name: jumpForce, type: float }
      - { name: transform, type: Transform }
```

### 4.3 Lua 绑定生成

```cpp
// generated/Transform_lua.cpp

static void register_Transform_lua(sol::state& lua) {
    sol::usertype<Transform> ut = lua.create_usertype<Transform>();
    
    ut["position"] = sol::property(
        [](Transform& t) { return t.position; },
        [](Transform& t, const Vector3& v) { t.position = v; }
    );
    ut["rotation"] = sol::property(
        [](Transform& t) { return t.rotation; },
        [](Transform& t, const Quaternion& q) { t.rotation = q; }
    );
    ut["scale"] = sol::property(
        [](Transform& t) { return t.scale; },
        [](Transform& t, const Vector3& v) { t.scale = v; }
    );
    
    // 工厂函数
    lua["Transform"] = sol::factories([]() {
        return new Transform();
    });
}

AYTREGISTER_LUA(Transform);
```

### 4.4 Python 绑定生成

```cpp
// generated/Transform_python.cpp

static void register_Transform_python(py::module& m) {
    py::class_<Transform>(m, "Transform")
        .def_property("position",
            &Transform::getPosition, &Transform::setPosition)
        .def_property("rotation",
            &Transform::getRotation, &Transform::setRotation)
        .def_property("scale",
            &Transform::getScale, &Transform::setScale)
        .def(py::init<>());
}

AYTREGISTER_PYTHON(Transform);
```

### 4.5 JavaScript 绑定生成

```cpp
// generated/Transform_js.cpp

static void register_Transform_js(JSContext* ctx) {
    JSValue proto = JS_NewObjectProto(ctx, JS_NULL);
    
    JS_SetPropertyStr(ctx, proto, "position",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
            // getter/setter
        }, "position", 0));
    
    // 构造函数
    JS_SetPropertyStr(ctx, ctx->globalObject, "Transform",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
            return JS_NewObjectProto(ctx, proto);
        }, "Transform", 0));
}

AYTREGISTER_JS(Transform);
```

---

## 5. 脚本子系统

### 5.1 IScriptSubSystem

```cpp
class IScriptSubSystem : public ISubSystem {
public:
    // 设置脚本语言
    virtual void setLanguage(const char* language) = 0;
    virtual const char* getLanguage() const = 0;
    
    // 绑定接口
    virtual IAYScriptBridge* getBridge() = 0;
    
    // 核心类型注册
    virtual void registerCoreTypes() = 0;
    virtual void registerType(const char* typeName) = 0;
    
    // 脚本文件加载
    virtual void loadScript(const char* path) = 0;
    virtual void reloadScript(const char* path) = 0;
    virtual bool scriptExists(const char* path) const = 0;
    
    // 热更新
    virtual void enableHotReload(bool enable) = 0;
    virtual void reloadModified() = 0;
    
    // 脚本组件
    virtual void registerScriptComponent(const char* name, 
        std::function<void*(Entity*)> factory) = 0;
};
```

### 5.2 ScriptSubSystem 实现

```cpp
class ScriptSubSystem : public IScriptSubSystem {
public:
    bool initialize() override {
        // 初始化 VM
        _bridge = createBridge(getLanguage());
        _bridge->collectGarbage();
        
        // 注册核心类型
        registerCoreTypes();
        
        // 加载启动脚本
        loadScript("scripts/init.lua");
        
        return true;
    }
    
    void update(float deltaTime) override {
        // 调用脚本 update
        _bridge->call("onUpdate", deltaTime);
        
        // 热更新检查
        if (_hotReloadEnabled) {
            checkModifiedScripts();
        }
    }
    
    void registerCoreTypes() override {
        auto& registry = ayt::reflect::TypeRegistry::instance();
        for (auto* type : registry.getAllTypes()) {
            _bridge->registerType(type);
        }
    }
    
private:
    std::unique_ptr<IAYScriptBridge> _bridge;
    std::string _language = "lua";
    bool _hotReloadEnabled = false;
    std::unordered_map<std::string, std::filesystem::file_time_type> _scriptTimes;
};
```

---

## 6. 脚本组件系统

### 6.1 脚本组件接口

```cpp
// 脚本组件 - 作为组件附加到实体，但行为由脚本定义
class ScriptComponent : public IComponent {
public:
    const char* getName() const override { return "Script"; }
    
    void onAttach(Entity* entity) override {
        // 调用脚本的 onStart
        _bridge->call(_scriptName + ".onStart", entity);
    }
    
    void onUpdate(float deltaTime) override {
        // 调用脚本的 onUpdate
        _bridge->call(_scriptName + ".onUpdate", deltaTime);
    }
    
    void onDetach() override {
        // 调用脚本的 onDestroy
        _bridge->call(_scriptName + ".onDestroy");
    }
    
    void setScript(const char* scriptName) {
        _scriptName = scriptName;
    }
    
private:
    std::string _scriptName;
    IAYScriptBridge* _bridge;  // 从 ScriptSubSystem 获取
};
```

### 6.2 脚本组件注册

```cpp
// 游戏项目注册脚本组件
class MyGame : public IApplication {
public:
    void registerSubSystems() override {
        auto& script = GameLoop::instance().getScript();
        
        // 注册脚本组件
        script.registerScriptComponent("PlayerAI", [](Entity* e) {
            return new ScriptComponent(e, "PlayerAI");
        });
        
        script.registerScriptComponent("EnemyAI", [](Entity* e) {
            return new ScriptComponent(e, "EnemyAI");
        });
    }
};
```

### 6.3 脚本中使用实体

```lua
-- PlayerAI.lua

function onStart(entity)
    -- 获得实体引用
    self.entity = entity
    
    -- 获得组件
    self.transform = entity:getComponent("Transform")
    self.health = entity:getComponent("Health")
end

function onUpdate(dt)
    -- AI 逻辑
    local pos = self.transform.position
    pos.x = pos.x + self.speed * dt
    self.transform.position = pos
    
    -- 检测死亡
    if self.health.hp <= 0 then
        self:destroy()
    end
end

function onDestroy()
    -- 清理
    print("PlayerAI destroyed")
end
```

---

## 7. 热更新系统

### 7.1 热更新接口

```cpp
class HotReloadSystem {
public:
    // 启用热更新
    void enable(const char* scriptDir);
    void disable();
    
    // 检查并重载修改的文件
    void checkModified();
    
    // 强制重载指定脚本
    void reload(const char* scriptName);
    
    // 重载并保留状态
    void reloadWithState(const char* scriptName, ScriptState& state);
    
    // 回调
    using ReloadCallback = std::function<void(const char* scriptName, bool success)>;
    void onReload(ReloadCallback callback);
};
```

### 7.2 状态保留机制

```cpp
// 热更新时保留的状态
struct ScriptState {
    std::string scriptName;
    void* entity;                    // 关联的实体
    std::vector<uint8_t> data;      // 序列化状态
};

// 重载时
void HotReloadSystem::reloadWithState(const char* scriptName, ScriptState& state) {
    // 1. 序列化旧实例状态
    serializeState(state.data);
    
    // 2. 重载脚本
    loadScript(scriptName);
    
    // 3. 反序列化到新实例
    deserializeState(state.data);
    
    // 4. 调用 onReload
    callScriptFunction(scriptName, "onReload");
}
```

### 7.3 文件监视

```cpp
// 基于 AYConfig 的 FileWatcher
class FileWatcher {
public:
    using Callback = std::function<void(const std::filesystem::path&)>;
    
    void watch(const std::filesystem::path& dir, Callback onModified) {
        // 使用 OS 原生文件监视 API
        // Windows: ReadDirectoryChangesW
        // Linux: inotify
        // macOS: FSEvents
    }
};
```

---

## 8. 核心 API 绑定

### 8.1 引擎全局函数

```cpp
// 注册到所有脚本语言
bridge->registerGlobalFunction("print", [](const char* msg) {
    std::cout << msg << std::endl;
});

bridge->registerGlobalFunction("log", [](LogLevel level, const char* msg) {
    AY_LOG(level, "%s", msg);
});

// 时间
bridge->registerGlobalFunction("getTime", []() {
    return AYClock::now();
});

bridge->registerGlobalFunction("getDeltaTime", []() {
    return AYGameLoop::instance().getDeltaTime();
});

// 实体创建
bridge->registerGlobalFunction("createEntity", []() {
    return AYEntity::create();
});

bridge->registerGlobalFunction("destroyEntity", [](Entity* e) {
    e->destroy();
});
```

### 8.2 Entity 绑定

```lua
-- Lua 中使用 Entity
local entity = createEntity()
entity:setName("Player")
entity:setPosition(0, 0, 0)

local transform = entity:getComponent("Transform")
transform.position = Vector3.new(1, 2, 3)

local mesh = entity:addComponent("Mesh")
mesh.meshName = "player.fbx"

entity:destroy()
```

### 8.3 组件绑定

```lua
-- 添加脚本组件
local ai = entity:addComponent("Script", { scriptName = "PlayerAI" })

-- 访问组件
local health = entity:getComponent("Health")
health.hp = 100
health.maxHp = 100

-- 移除组件
entity:removeComponent("Health")
```

### 8.4 事件系统绑定

```lua
-- 订阅事件
subscribe("PlayerDamaged", function(data)
    print("Player " .. data.playerId .. " took " .. data.damage .. " damage")
end)

-- 发布事件
emit("PlayerDamaged", { playerId = 1, damage = 25 })

-- 延迟调用
delay(1.0, function()
    print("Delayed call")
end)
```

---

## 9. 目录结构

```
AYScript/
├── design.md
├── CMakeLists.txt
│
├── interface/
│   ├── IAYScript.h                # 脚本子系统接口
│   ├── IAYScriptBridge.h          # 统一绑定接口
│   └── IReflect.h                 # 反射系统接口
│
├── include/
│   ├── AYScript.h                 # 主入口
│   ├── AYReflect.h                # 反射系统
│   ├── TypeRegistry.h             # 类型注册表
│   │
│   └── detail/
│       ├── TypeInfo.h             # 类型信息
│       ├── FieldInfo.h            # 字段信息
│       └── Macros.h               # 声明宏
│
├── src/
│   ├── AYScriptSubSystem.cpp      # 子系统实现
│   ├── TypeRegistry.cpp
│   │
│   └── bridges/
│       ├── LuaBridge.cpp          # Lua 绑定
│       ├── PythonBridge.cpp       # Python 绑定
│       └── JSBridge.cpp          # JS 绑定
│
└── tools/
    └── codegen/
        ├── scanner.py             # 类型扫描器
        ├── generator.py           # 代码生成器
        └── templates/             # 模板文件
            ├── lua_bind.template
            ├── python_bind.template
            └── js_bind.template
```

---

## 10. 构建系统集成

### 10.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)

project(AYScript)

add_library(${PROJECT_NAME} SUBSYSTEM)

target_sources(${PROJECT_NAME} PRIVATE
    src/AYScriptSubSystem.cpp
    src/TypeRegistry.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    AYCore
    AYGameLoop
    AYReflect  # 反射系统
)

# 脚本语言选项
option(AY_SCRIPT_ENABLE_LUA "Enable Lua support" ON)
option(AY_SCRIPT_ENABLE_PYTHON "Enable Python support" OFF)
option(AY_SCRIPT_ENABLE_JS "Enable JavaScript support" OFF)

if(AY_SCRIPT_ENABLE_LUA)
    find_package(Lua REQUIRED)
    target_sources(${PROJECT_NAME} PRIVATE src/bridges/LuaBridge.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LUA_LIBRARIES})
endif()

if(AY_SCRIPT_ENABLE_PYTHON)
    find_package(Python REQUIRED)
    target_sources(${PROJECT_NAME} PRIVATE src/bridges/PythonBridge.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${Python_LIBRARIES})
endif()

if(AY_SCRIPT_ENABLE_JS)
    target_sources(${PROJECT_NAME} PRIVATE src/bridges/JSBridge.cpp)
    # 使用 Goja 或 QuickJS
endif()
```

### 10.2 代码生成集成

```cmake
# 运行代码生成器
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/types.yaml
    COMMAND python ${CMAKE_SOURCE_DIR}/tools/codegen/scanner.py
        --source-dir ${CMAKE_SOURCE_DIR}/AYRuntime
        --output ${CMAKE_BINARY_DIR}/generated/types.yaml
    DEPENDS 
        ${CMAKE_SOURCE_DIR}/tools/codegen/scanner.py
        ${CMAKE_SOURCE_DIR}/AYRuntime
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/lua_bindings.cpp
    COMMAND python ${CMAKE_SOURCE_DIR}/tools/codegen/generator.py
        --input ${CMAKE_BINARY_DIR}/generated/types.yaml
        --language lua
        --output ${CMAKE_BINARY_DIR}/generated/lua_bindings.cpp
    DEPENDS 
        ${CMAKE_BINARY_DIR}/generated/types.yaml
        ${CMAKE_SOURCE_DIR}/tools/codegen/generator.py
)

add_custom_target(AYScriptCodegen ALL
    DEPENDS ${CMAKE_BINARY_DIR}/generated/lua_bindings.cpp
)
```

---

## 11. 实现优先级

### Phase 1: 反射系统
- [ ] TypeInfo / FieldInfo 基类
- [ ] TypeRegistry 注册表
- [ ] AYTYPE_DECLARE / AYTYPE_FIELD 宏
- [ ] 内置类型注册

### Phase 2: 统一绑定接口
- [ ] IAYScriptBridge 接口
- [ ] LuaBridge 实现
- [ ] PythonBridge 实现
- [ ] JSBridge 实现

### Phase 3: 脚本子系统
- [ ] ScriptSubSystem 子系统
- [ ] 核心 API 绑定
- [ ] Entity 绑定
- [ ] 组件绑定

### Phase 4: 脚本组件
- [ ] ScriptComponent 实现
- [ ] 生命周期钩子
- [ ] 脚本组件注册

### Phase 5: 热更新
- [ ] FileWatcher
- [ ] 状态序列化
- [ ] 增量重载

### Phase 6: 代码生成
- [ ] TypeScanner
- [ ] LuaGenerator
- [ ] PythonGenerator
- [ ] JSGenerator

---

## 12. 与工业级引擎对比

| 功能 | AYScript | Unity (C#) | Unreal (Blueprints) | O3DE (Lua) |
|------|----------|------------|---------------------|------------|
| 多语言支持 | 规划 | C# only | C++/Blueprints | Lua |
| 元数据系统 | ✅ | ✅ (反射) | ✅ | ❌ |
| 代码生成 | 规划 | ✅ (IDE) | ✅ | ❌ |
| 脚本组件 | 规划 | ✅ | ✅ | ✅ |
| 热更新 | 规划 | ✅ | ✅ (C++) | ✅ |
| 统一绑定 | 规划 | N/A | N/A | ❌ |
| 编辑器集成 | 规划 | ✅ | ✅ | ❌ |

---

## 13. 参考

- [sol2 - Lua C++ Binding](https://sol2.readthedocs.io/)
- [pybind11 - Python C++ Binding](https://pybind11.readthedocs.io/)
- [Goja - JavaScript in Go](https://github.com/ericfreese/goja)
- [Unity Scripting](https://docs.unity3d.com/Packages/com.unity.scriptingcsharp@latest/)
- [Unreal Reflection System](https://docs.unrealengine.com/en-US/ProgrammingAndScripting/GameplaySystems/Framework/Timestamp/)
- [AutoCodeGen in O3DE](https://o3de.org/docs/)