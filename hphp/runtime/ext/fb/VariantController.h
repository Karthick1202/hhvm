/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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
#ifndef VARIANTCONTROLLER_H
#define VARIANTCONTROLLER_H

#include <algorithm>
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/ext/fb/FBSerialize/FBSerialize.h"

namespace HPHP {

enum VariantControllerHackArraysMode {
  OFF,
  ON,
};

/**
 * Hphp datatype conforming to datatype requirements in FBSerialize.h
 */
template <VariantControllerHackArraysMode HackArraysMode>
struct VariantControllerImpl {
  typedef Variant VariantType;
  typedef Array MapType;
  typedef Array VectorType;
  typedef String StringType;

  // variant accessors
  static HPHP::serialize::Type type(const_variant_ref obj) {
    switch (obj.getType()) {
      case KindOfUninit:
      case KindOfNull:       return HPHP::serialize::Type::NULLT;
      case KindOfBoolean:    return HPHP::serialize::Type::BOOL;
      case KindOfDouble:     return HPHP::serialize::Type::DOUBLE;
      case KindOfInt64:      return HPHP::serialize::Type::INT64;
      case KindOfFunc:
      case KindOfClass:
      case KindOfPersistentString:
      case KindOfString:     return HPHP::serialize::Type::STRING;
      case KindOfObject:     return HPHP::serialize::Type::OBJECT;
      case KindOfPersistentShape:
      case KindOfShape: { // TODO(T31134050)
        if (RuntimeOption::EvalHackArrDVArrs &&
            HackArraysMode != VariantControllerHackArraysMode::ON) {
          throw HPHP::serialize::HackArraySerializeError{};
        }
        return HPHP::serialize::Type::MAP;
      }
      case KindOfPersistentArray:
      case KindOfArray:      return HPHP::serialize::Type::MAP;
      case KindOfPersistentDict:
      case KindOfDict: {
        if (HackArraysMode == VariantControllerHackArraysMode::ON) {
          return HPHP::serialize::Type::MAP;
        }
        throw HPHP::serialize::HackArraySerializeError{};
      }
      case KindOfPersistentVec:
      case KindOfVec: {
        if (HackArraysMode == VariantControllerHackArraysMode::ON) {
          return HPHP::serialize::Type::LIST;
        }
        throw HPHP::serialize::HackArraySerializeError{};
      }
      case KindOfPersistentKeyset:
      case KindOfKeyset:
        throw HPHP::serialize::KeysetSerializeError{};

      case KindOfClsMeth:
        if (RuntimeOption::EvalHackArrDVArrs) {
          if (HackArraysMode == VariantControllerHackArraysMode::ON) {
            return HPHP::serialize::Type::LIST;
          }
          throw HPHP::serialize::HackArraySerializeError{};
        } else {
          return HPHP::serialize::Type::MAP;
        }

      case KindOfResource:
      case KindOfRef:
      case KindOfRecord: // TODO(T41025646): implement serialization for records
        throw HPHP::serialize::SerializeError(
          "don't know how to serialize HPHP Variant");
    }
    not_reached();
  }
  static int64_t asInt64(const_variant_ref obj) { return obj.toInt64(); }
  static bool asBool(const_variant_ref obj) { return obj.toInt64() != 0; }
  static double asDouble(const_variant_ref obj) { return obj.toDouble(); }
  static String asString(const_variant_ref obj) { return obj.toString(); }
  static Array asMap(const_variant_ref obj) { return obj.toArray(); }
  static Array asVector(const_variant_ref obj) { return obj.toArray(); }

  // variant creators
  static VariantType createNull() { return init_null(); }
  static VariantType fromInt64(int64_t val) { return val; }
  static VariantType fromBool(bool val) { return val; }
  static VariantType fromDouble(double val) { return val; }
  static VariantType fromString(const StringType& str) { return str; }
  static VariantType fromMap(const MapType& map) { return map; }
  static VariantType fromVector(const VectorType& vec) { return vec; }

  // map methods
  static MapType createMap() {
    return HackArraysMode == VariantControllerHackArraysMode::ON
      ? empty_dict_array() : empty_array();
  }
  static MapType createMap(ArrayInit&& map) {
    return map.toArray();
  }
  static ArrayInit reserveMap(size_t n) {
    ArrayInit res(n, ArrayInit::Map{}, CheckAllocation{});
    return res;
  }
  static MapType getStaticEmptyMap() {
    return MapType(staticEmptyArray());
  }
  static HPHP::serialize::Type mapKeyType(const Variant& k) {
    return type(k);
  }
  static int64_t mapKeyAsInt64(const Variant& k) { return k.toInt64(); }
  static String mapKeyAsString(const Variant& k) {
    return k.toString();
  }

  static void mapSet(MapType& map, StringType&& k, VariantType&& v) {
    auto const arrkey = map.convertKey<IntishCast::CastSilently>(k);
    Variant val(std::move(v));
    map.set(arrkey, *val.asTypedValue());
  }

  static void mapSet(MapType& map, int64_t k, VariantType&& v) {
    map.set(k, std::move(v));
  }

  template <typename Key>
  static void mapSet(ArrayInit& map, Key&& k, VariantType&& v) {
    map.setUnknownKey<IntishCast::CastSilently>(std::move(k), std::move(v));
  }
  static int64_t mapSize(const MapType& map) { return map.size(); }
  static ArrayIter mapIterator(const MapType& map) {
    return ArrayIter(map);
  }
  static bool mapNotEnd(const MapType& /*map*/, ArrayIter& it) {
    return !it.end();
  }
  static void mapNext(ArrayIter& it) { ++it; }
  static Variant mapKey(ArrayIter& it) { return it.first(); }
  static const_variant_ref mapValue(ArrayIter& it) { return it.secondRef(); }

  // vector methods
  static VectorType createVector() {
    return HackArraysMode == VariantControllerHackArraysMode::ON
      ? empty_vec_array() : empty_array();
  }
  static int64_t vectorSize(const VectorType& vec) {
    return vec.size();
  }
  static void vectorAppend(VectorType& vec, const VariantType& v) {
    vec.append(v);
  }
  static ArrayIter vectorIterator(const VectorType& vec) {
    return ArrayIter(vec);
  }
  static bool vectorNotEnd(const VectorType& /*vec*/, ArrayIter& it) {
    return !it.end();
  }
  static void vectorNext(ArrayIter& it) { ++it; }
  static const_variant_ref vectorValue(ArrayIter& it) {
    return it.secondRef();
  }

  // string methods
  static StringType createMutableString(size_t n) {
    String ret(n, ReserveString);
    ret.setSize(n);
    return ret;
  }
  static StringType createStaticString(const char* str, size_t len) {
    String ret = String(makeStaticString(str, len));
    return ret;
  }
  static StringType getStaticEmptyString() {
    return empty_string();
  }
  static char* getMutablePtr(StringType& s) {
    return s.mutableData();
  }
  static void shrinkString(String& s, size_t length) {
    s.shrink(length);
  }
  static StringType stringFromData(const char* src, int n) {
    return StringType::attach(StringData::Make(src, n, CopyString));
  }
  static StringType stringFromMutableString(StringType&& s) {
    return std::move(s);
  }
  static size_t stringLen(const StringType& str) { return str.size(); }
  static const char* stringData(const StringType& str) {
    return str.data();
  }
};

using VariantController =
  VariantControllerImpl<VariantControllerHackArraysMode::OFF>;
using VariantControllerUsingHackArrays =
  VariantControllerImpl<VariantControllerHackArraysMode::ON>;
}


#endif
