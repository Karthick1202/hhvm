/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/runtime/base/autoload-handler.h"

#include <algorithm>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/unit-util.h"
#include "hphp/runtime/vm/vm-regs.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString
  s_class("class"),
  s_function("function"),
  s_constant("constant"),
  s_type("type"),
  s_record("record"),
  s_failure("failure"),
  s_autoload("__autoload"),
  s_exception("exception"),
  s_error("Error"),
  s_previous("previous");

using DecodedHandlerPtr = req::unique_ptr<AutoloadHandler::DecodedHandler>;

//////////////////////////////////////////////////////////////////////

Variant invoke_for_autoload(const String& function, const Variant& params) {
  Func* func = Unit::loadFunc(function.get());
  if (func && (isContainer(params) || params.isNull())) {
    return Variant::attach(g_context->invokeFunc(func, params));
  }
  raise_warning("call_user_func to non-existent function %s",
    function.c_str());
  return Variant(false);
}

/*
 * Wraps calling an (autoload) PHP function from a DecodedHandler.
 */
Variant vm_call_decoded_handler(const AutoloadHandler::DecodedHandler& handler,
                                const Array& params) {
  ObjectData* obj = handler.m_obj.get();
  Class* cls = handler.m_cls;
  const Func* f = handler.m_func;
  StringData* invName = handler.m_name.get();
  assertx(!obj || !cls);
  if (invName) {
    invName->incRefCount();
  }
  return Variant::attach(
    g_context->invokeFunc(f, params, obj, cls, nullptr, invName,
                          ExecutionContext::InvokeNormal, false,
                          handler.m_dynamic)
  );
}

/*
 * Helper method from converting between a PHP function and a DecodedHandler.
 */
bool vm_decode_handler(const Variant& function, DecodedHandlerPtr& handler) {
  ObjectData* obj = nullptr;
  Class* cls = nullptr;
  StringData* name = nullptr;
  ArrayData* reifiedGenerics = nullptr;
  bool dynamic;
  // Don't warn here, let the caller decide what to do if the func is nullptr.
  auto const func = vm_decode_function(function, GetCallerFrame(),
                                       obj, cls, name, dynamic,
                                       reifiedGenerics,
                                       DecodeFlags::NoWarn);
  if (func == nullptr) {
    return false;
  }

  handler = req::make_unique<AutoloadHandler::DecodedHandler>(
    obj, obj ? nullptr : cls, func, name, dynamic);
  return true;
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

IMPLEMENT_REQUEST_LOCAL(AutoloadHandler, AutoloadHandler::s_instance);

void AutoloadHandler::requestInit() {
  assertx(m_map.get() == nullptr);
  assertx(m_map_root.get() == nullptr);
  assertx(m_loading.get() == nullptr);
  m_spl_stack_inited = false;
  new (&m_handlers) req::deque<HandlerBundle>();
  m_handlers_valid = true;
}

void AutoloadHandler::requestShutdown() {
  m_map.reset();
  m_map_root.reset();
  m_loading.reset();
  m_handlers_valid = false;
  // m_spl_stack_inited will be re-initialized by the next requestInit
  // m_handlers will be re-initialized by the next requestInit
}

bool AutoloadHandler::setMap(const Array& map, const String& root) {
  this->m_map = map;
  this->m_map_root = root;
  return true;
}

namespace {
struct FuncExistsChecker {
  const StringData* m_name;
  mutable NamedEntity* m_ne;
  explicit FuncExistsChecker(const StringData* name)
    : m_name(name), m_ne(nullptr) {}
  bool operator()() const {
    if (!m_ne) {
      m_ne = NamedEntity::get(m_name, false);
      if (!m_ne) {
        return false;
      }
    }
    auto f = m_ne->getCachedFunc();
    return (f != nullptr) &&
           (f->arFuncPtr() != Native::unimplementedWrapper);
  }
};
struct ClassExistsChecker {
  const String& m_name;
  mutable NamedEntity* m_ne;
  explicit ClassExistsChecker(const String& name)
    : m_name(name), m_ne(nullptr) {}
  bool operator()() const {
    if (!m_ne) {
      m_ne = NamedEntity::get(m_name.get(), false);
      if (!m_ne) {
        return false;
      }
    }
    return m_ne->getCachedClass() != nullptr;
  }
};
struct ConstExistsChecker {
  const StringData* m_name;
  explicit ConstExistsChecker(const StringData* name)
    : m_name(name) {}
  bool operator()() const {
    return Unit::lookupCns(m_name) != nullptr;
  }
};
struct TypeExistsChecker {
  const String& m_name;
  mutable NamedEntity* m_ne;
  explicit TypeExistsChecker(const String& name)
    : m_name(name), m_ne(nullptr) {}
  bool operator()() const {
    if (!m_ne) {
      m_ne = NamedEntity::get(m_name.get(), false);
      if (!m_ne) {
        return false;
      }
    }
    return m_ne->getCachedTypeAlias() != nullptr;
  }
};
// TODO (T41179180:Support records)
struct ClassOrTypeExistsChecker {
  const String& m_name;
  mutable NamedEntity* m_ne;
  explicit ClassOrTypeExistsChecker(const String& name)
    : m_name(name), m_ne(nullptr) {}
  bool operator()() const {
    if (!m_ne) {
      m_ne = NamedEntity::get(m_name.get(), false);
      if (!m_ne) {
        return false;
      }
    }
    return m_ne->getCachedClass() != nullptr ||
           m_ne->getCachedTypeAlias() != nullptr;
  }
};
struct RecordExistsChecker {
  const String& m_name;
  mutable NamedEntity* m_ne;
  explicit RecordExistsChecker(const String& name)
    : m_name(name), m_ne(nullptr) {}
  bool operator()() const {
    if (!m_ne) {
      m_ne = NamedEntity::get(m_name.get(), false);
      if (!m_ne) {
        return false;
      }
    }
    return m_ne->getCachedRecord() != nullptr;
  }
};
}

const StaticString
  s_file("file"),
  s_line("line");

template <class T>
AutoloadHandler::Result
AutoloadHandler::loadFromMapImpl(const String& clsName,
                                 const String& kind,
                                 bool toLower,
                                 const T &checkExists,
                                 Variant& err) {
  assertx(!m_map.isNull());
  // Always normalize name before autoloading
  const String& name = normalizeNS(clsName);
  auto const type_map = m_map.get()->get(kind).unboxed();
  if (!isArrayType(type_map.type()) && !isDictType(type_map.type())) {
    return Failure;
  }
  String canonicalName = toLower ? HHVM_FN(strtolower)(name) : name;
  auto const file = type_map.val().parr->get(canonicalName).unboxed();
  bool ok = false;
  if (isStringType(file.type())) {
    String fName{file.val().pstr};
    if (fName.get()->data()[0] != '/') {
      if (!m_map_root.empty()) {
        fName = m_map_root + fName;
      }
    }
    try {
      VMRegAnchor _;
      bool initial;
      auto const ec = g_context.getNoCheck();
      auto const unit = lookupUnit(fName.get(), "", &initial,
                                   Native::s_noNativeFuncs);
      if (unit) {
        if (initial) {
          tvDecRefGen(
            ec->invokeFunc(unit->getMain(nullptr), init_null_variant,
                           nullptr, nullptr, nullptr, nullptr,
                           ExecutionContext::InvokePseudoMain)
          );
        }
        ok = true;
      }
    } catch (ExitException&) {
      throw;
    } catch (ResourceExceededException&) {
      throw;
    } catch (PhpNotSupportedException&) {
      throw;
    } catch (ExtendedException& ee) {
      auto fileAndLine = ee.getFileAndLine();
      err = (fileAndLine.first.empty())
        ? ee.getMessage()
        : folly::format("{} in {} on line {}",
                        ee.getMessage(), fileAndLine.first,
                        fileAndLine.second).str();
    } catch (Exception& e) {
      err = e.getMessage();
    } catch (Object& e) {
      err = e;
    } catch (...) {
      err = String("Unknown Exception");
    }
  }
  if (ok && checkExists()) {
    return Success;
  }
  return Failure;
}

template <class T>
AutoloadHandler::Result
AutoloadHandler::loadFromMap(const String& clsName,
                             const String& kind,
                             bool toLower,
                             const T &checkExists) {
  while (true) {
    Variant err{Variant::NullInit()};
    Result res = loadFromMapImpl(clsName, kind, toLower, checkExists, err);
    if (res == Success) return Success;
    auto const func = m_map.get()->get(s_failure);
    if (isNullType(func.unboxed().type())) return Failure;
    res = invokeFailureCallback(const_variant_ref{func}, kind, clsName, err);
    if (checkExists()) return Success;
    if (res == RetryAutoloading) {
      continue;
    }
    return res;
  }
}

AutoloadHandler::Result
AutoloadHandler::invokeFailureCallback(
    const_variant_ref func, const String& kind,
    const String& name, const Variant& err
) {
  // can throw, otherwise
  //  - true means the map was updated. try again
  //  - false means we should stop applying autoloaders (only affects classes)
  //  - anything else means keep going
  Variant action = vm_call_user_func(func, make_vec_array(kind, name, err));
  auto const actionCell = action.toCell();
  if (actionCell->m_type == KindOfBoolean) {
    return actionCell->m_data.num ? RetryAutoloading : StopAutoloading;
  }
  return ContinueAutoloading;
}

bool AutoloadHandler::autoloadFunc(StringData* name) {
  return !m_map.isNull() &&
    loadFromMap(String{stripInOutSuffix(name)},
                s_function,
                true,
                FuncExistsChecker(name)) != Failure;
}

bool AutoloadHandler::autoloadConstant(StringData* name) {
  return !m_map.isNull() &&
    loadFromMap(String{name},
                s_constant,
                false,
                ConstExistsChecker(name)) != Failure;
}

bool AutoloadHandler::autoloadType(const String& name) {
  return !m_map.isNull() &&
    loadFromMap(name, s_type, true, TypeExistsChecker(name)) != Failure;
}

/**
 * Taken from php-src
 * https://github.com/php/php-src/blob/PHP-5.6/Zend/zend_execute_API.c#L960
 */
bool is_valid_class_name(folly::StringPiece className) {
  return strspn(
    className.data(),
    "0123456789_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\177"
    "\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217\220"
    "\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237\240\241"
    "\242\243\244\245\246\247\250\251\252\253\254\255\256\257\260\261\262"
    "\263\264\265\266\267\270\271\272\273\274\275\276\277\300\301\302\303"
    "\304\305\306\307\310\311\312\313\314\315\316\317\320\321\322\323\324"
    "\325\326\327\330\331\332\333\334\335\336\337\340\341\342\343\344\345"
    "\346\347\350\351\352\353\354\355\356\357\360\361\362\363\364\365\366"
    "\367\370\371\372\373\374\375\376\377\\"
  ) == className.size();
}

bool AutoloadHandler::autoloadClass(const String& clsName,
                                    bool forceSplStack /* = false */) {
  if (clsName.empty()) return false;
  const String& className = normalizeNS(clsName);
  // Verify class name before passing it to __autoload()
  if (!is_valid_class_name(className.slice())) {
    return false;
  }
  if (!m_map.isNull()) {
    ClassExistsChecker ce(className);
    Result res = loadFromMap(className, s_class, true, ce);
    if (res == Success || ce()) return true;
    if (res == StopAutoloading) return false;
  }
  return autoloadClassPHP5Impl(className, forceSplStack);
}

bool AutoloadHandler::autoloadRecord(const String& recName) {
  if (recName.empty()) return false;
  return !m_map.isNull() &&
         loadFromMap(recName, s_record,
                     true, RecordExistsChecker(recName)) != Failure;
}

template<>
bool AutoloadHandler::autoloadType<Class>(const String& name) {
  return autoloadClass(name);
}
template<>
bool AutoloadHandler::autoloadType<Record>(const String& name) {
  return autoloadRecord(name);
}

bool AutoloadHandler::autoloadClassPHP5Impl(const String& className,
                                            bool forceSplStack) {
  // If we end up in a recursive autoload loop where we try to load the
  // same class twice, just fail the load to match PHP5 as many frameworks
  // rely on it unless we are forcing a restart (due to spl_autoload_call)
  // in which case autoload is allowed to be reentrant.
  if (!forceSplStack) {
    const auto arrkey =
      m_loading.convertKey<IntishCast::CastSilently>(className);
    if (m_loading.exists(arrkey)) { return false; }
    m_loading.set(arrkey, make_tv<KindOfString>(className.get()));
  } else {
    // We can still overflow the stack if there is a loop when using
    // spl_autoload_call directly, but this behavior matches PHP5.
    m_loading.append(className);
  }

  // Make sure state is cleaned up from this load; autoloading of arbitrary
  // code below can throw
  SCOPE_EXIT {
    DEBUG_ONLY auto const l_className = m_loading.pop().toString();
    assertx(l_className == className);
  };

  Array params = PackedArrayInit(1).append(className).toArray();
  if (!m_spl_stack_inited && !forceSplStack) {
    if (function_exists(s_autoload)) {
      invoke_for_autoload(s_autoload, params);
      return true;
    }
    return false;
  }
  if (!m_spl_stack_inited || m_handlers.empty()) {
    return false;
  }
  Object autoloadException;
  for (const HandlerBundle& hb : m_handlers) {
    try {
      vm_call_decoded_handler(*hb.m_decodedHandler, params);
    } catch (Object& ex) {
      assertx(ex.instanceof(SystemLib::s_ThrowableClass));
      if (autoloadException.isNull()) {
        autoloadException = ex;
      } else {
        Object cur = ex;
        auto const ctx = cur->instanceof(SystemLib::s_ExceptionClass)
          ? s_exception
          : s_error;
        Variant next = cur->o_get(s_previous, false, ctx);
        while (next.isObject()) {
          cur = next.toObject();
          next = cur->o_get(s_previous, false, ctx);
        }
        cur->o_set(s_previous, autoloadException, ctx);
        autoloadException = ex;
      }
    }
    if (Unit::lookupClass(className.get()) != nullptr) {
      break;
    }
  }
  if (!autoloadException.isNull()) {
    throw_object(autoloadException);
  }
  return true;
}

template <class T>
AutoloadHandler::Result
AutoloadHandler::loadFromMapPartial(const String& className,
                                    const String& kind,
                                    bool toLower,
                                    const T &checkExists,
                                    Variant& err) {
  Result res = loadFromMapImpl(className, kind, toLower, checkExists, err);
  if (res == Success) {
    return Success;
  }
  assertx(res == Failure);
  if (!err.isNull()) {
    auto const func = m_map.get()->get(s_failure);
    if (!isNullType(func.unboxed().type())) {
      res = invokeFailureCallback(
        const_variant_ref{func}, kind, className, err
      );
      assertx(res != Failure);
      if (checkExists()) {
        return Success;
      }
    }
  }
  return res;
}

bool AutoloadHandler::autoloadClassOrType(const String& clsName) {
  if (clsName.empty()) return false;
  const String& className = normalizeNS(clsName);
  if (!m_map.isNull()) {
    ClassOrTypeExistsChecker cte(className);
    bool tryClass = true, tryType = true;
    Result classRes = RetryAutoloading, typeRes = RetryAutoloading;
    while (true) {
      Variant classErr{Variant::NullInit()};
      if (tryClass) {
        // Try consulting the 'class' map first, but don't call the failure
        // callback unless there was an uncaught exception or a fatal error
        // during the include operation.
        classRes = loadFromMapPartial(className, s_class, true, cte, classErr);
        if (classRes == Success) return true;
      }
      Variant typeErr{Variant::NullInit()};
      if (tryType) {
        // Next, try consulting the 'type' map. Again, don't call the failure
        // callback unless there was an uncaught exception or fatal error.
        typeRes = loadFromMapPartial(className, s_type, true, cte, typeErr);
        if (typeRes == Success) return true;
      }
      auto const func = Variant::wrap(m_map.get()->get(s_failure).tv());
      // If we reach this point, then for each map either nothing was found
      // or the file we included didn't define a class or type alias with the
      // specified name, and the failure callback (if one exists) did not throw
      // or raise a fatal error.
      if (!func.isNull()) {
        // First, call the failure callback for 'class' if we didn't do so
        // above
        if (classRes == Failure) {
          assertx(tryClass);
          classRes = invokeFailureCallback(func, s_class, className, classErr);
          // The failure callback may have defined a class or type alias for
          // us, in which case we're done.
          if (cte()) return true;
        }
        // Next, call the failure callback for 'type' if we didn't do so above
        if (typeRes == Failure) {
          assertx(tryType);
          typeRes = invokeFailureCallback(func, s_type, className, typeErr);
          // The failure callback may have defined a class or type alias for
          // us, in which case we're done.
          if (cte()) return true;
        }
        assertx(classRes != Failure && typeRes != Failure);
        tryClass = (classRes == RetryAutoloading);
        tryType = (typeRes == RetryAutoloading);
        // If the failure callback requested a retry for 'class' or 'type'
        // or both, jump back to the top to try again.
        if (tryClass || tryType) {
          continue;
        }
        if (classRes == StopAutoloading) {
          // If the failure callback requested that we stop autoloading for
          // 'class', then return false here so we don't fall through to the
          // PHP5 autoload impl below.
          return false;
        }
      }
      // Break out of the while loop so that we can fall through to the
      // to the call the PHP5 autoload impl below.
      break;
    }
  }
  return autoloadClassPHP5Impl(className, false);
}

Array AutoloadHandler::getHandlers() {
  if (!m_spl_stack_inited) {
    return Array();
  }

  PackedArrayInit handlers(m_handlers.size());

  for (const HandlerBundle& hb : m_handlers) {
    DecodedHandler* decodedHandler = hb.m_decodedHandler.get();
    const HPHP::Func* f = decodedHandler->m_func;

    if (hb.m_handler.isObject()) {
      handlers.append(hb.m_handler);
    } else if (decodedHandler->m_cls) {
      PackedArrayInit callable(2);
      callable.append(String(decodedHandler->m_cls->nameStr()));
      callable.append(String(f->nameStr()));
      handlers.append(callable.toArray());
    } else if (decodedHandler->m_obj) {
      PackedArrayInit callable(2);
      callable.append(decodedHandler->m_obj);
      callable.append(String(f->nameStr()));
      handlers.append(callable.toArray());
    } else {
      handlers.append(String(f->nameStr()));
    }
  }

  return handlers.toArray();
}

bool AutoloadHandler::CompareBundles::operator()(const HandlerBundle& hb) {
  auto const& lhs = *m_decodedHandler;
  auto const& rhs = *hb.m_decodedHandler;

  return lhs.m_func == rhs.m_func && lhs.m_cls == rhs.m_cls &&
         lhs.m_obj.get() == rhs.m_obj.get();
}

bool AutoloadHandler::addHandler(const Variant& handler, bool prepend) {
  DecodedHandlerPtr decodedHandler = nullptr;
  if (!vm_decode_handler(handler, decodedHandler)) {
    return false;
  }

  m_spl_stack_inited = true;

  // Zend doesn't modify the order of the list if the handler is already
  // registered.
  auto const& compareBundles = CompareBundles(decodedHandler.get());
  if (std::find_if(m_handlers.begin(), m_handlers.end(), compareBundles) !=
      m_handlers.end()) {
    return true;
  }

  if (!prepend) {
    m_handlers.emplace_back(handler, decodedHandler);
  } else {
    m_handlers.emplace_front(handler, decodedHandler);
  }

  return true;
}

bool AutoloadHandler::isRunning() {
  return !m_loading.empty();
}

void AutoloadHandler::removeHandler(const Variant& handler) {
  DecodedHandlerPtr decodedHandler = nullptr;
  if (!vm_decode_handler(handler, decodedHandler)) {
    return;
  }

  // Use find_if instead of remove_if since we know there can only be one match
  // in the vector.
  auto const& compareBundles = CompareBundles(decodedHandler.get());
  auto it = std::find_if(m_handlers.begin(), m_handlers.end(), compareBundles);
  if (it != m_handlers.end()) {
    m_handlers.erase(it);
  }
}

void AutoloadHandler::removeAllHandlers() {
  m_spl_stack_inited = false;
  m_handlers.clear();
}


//////////////////////////////////////////////////////////////////////

}
