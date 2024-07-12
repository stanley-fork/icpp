/* Interpreting C++, executing the source and executable like a script */
/* By Jesse Liu < neoliu2011@gmail.com >, 2024 */
/* Copyright (c) vpand.com 2024. This file is released under LGPL2.
   See LICENSE in root directory for more details
*/

#include "loader.h"
#include "exec.h"
#include "log.h"
#include "object.h"
#include "platform.h"
#include "runcfg.h"
#include "runtime.h"
#include <cstdio>
#include <iostream>
#include <llvm/Config/config.h>
#include <locale>
#include <map>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <unordered_map>

namespace icpp {

// some simulated system global variables
static uint64_t __dso_handle = 0;

#if __linux__ && __x86_64__
extern "C" {
void __divti3(void);
void __modti3(void);
void __udivti3(void);
void __umodti3(void);
}
#endif

struct ModuleLoader {
  ModuleLoader() : mainid_(std::this_thread::get_id()) {
    // these symbols are extern in object but finally linked in exe/lib,
    // herein simulates this behaviour
    syms_.insert({"___dso_handle", &__dso_handle});
#if __linux__ && __x86_64__
    syms_.insert({"__divti3", reinterpret_cast<const void *>(&__divti3)});
    syms_.insert({"__modti3", reinterpret_cast<const void *>(&__modti3)});
    syms_.insert({"__udivti3", reinterpret_cast<const void *>(&__udivti3)});
    syms_.insert({"__umodti3", reinterpret_cast<const void *>(&__umodti3)});
#endif

    // load c++ runtime library
    auto libcxx = fs::absolute(RunConfig::inst()->program).parent_path() /
                  "libc++" LLVM_PLUGIN_EXT;
    loadLibrary(libcxx.string());

    // initialize the symbol hashes for the third-party modules lazy loading
    if (fs::exists(RuntimeLib::inst().repo(false)))
      RuntimeLib::inst().initHashes();
  }

  ~ModuleLoader() {}

  void cacheAndClean(int exitcode) {
    // generate the iobject module cache if everything went well
    if (!exitcode) {
      for (auto io : imods_) {
        if (io->isCache())
          continue;
        io->generateCache();
      }
    }
    imods_.clear();
  }

  void cacheSymbol(std::string_view name, const void *impl) {
    syms_.insert({name.data(), impl});
  }

  bool isMain() { return mainid_ == std::this_thread::get_id(); }

  struct LockGuard {
    LockGuard(ModuleLoader *p, std::recursive_mutex &m)
        : parent_(p), mutex_(m) {
      if (!parent_->isMain())
        mutex_.lock();
    }

    ~LockGuard() {
      if (!parent_->isMain())
        mutex_.unlock();
    }

    ModuleLoader *parent_;
    std::recursive_mutex &mutex_;
  };

  const void *loadLibrary(std::string_view path);
  const void *resolve(const void *handle, std::string_view name, bool data);
  const void *resolve(std::string_view name, bool data);
  std::string find(const void *addr, bool update);

  // it'll be invoked by execute engine when loaded a iobject module
  void cacheObject(std::shared_ptr<Object> imod);

  bool executable(uint64_t vm, Object **iobject) {
    for (auto m : imods_) {
      if (m->executable(vm, nullptr)) {
        iobject[0] = m.get();
        return true;
      }
    }
    return false;
  }

  bool belong(uint64_t vm) {
    for (auto m : imods_) {
      if (m->belong(vm))
        return true;
    }
    return false;
  }

private:
  const void *lookup(std::string_view name, bool data);
#if __linux__
  friend int iter_so_callback(dl_phdr_info *info, size_t size, void *data);
#endif

  std::thread::id mainid_;
  std::recursive_mutex mutex_;

  // cached symbols
  std::unordered_map<std::string, const void *> syms_;

  // native modules
  std::map<uint64_t, std::string> mods_;
  std::vector<std::map<uint64_t, std::string>::iterator> modits_;
  std::map<std::string, const void *> mhandles_;

  // iobject modules
  std::vector<std::shared_ptr<Object>> imods_;
};

// the module/object loader
static std::unique_ptr<ModuleLoader> moloader;

const void *ModuleLoader::loadLibrary(std::string_view path) {
  LockGuard lock(this, mutex_);
  auto found = mhandles_.find(path.data());
  if (found == mhandles_.end()) {
    auto addr = load_library(path.data());
    if (!addr) {
      if (path.ends_with(obj_ext) || path.ends_with(iobj_ext)) {
        // check the already loaded/cached iobject module
        auto found = mhandles_.find(path.data());
        if (found != mhandles_.end()) {
          return found->second;
        }

        auto object = create_object("", path);
        if (object && object->valid()) {
          // initialize this iobject module, call its construction functions,
          // it'll call the Loader::cacheObject after executing the ctors
          init_library(object);
          addr = object.get();
        }
      }
      if (!addr) {
        log_print(Runtime, "Failed to load library: {}", path.data());
        return nullptr;
      }
    }
    if (addr)
      log_print(Develop, "Loaded module {}.", path.data());
    found = mhandles_.insert({path.data(), addr}).first;
  }
  return found->second;
}

const void *ModuleLoader::resolve(const void *handle, std::string_view name,
                                  bool data) {
  LockGuard lock(this, mutex_);
  auto found = syms_.find(name.data());
  if (found != syms_.end()) {
    return data ? &found->second : found->second;
  }

  const void *target = nullptr;

  // check it in iobject modules
  for (auto io : imods_) {
    if (handle != io.get())
      continue;
    auto t = io->locateSymbol(name);
    if (t) {
      target = t;
      break;
    }
  }

  // check it in native modules
  if (!target) {
    target = find_symbol(const_cast<void *>(handle), name);
  }

  if (!target)
    return nullptr;
  found = syms_.insert({name.data(), target}).first;
  return data ? &found->second : found->second;
}

const void *ModuleLoader::resolve(std::string_view name, bool data) {
  LockGuard lock(this, mutex_);
  auto found = syms_.find(name.data());
  if (found != syms_.end()) {
    return data ? &found->second : found->second;
  }
  return lookup(name, data);
}

const void *ModuleLoader::lookup(std::string_view name, bool data) {
  // load boost libraries lazily
  static bool boost = false;
  if (!boost && name.find("boost") != std::string_view::npos) {
    boost = true;

    for (auto &entry : fs::recursive_directory_iterator(
             fs::absolute(RunConfig::inst()->program).parent_path() / ".." /
             "lib" / "boost")) {
      auto libpath = entry.path();
      if (entry.is_regular_file() && libpath.extension() == LLVM_PLUGIN_EXT) {
        if (!load_library(libpath.string()))
          log_print(Runtime, "Failed load boost library: {}.",
                    libpath.string());
      }
    }
  }

  const void *target = nullptr;

  // check it in iobject modules
  for (auto io : imods_) {
    auto t = io->locateSymbol(name);
    if (t) {
      target = t;
      break;
    }
  }

  // check it in loaded modules
  for (auto &m : mhandles_) {
    if ((target = find_symbol(m.second, name)))
      break;
  }

  // check it in native system modules
  if (!target)
    target = find_symbol(nullptr, name);

  if (!target) {
    // the final chance to resolve this symbol
    auto path = RuntimeLib::inst().find(name);
    if (!path.empty()) {
      auto handle = loadLibrary(path.string());
      target = resolve(handle, name, data);
    }
    // Oops...
    if (!target) {
      log_print(Runtime,
                "Fatal error, failed to resolve symbol {}, redirect to abort.",
                name.data());
      target = reinterpret_cast<const void *>(&abort);
    }
  }

  // cache it
  auto newit = syms_.insert({name.data(), target}).first;
  return data ? &newit->second : newit->second;
}

std::string ModuleLoader::find(const void *addr, bool update) {
  if (mods_.size() == 0 || update) {
    LockGuard lock(this, mutex_);
    iterate_modules([](uint64_t base, std::string_view path) {
      moloader->mods_.insert({base, path.data()});
      return false;
    });
    // reset module iterators
    modits_.clear();
    for (auto it = mods_.begin(); it != mods_.end(); it++) {
      modits_.push_back(it);
    }
  }
  // check it in iobject module
  for (auto m : imods_) {
    if (m->belong(reinterpret_cast<uint64_t>(addr))) {
      return m->cachePath();
    }
  }
  // binary search for the module
  long low = 0;
  long high = modits_.size() - 1;
  auto target = reinterpret_cast<uint64_t>(addr);
  while (low <= high) {
    auto mid = (low + high) / 2;
    if (mid + 1 == modits_.size()) {
      return modits_[mid]->second.data();
    }
    auto base0 = modits_[mid]->first;
    auto base1 = modits_[mid + 1]->first;
    if (base0 <= target && target < base1) {
      // if target is between base0 and base1, we think it belongs to base0
      return modits_[mid]->second.data();
    }
    if (base0 > target) {
      // back forward
      high = mid - 1;
    } else {
      // go forward
      low = mid + 1;
    }
  }
  return "";
}

void ModuleLoader::cacheObject(std::shared_ptr<Object> imod) {
  if (mhandles_.find(imod->path().data()) != mhandles_.end())
    return;
  imods_.push_back(imod);
  mhandles_.insert({imod->path().data(), reinterpret_cast<void *>(imod.get())});
}

void Loader::initialize() {
  if (!moloader)
    moloader = std::make_unique<ModuleLoader>();
}

void Loader::deinitialize(int exitcode) {
  if (!moloader)
    return;
  moloader->cacheAndClean(exitcode);
}

Loader::Loader(Object *object, const std::vector<std::string> &deps)
    : object_(object) {
  for (auto &m : deps) {
    moloader->loadLibrary(m);
  }
}

Loader::Loader(std::string_view module) {
  handle_ = moloader->loadLibrary(module);
}

Loader::~Loader() {}

bool Loader::valid() { return object_ || handle_; }

const void *Loader::locate(std::string_view name, bool data) {
  return moloader->resolve(handle_, name, data);
}

const void *Loader::locateSymbol(std::string_view name, bool data) {
  return moloader->resolve(name, data);
}

std::string Loader::locateModule(const void *addr, bool update) {
  return moloader->find(addr, update);
}

void Loader::cacheObject(std::shared_ptr<Object> imod) {
  moloader->cacheObject(imod);
}

void Loader::cacheSymbol(std::string_view name, const void *impl) {
  moloader->cacheSymbol(name, impl);
}

bool Loader::executable(uint64_t vm, Object **iobject) {
  return moloader->executable(vm, iobject);
}

bool Loader::belong(uint64_t vm) { return moloader->belong(vm); }

} // namespace icpp
