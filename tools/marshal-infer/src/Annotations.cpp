// Annotations.cpp — see Annotations.h. Built-in defaults + JSON file loader.
#include "Annotations.h"

#include "llvm/Support/JSON.h"

#include <fstream>
#include <sstream>

using namespace llvm;

namespace marshal {

Annotations &annotations() {
  static Annotations a = [] {
    Annotations d;
    // Opaque, library-owned handle types (never deep-copied; translated via the
    // token table). Keyed by the canonical DWARF struct name.
    d.handleTypes = {{"_IO_FILE", "FILE"},
                     {"__dirstream", "DIR"},
                     {"__locale_struct", "locale"}};
    // Allocators (the `allocsize` attribute is stripped at -O1). size in bytes is
    // the product of the listed arg values; realloc-style -> force_local.
    d.allocators = {
        {"malloc", {{0}, false}},        {"valloc", {{0}, false}},
        {"pvalloc", {{0}, false}},        {"aligned_alloc", {{1}, false}},
        {"memalign", {{1}, false}},       {"calloc", {{0, 1}, false}},
        {"realloc", {{}, true}},          {"reallocarray", {{}, true}},
        {"posix_memalign", {{}, true}},   {"strdup", {{}, true}},
        {"strndup", {{}, true}}};
    // Searchers whose return is a pointer into arg0, computed in a sibling TU.
    d.returnIntoArg0 = {"strchr",    "strrchr", "index",  "rindex",
                        "strchrnul", "memchr",  "memrchr","rawmemchr",
                        "strstr",    "strcasestr", "strpbrk"};
    // NULL-terminated char* arrays (argv/envp). Indices are DWARF arg positions.
    d.ptrArrayArgs = {
        {"execv", {1}},        {"execvp", {1}},      {"execvpe", {1, 2}},
        {"execve", {1, 2}},    {"fexecve", {1, 2}},  {"execveat", {2, 3}},
        {"getopt", {1}},       {"getopt_long", {1}}, {"getopt_long_only", {1}},
        {"posix_spawn", {4, 5}}, {"posix_spawnp", {4, 5}}};
    return d;
  }();
  return a;
}

bool loadAnnotationsFile(StringRef path, std::string &err) {
  std::ifstream in(path.str());
  if (!in) { err = "cannot open " + path.str(); return false; }
  std::stringstream ss;
  ss << in.rdbuf();
  // Named, not a temporary: json::parse's Value/Object tree keeps StringRefs
  // pointing INTO this buffer rather than copying out of it, so it must
  // outlive every use of `j`/`obj` below, not just the parse() call itself.
  std::string text = ss.str();
  Expected<json::Value> j = json::parse(text);
  if (!j) { err = toString(j.takeError()); return false; }
  json::Object *obj = j->getAsObject();
  if (!obj) { err = "top-level JSON must be an object"; return false; }

  // json::Object's typed convenience accessors (getObject/getArray/
  // getBoolean/...) are UNRELIABLE in this LLVM 18.1.8 build -- confirmed
  // via a minimal standalone reproduction outside this codebase (see
  // Config.cpp's own comment on the same finding for the full writeup).
  // Object::get() and every Value::getAsXxx() are defined INLINE in the
  // header and are the only forms confirmed safe; every lookup below goes
  // through that pattern instead.
  Annotations &a = annotations();
  if (const json::Value *htv = obj->get("handle_types"))
    if (const json::Object *ht = htv->getAsObject())
      for (auto &kv : *ht)
        if (auto s = kv.second.getAsString())
          a.handleTypes[StringRef(kv.first).str()] = s->str();

  if (const json::Value *alv = obj->get("allocators"))
    if (const json::Object *al = alv->getAsObject())
      for (auto &kv : *al) {
        AllocSpec sp;
        if (const json::Object *o = kv.second.getAsObject()) {
          if (const json::Value *flv = o->get("force_local"))
            if (auto fl = flv->getAsBoolean()) sp.forceLocal = *fl;
          if (const json::Value *sav = o->get("size_args"))
            if (const json::Array *sa = sav->getAsArray())
              for (auto &v : *sa)
                if (auto i = v.getAsInteger()) sp.sizeArgs.push_back((int)*i);
        }
        a.allocators[StringRef(kv.first).str()] = sp;
      }

  if (const json::Value *riv = obj->get("return_into_arg0"))
    if (const json::Array *ri = riv->getAsArray())
      for (auto &v : *ri)
        if (auto s = v.getAsString()) a.returnIntoArg0.insert(s->str());

  if (const json::Value *pav = obj->get("ptr_array_args"))
    if (const json::Object *pa = pav->getAsObject())
      for (auto &kv : *pa) {
        std::vector<int> idxs;
        if (const json::Array *ar = kv.second.getAsArray())
          for (auto &v : *ar)
            if (auto i = v.getAsInteger()) idxs.push_back((int)*i);
        a.ptrArrayArgs[StringRef(kv.first).str()] = idxs;
      }

  return true;
}

} // namespace marshal
