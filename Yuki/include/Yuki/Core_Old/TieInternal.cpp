#include "MacrosForTie.h"

namespace DE {

  namespace {

    // @todo (Nekomiya) bad!!
    std::atomic<const BaseUnknown*> g_lastCreateTie = nullptr;

  }  // namespace

  void TIEInternal::SetLastCreatedTie(const BaseUnknown* iTie) {
    g_lastCreateTie.store(iTie);
  }

  const BaseUnknown* TIEInternal::GetLastCreatedTie() {
    return g_lastCreateTie.load();
  }

  // [Original Usage]
  //   → TIETSTIGraphicPropTSTOmPointGPExt:
  //        Tie_Construct(this, _metaObject, &NecessaryData.ForTIE, 1, pt,
  //                      TSTOmPointGPExt::ClassId(),
  //                      TSTOmPointGPExt::MetaObject()->GetTypeOfClass(), ptstat,
  //                      TSTOmPointGPExt::CreateItself, delegue, &delegate);

  SystemStatus TIEInternal::Tie_Construct_Implementation(BaseUnknown* ioTie, BaseUnknown* iImplentation) {
    ioTie->_data.refCount = 1;

    ioTie->_data.SetTieBase(iImplentation);

    if (ioTie->ClassType() == TypeOfClass::TIEchain && iImplentation) {
      // todo: if the function is called when querying interface, then no need to check duplication
      //       now we just check in every call
      iImplentation->_data.Append(ChainedObject::Type::BaseUnknownTIE, ioTie, true);
    }

    return StatusCode::OK;
  }

  SystemStatus TIEInternal::Tie_Construct_DataExtension(
      BaseUnknown* ioTie, BaseUnknown* iExtendee, ArgsForExtension& iAdditionalArgs
  ) {
    //
    // [Instance]
    //                 ExtIntf                   TIE knows to create ExtImpl
    //                    |                      and put it in ForTIE, by
    //            Inherit |                      looking into the type of
    //                    |               ↙---- ExtImpl, which is Extension.
    //                   TIE ...............................
    //                    |                                .
    //             ForTIE |                                .
    //                    |         ForExtension           .
    //                 ExtImpl <===================> Implementation (In)
    //                           ForImplementation[]
    //
    // [Meta]
    //  Some-Intf-Of-Implementation
    //      ↘ Query
    //      Implementation
    //          ↘ Query
    //            (Meta<Implementation> interface chain)
    //            (Meta<Derivation> dictionary) - i.e. transverse the inheritance tree
    //                 |
    //                 |    ↓ what's in the dictionary?
    //                 |
    //                 |--- interfaces implemented directly
    //                 └--- interfaces implemented via extensions <== we find them here!
    //
    //                      ↓ how to load the creation function for the tie
    //
    //                 load library -> the creation function will be registered via the instantiation of a static
    //                                 Filler class
    //                              -> <ExtImpl, ExtIntf> Filler will register the interface for its extendees as
    //                              well
    //
    //          ↘ Create
    //            (Create the extension's impl
    //            (Create the TIE for the extension and its interface)
    //            (link the extension's impl with what it extends)
    //
    //
    ioTie->_data.refCount = 1;

    BaseUnknown* manipulatedExt = nullptr;

    auto extendee = iExtendee;
    if (extendee) {
      // we are going to create/find an extension and attach it to some implementation class instance
      auto& extendeeChain = extendee->_data;

      // 1. search if the impl already have the extension instance
      auto existedExt = extendeeChain.Search(ChainedObject::Type::Extension, iAdditionalArgs.compName);
      if (existedExt) {
        manipulatedExt = existedExt->object;
      }
      // 2. if not found, then create one and append it to the extendee's list
      if (!manipulatedExt) {
        manipulatedExt = iAdditionalArgs.compCreatorFunc();
        extendeeChain.Append(ChainedObject::Type::Extension, manipulatedExt);
        manipulatedExt->_data.SetExtendee(extendee);
      }
      else {
        ++manipulatedExt->_data.refCount;
      }
    }
    else {
      // we are creating an extension without attaching to any implementation class instance
      manipulatedExt = iAdditionalArgs.compCreatorFunc();
    }

    ioTie->_data.SetTieBase(manipulatedExt);

    if (ioTie->ClassType() == TypeOfClass::TIEchain && iExtendee) {
      // todo: if the function is called when querying interface, then no need to check duplication
      //       now we just check in every call
      iExtendee->_data.Append(ChainedObject::Type::BaseUnknownTIE, ioTie, true);
    }

    return StatusCode::OK;
  }

  SystemStatus TIEInternal::Tie_Construct_TransientExtension(
      BaseUnknown* ioTie, BaseUnknown* iImplentation, ArgsForExtension& iArgs
  ) {
    return Tie_Construct_DataExtension(ioTie, iImplentation, iArgs);
  }

  SystemStatus TIEInternal::Tie_Construct_CacheExtension(
      BaseUnknown* ioTie, BaseUnknown* iExtendee, ArgsForExtension& iArgs
  ) {
    ioTie->_data.refCount = 1;

    // doesn't support tiechain?
    ioTie->GetMeta()->SetClassType(TypeOfClass::TIE);
    auto manipulatedExt = iArgs.compCreatorFunc();
    ioTie->_data.SetTieBase(manipulatedExt);
    if (manipulatedExt) {
      manipulatedExt->_data.SetExtendee(iExtendee);
    }

    if (ioTie->ClassType() == TypeOfClass::TIEchain && iExtendee) {
      // todo: if the function is called when querying interface, then no need to check duplication
      //       now we just check in every call
      iExtendee->_data.Append(ChainedObject::Type::BaseUnknownTIE, ioTie, true);
    }

    return StatusCode::OK;
  }

  SystemStatus TIEInternal::Tie_Construct_CodeExtension(
      BaseUnknown* ioTie, BaseUnknown* iExtendee, ArgsForExtension& iArgs
  ) {
    ioTie->_data.refCount = 1;

    auto shared = ioTie->GetSharedObject();  // the unique instance of code extension
    if (!shared) {
      shared = iArgs.compCreatorFunc();
      // shared->_data.GetExtendee() = iExtendee;
      ioTie->_data.SetTieBase(iExtendee);
      ioTie->SetSharedObject(shared);
    }
    else {
      ioTie->_data.SetTieBase(iExtendee);
      ++shared->_data.refCount;
    }

    if (ioTie->ClassType() == TypeOfClass::TIEchain && iExtendee) {
      // todo: if the function is called when querying interface, then no need to check duplication
      //       now we just check in every call
      iExtendee->_data.Append(ChainedObject::Type::BaseUnknownTIE, ioTie, true);
    }

    return StatusCode::OK;
  }

  SystemStatus TIEInternal::Tie_Query(
      BaseUnknown* iTie, const std::string_view& iInterfaceName, BaseUnknown*& ioInterfaceInstance
  ) {
    auto* impl = iTie->_data.GetTieBase();
    if (!impl) {
      ioInterfaceInstance = nullptr;
      return CHECK_STATUS(StatusCode::Err_HangingTie);
    }

    // todo: handle querying MetaClass::ClassId case
    // todo: handle delegate

    return impl->QueryInterface(iInterfaceName, ioInterfaceInstance);
  }

  uint32_t TIEInternal::Tie_AddRef(BaseUnknown* ioTie, uint32_t& ioRef) {
    auto* impl = ioTie->_data.GetTieBase();
    if (!impl) {
      M_ASSERT(false);
      return ++ioRef;
    }

    impl->AddRef();
    return ++ioRef;
  }

  uint32_t TIEInternal::Tie_Release(BaseUnknown* ioTie, uint32_t& ioRef, bool& oDestruct) {
    auto* impl = ioTie->_data.GetTieBase();
    // todo: handle tie chain

    oDestruct = false;

    if (!ioRef) {
      M_ASSERT(false);
    }
    ioRef--;

    TypeOfClass type = ioTie->GetMeta()->GetTypeOfClass();
    switch (type) {
      case TypeOfClass::TIEchain:
        if (impl) {
          return impl->Release();
        }
        break;
      case TypeOfClass::TIE:
        if (impl && !impl->Release()) {
          // this is important as ~BaseUnknown will skip tie by if it is null
          ioTie->_data.SetTieBase(nullptr);
        }
        break;
      default: M_ASSERT_MSG(false, "Tie_Release should only be applied on tie/tiechain");
    }

    // this is the last tie, gonna trigger deleting
    if (!ioRef) {
      oDestruct = true;
    }
    return ioRef;
  }

  BaseUnknown* TIEInternal::Tie_Link(BaseUnknown* iImpl, std::string_view iInterfaceName) {
    if (!iImpl) {
      return nullptr;
    }

    M_ASSERT(iImpl->ClassType() == TypeOfClass::Implementation || iImpl->ClassType() == TypeOfClass::NothingType);

    auto existedTieItem = iImpl->_data.Search(ChainedObject::Type::BaseUnknownTIE, iInterfaceName);
    if (!existedTieItem) {
      return nullptr;
    }

    M_ASSERT(existedTieItem->object);
    auto existedTie = existedTieItem->object;
    if (!existedTie) {
      return nullptr;
    }

    // this will also add a ref to the impl, which is not desired, we only increment the ref of tie. So impl will
    // decrease one ref correspondingly to stay invariant
    existedTie->AddRef();
    iImpl->_data.refCount--;
    return existedTie;
  }

}  // namespace DE