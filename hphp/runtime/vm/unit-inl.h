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

#ifndef incl_HPHP_VM_UNIT_INL_H_
#error "unit-inl.h should only be included by unit.h"
#endif

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/repo-auth-type-array.h"
#include "hphp/runtime/vm/hhbc-codec.h"
#include "hphp/runtime/vm/litstr-table.h"
#include "hphp/runtime/vm/unit-util.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
// SourceLoc.

inline SourceLoc::SourceLoc(const Location::Range& r) {
  setLoc(&r);
}

inline void SourceLoc::reset() {
  line0 = char0 = line1 = char1 = 1;
}

inline bool SourceLoc::valid() const {
  return line0 != 1 || char0 != 1 || line1 != 1 || char1 != 1;
}

inline void SourceLoc::setLoc(const Location::Range* l) {
  line0 = l->line0;
  char0 = l->char0;
  line1 = l->line1;
  char1 = l->char1;
}

inline bool SourceLoc::same(const SourceLoc* l) const {
  return (this == l) ||
         (line0 == l->line0 && char0 == l->char0 &&
          line1 == l->line1 && char1 == l->char1);
}

inline bool SourceLoc::operator==(const SourceLoc& l) const {
  return same(&l);
}

///////////////////////////////////////////////////////////////////////////////
// Location tables.

template<typename T>
Offset TableEntry<T>::pastOffset() const {
  return m_pastOffset;
}

template<typename T>
T TableEntry<T>::val() const {
  return m_val;
}

template<typename T>
bool TableEntry<T>::operator<(const TableEntry& other) const {
  return m_pastOffset < other.m_pastOffset;
}

template<typename T>
template<class SerDe>
void TableEntry<T>::serde(SerDe& sd) {
  sd(m_pastOffset)(m_val);
}

///////////////////////////////////////////////////////////////////////////////
// Unit::MergeInfo.

inline Func** Unit::MergeInfo::funcBegin() const {
  return (Func**)m_mergeables;
}

inline Func** Unit::MergeInfo::funcEnd() const {
  return funcBegin() + m_firstHoistablePreClass;
}

inline Func** Unit::MergeInfo::funcHoistableBegin() const {
  return funcBegin() + m_firstHoistableFunc;
}

inline
Unit::MergeInfo::FuncRange Unit::MergeInfo::funcs() const {
  return { funcBegin(), funcEnd() };
}

inline
Unit::MergeInfo::MutableFuncRange Unit::MergeInfo::mutableFuncs() const {
  return { funcBegin(), funcEnd() };
}

inline
Unit::MergeInfo::MutableFuncRange Unit::MergeInfo::nonMainFuncs() const {
  return { funcBegin() + 1, funcEnd() };
}

inline void*& Unit::MergeInfo::mergeableObj(int idx) {
  return m_mergeables[idx];
}

inline void** Unit::MergeInfo::mergeableData(int idx) {
  return m_mergeables + idx;
}

///////////////////////////////////////////////////////////////////////////////
// Basic accessors.

inline UnitExtended* Unit::getExtended() {
  assertx(m_extended);
  return static_cast<UnitExtended*>(this);
}

inline const UnitExtended* Unit::getExtended() const {
  assertx(m_extended);
  return static_cast<const UnitExtended*>(this);
}

inline int Unit::repoID() const {
  return m_repoId;
}

inline int64_t Unit::sn() const {
  return m_sn;
}

inline MD5 Unit::md5() const {
  return m_md5;
}

inline const StringData* Unit::filepath() const {
  assertx(m_filepath);
  return m_filepath;
}

inline const StringData* Unit::dirpath() const {
  assertx(m_dirpath);
  return m_dirpath;
}

inline bool Unit::isICE() const {
  return m_ICE;
}

///////////////////////////////////////////////////////////////////////////////
// Bytecode.

inline PC Unit::entry() const {
  return m_bc;
}

inline Offset Unit::bclen() const {
  return m_bclen;
}

inline PC Unit::at(Offset off) const {
  assertx(off >= 0 && off <= Offset(m_bclen));
  return m_bc + off;
}

inline Offset Unit::offsetOf(PC pc) const {
  assertx(contains(pc));
  return pc - m_bc;
}

inline bool Unit::contains(PC pc) const {
  return pc >= m_bc && pc <= m_bc + m_bclen;
}

inline Op Unit::getOp(Offset instrOffset) const {
  assertx(instrOffset < m_bclen);
  return peek_op(m_bc + instrOffset);
}

///////////////////////////////////////////////////////////////////////////////
// Litstrs and NamedEntitys.

inline size_t Unit::numLitstrs() const {
  if (!m_extended) return 0;
  return getExtended()->m_namedInfo.size();
}

inline bool Unit::isLitstrId(Id id) const {
  if (isGlobalLitstrId(id)) {
    auto globalID = decodeGlobalLitstrId(id);
    return LitstrTable::get().contains(globalID);
  }
  return m_extended && getExtended()->m_namedInfo.contains(id);
}

inline StringData* Unit::lookupLitstrId(Id id) const {
  if (isGlobalLitstrId(id)) {
    auto globalID = decodeGlobalLitstrId(id);
    return LitstrTable::get().lookupLitstrId(globalID);
  }
  return getExtended()->m_namedInfo.lookupLitstr(id);
}

inline const NamedEntity* Unit::lookupNamedEntityId(Id id) const {
  return lookupNamedEntityPairId(id).second;
}

inline NamedEntityPair Unit::lookupNamedEntityPairId(Id id) const {
  if (isGlobalLitstrId(id)) {
    auto globalID = decodeGlobalLitstrId(id);
    return LitstrTable::get().lookupNamedEntityPairId(globalID);
  }
  return getExtended()->m_namedInfo.lookupNamedEntityPair(id);
}

///////////////////////////////////////////////////////////////////////////////
// Arrays.

inline size_t Unit::numArrays() const {
  return m_arrays.size();
}

inline const ArrayData* Unit::lookupArrayId(Id id) const {
  assertx(id < m_arrays.size());
  return m_arrays[id];
}

inline const RepoAuthType::Array* Unit::lookupArrayTypeId(Id id) const {
  return RuntimeOption::RepoAuthoritative ?
    globalArrayTypeTable().lookup(id) :
    getExtended()->m_arrayTypeTable.lookup(id);
}

///////////////////////////////////////////////////////////////////////////////
// Funcs and PreClasses and Records.

inline Func* Unit::lookupFuncId(Id id) const {
  assertx(id < Id(mergeInfo()->m_firstHoistablePreClass));
  return mergeInfo()->funcBegin()[id];
}

inline PreClass* Unit::lookupPreClassId(Id id) const {
  assertx(id < Id(m_preClasses.size()));
  return m_preClasses[id].get();
}

inline Record* Unit::lookupRecordId(Id id) const {
  assertx(id < Id(m_records.size()));
  return m_records[id].get();
}

inline Unit::FuncRange Unit::funcs() const {
  return mergeInfo()->funcs();
}

inline folly::Range<PreClassPtr*> Unit::preclasses() {
  return { m_preClasses.data(), m_preClasses.size() };
}

inline folly::Range<const PreClassPtr*> Unit::preclasses() const {
  return { m_preClasses.data(), m_preClasses.size() };
}

inline folly::Range<RecordPtr*> Unit::records() {
  return { m_records.data(), m_records.size() };
}

inline folly::Range<const RecordPtr*> Unit::records() const {
  return { m_records.data(), m_records.size() };
}

template<class Fn> void Unit::forEachFunc(Fn fn) const {
  for (auto& func : funcs()) {
    fn(func);
  }
  for (auto& c : preclasses()) {
    auto methods = FuncRange{c->methods(), c->methods() + c->numMethods()};
    for (auto& method : methods) {
      fn(method);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Type aliases

inline folly::Range<TypeAlias*> Unit::typeAliases() {
  return { m_typeAliases.begin(), m_typeAliases.end() };
}

inline folly::Range<const TypeAlias*> Unit::typeAliases() const {
  return { m_typeAliases.begin(), m_typeAliases.end() };
}

///////////////////////////////////////////////////////////////////////////////
// File attributes

inline const UserAttributeMap& Unit::fileAttributes() const {
  return m_fileAttributes;
}

///////////////////////////////////////////////////////////////////////////////
// Record lookup.

inline Record* Unit::lookupRecord(const NamedEntity* ne) {
  return ne->getCachedRecord();
}

inline Record* Unit::lookupRecord(const StringData* name) {
  return lookupRecord(NamedEntity::get(name));
}

///////////////////////////////////////////////////////////////////////////////
// Class lookup.

inline Class* Unit::lookupClass(const NamedEntity* ne) {
  return ne->getCachedClass();
}

inline Class* Unit::lookupClass(const StringData* name) {
  return lookupClass(NamedEntity::get(name));
}

inline const Class* Unit::lookupUniqueClassInContext(const NamedEntity* ne,
                                                     const Class* ctx) {
  Class* cls = ne->clsList();
  if (UNLIKELY(cls == nullptr)) return nullptr;
  if (cls->attrs() & AttrUnique) return cls;
  if (!ctx) return nullptr;
  return ctx->getClassDependency(cls->name());
}

inline const Class* Unit::lookupUniqueClassInContext(const StringData* name,
                                                     const Class* ctx) {
  return lookupUniqueClassInContext(NamedEntity::get(name), ctx);
}

inline Class* Unit::loadClass(const StringData* name) {
  String normStr;
  auto ne = NamedEntity::get(name, true, &normStr);
  if (normStr) {
    name = normStr.get();
  }
  auto class_ = loadClass(ne, name);
  if (LIKELY(class_ != nullptr) || !isReifiedName(name)) return class_;
  // We are loading a reified class for the first time
  name = stripTypeFromReifiedName(name);
  auto generic_ne = NamedEntity::get(name, true, &normStr);
  if (normStr) {
    name = normStr.get();
  }
  class_ = loadClass(generic_ne, name);
  // If the class still does not exists, return null
  if (!class_) return nullptr;
  ne->m_cachedClass.bind(
    classHasPersistentRDS(class_) ? rds::Mode::Persistent : rds::Mode::Normal);
  ne->setCachedClass(class_);
  return class_;
}

inline Class* Unit::getClass(const StringData* name, bool tryAutoload) {
  String normStr;
  auto ne = NamedEntity::get(name, true, &normStr);
  if (normStr) {
    name = normStr.get();
  }
  return getClass(ne, name, tryAutoload);
}

///////////////////////////////////////////////////////////////////////////////
// Merge.

inline bool Unit::isMergeOnly() const {
  return m_mergeOnly;
}

inline bool Unit::isEmpty() const {
  return m_mergeState.load(std::memory_order_relaxed) & MergeState::Empty;
}

inline const TypedValue* Unit::getMainReturn() const {
  assertx(isMergeOnly());
  return &m_mainReturn;
}

///////////////////////////////////////////////////////////////////////////////
// Other methods.

inline bool Unit::isInterpretOnly() const {
  return m_interpretOnly;
}

inline void Unit::setInterpretOnly() {
  m_interpretOnly = true;
}

inline bool Unit::isHHFile() const {
  return m_isHHFile;
}

inline bool Unit::useStrictTypes() const {
  return m_useStrictTypes;
}

inline bool Unit::useStrictTypesForBuiltins() const {
  return m_useStrictTypesForBuiltins;
}

inline UserAttributeMap Unit::metaData() const {
  return m_metaData;
}

///////////////////////////////////////////////////////////////////////////////
}
