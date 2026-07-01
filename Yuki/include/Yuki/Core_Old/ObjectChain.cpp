#include "ObjectChain.h"

#include "BaseUnknown.h"
#include "DESysWeakRef.h"
#include "WeakRef.h"

namespace DE {

  using COType = BaseUnknownData::Type;

  ChainedObjectItem* BaseUnknownData::GetChainedObject() const {
    M_ASSERT_MSG(
        type == ForImplementation || type == None, "GetChainedObject can be only called for implementation data"
    );
    return chain;
  }

  BaseUnknown* BaseUnknownData::GetExtendee() const {
    // M_ASSERT_MSG(type == ForExtension || type == None, "GetExtendee can be only called for extension data");
    return extendee;
  }

  BaseUnknown* BaseUnknownData::GetTieBase() const {
    // M_ASSERT_MSG(type == ForTie || type == None, "GetTieBase can be only called for tie data");
    return tieBase;
  }

  void BaseUnknownData::SetChainedObject(ChainedObjectItem* iChain) {
    M_ASSERT_MSG(
        type == ForImplementation || type == None, "SetChainedObject can be only called for implementation data"
    );
    chain = iChain;
    type = ForImplementation;
  }

  void BaseUnknownData::SetExtendee(BaseUnknown* iExtendee) {
    M_ASSERT_MSG(type == ForExtension || type == None, "SetExtendee can be only called for extension data");
    extendee = iExtendee;
    type = ForExtension;
  }

  void BaseUnknownData::SetTieBase(BaseUnknown* iTieBase) {
    M_ASSERT_MSG(type == ForTie || type == None, "SetTieBase can be only called for tie data");
    tieBase = iTieBase;
    type = ForTie;
  }

  BaseUnknown* BaseUnknownData::GetOwner() const {
    auto addr = -offsetof(BaseUnknown, _data) + reinterpret_cast<char*>(const_cast<BaseUnknownData*>(this));
    return reinterpret_cast<BaseUnknown*>(addr);
  }

  bool BaseUnknownData::IsEmpty() const {
    return !chain;
  }

  ChainedObjectItem* BaseUnknownData::begin() {
    M_ASSERT_MSG(type == ForImplementation || type == None, "begin can be only called for implementation data");
    return chain;
  }

  const ChainedObjectItem* BaseUnknownData::begin() const {
    M_ASSERT_MSG(type == ForImplementation || type == None, "begin can be only called for implementation data");
    return chain;
  }

  SystemStatus BaseUnknownData::Append(DESysWeakRef* iWeakRef, uint8_t iCheckDuplicate) {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Append can be only called for implementation data");
    return Append(Type::WeakRef, reinterpret_cast<BaseUnknown*>(iWeakRef), iCheckDuplicate);
  }

  SystemStatus BaseUnknownData::Append(COType iItemType, BaseUnknown* iObjectImpl, uint8_t iCheckDuplicate) {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Append can be only called for implementation data");

    if (!iObjectImpl) {
      return StatusCode::Err_AddingEmptyChainedObject;
    }

    bool isDuplicated = false;
    switch (iCheckDuplicate) {
      case CheckNothing: break;
      case CheckPointer: {
        isDuplicated = !!Search(COType::AnyType, iObjectImpl);
        break;
      }
      case CheckClassName: {
        isDuplicated = !!Search(iObjectImpl->ClassName());
        break;
      }
      case CheckChainObjectType: {
        isDuplicated = !!Search(iItemType);
        break;
      }
      case CheckPointer | CheckClassName: {
        isDuplicated = !!Search(COType::AnyType, iObjectImpl->ClassName(), iObjectImpl);
        break;
      }
      case CheckPointer | CheckChainObjectType: {
        isDuplicated = !!Search(iItemType, iObjectImpl);
        break;
      }
      case CheckClassName | CheckChainObjectType: {
        isDuplicated = !!Search(iItemType, iObjectImpl->ClassName());
        break;
      }
      default: {
        isDuplicated = !!Search(iItemType, iObjectImpl->ClassName(), iObjectImpl);
        break;
      }
    }

    if (isDuplicated) {
      return StatusCode::Err_ChainedObjectItemAlreadyExists;
    }

    auto* newItem = new ChainedObjectItem();
    newItem->field = ChainedObjectItemField(iItemType, 0, 0);
    newItem->object = iObjectImpl;
    newItem->next = GetChainedObject();
    SetChainedObject(newItem);

    return StatusCode::OK;
  }

  ChainedObjectItem* BaseUnknownData::Search(COType iType) const {
    return Search(static_cast<uint8_t>(iType));
  }

  ChainedObjectItem* BaseUnknownData::Search(uint8_t iType) const {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Search can be only called for implementation data");
    auto curItem = GetChainedObject();
    while (curItem) {
      if (curItem->field.type & iType) {
        return curItem;
      }
      curItem = curItem->next;
    }
    return nullptr;
  }

  ChainedObjectItem* BaseUnknownData::Search(std::string_view iName) const {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Search can be only called for implementation data");
    auto curItem = GetChainedObject();
    while (curItem) {
      if (curItem->object && curItem->object->ClassName() == iName) {
        return curItem;
      }
      curItem = curItem->next;
    }
    return nullptr;
  }

  ChainedObjectItem* BaseUnknownData::Search(COType iType, BaseUnknown* iItem) const {
    return Search(static_cast<uint8_t>(iType), iItem);
  }

  ChainedObjectItem* BaseUnknownData::Search(uint8_t iType, BaseUnknown* iItem) const {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Search can be only called for implementation data");
    // todo: rewrite this
    if (iType & COType::BaseUnknownTIE) {
      iType = iType | COType::Extension;
    }
    if (iType & COType::Extension) {
      iType = iType | COType::BOA;
    }

    auto curItem = GetChainedObject();
    while (curItem) {
      if (curItem->object == iItem && (curItem->field.type & iType) /* && curItem->field.def == */) {
        return curItem;
      }
      curItem = curItem->next;
    }
    return nullptr;
  }

  ChainedObjectItem* BaseUnknownData::Search(COType iType, std::string_view iClassName) const {
    return Search(static_cast<uint8_t>(iType), iClassName);
  }

  ChainedObjectItem* BaseUnknownData::Search(uint8_t iType, std::string_view iClassName) const {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Search can be only called for implementation data");
    // todo: rewrite this
    if (iType & COType::BaseUnknownTIE) {
      iType = iType | COType::Extension;
    }
    if (iType & COType::Extension) {
      iType = iType | COType::BOA;
    }

    auto curItem = GetChainedObject();
    while (curItem) {
      if (curItem->field.type & iType && curItem->object->ClassName() == iClassName /* && curItem->field.def == */) {
        return curItem;
      }
      curItem = curItem->next;
    }
    return nullptr;
  }

  ChainedObjectItem* BaseUnknownData::Search(Type iType, std::string_view iClassName, BaseUnknown* iItem) const {
    return Search(static_cast<uint8_t>(iType), iClassName, iItem);
  }

  ChainedObjectItem* BaseUnknownData::Search(uint8_t iType, std::string_view iClassName, BaseUnknown* iItem) const {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Remove can be only called for implementation data");
    // todo: rewrite this
    if (iType & COType::BaseUnknownTIE) {
      iType = iType | COType::Extension;
    }
    if (iType & COType::Extension) {
      iType = iType | COType::BOA;
    }

    auto curItem = GetChainedObject();
    while (curItem) {
      bool isRightName = curItem->object->ClassName() == iClassName;
      bool isRightType = curItem->field.type & iType;
      bool isRightObject = curItem->object == iItem;
      if (isRightType && isRightName && isRightObject) {
        return curItem;
      }
      curItem = curItem->next;
    }
    return nullptr;
  }

  bool BaseUnknownData::Remove(BaseUnknown* iObjectToRemove, bool iUpdateLink) {
    return Remove(BaseUnknownData::Type::AnyType, iObjectToRemove, iUpdateLink);
  }

  bool BaseUnknownData::Remove(BaseUnknownData::Type iType, BaseUnknown* iObjectToRemove, bool iUpdateLink) {
    return Remove(static_cast<uint8_t>(iType), iObjectToRemove, iUpdateLink);
  }

  bool BaseUnknownData::Remove(uint8_t iType, BaseUnknown* iObjectToRemove, bool iUpdateLink) {
    M_ASSERT_MSG(type == ForImplementation || type == None, "Remove can be only called for implementation data");

    ChainedObjectItem *curItem = GetChainedObject(), *head = GetChainedObject(), *prev = nullptr;

    auto RemoveCurItemAndProceed = [&]() {
      if (curItem == head) {
        M_ASSERT(!prev);
        SetChainedObject(curItem->next);
        delete curItem;
        curItem = head = GetChainedObject();
      }
      else {
        M_ASSERT(prev);
        prev->next = curItem->next;
        delete curItem;
        curItem = prev->next;
      }
    };

    while (curItem) {
      // In the case the removed object is referenced by some chained TIE objects
      // in the chained list, remove these TIEs.
      if (iUpdateLink && (curItem->field.type & Type::BaseUnknownTIE)) {
        bool referredGivenObj = curItem->object && curItem->object->_data.GetTieBase() == iObjectToRemove;
        if (referredGivenObj) {
          curItem->object->_data.SetTieBase(nullptr);
          delete curItem->object;
          curItem->object = nullptr;

          RemoveCurItemAndProceed();
          continue;
        }
      }

      // not what we're looking for, just go to the next one
      if (curItem->object != iObjectToRemove) {
        if (!(iType & curItem->field.type)) {
          M_ASSERT_MSG(false, "Type in the chain not conform.");
          return false;
        }
        prev = curItem;
        curItem = curItem->next;
        continue;
      }

      // found it! we are going to delete it.
      RemoveCurItemAndProceed();

      if (!iUpdateLink) {
        break;
      }
    }
    return true;
  }

  void BaseUnknownData::RemoveAll() {
    M_ASSERT_MSG(type == ForImplementation || type == None, "RemoveAll can be only called for implementation data");

    using Type = BaseUnknownData::Type;
    // todo: bad and dangerous here
    BaseUnknown* ownerOfThisChain = GetOwner();

    M_ASSERT_MSG(ownerOfThisChain->_data.GetChainedObject() == chain, "Owner not conform with this instance.");
    auto curItem = GetChainedObject();
    while (curItem) {
      switch (auto type = curItem->field.type) {
        // todo: resolve delegating
      }

      curItem = curItem->next;
    }

    curItem = GetChainedObject();
    while (curItem) {
      auto type = curItem->field.type;
      switch (type) {
        case Type::BOA: break;
        case Type::DelegatedObject: {
          if (curItem->object) {
            Remove(Type::DelegatingObject, curItem->object, false);
            curItem->object = nullptr;
          }
          break;
        }
        case Type::DelegatingObject: {
          M_VERIFY_UNREACHABLE_MSG("This type is deprecated, use the new delegating system");
#if 0
          if (curItem->object) {
            M_ASSERT(ownerOfThisChain);
            curItem->object->RemoveDelegatedInterface(ownerOfThisChain, true);
            curItem->object = nullptr;
          }
#endif
          break;
        }
        case Type::WeakRef: {
          if (curItem->weakRef) {
            curItem->weakRef->KillRef();
            curItem->weakRef = nullptr;
          }
          break;
        }
        case Type::Extension: {
          if (curItem->object) {
            curItem->object->_data.SetExtendee(nullptr);
            delete curItem->object;
            curItem->object = nullptr;
          }
          break;
        }
        case Type::BaseUnknownTIE: {
          if (curItem->object) {
            curItem->object->_data.SetTieBase(nullptr);
            delete curItem->object;
            curItem->object = nullptr;
          }
          break;
        }
        default: M_ASSERT(false);
      }

      auto curItemCached = curItem;
      curItem = curItem->next;
      delete curItemCached;
      curItemCached = nullptr;
    }

    SetChainedObject(nullptr);
  }

}  // namespace DE