#pragma once

#include <type_traits>

#include "MacrosForBaseUnknown.h"
#include "MacrosForTie.h"

#define Internal_DeclareExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, EXTENSION_TYPE) \
  DeclareClassComponents(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, EXTENSION_TYPE);         \
                                                                                                 \
  template <class... Args>                                                                       \
  inline static BaseUnknown* Construct(Args&&... args) {                                     \
    return new IMPLEMENTATION_NAME(std::forward<Args>(args)...);                                 \
  }                                                                                              \
                                                                                                 \
 private:

#define DeclareDataExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME) \
  Internal_DeclareExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, DataExtension)

#define DeclareCodeExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME) \
  Internal_DeclareExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, CodeExtension)

#define DeclareCacheExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME) \
  Internal_DeclareExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, CacheExtension)

#define DeclareTransientExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME) \
  Internal_DeclareExtension(IMPLEMENTATION_NAME, BASE_IMPLEMENTATION_NAME, TransientExtension)

#define ImplementExtensionBegin(IMPLEMENTATION_NAME)                                                                   \
  static_assert(                                                                                                       \
      has_flag(TypeOfClass::Any_Extension, IMPLEMENTATION_NAME::Type), "Class type mismatch for " #IMPLEMENTATION_NAME \
  );                                                                                                                   \
  static_assert(                                                                                                       \
      IMPLEMENTATION_NAME::Type != TypeOfClass::CodeExtension                                                          \
          || sizeof(IMPLEMENTATION_NAME) == sizeof(BaseUnknown),                                                   \
      "CodeExtension " #IMPLEMENTATION_NAME " seems to contain data. It is larger than BaseUnknown."               \
  );                                                                                                                   \
  static_assert(                                                                                                       \
      IMPLEMENTATION_NAME::Type != TypeOfClass::Any_TIE && IMPLEMENTATION_NAME::Type != TypeOfClass::Any_Extension     \
          && IMPLEMENTATION_NAME::Type != TypeOfClass::Any_Component,                                                  \
      "Compound class type Any_* is forbidden."                                                                        \
  );                                                                                                                   \
  template DictionaryFiller RecordAppenderBOA<IMPLEMENTATION_NAME, IMPLEMENTATION_NAME>;                               \
  ImplementClassBegin(IMPLEMENTATION_NAME)

#define ImplementExtensionEnd() \
  ImplementClassEnd             \
  ()

#define Extension_Bind_Inplace(EXTENDEE_NAME) \
  _metaObject->SetExtensionOf(GetDictionaryManager().GetMetaClass(#EXTENDEE_NAME));

#define Extension_Bind_Inplace_By_Class(EXTENDEE_NAME) \
  _metaObject->SetExtensionOf(GetDictionaryManager().GetMetaClass(EXTENDEE_NAME::GetMetaStatic()));

// extension implements the interface, and is bound to extendee
#define Extension_Bind(EXTENSION_NAME, EXTENDEE_NAME, INTERFACE_NAME)                                           \
  static ExtensionFiller AppendRecordFor_Extension_##EXTENSION_NAME##_##EXTENDEE_NAME##_##INTERFACE_NAME(       \
      #EXTENSION_NAME, #EXTENDEE_NAME, #INTERFACE_NAME, (CreationFunc)CreateTIE<INTERFACE_NAME, EXTENSION_NAME> \
  );

#define DEBeginImplementClass(IMPLEMENTATION_NAME, CacheExtension, BASE_CLASS, EXTENDEE_NAME) \
  ImplementExtensionBegin(IMPLEMENTATION_NAME)                                                  \
    ;                                                                                           \
    Extension_Bind_Inplace(EXTENDEE_NAME)

#define DEAddClassExtension(EXTENDEE_NAME) Extension_Bind_Inplace(EXTENDEE_NAME)

#define DEEndImplementClass(IMPLEMENTATION_NAME) \
  ImplementExtensionEnd                            \
  ()

#define DEAddExtendedImplementation(EXTENDEE_NAME) Extension_Bind_Inplace(EXTENDEE_NAME)

#define DEEndImplementKindOf(DEParamEdit) \
  ImplementExtensionEnd                       \
  ()

#define DEBeginImplementKindOf(IMPLEMENTATION_NAME, CacheExtension, BASE_CLASS, EXTENDEE_NAME) \
  ImplementExtensionBegin(IMPLEMENTATION_NAME)                                                   \
    ;                                                                                            \
    Extension_Bind_Inplace(EXTENDEE_NAME)