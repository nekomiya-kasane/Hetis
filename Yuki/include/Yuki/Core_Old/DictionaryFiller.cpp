
#include "DictionaryFiller.h"

#include "BaseUnknown.h"
#include "MetaClass.h"
#include "DictionaryManager.h"

namespace DE {

  DictionaryFiller::DictionaryFiller(
      const std::string_view& iComponentName, const std::string_view& iInterfaceName, CreationFunc iCreator
  ) {
    // Record of a class and MetaClass will be appended while constructing/newing
    // its meta class
    if (iInterfaceName == MetaClass::Name) {
      return;
    }

    const auto compMeta = GetDictionaryManager().GetMetaClass(iComponentName);
    const auto intfMeta = GetDictionaryManager().GetMetaClass(iInterfaceName);

    new (this) DictionaryFiller(compMeta, intfMeta, iCreator);
  }

  DictionaryFiller::DictionaryFiller(
      const MetaClass* iComponentMeta, const MetaClass* iInterfaceMeta, CreationFunc iCreator
  ) {
    if (!iComponentMeta || !iInterfaceMeta) {
      return;
    }

    // the impl will support all methods in the classes, which are in the
    // inheritance tree of interface classes
    switch (iComponentMeta->GetTypeOfClass()) {
      default: std::unreachable();
      case TypeOfClass::NothingType:  // @todo (Nekomiya) unusual
      case TypeOfClass::Implementation:
        FillDictionaryForImplementation(iComponentMeta, iInterfaceMeta, iCreator);
        break;
      case TypeOfClass::DataExtension:
      case TypeOfClass::CodeExtension:
      case TypeOfClass::CacheExtension:
      case TypeOfClass::TransientExtension: FillDictionaryForExtension(iComponentMeta, iInterfaceMeta, iCreator); break;
    }
  }

  void DictionaryFiller::FillDictionaryForImplementation(
      const MetaClass* iComponentMeta, const MetaClass* iInterfaceMeta, CreationFunc iCreator
  ) {
    const auto implName = iComponentMeta->ClassName();
    const MetaClass* curMeta = iInterfaceMeta;

    auto priority = CompIntfRecord::Priority::DIRECTLY_IMPLEMENTED;

    while (curMeta) {
      static auto baseUnknownName = GetDictionaryManager().AddString(BaseUnknown::Name);
      const bool isIndirectImpl = priority == CompIntfRecord::INDIRECTLY_IMPLEMENTED;
      const bool isBaseUnknown = curMeta->IsA() == baseUnknownName;
      if (isIndirectImpl && isBaseUnknown) {
        break;  // Do not add BaseUnknown interface
      }

      CompIntfRecord record = {
        .componentName = implName,
        .interfaceName = curMeta->ClassName(),
        .libraryName = "",
        .creationFunc = static_cast<void*>(iCreator),
        .priority = priority,
        .status = CompIntfRecord::LOADED,
      };

      // todo: should MetaClass cache the interfaces?

      GetDictionaryManager().AddCompIntfRecord(record);

      curMeta = curMeta->GetBaseClass();
      priority = CompIntfRecord::INDIRECTLY_IMPLEMENTED;
    }
  }

  void DictionaryFiller::FillDictionaryForExtension(
      const MetaClass* iExtensionMeta, const MetaClass* iInterfaceMeta, CreationFunc iCreator
  ) {
    //
    // [0] Extendee_1
    // Add     |
    // -->   Intf --> IntfBase --> IntfBaseBase
    //
    // [1] Extendee_2
    // Add     |
    // -->   Intf --> IntfBase --> IntfBaseBase
    //
    // [2] Extendee_3
    // Add     |
    // -->   Intf --> IntfBase --> IntfBaseBase
    //
    auto& manager = GetDictionaryManager();

    const auto extendees = iExtensionMeta->GetExtendees();
    // todo: use openmp to accelerate, the action on different extendees are totally independent
    // todo: as some extendees may share a common ancestor, stopping transversing upwards on intf base is possible
    for (size_t i = 0; extendees[i]; ++i) {
      const auto curExtendee = extendees[i];
      const auto curExtendeeName = curExtendee->ClassName();

      auto priority = CompIntfRecord::Priority::DIRECTLY_IMPLEMENTED;

      for (auto curIntfBase = iInterfaceMeta; curIntfBase;) {
        if (curIntfBase->GetTypeOfClass() == TypeOfClass::BaseUnknown) {
          break;
        }

        const auto curIntfBaseName = curIntfBase->ClassName();
        CompIntfRecord record = {
          .componentName = curExtendeeName,
          .interfaceName = curIntfBaseName,
          .libraryName = "",
          .creationFunc = static_cast<void*>(iCreator),
          .priority = priority,
          .status = CompIntfRecord::RecordStatus::LOADED
        };
        manager.AddCompIntfRecord(record);

        curIntfBase = curIntfBase->GetBaseClass();
        priority = CompIntfRecord::INDIRECTLY_IMPLEMENTED;
      }
    }
  }

}  // namespace DE