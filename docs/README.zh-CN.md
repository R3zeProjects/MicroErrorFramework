# MicroErrorSystem — 文档

`MicroErrorSystem` 是一个基于 C++23 的 header-only 模块，用于描述错误、
为错误分类，并将错误路由到对应类别的寄存器。对于需要返回值或错误的
操作，模块使用 `std::expected`。

## 功能

- `Error`：包含类别、代码和消息的错误值；
- `IRegister`：错误寄存器接口；
- `CategoryRegister<Category>`：单一类别寄存器的基类；
- `Handler`：根据类别路由错误；
- `Result<T>`：`std::expected<T, Error>` 的别名；
- `vosp::error::predefined` 中提供预定义错误；
- 用于线程安全日志记录的 `ILogger`、`Logger`、`PolicyLogger`、`ILogSink` 和 `ConsoleSink`；
- 具有限制、背压和等待任务取消功能的 `IndustrialWorkerPool`。

## 环境与构建

需要 CMake 3.25 或更高版本、支持 C++23 的编译器，以及支持
`std::expected` 的标准库。

```text
cmake -S MicroErrorSystem -B MicroErrorSystem/cmake-build-debug -DBUILD_TESTING=ON
cmake --build MicroErrorSystem/cmake-build-debug --parallel
ctest --test-dir MicroErrorSystem/cmake-build-debug --output-on-failure
```

## 引入模块

模块是 header-only，只需包含一个头文件：

```cpp
#include "vosp.hpp"

using namespace vosp::error;
```

`vosp.hpp` 是统一的公共入口。为了减少编译时间，也可以分别包含
`vosp_error.hpp` 和 `vosp_logger.hpp`。

## Benchmark 与 Sanitizer

使用 `-DBUILD_BENCHMARKS=ON` 构建 benchmark，它会测量 `MemoryRegister` 的
插入操作。使用 Clang/Ninja 和 `-DENABLE_SANITIZERS=ON` 可以运行
AddressSanitizer 与 UndefinedBehaviorSanitizer 检查。
在使用 Clang 的 Unix 平台上，可以通过 `-DBUILD_FUZZERS=ON` 构建可选的
LibFuzzer 目标。
可选的集成压力测试使用固定版本 `v3.12.0` 的 `nlohmann/json` 解析器作为
外部 workload，不会将其加入运行时 API。

## 创建错误

构造函数依次接收类别、稳定的数值代码和可读消息：

```cpp
const Error error{
    Category::NETWORK,
    1001,
    "Connection refused"
};

std::cout << error.code() << '\n';
std::cout << error.message() << '\n';

if (error.has_category()) {
    // 该错误可以被路由到寄存器。
}
```

错误对象会比较类别、代码和消息三个字段：

```cpp
const Error same{Category::NETWORK, 1001, "Connection refused"};
const bool equal = error == same;
```

## 实现寄存器

`CategoryRegister` 自动提供 `category()`。具体寄存器只需要实现 `add()` 和
`remove()`：

```cpp
class NetworkRegister final : public CategoryRegister<Category::NETWORK>
{
public:
    OperationResult add(const Error& error) override
    {
        errors_.push_back(error);
        return {};
    }

    OperationResult remove(const Error& error) override
    {
        // 生产实现应当从自己的存储中删除 error。
        return {};
    }

private:
    std::vector<Error> errors_;
};
```

寄存器负责管理自己的存储、重复项规则、删除策略和线程安全策略。

## 使用 Handler 路由错误

`Handler` 不拥有寄存器。传入的所有寄存器必须比 `Handler` 存活时间更长。
执行操作时，`Handler` 会选择第一个类别匹配的寄存器：

```cpp
NetworkRegister network;
DatabaseRegister database;

Handler handler{network, database};

const Error error{Category::NETWORK, 1001, "Connection refused"};

const OperationResult result = handler.add(error);
if (!result) {
    // 没有匹配的寄存器，或寄存器拒绝了该错误。
    std::cerr << result.error().message();
}

handler.remove(error);
```

建议每个类别只使用一个寄存器。如果存在多个相同类别的寄存器，将使用第
一个匹配的寄存器。

## 选择执行模式

执行模式通过专用的系统类型别名在编译期选择：

```cpp
using System = MultiThreadedSystem<
    NetworkRegister,
    DatabaseRegister
>;

System system{network, database};
system.add(error);
```

可用模式：

- `SingleThreadedRegister` + `SingleThreadedHandler`：不加锁；
- `MultiThreadedRegister` + `MultiThreadedHandler`：操作使用 mutex 保护；
- `AsyncRegister<Executor>` + `AsyncHandler<Executor>`：操作提交给外部
  executor，并返回 `std::future<OperationResult>`。

executor 示例：

```cpp
class Executor
{
public:
    std::future<OperationResult> submit(std::function<OperationResult()> job)
    {
        return std::async(std::launch::async, std::move(job));
    }
};

using System = AsyncSystem<Executor, NetworkRegister>;

Executor executor;
System system{executor, network};
std::future<OperationResult> operation = system.add(error);
const OperationResult result = operation.get();
```

`AsyncSystem` 不拥有 executor，也不会创建隐藏的线程池。executor 和寄存器
必须一直存活到系统创建的所有 future 完成。

生产环境的异步执行可以使用内置 worker pool：

```cpp
vosp::async::IndustrialWorkerPool pool{4};
using Async = AsyncSystem<decltype(pool), NetworkRegister>;

Async system{pool, network};
std::future<OperationResult> task = system.add(error);
OperationResult result = task.get();
```

线程池复用固定数量的 worker。队列最多包含 1024 个等待任务；队列满时
`submit()` 会等待空间，`clear_queue()` 会用取消错误完成等待任务的 future。
已经开始执行的函数不会被强制中断。
需要协作式取消时，请使用 `submit_cancellable()`，并在任务中检查传入的
`std::stop_token`。

## 使用 Result<T>

当函数需要返回一个值或详细错误时，使用 `Result<T>`：

```cpp
Result<int> read_attempts()
{
    if (/* 数据可用 */ true) {
        return 3;
    }

    return std::unexpected(predefined::database_error);
}

const Result<int> result = read_attempts();
if (result) {
    const int attempts = *result;
} else {
    const Error& error = result.error();
}
```

没有返回值的操作使用 `Result<void>`：

```cpp
Result<void> initialize()
{
    return {};
}
```

## 日志记录

Logger 与寄存器分离：寄存器负责保存错误，logger 负责将事件发布到已连接
的 sink。

```cpp
#include "vosp_logger.hpp"
#include <iostream>

using namespace vosp::logger;

ConsoleSink console{std::cout};
Logger logger{console};

const Error error{Category::NETWORK, 1001, "Connection refused"};
logger.error(error);
```

输出：

```text
[ERROR] [NETWORK] code=1001 message=Connection refused
```

自定义 sink 时需要重写 `ILogSink::write()`。Logger 不拥有 sink，并且可以从
多个线程调用其方法。

## 预定义错误

模块提供以下可直接使用的错误值：

```cpp
using namespace vosp::error::predefined;

const Error& network = network_error;
const Error& database = database_error;
const Error& filesystem = filesystem_error;
const Error& unknown = uncategorized_error;
```

这些对象声明为 `inline const`，调用方不需要负责其生命周期，也可以通过
`std::unexpected` 返回。

## 扩展模块

1. 在 `Category` 中添加新类别。
2. 创建 `CategoryRegister<NewCategory>`。
3. 实现 `add()` 和 `remove()`。
4. 将寄存器传递给 `Handler`。
5. 添加路由测试并同步更新文档。

## 当前限制

- `Handler` 不拥有寄存器，也不负责同步访问；
- `Category::NONE` 不会被路由到专用寄存器；
- `add/remove` 返回 `OperationResult`，可通过 `result.error()` 获取拒绝原因；
- 由于 `Error` 使用 `std::string` 保存消息，当前不使用
  `inline constexpr Error`。
