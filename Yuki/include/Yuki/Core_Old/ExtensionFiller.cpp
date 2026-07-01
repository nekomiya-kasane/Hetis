#include "ExtensionFiller.h"

#include "DictionaryManager.h"

namespace DE {

  ExtensionFiller::ExtensionFiller(MetaClass* iMetaExtension, MetaClass* iMetaExtendee) {
    if (!iMetaExtension || !iMetaExtendee) {
      return;
    }

    iMetaExtension->SetExtensionOf(iMetaExtendee);
  }

  ExtensionFiller::ExtensionFiller(MetaClass* iMetaExtension, std::string_view iExtendeeName) {
    if (!iMetaExtension || iExtendeeName.empty()) {
      return;
    }

    // is the extendee already has a meta class?
    auto metaExtendee = GetDictionaryManager().GetMetaClass(iExtendeeName);

    iMetaExtension->SetExtensionOf(metaExtendee);
  }

  ExtensionFiller::ExtensionFiller(std::string_view iExtensionName, MetaClass* iMetaExtendee) {
    if (!iMetaExtendee || iExtensionName.empty()) {
      return;
    }

    auto metaExtendee = GetDictionaryManager().GetMetaClass(iExtensionName);

    metaExtendee->SetExtensionOf(iMetaExtendee);
  }

}  // namespace DE
