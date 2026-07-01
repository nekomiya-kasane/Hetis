#pragma once

#include "gin/predefs.h"

#include "SystemStatus.h"

#include <string>

#include "core_export.h"

namespace DE {

  class BaseUnknown;

  /**
   * Creation Function prototype
   */
  using CreationFunc = BaseUnknown* (*)(BaseUnknown * iBaseImpl);
  using DefaultCreationFunc = BaseUnknown* (*)();

  /**
   * Possible type of class.
   */
  enum class TypeOfClass : uint8_t {
    NothingType = 0x01,
    BaseUnknown = 0x02,

    Interface = 0x03,
    TIE = 0x04,
    TIEchain = 0x05,

    Implementation = 0x08,
    DataExtension = 0x10,
    CodeExtension = 0x20,
    CacheExtension = 0x40,
    TransientExtension = 0x80,

    Any_Extension = DataExtension | CodeExtension | CacheExtension | TransientExtension,
    Any_Component = Any_Extension | Implementation,
    Any_TIE = TIE | TIEchain,
  };

  /**
   * Class used to store creation function for the interfaces implementations.
   */
  class InterfaceImpl;

  /**
   * Class used to store and manage data for classes that implement interfaces.
   * There is one instance of MetaClass per class, called the meta object
   * for that class. It contains data specific to the class, such as the ClassID
   * of the class, the class name.
   */
  // todo: give a memory pool to meta class
  class CORE_EXPORT MetaClass {
   public:
    constexpr static std::string_view Name = "MetaClass";  // todo: change this into string
    constexpr static TypeOfClass Type = TypeOfClass::BaseUnknown;
    /**
     * Returns the class name for which the given object is an instance.
     */
    [[nodiscard]] const char* IsA() const {
      return _className.data();
    }

    /**
     * Determines whether the given object's class derives from a given class.
     *
     * @param iClassName The class identifier (ClassID) from which the given object's class is supposed to derive.
     * @return 1 if the given object's class derives from the class with the given name as ClassID and 0 otherwise.
     */
    [[nodiscard]] bool IsAKindOf(const std::string_view& iClassName) const;

    /**
     * Determines whether the given object's class derives from a given class whose meta object is iMetaObject
     *
     * @param iMetaObject The meta object of the class from which the given object class is supposed to derive.
     * @return 1 if the given object's class derives from the class whose meta object is <tt>iMetaObject</tt> and 0
     * otherwise.
     */
    [[nodiscard]] bool IsAKindOf(const MetaClass* iMetaObject) const;

    /**
     * Returns the meta object for the base class of the given object.
     */
    [[nodiscard]] MetaClass* GetBaseClass() const {
      return _metaOfBase;
    }

    /**
     * Returns the meta object's associated class type.
     */
    [[nodiscard]] TypeOfClass GetTypeOfClass() const {
      return _classType;
    }

    [[nodiscard]] std::string_view ClassName() const {
      return _className;
    }

    void SetClassType(TypeOfClass iType) {
      _classType = iType;
    }

    void SetBaseClassMeta(MetaClass* iMeta) {
      _metaOfBase = iMeta;
    }

    // return the first associated implementation class in the list
    // MetaClass *IsExtensionOf() const;

    // MetaClass *IsTieOf() const;

    [[nodiscard]] bool HasInterface(std::string_view iInterfaceName) const;

    /**
     * Adds the given meta object <tt>iMetaObjectOfClassToExtend</tt> to the list
     * of supported implementations.
     */
    void SetExtensionOf(MetaClass* iMetaExtendee);

    void SetImplementationClass(MetaClass* iComp);  // for tie

    [[nodiscard]] MetaClass** GetExtendees() const;

    [[nodiscard]] MetaClass* GetTieBaseImplementation() const;

    /**
     * Searches in the meta object's inheritance tree the creation function for
     * the specified interface.
     *
     * @param iInterfaceName The interface identifier.
     * @return The creation function if it exists, nullptr otherwise.
     */
    CreationFunc QueryDerivationInterfaceFactory(const std::string_view& iInterfaceName);

    /**
     * Constructs a meta object, instance of the MetaClass class, for a given class.
     *
     * @param iName The name of this class.
     * @param iBaseClass The pointer to the meta object associated with this class's base class, that is the class from
     * which this class derives.
     * @param iType The class type.
     */
    MetaClass(const std::string& iName, MetaClass* iBaseClass, TypeOfClass iType);
    MetaClass(const std::string_view& iName, MetaClass* iBaseClass, TypeOfClass iType);

    [[nodiscard]] DefaultCreationFunc GetCreateFunction() const;

   private:
    ~MetaClass();

    /**
     * Adds an interface to the meta object's associated class.
     *
     * @param iInterfaceName The interface identifier to add
     * @param iCreateFunction The Create function of the object which implements the interface.
     * @return 0 if the interface is added and 1 otherwise.
     */
    SystemStatus AddInterfaceCache(const std::string_view& iInterfaceName, CreationFunc iCreateFunction);

    bool ReallocInterfaceFactory(size_t iNewSize);

    /**
     * Returns the creation function for an interface implemented by the meta object's associated class.
     *
     * @param iInterfaceName The interface identifier.
     * @return The creation function if it exists, nullptr otherwise.
     */
    [[nodiscard]] CreationFunc GetInterfaceFactory(const std::string_view& iInterfaceName) const;

    /**
     * Searches in the dictionary the creation function for the specified interface.
     *
     * @param iInterfaceName The interface identifier.
     * @return The creation function if it exists, nullptr otherwise.
     */
    [[nodiscard]] CreationFunc QueryFromDictionary(const std::string_view& iInterfaceName) const;

    /**
     * Destructs all the created MetaClass instances. Must be called at the exit only.
     */
    static void Destruct();

    // name of the class
    std::string_view _className;
    // contains extendees and
    union {
      MetaClass* _implementation = nullptr;
      MetaClass** _extendees;
    };
    // the meta object of the base class
    MetaClass* _metaOfBase = nullptr;
    // head of the chained interfaces
    InterfaceImpl* _interfaces_head = nullptr;  // todo: move this to some kind of container
    // number of these interfaces
    unsigned short _interfaces_number = 0;
    // allocated size for those interfaces
    unsigned short _interfaces_allocated = 0;
    // Next MetaClass in the chain (unused)
    MetaClass* _next = nullptr;
    // type of the corresponding class
    TypeOfClass _classType;

    DefaultCreationFunc _creatorFunc = nullptr;
  };

  /**
   * Class to use in the macro ImplementClass as fourth parameter
   * when the class which uses this macro is not an extension.
   */
  class Null {
   public:
    /**
     * Returns the meta object associated with Null.
     */
    static MetaClass* MetaObject() {
      return nullptr;
    }
  };

  CORE_EXPORT BaseUnknown* SafeStaticCast(const MetaClass* iBaseMeta, BaseUnknown* iObject);

  template <class T>
  BaseUnknown* SafeStaticCast(BaseUnknown* iObject) {
    return SafeStaticCast(T::MetaObject(), iObject);
  }

}  // namespace DE