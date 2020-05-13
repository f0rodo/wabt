/*
 * Copyright 2020 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This is an experiment and currently extremely limited implementation
 * of the WASI syscall API.  The implementation of the API itself is coming from
 * uvwasi: https://github.com/cjihrig/uvwasi.
 * Most of the code in the file is mostly marshelling data between the wabt
 * interpreter and uvwasi.
 */

#include "src/interp/interp-wasi.h"
#include "src/interp/interp-util.h"

#ifdef WITH_WASI

#include "uvwasi.h"

#include <cinttypes>
#include <unordered_map>

using namespace wabt;
using namespace wabt::interp;

namespace {

// Types that align with WASI specis on size and alignment.
// TODO(sbc): Auto-generate this from witx.
typedef uint32_t __wasi_size_t;
typedef uint32_t __wasi_ptr_t;

_Static_assert(sizeof(__wasi_size_t) == 4, "witx calculated size");
_Static_assert(_Alignof(__wasi_size_t) == 4, "witx calculated align");
_Static_assert(sizeof(__wasi_ptr_t) == 4, "witx calculated size");
_Static_assert(_Alignof(__wasi_ptr_t) == 4, "witx calculated align");

struct __wasi_iovec_t {
  __wasi_ptr_t buf;
  __wasi_size_t buf_len;
};

_Static_assert(sizeof(__wasi_iovec_t) == 8, "witx calculated size");
_Static_assert(_Alignof(__wasi_iovec_t) == 4, "witx calculated align");
_Static_assert(offsetof(__wasi_iovec_t, buf) == 0, "witx calculated offset");
_Static_assert(offsetof(__wasi_iovec_t, buf_len) == 4, "witx calculated offset");

#define __WASI_ERRNO_NOENT (UINT16_C(44))

class WasiInstance {
 public:
  WasiInstance(Instance::Ptr instance,
               uvwasi_s* uvwasi,
               Memory* memory,
               Stream* trace_stream)
      : trace_stream(trace_stream),
        instance(instance),
        uvwasi(uvwasi),
        memory(memory) {}

  Result proc_exit(const Values& params, Values& results, Trap::Ptr* trap) {
    const Value arg0 = params[0];
    uvwasi_proc_exit(uvwasi, arg0.i32_);
    return Result::Ok;
  }

  Result fd_fdstat_get(const Values& params, Values& results, Trap::Ptr* trap) {
    int32_t fd = params[0].i32_;
    if (trace_stream) {
      trace_stream->Writef("fd_fdstat_get %d\n", fd);
    }
    results[0].i32_ = __WASI_ERRNO_NOENT;
    return Result::Ok;
  }

  Result fd_read(const Values& params, Values& results, Trap::Ptr* trap) {
    int32_t fd = params[0].i32_;
    int32_t iovptr = params[1].i32_;
    int32_t iovcnt = params[2].i32_;
    if (trace_stream) {
      trace_stream->Writef("fd_read %d [%d]\n", fd, iovcnt);
    }
    __wasi_iovec_t* wasm_iovs = getMemPtr<__wasi_iovec_t>(iovptr, iovcnt, trap);
    if (!wasm_iovs) {
      return Result::Error;
    }
    uvwasi_iovec_t* iovs = new uvwasi_iovec_t[iovcnt];
    for (int i = 0; i < iovcnt; i++) {
      iovs[i].buf_len = wasm_iovs[i].buf_len;
      iovs[i].buf =
          getMemPtr<uint8_t>(wasm_iovs[i].buf, wasm_iovs[i].buf_len, trap);
      if (!iovs[i].buf) {
        return Result::Error;
      }
    }
    __wasi_ptr_t* out_addr = getMemPtr<__wasi_ptr_t>(params[3].i32_, 1, trap);
    if (!out_addr) {
      return Result::Error;
    }
    results[0].i32_ = uvwasi_fd_read(uvwasi, fd, iovs, iovcnt, out_addr);
    delete[] iovs;
    if (trace_stream) {
      trace_stream->Writef("fd_read -> %d\n", results[0].i32_);
    }
    return Result::Ok;
  }

  Result fd_write(const Values& params, Values& results, Trap::Ptr* trap) {
    int32_t fd = params[0].i32_;
    int32_t iovptr = params[1].i32_;
    int32_t iovcnt = params[2].i32_;
    __wasi_iovec_t* wasm_iovs = getMemPtr<__wasi_iovec_t>(iovptr, iovcnt, trap);
    if (!wasm_iovs) {
      return Result::Error;
    }
    uvwasi_ciovec_t* iovs = new uvwasi_ciovec_t[iovcnt];
    for (int i = 0; i < iovcnt; i++) {
      iovs[i].buf_len = wasm_iovs[i].buf_len;
      iovs[i].buf =
          getMemPtr<uint8_t>(wasm_iovs[i].buf, wasm_iovs[i].buf_len, trap);
      if (!iovs[i].buf) {
        return Result::Error;
      }
    }
    __wasi_ptr_t* out_addr = getMemPtr<__wasi_ptr_t>(params[3].i32_, 1, trap);
    if (!out_addr) {
      return Result::Error;
    }
    results[0].i32_ = uvwasi_fd_write(uvwasi, fd, iovs, iovcnt, out_addr);
    delete[] iovs;
    return Result::Ok;
  }

  Result environ_get(const Values& params, Values& results, Trap::Ptr* trap) {
    uvwasi_size_t environc;
    uvwasi_size_t environ_buf_size;
    uvwasi_environ_sizes_get(uvwasi, &environc, &environ_buf_size);
    uint32_t wasm_buf = params[1].i32_;
    char* buf = getMemPtr<char>(wasm_buf, environ_buf_size, trap);
    if (!buf) {
      return Result::Error;
    }
    char** host_env = new char*[environc];
    uvwasi_environ_get(uvwasi, host_env, buf);

    // Copy host_env pointer array wasm_env)
    for (uvwasi_size_t i = 0; i < environc; i++) {
      uint32_t rel_address = host_env[i] - buf;
      uint32_t dest = params[0].i32_ + (i * sizeof(uint32_t));
      CHECK_RESULT(writeValue<uint32_t>(wasm_buf + rel_address, dest, trap));
    }

    delete[] host_env;
    return Result::Ok;
  }

  Result environ_sizes_get(const Values& params,
                           Values& results,
                           Trap::Ptr* trap) {
    uvwasi_size_t environc;
    uvwasi_size_t environ_buf_size;
    uvwasi_environ_sizes_get(uvwasi, &environc, &environ_buf_size);
    CHECK_RESULT(writeValue<uint32_t>(environc, params[0].i32_, trap));
    CHECK_RESULT(writeValue<uint32_t>(environ_buf_size, params[1].i32_, trap));
    if (trace_stream) {
      trace_stream->Writef("environ_sizes_get -> %d %d\n", environc, environ_buf_size);
    }
    return Result::Ok;
  }

  Result args_get(const Values& params, Values& results, Trap::Ptr* trap) {
    uvwasi_size_t argc;
    uvwasi_size_t arg_buf_size;
    uvwasi_args_sizes_get(uvwasi, &argc, &arg_buf_size);
    uint32_t wasm_buf = params[1].i32_;
    char* buf = getMemPtr<char>(wasm_buf, arg_buf_size, trap);
    if (!buf) {
      return Result::Error;
    }
    char** host_args = new char*[argc];
    uvwasi_args_get(uvwasi, host_args, buf);

    // Copy host_args pointer array wasm_args)
    for (uvwasi_size_t i = 0; i < argc; i++) {
      uint32_t rel_address = host_args[i] - buf;
      uint32_t dest = params[0].i32_ + (i * sizeof(uint32_t));
      CHECK_RESULT(writeValue<uint32_t>(wasm_buf + rel_address, dest, trap));
    }

    delete[] host_args;
    return Result::Ok;
  }

  Result args_sizes_get(const Values& params,
                        Values& results,
                        Trap::Ptr* trap) {
    uvwasi_size_t argc;
    uvwasi_size_t arg_buf_size;
    uvwasi_args_sizes_get(uvwasi, &argc, &arg_buf_size);
    CHECK_RESULT(writeValue<uint32_t>(argc, params[0].i32_, trap));
    CHECK_RESULT(writeValue<uint32_t>(arg_buf_size, params[1].i32_, trap));
    if (trace_stream) {
      trace_stream->Writef("args_sizes_get -> %d %d\n", argc, arg_buf_size);
    }
    return Result::Ok;
  }

  // The trace stream accosiated with the instance.
  Stream* trace_stream;

  Instance::Ptr instance;

 private:
  template <typename T>
  Result writeValue(T value, uint32_t target_address, Trap::Ptr* trap) {
    T* ptr = getMemPtr<T>(target_address, sizeof(T), trap);
    if (!ptr) {
      return Result::Error;
    }
    *ptr = value;
    return Result::Ok;
  }

  template <typename T>
  T* getMemPtr(uint32_t address, uint32_t size, Trap::Ptr* trap) {
    if (!memory->IsValidAccess(address, 0, size * sizeof(T))) {
      *trap = Trap::New(*instance.store(),
                        StringPrintf("out of bounds memory access: "
                                     "[%u, %" PRIu64 ") >= max value %u",
                                     address, u64{address} + size * sizeof(T),
                                     memory->ByteSize()));
      return nullptr;
    }
    return reinterpret_cast<T*>(memory->UnsafeData() + address);
  }

  uvwasi_s* uvwasi;
  // The memory accociated with the instance.  Looked up once on startup
  // and cached here.
  Memory* memory;
};

std::unordered_map<Instance*, WasiInstance*> wasiInstances;

// TODO(sbc): Auto-generate this.

#define WASI_CALLBACK(NAME)                                                 \
  static Result NAME(Thread& thread, const Values& params, Values& results, \
                     Trap::Ptr* trap) {                                     \
    Instance* instance = thread.GetCallerInstance();                        \
    assert(instance);                                                       \
    WasiInstance* wasi_instance = wasiInstances[instance];                  \
    if (wasi_instance->trace_stream) {                                      \
      wasi_instance->trace_stream->Writef(                                  \
          ">>> running wasi function \"%s\":\n", #NAME);                    \
    }                                                                       \
    return wasi_instance->NAME(params, results, trap);                      \
  }

#define WASI_FUNC(NAME) WASI_CALLBACK(NAME)
#include "wasi_api.def"
#undef WASI_FUNC


}  // namespace

namespace wabt {
namespace interp {

Result WasiBindImports(const Module::Ptr& module,
                       RefVec& imports,
                       Stream* stream,
                       Stream* trace_stream) {
  Store* store = module.store();
  for (auto&& import : module->desc().imports) {
    if (import.type.type->kind != ExternKind::Func) {
      stream->Writef("wasi error: invalid import type: %s\n",
                     import.type.name.c_str());
      return Result::Error;
    }

    if (import.type.module != "wasi_snapshot_preview1") {
      stream->Writef("wasi error: unknown module import: `%s`\n",
                     import.type.module.c_str());
      return Result::Error;
    }

    auto func_type = *cast<FuncType>(import.type.type.get());
    auto import_name = StringPrintf("%s.%s", import.type.module.c_str(),
                                    import.type.name.c_str());
    HostFunc::Ptr host_func;

#define WASI_FUNC(NAME)                                 \
  if (import.type.name == #NAME) {                      \
    host_func = HostFunc::New(*store, func_type, NAME); \
    goto found;                                         \
  }
#include "wasi_api.def"
#undef WASI_FUNC

    stream->Writef("unknown `wasi_snapshot_preview1` import: `%s`\n",
                   import.type.name.c_str());
    return Result::Error;
found:
    imports.push_back(host_func.ref());
  }

  return Result::Ok;
}

Result WasiRunStart(const Instance::Ptr& instance,
                    uvwasi_s* uvwasi,
                    Stream* err_stream,
                    Stream* trace_stream) {
  Store* store = instance.store();
  auto module = store->UnsafeGet<Module>(instance->module());
  auto&& module_desc = module->desc();

  Func::Ptr start;
  Memory::Ptr memory;
  for (auto&& export_ : module_desc.exports) {
    if (export_.type.name == "memory") {
      if (export_.type.type->kind != ExternalKind::Memory) {
        err_stream->Writef("wasi error: memory export has incorrect type\n");
        return Result::Error;
      }
      memory = store->UnsafeGet<Memory>(instance->memories()[export_.index]);
    }
    if (export_.type.name == "_start") {
      if (export_.type.type->kind != ExternalKind::Func) {
        err_stream->Writef("wasi error: _start export is not a function\n");
        return Result::Error;
      }
      start = store->UnsafeGet<Func>(instance->funcs()[export_.index]);
    }
    if (start && memory) {
      break;
    }
  }

  if (!start) {
    err_stream->Writef("wasi error: _start export not found\n");
    return Result::Error;
  }

  if (!memory) {
    err_stream->Writef("wasi error: memory export not found\n");
    return Result::Error;
  }

  if (start->type().params.size() || start->type().results.size()) {
    err_stream->Writef("wasi error: invalid _start signature\n");
    return Result::Error;
  }

  // Register memory
  auto* wasi = new WasiInstance(instance, uvwasi, memory.get(), trace_stream);
  wasiInstances[instance.get()] = wasi;

  // Call start ([] -> [])
  Values params;
  Values results;
  Trap::Ptr trap;
  Result res = start->Call(*store, params, results, &trap, trace_stream);
  if (trap) {
    WriteTrap(err_stream, "error", trap);
  }

  // Unregister memory
  wasiInstances.erase(instance.get());
  delete wasi;
  return res;
}

}  // namespace interp
}  // namespace wabt

#endif
