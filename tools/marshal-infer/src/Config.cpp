// Config.cpp — see Config.h. JSON config file loader with strict, closed-
// schema validation (unlike Annotations.cpp's looser best-effort merge).
#include "Config.h"

#include "llvm/Support/JSON.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

using namespace llvm;

namespace marshal {
namespace {

// json::Object's typed convenience accessors (getInteger/getBoolean/
// getNumber/getString/getObject/getArray) are UNRELIABLE in this LLVM
// 18.1.8 build -- confirmed via a minimal standalone reproduction outside
// this codebase: getInteger/getBoolean return a corrupted std::optional
// even for a present, well-typed key, and getNumber crashes outright for
// an ABSENT key, while others happen to work for the specific
// present/absent combinations tried. Not a caller-compiler/ABI mismatch
// (reproduces identically when built with the bundled clang++ that
// produced the linked static library). All of these accessors are
// declared in JSON.h but DEFINED out-of-line in the precompiled static
// library; Object::get()/find() and every Value::getAsXxx() are defined
// INLINE directly in the header instead, executing entirely within
// whichever TU calls them -- and are the only forms confirmed safe. Every
// lookup in this file goes through get() + a Value-level accessor for
// that reason; never call an Object::getXxx(key) convenience method here.
// Each `requireXxx` below distinguishes a key that is ABSENT (returns true,
// `out` left untouched -- the caller keeps its own default) from one that is
// PRESENT but the WRONG JSON type (returns false with `err` set). This
// distinction matters: Value::getAsInteger()/getAsBoolean()/etc. return
// nullopt for EITHER case, so code that only checked "did I get a value
// back" (the tool's own earlier behavior) silently treated a present,
// malformed field -- e.g. `"max_type_depth": "six"` -- exactly like an
// omitted one, keeping the built-in default instead of rejecting the
// config. A config file is meant to be checked in and trusted; a typo in it
// must fail loudly, not be quietly downgraded to "unspecified".
bool requireInt(const json::Object &obj, StringRef key, const std::string &context,
                std::optional<int64_t> &out, std::string &err) {
  const json::Value *v = obj.get(key);
  if (!v) return true;
  out = v->getAsInteger();
  if (!out) {
    err = context + "." + key.str() + ": must be an integer";
    return false;
  }
  return true;
}
bool requireBool(const json::Object &obj, StringRef key, const std::string &context,
                 std::optional<bool> &out, std::string &err) {
  const json::Value *v = obj.get(key);
  if (!v) return true;
  out = v->getAsBoolean();
  if (!out) {
    err = context + "." + key.str() + ": must be a boolean";
    return false;
  }
  return true;
}
bool requireNum(const json::Object &obj, StringRef key, const std::string &context,
                std::optional<double> &out, std::string &err) {
  const json::Value *v = obj.get(key);
  if (!v) return true;
  out = v->getAsNumber();
  if (!out) {
    err = context + "." + key.str() + ": must be a number";
    return false;
  }
  return true;
}
bool requireStr(const json::Object &obj, StringRef key, const std::string &context,
                std::optional<StringRef> &out, std::string &err) {
  const json::Value *v = obj.get(key);
  if (!v) return true;
  out = v->getAsString();
  if (!out) {
    err = context + "." + key.str() + ": must be a string";
    return false;
  }
  return true;
}
bool requireObj(const json::Object &obj, StringRef key, const std::string &context,
                const json::Object *&out, std::string &err) {
  const json::Value *v = obj.get(key);
  if (!v) { out = nullptr; return true; }
  out = v->getAsObject();
  if (!out) {
    err = context + "." + key.str() + ": must be an object";
    return false;
  }
  return true;
}

// Rejects any key in `obj` not listed in `allowed` -- a closed schema, not
// an ignore-unknown-fields merge: a typo or a hoped-for-but-unimplemented
// key must fail loudly here, not be silently dropped.
bool checkKeys(const json::Object &obj, const std::set<std::string> &allowed,
               const std::string &context, std::string &err) {
  for (auto &kv : obj) {
    std::string k = StringRef(kv.first).str();
    if (!allowed.count(k)) {
      err = context + ": unknown key '" + k + "'";
      return false;
    }
  }
  return true;
}

// Argument-index values pass through a JSON int64_t -> int narrowing
// conversion wherever they're stored (ContractExtentOperand::argIndex,
// FunctionContract's map key). No real function has anywhere close to this
// many parameters; the bound exists purely to make that narrowing provably
// safe rather than implementation-defined for a pathological config value.
constexpr int64_t kMaxArgIndex = 4096;

bool parseExtentOperand(const json::Object &obj, const char *field,
                        ContractExtentOperand &out, const std::string &context,
                        std::string &err) {
  const json::Value *v = obj.get(field);
  if (!v) {
    err = context + ": missing '" + field + "'";
    return false;
  }
  const json::Object *o = v->getAsObject();
  if (!o) {
    err = context + "." + field + ": must be an object";
    return false;
  }
  std::string fctx = context + "." + field;
  if (!checkKeys(*o, {"arg_index", "source"}, fctx, err))
    return false;
  std::optional<int64_t> idx;
  if (!requireInt(*o, "arg_index", fctx, idx, err))
    return false;
  if (!idx) {
    err = fctx + ": missing 'arg_index'";
    return false;
  }
  if (*idx < 0 || *idx > kMaxArgIndex) {
    err = fctx + ".arg_index: must be between 0 and " +
          std::to_string(kMaxArgIndex);
    return false;
  }
  std::optional<StringRef> src;
  if (!requireStr(*o, "source", fctx, src, err))
    return false;
  if (!src || (*src != "value" && *src != "pointee_i32")) {
    err = fctx + ".source: must be \"value\" or \"pointee_i32\"";
    return false;
  }
  out.argIndex = (int)*idx;
  out.source = src->str();
  return true;
}

bool parseStrideVectorContract(const json::Object &obj, const std::string &context,
                               StrideVectorContract &out, std::string &err) {
  if (!checkKeys(obj, {"size_operand", "stride_operand", "const_size", "dir"},
                 context, err))
    return false;
  if (!parseExtentOperand(obj, "size_operand", out.sizeOperand, context, err))
    return false;
  if (!parseExtentOperand(obj, "stride_operand", out.strideOperand, context, err))
    return false;
  std::optional<int64_t> cs;
  if (!requireInt(obj, "const_size", context, cs, err))
    return false;
  if (!cs || *cs <= 0 || *cs > (int64_t)UINT32_MAX) {
    err = context + ".const_size: must be a positive integer (<= " +
          std::to_string(UINT32_MAX) + ")";
    return false;
  }
  out.constSize = (uint64_t)*cs;
  std::optional<StringRef> dir;
  if (!requireStr(obj, "dir", context, dir, err))
    return false;
  if (dir) {
    if (*dir != "in" && *dir != "out" && *dir != "inout") {
      err = context + ".dir: must be \"in\", \"out\", or \"inout\"";
      return false;
    }
    out.dir = dir->str();
  }
  return true;
}

} // namespace

bool loadConfig(const std::string &path, Config &out, std::string &err) {
  std::ifstream in(path);
  if (!in) {
    err = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  // Named, not a temporary: json::parse's Value/Object/ObjectKey tree keeps
  // StringRefs pointing INTO this buffer rather than copying out of it (a
  // zero-copy parser design) -- it must outlive every use of `parsed`/`root`
  // below, not just the parse() call itself.
  std::string text = ss.str();
  Expected<json::Value> parsed = json::parse(text);
  if (!parsed) {
    err = path + ": " + toString(parsed.takeError());
    return false;
  }
  const json::Object *root = parsed->getAsObject();
  if (!root) {
    err = path + ": top-level JSON must be an object";
    return false;
  }
  if (!checkKeys(*root,
                 {"config_version", "profile_name", "analysis", "coverage",
                  "contracts"},
                 path, err))
    return false;

  std::optional<int64_t> ver;
  if (!requireInt(*root, "config_version", path, ver, err))
    return false;
  if (!ver) {
    err = path + ": missing integer 'config_version'";
    return false;
  }
  if (*ver != 1) {
    err = path + ": unsupported config_version " + std::to_string(*ver) +
          " (this build of marshal-infer only understands version 1)";
    return false;
  }

  Config cfg;
  cfg.configVersion = (int)*ver;
  cfg.sourcePath = path;
  std::optional<StringRef> pn;
  if (!requireStr(*root, "profile_name", path, pn, err))
    return false;
  if (pn)
    cfg.profileName = pn->str();

  const json::Object *an = nullptr;
  if (!requireObj(*root, "analysis", path, an, err))
    return false;
  if (an) {
    std::string actx = path + ".analysis";
    if (!checkKeys(*an, {"max_type_depth", "max_delegation_hops"}, actx, err))
      return false;
    std::optional<int64_t> d;
    if (!requireInt(*an, "max_type_depth", actx, d, err))
      return false;
    if (d) {
      if (*d < 1 || *d > 64) {
        err = actx + ".max_type_depth: must be between 1 and 64";
        return false;
      }
      cfg.maxTypeDepth = (unsigned)*d;
    }
    std::optional<int64_t> h;
    if (!requireInt(*an, "max_delegation_hops", actx, h, err))
      return false;
    if (h) {
      if (*h != 0 && *h != 1) {
        err = actx +
              ".max_delegation_hops: only 0 or 1 is currently "
              "implemented (multi-hop delegation does not exist yet -- "
              "requesting it would silently do nothing, which this loader "
              "refuses to accept)";
        return false;
      }
      cfg.maxDelegationHops = (unsigned)*h;
    }
  }

  const json::Object *cov = nullptr;
  if (!requireObj(*root, "coverage", path, cov, err))
    return false;
  if (cov) {
    std::string cctx = path + ".coverage";
    if (!checkKeys(*cov, {"enabled", "min_marshal_count", "min_marshal_pct"},
                  cctx, err))
      return false;
    std::optional<bool> en;
    if (!requireBool(*cov, "enabled", cctx, en, err))
      return false;
    if (en)
      cfg.coverage.enabled = *en;
    std::optional<int64_t> mc;
    if (!requireInt(*cov, "min_marshal_count", cctx, mc, err))
      return false;
    if (mc) {
      if (*mc < 0 || *mc > INT32_MAX) {
        err = cctx + ".min_marshal_count: must be between 0 and " +
              std::to_string(INT32_MAX);
        return false;
      }
      cfg.coverage.minMarshalCount = (int)*mc;
    }
    std::optional<double> mp;
    if (!requireNum(*cov, "min_marshal_pct", cctx, mp, err))
      return false;
    if (mp) {
      if (*mp < 0 || *mp > 100) {
        err = cctx + ".min_marshal_pct: must be between 0 and 100";
        return false;
      }
      cfg.coverage.minMarshalPct = *mp;
    }
    if (cfg.coverage.enabled && cfg.coverage.minMarshalCount == 0 &&
        cfg.coverage.minMarshalPct == 0.0) {
      err = cctx +
            ": enabled but neither min_marshal_count nor "
            "min_marshal_pct was set -- a vacuous threshold is almost "
            "certainly a mistake";
      return false;
    }
  }

  const json::Object *co = nullptr;
  if (!requireObj(*root, "contracts", path, co, err))
    return false;
  if (co) {
    for (auto &fkv : *co) {
      std::string fname = StringRef(fkv.first).str();
      const json::Object *fobj = fkv.second.getAsObject();
      if (!fobj) {
        err = path + ".contracts." + fname + ": must be an object";
        return false;
      }
      FunctionContract fc;
      for (auto &akv : *fobj) {
        std::string argKey = StringRef(akv.first).str();
        char *endp = nullptr;
        errno = 0;
        long argIdx = std::strtol(argKey.c_str(), &endp, 10);
        if (argKey.empty() || *endp != '\0' || errno == ERANGE ||
            argIdx < 0 || argIdx > kMaxArgIndex) {
          err = path + ".contracts." + fname + ": argument key '" + argKey +
                "' must be an integer string between 0 and " +
                std::to_string(kMaxArgIndex);
          return false;
        }
        const json::Object *aobj = akv.second.getAsObject();
        if (!aobj) {
          err = path + ".contracts." + fname + "." + argKey +
                ": must be an object";
          return false;
        }
        StrideVectorContract sv;
        std::string ctx = path + ".contracts." + fname + "." + argKey;
        if (!parseStrideVectorContract(*aobj, ctx, sv, err))
          return false;
        fc[(int)argIdx] = sv;
      }
      cfg.contracts[fname] = std::move(fc);
    }
  }

  out = std::move(cfg);
  return true;
}

} // namespace marshal
