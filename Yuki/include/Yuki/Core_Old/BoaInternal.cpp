/*
 * @refactor no logical changes. Just make BOA_Construct a template
 *
 * BOA_Construct<Implementation>
 * BOA_Construct<CacheExtension>
 * ...
 */

#include "MacrosForTie.h"

namespace DE {

  BaseUnknown* BOAInternal::BOA_Construct_Implementation(BaseUnknown* iBaseImpl) {
    M_ASSERT(iBaseImpl);
    // for BOA, impl is the BOA of itself. so we need to add itself to itself's chain.
    iBaseImpl->_data.Append(ChainedObject::Type::BOA, iBaseImpl, true);
    return iBaseImpl;
  }

  BaseUnknown* BOAInternal::BOA_Construct_CacheExtension(BaseUnknown* iBaseImpl, ArgsForExtension& iArgs) {
    BaseUnknown* extension
        = iArgs.compBaseCreatorFunc ? iArgs.compBaseCreatorFunc(iBaseImpl) : iArgs.compCreatorFunc();
    extension->_data.SetExtendee(iBaseImpl);
    return extension;
  }

  BaseUnknown* BOAInternal::BOA_Construct_DataExtension(BaseUnknown* iBaseImpl, ArgsForExtension& iArgs) {
    BaseUnknown* extension = nullptr;

    // 1. search already exisiting extensions of the same type
    auto& chain = iBaseImpl->_data;
    auto item = chain.Search(ChainedObject::Type::Extension, iArgs.compName);
    if (item) {
      extension = item->object;
    }

    // 2. existing extension of the same type can be found
    if (extension) {
      extension->_data.refCount++;  // no AddRef here, or iBaseImpl's refCount will be incremented as well.
    }
    // 3. otherwise create one extension, and bind
    else {
      extension = iArgs.compBaseCreatorFunc ? iArgs.compBaseCreatorFunc(iBaseImpl) : iArgs.compCreatorFunc();
      chain.Append(ChainedObject::Type::Extension, extension, false);
      extension->_data.SetExtendee(iBaseImpl);
    }

    // 4. todo: handle delegate objects

    return extension;
  }

  BaseUnknown* BOAInternal::BOA_Construct_TransientExtension(BaseUnknown* iBaseImpl, ArgsForExtension& iArgs) {
    return BOA_Construct_DataExtension(iBaseImpl, iArgs);
  }

}  // namespace DE