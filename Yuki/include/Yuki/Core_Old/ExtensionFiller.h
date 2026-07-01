#pragma once

#include "MetaClass.h"

namespace DE {

  class ExtensionFiller {
    /**
     * @brief 为扩展类添加被拓展类
     */
    ExtensionFiller(MetaClass* iMetaExtension, MetaClass* iMetaExtendee);
    ExtensionFiller(MetaClass* iMetaExtension, std::string_view iExtendeeName);
    ExtensionFiller(std::string_view iExtensionName, MetaClass* iMetaExtendee);
  };

}  // namespace DE
