#pragma once

#include "MetaClass.h"
#include "DictionaryFiller.h"

#include "gin/runtime/assertion.h"

#include <type_traits>

#define DeclareClassInterfaceStatic(CLASS_NAME, BASE_CLASS_NAME, CLASS_TYPE) \
 private:                                                                    \
  static MetaClass* _metaObject;                                         \
                                                                             \
 public:                                                                     \
  constexpr static std::string_view Name = #CLASS_NAME;                      \
  constexpr static TypeOfClass Type = TypeOfClass::CLASS_TYPE;               \
                                                                             \
  using Base = BASE_CLASS_NAME;                                              \
  using This = CLASS_NAME;                                                   \
                                                                             \
  [[nodiscard]] bool IsDelegation() const override {                         \
    return false;                                                            \
  }                                                                          \
                                                                             \
  static MetaClass* GetMetaStatic();                                  \
  /* virtual function for getting real meta object */                        \
  /*virtual MetaClass *GetMeta() const { */                        \
  /*  return _metaObject; */                                                 \
  /*} */

#define DeclareClassInterface(CLASS_NAME, BASE_CLASS_NAME, CLASS_TYPE)  \
  DeclareClassInterfaceStatic(CLASS_NAME, BASE_CLASS_NAME, CLASS_TYPE); \
                                                                        \
  [[nodiscard]] static CLASS_NAME* Cast(BaseUnknown* iFrom) {       \
    return dynamic_cast<CLASS_NAME*>(iFrom);                            \
  }                                                                     \
                                                                        \
  [[nodiscard]] constexpr std::string_view ClassName() const override { \
    static_assert(CLASS_NAME::Name == Name, "Wrong class name");        \
    return CLASS_NAME::Name;                                            \
  }                                                                     \
  [[nodiscard]] constexpr TypeOfClass ClassType() const override {      \
    return TypeOfClass::CLASS_TYPE;                                     \
  }                                                                     \
  [[nodiscard]] constexpr const char* IsA() const override {            \
    return CLASS_NAME::Name.data();                                     \
  }

#define DeclareClassComponents(CLASS_NAME, BASE_CLASS_NAME, CLASS_TYPE) \
  DeclareClassInterface(CLASS_NAME, BASE_CLASS_NAME, CLASS_TYPE);       \
  virtual MetaClass* GetMeta() const override {               \
    return _metaObject;                                                 \
  }

#define DeclareClass(CLASS_NAME, BASE_CLASS_NAME, CLASS_TYPE) Declare##CLASS_TYPE(CLASS_NAME, BASE_CLASS_NAME);

#define ImplementClassBegin_1(CLASS_NAME)                                                                           \
  static DictionaryFiller AppendRecordForMetaClass_##CLASS_NAME(                                                    \
      CLASS_NAME::Name, MetaClass::Name, (CreationFunc)CLASS_NAME::GetMetaStatic()                           \
  );                                                                                                                \
                                                                                                                    \
  static_assert(                                                                                                    \
      !std::is_same_v<CLASS_NAME, CLASS_NAME::Base>, "A class cannot be the base class of itself: " #CLASS_NAME     \
  );                                                                                                                \
  static_assert(std::is_base_of_v<CLASS_NAME::Base, CLASS_NAME>, "Base class check failed: " #CLASS_NAME);          \
  static_assert(std::is_base_of_v<BaseUnknown, CLASS_NAME>, "Not BaseUnknown-derived class: " #CLASS_NAME); \
                                                                                                                    \
  MetaClass* CLASS_NAME::_metaObject = nullptr;                                                                 \
  [[nodiscard]] MetaClass* CLASS_NAME::GetMetaStatic() {                                                     \
    if (!_metaObject) {                                                                                             \
      _metaObject = GetDictionaryManager().GetMetaClass(CLASS_NAME::Name);                                          \
      _metaObject->SetClassType(CLASS_NAME::Type);                                                                  \
      _metaObject->SetBaseClassMeta(CLASS_NAME::Base::GetMetaStatic());

#define ImplementClassBegin_2(NAMESPACE, CLASS_NAME)                                                            \
  static DictionaryFiller AppendRecordForMetaClass_##NAMESPACE##_##CLASS_NAME(                                  \
      NAMESPACE::CLASS_NAME::Name, MetaClass::Name, (CreationFunc)NAMESPACE::CLASS_NAME::GetMetaStatic() \
  );                                                                                                            \
                                                                                                                \
  static_assert(                                                                                                \
      !std::is_same_v<NAMESPACE::CLASS_NAME, NAMESPACE::CLASS_NAME::Base>,                                      \
      "A class cannot be the base class of itself: " #CLASS_NAME                                                \
  );                                                                                                            \
  static_assert(                                                                                                \
      std::is_base_of_v<NAMESPACE::CLASS_NAME::Base, NAMESPACE::CLASS_NAME>,                                    \
      "Base class check failed: " #NAMESPACE "::" #CLASS_NAME                                                   \
  );                                                                                                            \
  static_assert(                                                                                                \
      std::is_base_of_v<BaseUnknown, NAMESPACE::CLASS_NAME>,                                                \
      "Not BaseUnknown-derived class: " #NAMESPACE "::" #CLASS_NAME                                         \
  );                                                                                                            \
                                                                                                                \
  MetaClass* NAMESPACE::CLASS_NAME::_metaObject = nullptr;                                                  \
  [[nodiscard]] MetaClass* NAMESPACE::CLASS_NAME::GetMetaStatic() {                                      \
    if (!_metaObject) {                                                                                         \
      _metaObject = GetDictionaryManager().GetMetaClass(NAMESPACE::CLASS_NAME::Name);                           \
      _metaObject->SetClassType(NAMESPACE::CLASS_NAME::Type);                                                   \
      _metaObject->SetBaseClassMeta(NAMESPACE::CLASS_NAME::Base::GetMetaStatic());

#define Internal_ImplementClassBegin_Impl(...) \
  M_MACRO_OVERRIDING_HELPER(M_MACRO_OVERRIDING_IMPL_2(__VA_ARGS__, ImplementClassBegin_2, ImplementClassBegin_1))

#define ImplementClassBegin(...) M_MACRO_OVERRIDING_HELPER(Internal_ImplementClassBegin_Impl(__VA_ARGS__)(__VA_ARGS__))

#define ImplementClassEnd() \
  }                         \
  return _metaObject;       \
  }

// why begin and end? => for extensions to insert some other codes
#define ImplementClass_1(CLASS_NAME) \
  ImplementClassBegin_1(CLASS_NAME)  \
  ImplementClassEnd();

#define ImplementClass_2(NAMESPACE, CLASS_NAME) \
  ImplementClassBegin_2(NAMESPACE, CLASS_NAME)  \
  ImplementClassEnd();

#define Internal_ImplementClass_Impl(...) \
  M_MACRO_OVERRIDING_HELPER(M_MACRO_OVERRIDING_IMPL_2(__VA_ARGS__, ImplementClass_2, ImplementClass_1))

#define ImplementClass(...) M_MACRO_OVERRIDING_HELPER(Internal_ImplementClass_Impl(__VA_ARGS__)(__VA_ARGS__))

#define DeclareInterface(INTERFACE_NAME, BASE_INTERFACE_NAME)            \
  DeclareClassInterface(INTERFACE_NAME, BASE_INTERFACE_NAME, Interface); \
                                                                         \
 private:

#define ImplementInterface(INTERFACE_NAME) ImplementClass(INTERFACE_NAME)

#define DeclareImplementation(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME)             \
  DeclareClassComponents(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, Implementation); \
                                                                                         \
  template <class... Args>                                                               \
    requires std::constructible_from<IMPLEMENTATION_NAME, Args...>                       \
  [[nodiscard]] inline static IMPLEMENTATION_NAME* Construct(Args&&... args) {           \
    return new IMPLEMENTATION_NAME(std::forward<Args>(args)...);                         \
  }                                                                                      \
                                                                                         \
  inline static constexpr bool VirtualClass = false;                                     \
  inline static constexpr bool LateType = false;                                         \
                                                                                         \
 private:

#define DeclareVirtualImplementation(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME)      \
  DeclareClassComponents(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, Implementation); \
                                                                                         \
  template <class... Args>                                                               \
  inline static IMPLEMENTATION_NAME* Construct(Args&&... args) {                         \
    M_VERIFY_UNREACHABLE_MSG("Attempting to construct pure virtual class");              \
    return nullptr;                                                                      \
  }                                                                                      \
                                                                                         \
  inline static constexpr bool VirtualClass = true;                                      \
  inline static constexpr bool LateType = false;                                         \
                                                                                         \
 private:

#define DeclareLateTypeImplementation(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME)          \
  DeclareClassInterfaceStatic(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, Implementation); \
                                                                                              \
  static_assert(IMPLEMENTATION_NAME::Type == TypeOfClass::Implementation, "Not supported");   \
                                                                                              \
  [[nodiscard]] constexpr std::string_view ClassName() const override {                       \
    static_assert(IMPLEMENTATION_NAME::Name == Name, "Wrong class name");                     \
    return IMPLEMENTATION_NAME::Name;                                                         \
  }                                                                                           \
  [[nodiscard]] constexpr TypeOfClass ClassType() const override {                            \
    return Type;                                                                              \
  }                                                                                           \
  [[nodiscard]] constexpr const char* IsA() const override {                                  \
    return IMPLEMENTATION_NAME::Name.data();                                                  \
  }                                                                                           \
  [[nodiscard]] MetaClass* GetMeta() const override {                               \
    return DEObject::GetMeta();                                                       \
  }                                                                                           \
  template <class... Args>                                                                    \
    requires std::constructible_from<IMPLEMENTATION_NAME, Args...>                            \
  [[nodiscard]] inline static IMPLEMENTATION_NAME* Construct(Args&&... args) {                \
    return new IMPLEMENTATION_NAME(std::forward<Args>(args)...);                              \
  }                                                                                           \
                                                                                              \
  inline static constexpr bool VirtualClass = false;                                          \
  inline static constexpr bool LateType = true;                                               \
                                                                                              \
 private:

#define ImplementImplementation_1(IMPLEMENTATION_NAME)                                                           \
  ImplementClass_1(IMPLEMENTATION_NAME);                                                                         \
  static_assert(                                                                                                 \
      IMPLEMENTATION_NAME::VirtualClass == std::is_abstract_v<IMPLEMENTATION_NAME>, "Wrong class classification" \
  );                                                                                                             \
  static_assert(IMPLEMENTATION_NAME::LateType == false, "Use ImplementLateTypeImplementation type");

#define ImplementImplementation_2(NAMESPACE, IMPLEMENTATION_NAME)                                         \
  ImplementClass_2(NAMESPACE, IMPLEMENTATION_NAME);                                                       \
  static_assert(                                                                                          \
      NAMESPACE::IMPLEMENTATION_NAME::VirtualClass == std::is_abstract_v<NAMESPACE::IMPLEMENTATION_NAME>, \
      "Wrong class classification"                                                                        \
  );                                                                                                      \
  static_assert(NAMESPACE::IMPLEMENTATION_NAME::LateType == false, "Use ImplementLateTypeImplementation type");

#define Internal_ImplementImplementation_Impl(...)                                                 \
  M_MACRO_OVERRIDING_HELPER(                                                                       \
      M_MACRO_OVERRIDING_IMPL_2(__VA_ARGS__, ImplementImplementation_2, ImplementImplementation_1) \
  )

#define ImplementImplementation(...) \
  M_MACRO_OVERRIDING_HELPER(Internal_ImplementImplementation_Impl(__VA_ARGS__)(__VA_ARGS__))

#define ImplementLateTypeBegin(CLASS_NAME)                                                                          \
  static DictionaryFiller AppendRecordForMetaClass_##CLASS_NAME(                                                    \
      CLASS_NAME::Name, MetaClass::Name, (CreationFunc)CLASS_NAME::GetMetaStatic()                           \
  );                                                                                                                \
                                                                                                                    \
  static_assert(std::is_same_v<CLASS_NAME, CLASS_NAME::This>, "Declaration not matched: " #CLASS_NAME);             \
  static_assert(                                                                                                    \
      !std::is_same_v<CLASS_NAME, CLASS_NAME::Base>, "A class cannot be the base class of itself: " #CLASS_NAME     \
  );                                                                                                                \
  static_assert(std::is_base_of_v<CLASS_NAME::Base, CLASS_NAME>, "Base class check failed: " #CLASS_NAME);          \
  static_assert(std::is_base_of_v<BaseUnknown, CLASS_NAME>, "Not BaseUnknown-derived class: " #CLASS_NAME); \
  static_assert(std::is_base_of_v<DEObject, CLASS_NAME>, "Not DEObject-derived class: " #CLASS_NAME);           \
                                                                                                                    \
  static_assert(CLASS_NAME::VirtualClass == std::is_abstract_v<CLASS_NAME>, "Wrong class classification");          \
  static_assert(CLASS_NAME::LateType == true, "Not late type");                                                     \
                                                                                                                    \
  MetaClass* CLASS_NAME::_metaObject = nullptr;                                                                 \
  MetaClass* CLASS_NAME::GetMetaStatic() {                                                                   \
    if (!_metaObject) {                                                                                             \
      _metaObject = GetDictionaryManager().GetMetaClass(CLASS_NAME::Name);                                          \
      _metaObject->SetClassType(CLASS_NAME::Type);                                                                  \
      _metaObject->SetBaseClassMeta(DEImplementationAdapter::GetMetaStatic()); /* <- only this differs */

#define ImplementLateTypeEnd() \
  }                            \
  return _metaObject;          \
  }

// why begin and end? => for extensions to insert some other codes
#define ImplementLateType(CLASS_NAME) \
  ImplementLateTypeBegin(CLASS_NAME); \
  ImplementLateTypeEnd();

#define ImplementLateTypeImplementation(IMPLEMENTATION_NAME) ImplementLateType(IMPLEMENTATION_NAME);

#define DEGTMMacCreateBegin(iDEGeoFactoryGEO, iClassImpl, iClassInterface)       \
  if (!iDEGeoFactoryGEO->_Root) iDEGeoFactoryGEO->InitRootContainer();           \
  iClassInterface* NewInterface = nullptr;                                           \
  iClassImpl* NewObject = new iClassImpl();                                          \
  NewObject->SetContainerImpl(iDEGeoFactoryGEO->_Root);                            \
  NewObject->QueryInterface(iClassInterface::Name, (BaseUnknown*&)NewInterface); \
  NewObject->SetInterface(NewInterface)

#define DEGTMMacCreateEnd(iDEGeoFactoryGEO, iEvent)                    \
  ((DEGTMContainer*)(void*)(iDEGeoFactoryGEO->_Root))->Add(NewObject); \
  if (iEvent) {                                                            \
    NewObject->IsModified();                                               \
  }                                                                        \
  if (iDEGeoFactoryGEO->_CreateExplicit) {                               \
    NewObject->SetMode(DEGTMExplicit);                                   \
  }

#define DEMacCreateBegin(iClassImpl, iClassInterface) DEGTMMacCreateBegin(this, iClassImpl, iClassInterface);

#define DEMacCreateEnd(iEvent) DEGTMMacCreateEnd(this, iEvent)

#define DEGeoObjNewSameType(CLASS, INTERFACE)                                                             \
  virtual DEGTMObject* NewSameType() const {                                                              \
    CLASS* new_instance = new CLASS();                                                                      \
    if (!new_instance) {                                                                                    \
      return nullptr;                                                                                       \
    }                                                                                                       \
    BaseUnknown* NewItf = static_cast<BaseUnknown*>(new_instance->GetInterface());                  \
    if (!NewItf) {                                                                                          \
      new_instance->Release();                                                                              \
    }                                                                                                       \
    if (!NewItf) {                                                                                          \
      return nullptr;                                                                                       \
    }                                                                                                       \
    DEGTMObject* NewImpl = (DEGTMObject*)NewItf->GetImpl();                                             \
    return NewImpl;                                                                                         \
  }                                                                                                         \
  bool IsATypeOf(DEGeometricType TypeReference) const {                                                   \
    static DEGeometricType MaskIndex = 0x0000000F;                                                        \
    static DEGeometricType MaskArray[8]                                                                   \
        = {0x00000000, 0xF0000000, 0xFF000000, 0xFFF00000, 0xFFFF0000, 0xFFFFF000, 0xFFFFFF00, 0xFFFFFFF0}; \
    DEGeometricType Index = TypeReference & MaskIndex;                                                    \
    DEGeometricType NewReference = TypeReference & MaskArray[Index];                                      \
    DEGeometricType NewToCompare = INTERFACE##Type & MaskArray[Index];                                    \
    return (NewReference == NewToCompare);                                                                  \
  }                                                                                                         \
  DEGeometricType GetVolatileType() const {                                                               \
    return INTERFACE##Type;                                                                                 \
  }                                                                                                         \
  static BaseUnknown* CreationGTM() {                                                                   \
    CLASS* new_instance = new CLASS;                                                                        \
    if (!new_instance) {                                                                                    \
      return nullptr;                                                                                       \
    }                                                                                                       \
    BaseUnknown* new_itf = static_cast<BaseUnknown*>(new_instance->GetInterface());                 \
    if (!new_itf) {                                                                                         \
      new_instance->Release();                                                                              \
    }                                                                                                       \
    return new_itf;                                                                                         \
  }