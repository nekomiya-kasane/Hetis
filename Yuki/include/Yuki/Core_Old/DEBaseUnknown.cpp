
/**
 * @refactor clean the code, and adapt to new ChainObject
 */

#include "BaseUnknown.h"

#include "BaseError.h"
#include "DEBaseError.h"
#include "MetaClass.h"
#include "DESysWeakRef.h"
#include "DictionaryManager.h"
#include "ObjectChain.h"
#include "WeakRef.h"

#include "gin/runtime/assertion.h"

#include <cassert>
#include <print>
#include <string_view>

namespace DE {

  MetaClass* BaseUnknown::_metaObject = nullptr;

  BaseUnknown::BaseUnknown() {
    _data.refCount = 1;
  }

  BaseUnknown::~BaseUnknown() {
    switch (_data.type) {
      case BaseUnknownData::ForImplementation: {
        if (!_data.IsEmpty()) {
          _data.RemoveAll();
        }
        return;
      }
      case BaseUnknownData::ForTie: {
        // todo: should not be here
        M_ASSERT(!_data.GetTieBase());
        return;
      }
      case BaseUnknownData::ForExtension: {
        const auto extendee = _data.GetExtendee();
        if (!extendee) {
          return;
        }
        extendee->_data.Remove(this, true);
        extendee->_data.refCount -= _data.refCount - 1;
        // -1 item are the decrement reserver for the Release call below
        extendee->Release();
        _data.SetExtendee(nullptr);
      }
    }
  }

  namespace {

    BaseUnknown* g_someStackLikeBadVar = nullptr;  // todo: too bad!

  }

  SystemStatus BaseUnknown::QueryInterface(const std::string_view& iInterfaceName, BaseUnknown*& ioInstance) {
    // Initialization
    // ElapsedTimeTest
    ioInstance = nullptr;

    if (_data.refCount == 0) {
      M_ASSERT_MSG(false, "Query from unexpected BaseUnknown instance: _data.refCount == 0.");
      return StatusCode::Err_InvalidRefCount;
    }

    if (ClassType() != TypeOfClass::Implementation) {
      // M_ASSERT(_data.GetExtendee() != this);
      // auto res = _data.GetExtendee()->QueryInterface(iInterfaceName, ioInstance);
      // if (res == StatusCode::OK) {
      //   return res;
      const bool notNiseObject = ClassType() != TypeOfClass::NothingType;
      if (notNiseObject && _data.GetExtendee()) {
        // note: this doesn't mean that this class is an extension, it may also be a NiseObject.
        const auto res = _data.GetExtendee()->QueryInterface(iInterfaceName, ioInstance);
        if (res == StatusCode::OK) {
          return res;
        }
      }
    }

    [[unlikely]] if (iInterfaceName == Name) {
      _data.refCount++;
      ioInstance = this;
      return StatusCode::OK;
    }

    MetaClass* metaOfBaseImpl = GetMeta();
    if (!metaOfBaseImpl) {
      return StatusCode::Err_Unknown;
    }

    // implementation
    // 1. search in chained objects (extensions and already-created interfaces)
    // 2. search in derivation and try to create a new instance of tie or extension
    if (ClassType() == TypeOfClass::Implementation) {
      if (const auto chainItem = _data.Search(ChainedObject::Type::BaseUnknownTIE, iInterfaceName)) {
        // todo: resolve condition
        chainItem->object->AddRef();
        ioInstance = chainItem->object;
        return StatusCode::OK;
      }
    }

    if (has_flag(TypeOfClass::Any_Extension, ClassType())) {
      if (const auto impl = GetImpl()) {
        BaseUnknown* ret = nullptr;
        if (impl->QueryInterface(iInterfaceName, ret) == StatusCode::OK && ret) {
          ioInstance = ret;
          return StatusCode::OK;
        }
      }
    }

    // Search for the creation function corresponding to the given couple
    // implementation / interface. We search in the inheritance tree of the
    // metaclass of the implementation base.
    const CreationFunc func = metaOfBaseImpl->QueryDerivationInterfaceFactory(iInterfaceName);

    if (!func) {
      return StatusCode::Err_InterfaceNotFound;
    }

    /* todo: too bad! */ BaseUnknown* someStackLikeBadVar = g_someStackLikeBadVar;
    /* todo: too bad! */ g_someStackLikeBadVar = this;

    const auto intf = func(this);
    if (intf->IsDelegation() && intf->IsNull()) {
      intf->Release();
      return StatusCode::Err_InvalidDelegation;
    }

    /* todo: too bad! */ g_someStackLikeBadVar = someStackLikeBadVar;

    if (!intf) {
      return StatusCode::Err_InterfaceNotFound;
    }

    _data.refCount++;

    ioInstance = intf;
    return StatusCode::OK;
  }

  SystemStatus BaseUnknown::QueryInterface(
      const std::string_view& iImplName, const std::string_view& iIntfName, BaseUnknown*& ioInstance
  ) {
    // todo: check this function
    const auto metaOfBaseImpl = GetDictionaryManager().GetMetaClass(iImplName);

    const CreationFunc func = metaOfBaseImpl->QueryDerivationInterfaceFactory(iIntfName);

    if (!func) {
      return StatusCode::Err_InterfaceNotFound;
    }

    if (const auto intf = func(nullptr)) {
      ioInstance = intf;
      return StatusCode::OK;
    }
    return StatusCode::Err_InterfaceNotFound;
  }

  BaseUnknown* BaseUnknown::QueryInterface(const std::string_view& iInterfaceName) {
    if (BaseUnknown* res = nullptr; StatusCode::OK == QueryInterface(iInterfaceName, res)) {
      return res;
    }

    return nullptr;
  }

  uint32_t BaseUnknown::AddRef() {
    const uint32_t ret = ++_data.refCount;

    if (has_flag(TypeOfClass::Any_Extension, ClassType())) {
      _data.GetExtendee()->AddRef();  // todo: what about dangling extensions?
    }
    M_ASSERT_MSG(
        !has_flag(TypeOfClass::Any_TIE, ClassType()) || has_flag(ClassType(), TypeOfClass::NothingType),
        "You cannot call addref on tie/tiechain."
    );

    return ret;
  }

  uint32_t BaseUnknown::Release() {
    M_ASSERT_MSG(_data.refCount > 0, "Non positive reference count.");
    const uint32_t ret = --_data.refCount;

    const auto meta = GetMeta();
    M_ASSERT_MSG(!!meta, "Null meta object occurred.");

    const auto type = meta->GetTypeOfClass();
    M_ASSERT_MSG(type != TypeOfClass::TIE && type != TypeOfClass::TIEchain, "Unexpected release target type.");
    if (type == TypeOfClass::Implementation) {
      if (!_data.refCount) {
        _data.RemoveAll();
      }
    }
    else if (has_flag(TypeOfClass::Any_Extension, type)) {
      // release of extension causes a release of extendee
      if (_data.GetExtendee()) {
        const uint32_t extendeeRefCount = _data.GetExtendee()->Release();
        switch (type) {
          case TypeOfClass::CodeExtension: return _data.refCount;
          case TypeOfClass::DataExtension: return extendeeRefCount;
          case TypeOfClass::CacheExtension: break;
          case TypeOfClass::TransientExtension:
            // If the implementation base has been destroyed, the TransientExtension also !
            if (!extendeeRefCount) {
              return 0;
            }
            if (!_data.refCount) {
              _data.GetExtendee()->_data.Remove(this, true);
            }
          default: break;
        }
      }

      if (!_data.refCount) {
        _data.SetExtendee(nullptr);
      }
    }

    // Destroy the instance
    if (!_data.refCount) {
      delete this;
      return 0;
    }

    return ret;
  }

  bool BaseUnknown::IsSameTo(const BaseUnknown* iOther) const {
    if (!this && !iOther) {
      return true;
    }

    const MetaClass *meta = GetMeta(), *metaOther = iOther->GetMeta();
    assert(
        meta && metaOther && has_flag(TypeOfClass::Any_TIE, meta->GetTypeOfClass())
        && has_flag(TypeOfClass::Any_TIE, metaOther->GetTypeOfClass())
    );

    std::println("Warning: using BaseUnknown::IsSameTo");

    if (this == iOther) {
      return true;
    }

    if (this->_data.ForTie == iOther->_data.ForTie) {
      return true;
    }

    return false;
  }

  MetaClass* BaseUnknown::GetMetaStatic() {
    if (_metaObject) {
      return _metaObject;
    }

    _metaObject = GetDictionaryManager().GetMetaClass(Name);
    _metaObject->SetClassType(TypeOfClass::BaseUnknown);
    return _metaObject;
  }

  MetaClass* BaseUnknown::GetMeta() const {
    return GetMetaStatic();
  }

  const char* BaseUnknown::IsA() const {
    return Name.data();
  }

  BaseUnknown* BaseUnknown::GetImpl(bool iDisallowExtension) const {
    MetaClass* meta = GetMeta();
    if (!meta) {
      return nullptr;
    }

    switch (ClassType()) {
      case TypeOfClass::Implementation: return const_cast<BaseUnknown*>(this);
      case TypeOfClass::BaseUnknown:
      case TypeOfClass::NothingType:  // ?
      case TypeOfClass::CodeExtension: {
        if (const auto extendee = _data.GetExtendee()) {
          return extendee;
        }
        return g_someStackLikeBadVar;
      }
      case TypeOfClass::DataExtension:
      case TypeOfClass::CacheExtension:
      case TypeOfClass::TransientExtension: {
        if (iDisallowExtension) {
          return const_cast<BaseUnknown*>(this);
        }
        if (const auto extendee = _data.GetExtendee()) {
          return extendee;
        }
        return g_someStackLikeBadVar;  // PointerForGetImpl
      }
      case TypeOfClass::TIE:
      case TypeOfClass::TIEchain: {
        const auto implOrExt = meta->GetTieBaseImplementation();
        if (!implOrExt) {
          return nullptr;
        }
        M_ASSERT(has_flag(TypeOfClass::Any_Component, implOrExt->GetTypeOfClass()));
        if (iDisallowExtension) {
          return _data.GetTieBase();
        }

        // Omit extension
        bool isImplementation = implOrExt->GetTypeOfClass() == TypeOfClass::Implementation;
        bool isCodeExtension = implOrExt->GetTypeOfClass() == TypeOfClass::CodeExtension;
        if (isImplementation || isCodeExtension) {
          return _data.GetTieBase();
        }
        return _data.GetTieBase() ? _data.GetTieBase()->_data.GetExtendee() : _data.GetTieBase();
      }
      case TypeOfClass::Interface:
      default: {
        M_ASSERT_MSG(false, "Interface has no implementation object.");
      }
    }

    return nullptr;
  }

  SystemStatus BaseUnknown::AddExtension(BaseUnknown* iExtension, bool iCheckValidity) {
    if (!iExtension || !iExtension->GetMeta()) {
      return StatusCode::Err_InvalidExtension;
    }
    if (!iExtension->_data.IsEmpty()) {
      return StatusCode::Err_ExtensionBindedTwice;
    }

    if (iCheckValidity) {
      // 1. only implementation class can add extension
      auto metaThis = GetMeta(), metaExtension = iExtension->GetMeta();
      if (!metaThis || metaThis->GetTypeOfClass() != TypeOfClass::Implementation) {
        return StatusCode::Err_InvalidExtensionBind;
      }

      const auto extendees = metaExtension->GetExtendees();
      if (!extendees) {
        return StatusCode::Err_InvalidExtensionBind;
      }

      // 2. Only DataExtension and TransientExtension can be added to implementation as a ChainObjectItem,
      //    CodeExtensions are dynamically bound to multiple implementations, and
      //    CacheExtensions are bound without noticing the implementation
      bool isProperExtension = has_flag(metaExtension->GetTypeOfClass(), TypeOfClass::DataExtension);
      isProperExtension |= has_flag(metaExtension->GetTypeOfClass(), TypeOfClass::TransientExtension);
      if (!isProperExtension) {
        return StatusCode::Err_InvalidExtensionBind;
      }

      // 3. only extendees and derived classes of extendees can be bound
      MetaClass** curItem = extendees;
      while (*curItem) {
        if (metaThis->IsAKindOf(*curItem)) {
          break;
        }
        curItem++;
      }

      if (!*curItem) {
        return StatusCode::Err_InvalidExtensionBind;
      }
    }

    // passed validity checking
    // todo: append
    // auto res = _data.ForImplementation.Append();
    // todo: add ref count
    // iExtension->_data.SetExtendee(this);
    auto res = _data.Append(BaseUnknownData::Type::Extension, iExtension, true);
    if (IS_SUCCEEDED(res)) {
      // todo: in original system, this is done only when !internal_use
      _data.refCount += iExtension->_data.refCount;
      iExtension->_data.SetExtendee(this);
    }
    return StatusCode::OK;
  }

  WeakRef* BaseUnknown::GetWeakRef() const {
#if 0
    auto impl = GetImpl();

    // 1. find existing weak ref
    auto weakRefInChain = impl->_data.Search(ChainedObject::Type::WeakRef);
    if (weakRefInChain) {
      weakRefInChain->weakRef->AddRef();
      return weakRefInChain->weakRef;
    }

    // 2. no existing weak ref, create a new one
    auto weakRef = new WeakRef(impl);
    impl->_data.Append(weakRef, ChainedObject::CheckNothing);

    return weakRef;
#endif
    M_ASSERT_TODO();
    return nullptr;
  }

  DESysWeakRef* BaseUnknown::GetComponentWeakRef() {
    // recover implementation
    auto* impl = GetImpl(true);

    M_ASSERT(impl);

    // Search for an existing weak ref on the component
    DESysWeakRef* weakRef = nullptr;
    if (const auto chainedItem = impl->_data.Search(ChainedObject::Type::WeakRef)) {
      weakRef = chainedItem->weakRef;
    }

    // if already exist do not create it, just AddRef it.
    if (!weakRef) {
      weakRef = new DESysWeakRef(impl);
      impl->_data.Append(weakRef, false);
    }
    else {
      weakRef->AddRef();
    }
    return weakRef;
  }

  bool BaseUnknown::IsEqual(const BaseUnknown* iObject) const {
    BaseUnknown* impl = GetImpl();

    if (!impl) {
      return impl == iObject;
    }

    if (impl == this) {
      if (!iObject) {
        return impl == iObject;
      }
      else {
        return impl == iObject->GetImpl();
      }
    }
    else {
      return impl->IsEqual(iObject);
    }
  }

  /** BaseUnknown *SafeStaticCast(MetaClass *iBaseMeta, BaseUnknown *iObject) {
     if constexpr (!IS_DEBUG_MODE) {
       return iObject;
     }

     if (!iObject) {
       return nullptr;
     }

     if (auto meta = iObject->GetMeta(); meta && meta->IsAKindOf(iBaseMeta)) {
       return iObject;
     }

     return nullptr;
   }

   return nullptr;
 }
 */

}  // namespace DE