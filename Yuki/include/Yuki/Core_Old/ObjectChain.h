#pragma once

#include "SystemStatus.h"

#include <cstdint>
#include <string>

#include "core_export.h"

namespace DE {
  class DESysWeakRef;

  class WeakRef;
  class BaseUnknown;
  struct ChainedObjectItem;

  /**
   * @brief 支撑DE基础机制的信息，这里基础机制包括
   *        1. TIE/BOA/TIEchain绑定机制
   *        2. QueryInterface机制
   *        3. Extension机制
   * 其下存储了这些信息：
   * 1. 引用计数
   * 2. Data类型，是Implementation的还是Tie的还是Extension的
   * 3. 实际数据
   *    3.1 ForTIE: TIE绑定的组件类指针
   *    3.2 ForExtension: 被拓展对象的指针
   *    3.3 ForImplementation: 包括拓展的、TIEchain的、BOA指针在内的指针链表
   */
  class CORE_EXPORT BaseUnknownData final {
   public:
    enum PointerType : uint8_t {
      None = 0,
      ForImplementation = 1,
      ForTie = 2,
      ForExtension = 3,
    };

    /**
     * Possible kinds of objects chained to the implementation base.
     *
     * @param BOA
     * Chained BOA object.
     * @param Extension
     * Chained extension object.
     * @param BaseUnknownTIE
     * Chained TIE deriving from BaseUnknown.
     * @param ChainedDelegatedObject
     * Chained delegated object.
     * @param ChainedDelegatingObject
     * Chained delegating object.
     * @param WeakRef
     * Chained weak reference.
     */
    enum class Type : uint8_t {
      Null = 0x00,
      // by sematics, Extension and BOA are also BaseUnknown, while BOA is also Extension
      BOA = 0x01,
      Extension = 0x02,
      BaseUnknownTIE = 0x04,
      DelegatedObject = 0x08,
      DelegatingObject = 0x10,
      // one chain has at most one weak ref
      WeakRef = 0x20,
      AnyType = 0xFF,
    };
    static_assert(sizeof(Type) == 1);

    /**
     * @brief 控制Append添加链表内容时的行为
     *
     * @param CheckNothing 不做任何检查
     * @param CheckPointer 检查指针是否相同
     * @param CheckClassName 检查类型名是否相同
     * @param CheckChainObjectType 检查链表对象类型是否相同
     * @param _InternalUse 用户不应使用
     * @param _NonInternalUse 用户不应使用
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    enum DuplicateCheckFlag : uint8_t {
      CheckNothing = 0x00,
      CheckPointer = 0x01,
      CheckClassName = 0x02,
      CheckChainObjectType = 0x04,

      _InternalUse = CheckClassName | CheckChainObjectType,
      _NonInternalUse = CheckPointer | CheckChainObjectType,
    };

    /**
     * @brief 构造函数
     *
     * BaseUnknownData的链表初始化为空，引用计数为1，类型为None
     */
    BaseUnknownData() : refCount(1), type(None), chain(nullptr) {
    }

    /**
     * @brief
     * 向BaseUnknownData的链表中添加一个WeakRef。由于只有Implementation的BaseUnknownData是链表，次函数只适用于Implementation
     *
     * @param iItem WeakRef实例
     * @param iCheckDuplicate 检查模式
     *
     * @see DuplicateCheckFlag
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    SystemStatus Append(DESysWeakRef* iItem, uint8_t iCheckDuplicate = CheckNothing);

    /**
     * @brief 向BaseUnknownData的链表中添加一个对象。由于只有Implementation的BaseUnknownData是链表
     *
     * @param iItem 对象实例
     * @param iTypeOfItem 对象的类型
     * @param iCheckDuplicate 检查模式
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    SystemStatus Append(Type iTypeOfItem, BaseUnknown* iItem, uint8_t iCheckDuplicate = CheckNothing);

    /**
     * @brief 在BaseUnknownData的链表中按不同规则搜索
     *
     * @param iItem 对象实例
     * @param iTypeOfItem 对象的类型
     * @param iCheckDuplicate 检查模式
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    ChainedObjectItem* Search(Type iTypeOfItem) const;
    ChainedObjectItem* Search(uint8_t iTypeOfItem) const;
    ChainedObjectItem* Search(std::string_view iTypeOfItem) const;
    ChainedObjectItem* Search(Type iTypeOfItem, BaseUnknown* iItem) const;
    ChainedObjectItem* Search(uint8_t iTypeOfItem, BaseUnknown* iItem) const;
    ChainedObjectItem* Search(Type iTypeOfItem, std::string_view iClassName) const;
    ChainedObjectItem* Search(uint8_t iTypeOfItem, std::string_view iClassName) const;
    ChainedObjectItem* Search(Type iTypeOfItem, std::string_view iClassName, BaseUnknown* iItem) const;
    ChainedObjectItem* Search(uint8_t iTypeOfItem, std::string_view iClassName, BaseUnknown* iItem) const;

    /**
     * @brief 从BaseUnknownData的链表中移除对象
     *
     * 若 `iUpdataLink` 为true, 则 BaseUnknownData的链表中的TIE会被解绑（因为是双向绑定的）
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    bool Remove(WeakRef* iObjectToRemove, bool iUpdateLink);
    /**
     * @brief 从BaseUnknownData的链表中移除对象
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    bool Remove(BaseUnknown* iObjectToRemove, bool iUpdateLink);
    /**
     * @brief 从BaseUnknownData的链表中移除对象
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    bool Remove(Type iType, BaseUnknown* iObjectToRemove, bool iUpdateLink);
    /**
     * @brief 从BaseUnknownData的链表中移除对象
     *
     * @todo 此处仅按照原始代码模拟，规则比较混乱，需要重新设计
     */
    bool Remove(uint8_t iType, BaseUnknown* iObjectToRemove, bool iUpdateLink);
    /**
     * @brief 从BaseUnknownData的链表中移除所有对象
     */
    void RemoveAll();

    /**
     * @brief 以链表形式获取内容数据，只能用于实现类，Debug模式下会检查是否为实现类
     */
    ChainedObjectItem* GetChainedObject() const;
    /**
     * @brief 以对象指针形式获取被拓展对象，只能用于拓展类，Debug模式下会检查是否为拓展类
     */
    BaseUnknown* GetExtendee() const;
    /**
     * @brief 以对象指针形式获取绑定的对象，只能用于TIE或TIEchain类，Debug模式下会检查是否为TIE或TIEchain
     */
    BaseUnknown* GetTieBase() const;
    /**
     * @brief 以链表形式设置内容同数据，只能用于实现类，Debug模式下会检查是否为实现类
     *
     * @param 外部的Chain指针
     */
    void SetChainedObject(ChainedObjectItem* iChain);
    /**
     * @brief 设置被拓展对象，只能用于拓展类，Debug模式下会检查是否为拓展类
     *
     * @param iExtendee 被拓展对象，不能为NULL
     */
    void SetExtendee(BaseUnknown* iExtendee);
    /**
     * @brief 设置TIE/TIEchain的绑定对象，只能用于TIE或TIEchain类，Debug模式下会检查是否为TIE或TIEchain
     *
     * @param iTieBase TIE的绑定对象，不能为NULL
     */
    void SetTieBase(BaseUnknown* iTieBase);

    /**
     * @brief 设置TIE/TIEchain的绑定对象，只能用于TIE或TIEchain类，Debug模式下会检查是否为TIE或TIEchain
     */
    BaseUnknown* GetOwner() const;

    ChainedObjectItem* begin();
    const ChainedObjectItem* begin() const;

    bool IsEmpty() const;

    // todo: improve layout, it can take only 8 bytes
    uint32_t refCount;
    PointerType type;

   protected:
    union {
      BaseUnknown* tieBase;
      BaseUnknown* extendee;
      ChainedObjectItem* chain;
    };
  };
  // #pragma pack(pop)

  using ChainedObject = BaseUnknownData;

  constexpr size_t ChainedObjectSize = sizeof(ChainedObject);

  inline uint8_t operator|(ChainedObject::Type iType1, ChainedObject::Type iType2) {
    return static_cast<uint8_t>(iType1) | static_cast<uint8_t>(iType2);
  }
  inline uint8_t operator|(uint8_t iType1, ChainedObject::Type iType2) {
    return iType1 | static_cast<uint8_t>(iType2);
  }
  inline uint8_t operator|(ChainedObject::Type iType1, uint8_t iType2) {
    return static_cast<uint8_t>(iType1) | iType2;
  }
  inline uint8_t operator^(ChainedObject::Type iType1, ChainedObject::Type iType2) {
    return static_cast<uint8_t>(iType1) ^ static_cast<uint8_t>(iType2);
  }
  inline uint8_t operator^(uint8_t iType1, ChainedObject::Type iType2) {
    return iType1 ^ static_cast<uint8_t>(iType2);
  }
  inline uint8_t operator^(ChainedObject::Type iType1, uint8_t iType2) {
    return static_cast<uint8_t>(iType1) ^ iType2;
  }
  inline uint8_t operator&(ChainedObject::Type iType1, ChainedObject::Type iType2) {
    return static_cast<uint8_t>(iType1) & static_cast<uint8_t>(iType2);
  }
  inline uint8_t operator&(uint8_t iType1, ChainedObject::Type iType2) {
    return iType1 & static_cast<uint8_t>(iType2);
  }
  inline uint8_t operator&(ChainedObject::Type iType1, uint8_t iType2) {
    return static_cast<uint8_t>(iType1) & iType2;
  }

//
// items
//
#pragma pack(push, 1)
  struct ChainedObjectItemField {
    ChainedObject::Type type;
    uint8_t def : 4;
    uint8_t nocond : 4;

    ChainedObjectItemField() : type(ChainedObject::Type::Null), def(0), nocond(0) {};
    ChainedObjectItemField(ChainedObject::Type iType, uint8_t iDef, uint8_t iNocond)
        : type(iType), def(iDef), nocond(iNocond) {
    }
  };
  static_assert(sizeof(ChainedObjectItemField) == sizeof(uint16_t));
#pragma pack(pop)

  struct ChainedObjectItem final {
    ChainedObjectItem() : object(nullptr), next(nullptr) {};
    ChainedObjectItem(ChainedObjectItemField iField, BaseUnknown* iObject)
        : field(iField), object(iObject), next(nullptr) {
    }

    ChainedObjectItemField field;
    union {
      BaseUnknown* object;
      DESysWeakRef* weakRef;
    };

    ChainedObjectItem* next;
  };

}  // namespace DE