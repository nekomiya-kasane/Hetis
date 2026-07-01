#pragma once

#include "MetaClass.h"

#include "core_export.h"

namespace DE {

  /**
   * Do not use this class. For internal use only.
   * Class used to fill in the interface dictionary at runtime.
   */
  class CORE_EXPORT DictionaryFiller final {
   public:
    /**
     * Constructs a DictionaryFiller instance by means of a class GUID and
     * the interface GUID it implements.
     * @param iComponentName      GUID of the class implementing the interface
     * @param iInterfaceName  GUID of the interface
     * @param iCreator   creation function
     */
    DictionaryFiller(
        const std::string_view& iComponentName, const std::string_view& iInterfaceName, CreationFunc iCreator
    );

    /**
     * Constructs a DictionaryFiller instance by means of the meta objects
     * of both the interface class and the class that implements it.
     * @param iComponentMeta Meta object of the class implementing the interface.
     * @param iInterfaceMeta Meta object of the interface class.
     * @param iCreator Creation function.
     */
    DictionaryFiller(const MetaClass* iComponentMeta, const MetaClass* iInterfaceMeta, CreationFunc iCreator);

   private:
    static void FillDictionaryForImplementation(
        const MetaClass* iComponentMeta, const MetaClass* iInterfaceMeta, CreationFunc iCreator
    );
    static void FillDictionaryForExtension(
        const MetaClass* iExtensionMeta, const MetaClass* iInterfaceMeta, CreationFunc iCreator
    );
  };

}  // namespace DE