#pragma once

#include "gin/predefs.h"

#include "DictionaryManager.h"

/**
 * @file MacrosForDelegation.h
 *
 * @copydoc DEDelegation
 */

//
// Declarations
//

/**
 * @copydoc DEDelegation
 *
 * @brief The macro to declare a class to be a delegation extension in the generated TIE header
 * @param INTERFACE_NAME
 */
#define DeclareDelegation(INTERFACE_NAME)                                                                              \
 private:                                                                                                              \
  INTERFACE_NAME* _delegatee;                                                                                          \
  static MetaClass* _metaObject;                                                                                   \
                                                                                                                       \
 public:                                                                                                               \
  using Base = INTERFACE_NAME;                                                                                         \
  using This = DelegationExtension_##INTERFACE_NAME;                                                                   \
  constexpr static std::string_view Name                                                                               \
      = utils::concat(utils::fixed_string("DelegationExtension_" #INTERFACE_NAME "_"), Delegator);                     \
  constexpr static std::string_view DelegatorName = Delegator;                                                         \
  constexpr static std::string_view DelegateeName = Base::Name;                                                        \
  constexpr static TypeOfClass Type = TypeOfClass::CacheExtension;                                                     \
                                                                                                                       \
  DelegationExtension_##INTERFACE_NAME() = default;                                                                    \
  template <class... Args>                                                                                             \
  inline static BaseUnknown* Construct(Args&&... args) {                                                           \
    return new DelegationExtension_##INTERFACE_NAME(std::forward<Args>(args)...);                                      \
  }                                                                                                                    \
                                                                                                                       \
  static MetaClass* GetMetaStatic() {                                                                           \
    if (_metaObject) {                                                                                                 \
      return _metaObject;                                                                                              \
    }                                                                                                                  \
                                                                                                                       \
    auto&& dictManager = GetDictionaryManager();                                                                       \
                                                                                                                       \
    std::string_view name = utils::concat(utils::fixed_string("DelegationExtension_" #INTERFACE_NAME "_"), Delegator); \
    _metaObject = new MetaClass(name, Base::GetMetaStatic(), Type);                                             \
    _metaObject->SetClassType(Type);                                                                                   \
    _metaObject->SetBaseClassMeta(Base::GetMetaStatic());                                                           \
    _metaObject->SetExtensionOf(dictManager.GetMetaClass(DelegatorName));                                              \
                                                                                                                       \
    dictManager.AddMetaClass(name, _metaObject);                                                                       \
    return _metaObject;                                                                                                \
  }                                                                                                                    \
  MetaClass* GetMeta() const override {                                                                      \
    return GetMetaStatic();                                                                                         \
  }                                                                                                                    \
  constexpr std::string_view ClassName() const override {                                                              \
    static const auto s_name                                                                                           \
        = utils::concat(utils::fixed_string("DelegationExtension_" #INTERFACE_NAME "_"), Delegator);                   \
    return s_name;                                                                                                     \
  }                                                                                                                    \
  constexpr TypeOfClass ClassType() const override {                                                                   \
    return Type;                                                                                                       \
  }                                                                                                                    \
  constexpr const char* IsA() const override {                                                                         \
    return ClassName().data();                                                                                         \
  }                                                                                                                    \
                                                                                                                       \
  DelegationExtension_##INTERFACE_NAME(BaseUnknown* iDelegator) {                                                  \
    _delegatee = CanDelegate() ? dynamic_cast<INTERFACE_NAME*>(DEDelegation::CanDelegate(iDelegator, DelegateeName)) \
                               : nullptr;                                                                              \
  }                                                                                                                    \
  virtual ~DelegationExtension_##INTERFACE_NAME() {                                                                    \
    if (_delegatee) {                                                                                                  \
      _delegatee->Release();                                                                                           \
      _delegatee = nullptr;                                                                                            \
    }                                                                                                                  \
  }                                                                                                                    \
                                                                                                                       \
 public:                                                                                                               \
  virtual SystemStatus QueryInterface(const std::string_view& iInterfaceName, BaseUnknown*& iInstance) {           \
    if (iInterfaceName == DEIDelegation::INTERFACE_ON_DELEGATION) {                                                  \
      iInstance = _delegatee;                                                                                          \
      return StatusCode::OK;                                                                                           \
    }                                                                                                                  \
    return Base::QueryInterface(iInterfaceName, iInstance);                                                            \
  }                                                                                                                    \
                                                                                                                       \
  int IsNull() const override {                                                                                        \
    return !_delegatee;                                                                                                \
  }                                                                                                                    \
  bool IsDelegation() const override {                                                                                 \
    return true;                                                                                                       \
  }                                                                                                                    \
  bool CanDelegate() const;                                                                                            \
                                                                                                                       \
 private:

/**
 * @copydoc DEDelegation
 *
 * @brief The macro to define a delegation relationship. This must be placed in cpp files.
 * @param INTERFACE_NAME
 */
#define ImplementDelegation(DELEGATED_INTERFACE, DELEGATOR)                                                         \
  constexpr const char DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR[] = #DELEGATOR;                      \
  MetaClass* DelegationExtension_COMaI4<DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR>::_metaObject   \
      = nullptr;                                                                                                    \
  bool                                                                                                              \
  DelegationExtension_##DELEGATED_INTERFACE<DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR>::CanDelegate() \
      const {                                                                                                       \
    return true;                                                                                                    \
  }                                                                                                                 \
  template DLLEXPORT                                                                                                \
      DelegationExtension_##DELEGATED_INTERFACE<DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR>;           \
  static DictionaryFiller AppendRecordForMetaClass_DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR(         \
      DelegationExtension_##DELEGATED_INTERFACE<DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR>::Name,     \
      MetaClass::Name,                                                                                          \
      (CreationFunc)DelegationExtension_##DELEGATED_INTERFACE<                                                      \
          DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR>::GetMetaStatic()                              \
  );                                                                                                                \
  DictionaryFiller RecordAppenderBoa_DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR = {                    \
    DelegationExtension_##DELEGATED_INTERFACE<                                                                      \
        DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR>::GetMetaStatic(),                               \
    DELEGATED_INTERFACE::GetMetaStatic(),                                                                        \
    &CreateBOA<                                                                                                     \
        ##DELEGATED_INTERFACE##,                                                                                    \
        DelegationExtension_##DELEGATED_INTERFACE<DelegationExtension_##DELEGATED_INTERFACE##_##DELEGATOR>>         \
  };

#define Delegation_RxBind_Default() _delegatee
