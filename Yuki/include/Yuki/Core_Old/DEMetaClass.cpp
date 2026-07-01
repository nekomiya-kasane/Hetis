
#include "MetaClass.h"

#include "BaseUnknown.h"
#include "CoreSocketSender.h"
#include "DictionaryManager.h"

#include "gin/runtime/module_info.h"
#include "gin/utils/environment.h"

#include <cstring>

namespace DE {

  CoreSocketSender& GetSocketSender() {
    static CoreSocketSender s_sender;
    if (!s_sender.HasTriedConnecting()) {
      s_sender.Connect("127.0.0.1", 12543);
    }
    return s_sender;
  }

  std::string ToTypeString(TypeOfClass iType) {
    switch (iType) {
      case TypeOfClass::NothingType: return "nothing";
      case TypeOfClass::BaseUnknown: return "unknown";
      case TypeOfClass::Interface: return "interface";
      case TypeOfClass::TIE: return "tie";
      case TypeOfClass::TIEchain: return "tie";
      case TypeOfClass::Implementation: return "component";
      case TypeOfClass::DataExtension: return "data-extension";
      case TypeOfClass::CodeExtension: return "code-extension";
      case TypeOfClass::CacheExtension: return "cache-extension";
      case TypeOfClass::TransientExtension: return "transient-extension";
      default: return "unknown";
    }
  }

  /**
   * Structure used to store creation function for the interfaces implementations.
   */
  class InterfaceImpl {
   public:
    [[nodiscard]] bool operator==(const InterfaceImpl& iRight) const {
      return interfaceName == iRight.interfaceName;
    }
    [[nodiscard]] auto operator<=>(const InterfaceImpl& iRight) const {
      return interfaceName <=> iRight.interfaceName;
    }

    std::string_view interfaceName;
    CreationFunc creationFunc = nullptr;
  };

  class BaseUnknown;

  /**
   * Function used when the creation function of an interface cannot be found.
   */
  namespace {

    BaseUnknown* NoSuchInterface(BaseUnknown* iBaseImpl) {
      M_VERIFY_UNREACHABLE_MSG("Should not be here");
      return nullptr;
    };

  }  // namespace

  MetaClass::MetaClass(const std::string& iName, MetaClass* iBaseClass, TypeOfClass iType)
      : _metaOfBase(iBaseClass), _classType(iType) {
    _className = GetDictionaryManager().AddString(iName);

    std::string data
        = R"({"framework":"System", "command":"meta-class:create", "payload": {"name": ")" + iName + R"("}})";
    GetSocketSender().Send(data);

    if (iBaseClass) {
      data = R"({"framework":"System", "command":"meta-class:set-parent", "payload": {"name": ")" + iName
             + R"(", "parent": ")" + iBaseClass->IsA() + R"("}})";
      GetSocketSender().Send(data);
    }

    data = R"({"framework":"System", "command":"meta-class:set-type", "payload": {"name": ")" + iName
           + R"(", "type": ")" + ToTypeString(iType) + R"("}})";
    GetSocketSender().Send(data);
  }

  MetaClass::MetaClass(const std::string_view& iName, MetaClass* iBaseClass, TypeOfClass iType)
      : _metaOfBase(iBaseClass), _classType(iType) {
    _className = GetDictionaryManager().AddString(iName);

    std::string data = R"({"framework":"System", "command":"meta-class:create", "payload": {"name": ")"
                       + std::string(iName) + R"("}})";
    GetSocketSender().Send(data);

    if (iBaseClass) {
      data = R"({"framework":"System", "command":"meta-class:set-parent", "payload": {"name": ")" + std::string(iName)
             + R"(", "parent": ")" + iBaseClass->IsA() + R"("}})";
      GetSocketSender().Send(data);
    }

    data = R"({"framework":"System", "command":"meta-class:set-type", "payload": {"name": ")" + std::string(iName)
           + R"(", "type": ")" + ToTypeString(iType) + R"("}})";
    GetSocketSender().Send(data);
  }

  MetaClass::~MetaClass() {
    if (_interfaces_head) {
      delete[] _interfaces_head;
      _interfaces_head = nullptr;
    }
    _metaOfBase = nullptr;
    _next = nullptr;
  }

  bool MetaClass::IsAKindOf(const std::string_view& iClassName) const {
    const MetaClass* curMeta = this;
    while (curMeta) {
      if (iClassName == curMeta->_className) {
        return true;
      }

      curMeta = curMeta->_metaOfBase;
    }
    return false;
  }

  bool MetaClass::IsAKindOf(const MetaClass* iMetaObject) const {
    auto curMeta = this;
    while (curMeta) {
      if (iMetaObject == curMeta) {
        return true;
      }
      curMeta = curMeta->_metaOfBase;
    }
    return false;
  }

  bool MetaClass::HasInterface(std::string_view iInterfaceName) const {
    const auto* record = GetCompIntfRecord({ClassName().data(), iInterfaceName.data()});
    return record && record->status != CompIntfRecord::UNAUTHORIZED && record->status != CompIntfRecord::UNREACHABLE;
  }

  void MetaClass::SetExtensionOf(MetaClass* iMetaExtendee) {
    M_ASSERT_MSG(
        has_flag(TypeOfClass::Any_Extension, GetTypeOfClass()), "Only extension classes can append extendees."
    );
    if (!iMetaExtendee) {
      return;
    }

    // expand _implementations
    // todo: refactor this to some kind of containers
    MetaClass** oldList = _extendees;
    int nbImplementations = 0;
    if (oldList) {
      while (oldList[nbImplementations]) {
        if (oldList[nbImplementations] == iMetaExtendee) {
          return;
        }
        nbImplementations++;
      }
    }

    _extendees = new MetaClass*[nbImplementations + 2];
    if (_extendees) {
      if (oldList) {
        memcpy(_extendees, oldList, nbImplementations * sizeof(MetaClass*));
      }

      _extendees[nbImplementations] = iMetaExtendee;
      _extendees[nbImplementations + 1] = nullptr;

      if (oldList) {
        delete[] oldList;
        oldList = nullptr;
      }
    }
    else {
      _extendees = oldList;
    }
  }

  MetaClass** MetaClass::GetExtendees() const {
    M_ASSERT_MSG(has_flag(TypeOfClass::Any_Extension, GetTypeOfClass()), "Only extension classes have extendees.");
    return _extendees;
  }

  MetaClass* MetaClass::GetTieBaseImplementation() const {
    M_ASSERT_MSG(has_flag(TypeOfClass::Any_TIE, GetTypeOfClass()), "Only tie or tie chain has implementation object.");
    return _implementation;
  }

  void MetaClass::SetImplementationClass(MetaClass* iComp) {
    // todo, use _implementations but not 2d array
    M_ASSERT_MSG(
        has_flag(TypeOfClass::Any_TIE, GetTypeOfClass()), "Only tie or tie chain can set implementation object."
    );
    _implementation = iComp;
  }

  bool MetaClass::ReallocInterfaceFactory(size_t iNewSize) {
    // CATMutexFastLock(&_mutex);
    if (_interfaces_number < _interfaces_allocated) {
      return true;
    }

    if (auto* newAlloc = new InterfaceImpl[iNewSize]) {
      _interfaces_allocated = static_cast<decltype(_interfaces_allocated)>(iNewSize);
      // Copy the old structure
      if (_interfaces_number != 0) {
        memcpy(newAlloc, _interfaces_head, _interfaces_number * sizeof(InterfaceImpl));
        delete[] _interfaces_head;
      }
      _interfaces_head = newAlloc;
    }
    return true;
  }

  SystemStatus MetaClass::AddInterfaceCache(const std::string_view& iInterfaceName, CreationFunc iCreateFunction) {
    M_ASSERT(iCreateFunction);

    static size_t increment = 4;
    // todo: recover this using the new environment variables manager
    // IF_INT_FLAG_STATIC(metaClassInterfaceListExpansionUnit, expansionUnit, { increment = expansionUnit; });

    const bool needRealloc = _interfaces_number == _interfaces_allocated;
    const bool isReallocated = needRealloc && ReallocInterfaceFactory(_interfaces_allocated + increment);

    if (needRealloc && !isReallocated) {
      return StatusCode::Err_MemoryOut;
    }

    // Add the new interface implementation
    _interfaces_head[_interfaces_number].interfaceName = iInterfaceName;
    _interfaces_head[_interfaces_number].creationFunc = iCreateFunction;
    _interfaces_number++;

    // Sort the array if needed
    if (_interfaces_number > 1) {
      std::sort(_interfaces_head, _interfaces_head + _interfaces_number, [](InterfaceImpl& lhs, InterfaceImpl& rhs) {
        return lhs.interfaceName < rhs.interfaceName;
      });
    }

    // Add it to the dictionary
    // Seems not needed
    // if (iCreateFunction != NoSuchInterface) {
    //  GetDictionaryManager().AddCompIntfRecord({
    //    .componentName = _className,
    //    .interfaceName = iInterfaceName,
    //    .creationFunc = iCreateFunction,
    //    .status = CompIntfRecord::Status::Loaded,
    //  });
    //  // CATSysTSDictionary::AddDictionary(*_classID, iInterfaceID,
    //  // iCreateFunction,
    //  //                                   nullptr, 1);
    //}
    return StatusCode::OK;
  }

  CreationFunc MetaClass::GetInterfaceFactory(const std::string_view& iInterfaceName) const {
    CreationFunc retFunc = nullptr;

    if (_interfaces_number != 0) {
      // Search the correspondinf creation function
      InterfaceImpl reference;
      reference.interfaceName = iInterfaceName;

      auto res = std::find_if(
          _interfaces_head, _interfaces_head + _interfaces_number,
          [&iInterfaceName](const InterfaceImpl& iElement) { return iElement.interfaceName == iInterfaceName; }
      );
      if (res != _interfaces_head + _interfaces_number) {
        retFunc = res->creationFunc;
      }
    }
    return retFunc;
  }

  BaseUnknown* SafeStaticCast(const MetaClass* iBaseMeta, BaseUnknown* iObject) {
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

  CreationFunc MetaClass::QueryDerivationInterfaceFactory(const std::string_view& iInterfaceName) {
    // 1. search for the interface factory in the meta class itself
    CreationFunc function = GetInterfaceFactory(iInterfaceName);

    if (function) {
      return function == NoSuchInterface ? nullptr : function;
    }

    // 2. search in the inheritance tree
    const MetaClass* curMeta = this;
    M_ASSERT(curMeta->ClassName() != "MetaClass");
    while (curMeta) {
      // Search in the dictionary with the current meta class
      function = curMeta->QueryFromDictionary(iInterfaceName);
      if (function) {
        // We found the creation and condition function ; add them to the meta class
        AddInterfaceCache(iInterfaceName, function);

        return function;
      }
      curMeta = curMeta->GetBaseClass();
    }

    // We definitively cannot find the functions ; store the failure function in the meta class to avoid future search
    // in the dictionary for this couple (implementation / interface)
    //
    // nekomiya: if possible, maybe we can make the process read the dictionary file again
    AddInterfaceCache(iInterfaceName, NoSuchInterface);

    return function;
  }

  CreationFunc MetaClass::QueryFromDictionary(const std::string_view& iInterfaceName) const {
    const auto* record = GetCompIntfRecord({ClassName().data(), iInterfaceName.data()});
    if (!record) {
      return nullptr;
    }

    // 1. not in the dictionary
    // todo: recover this using the new environment variables manager
    // IF_ENABLED_STATIC(onlyAllowDictionaryRecord, {
    //  if (record->libraryName.empty()) {
    //    return nullptr;
    //  }
    //});

    // 2. in the dictionary but unreachable
    if (record->status == CompIntfRecord::RecordStatus::UNREACHABLE) {
      return nullptr;
    }

    // 3. in the dictionary
    // todo: check all loaded module to ensure no dup load
    // no creation function, try to load the dll, the creation function will be registered along with the
    // loading process due to global register DictionaryFiller's initialization
    if (!record->creationFunc && !record->libraryName.empty()) {
      // auto dllHandle = LoadDynamicModule(String{record->libraryName});
      auto handle = os::load_module(std::string{record->libraryName});
      if (!os::is_valid_module(handle)) {
        GetDictionaryManager().MarkLibraryAsUnreachable(record->libraryName);
        std::println("Failed to load module: {}", record->libraryName);
        return nullptr;
      }

      if (os::is_valid_module(handle) && !record->creationFunc) {
        std::println(
            "{}Module {} loaded but function not found for {} -> {}{}", terminal::fg(terminal::foreground_color::Red),
            std::string{record->libraryName}, record->componentName, iInterfaceName, terminal::clr_style()
        );
      }
    }

    return static_cast<CreationFunc>(record->creationFunc);  // still may be null

    // if (record->libraryName.empty()) {
    //   // The text file dictionary does not contain this entry, return nullptr
    //   // todo: do we really ban creating things which are not listed in the dict?
    //   return nullptr;
    // }
    //  If the found result is not final, try to load the shared library
    //  ?
    //  if (record->final != 1) {
    //   // Loading the library will potentially modify the pResult->creation value
    //   LoadDynamicModule(std::string{record->libraryName});

    //  // Add it to the dictionary
    //  // CATSysTSDictionary::AddDictionary(GetClassId(), iInterfaceID,
    //  //                                  pResult->creationFunc,
    //  //                                  (CATSysTSInfoDic *)pResult, 1);
    //  // 1 for loaded?
    //}

    // todo: support cond func
  }

  DefaultCreationFunc MetaClass::GetCreateFunction() const {
    return _creatorFunc;
  }

}  // namespace DE