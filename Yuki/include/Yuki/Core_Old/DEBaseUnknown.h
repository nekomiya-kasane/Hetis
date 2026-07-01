/**
 * @file BaseUnknown.h
 * @brief
 * @~chinese
 * DE基础类
 *
 * 提供了组件对象模型的核心功能：
 * - 接口查询和引用计数管理
 * - 组件扩展和委托机制（委托机制还没有实现）
 * - TIE/TIEchain支持
 *
 * @~english
 * Base Class
 *
 * Provides core functionality for component object model:
 * - Interface querying and reference counting
 * - Component extension and delegation mechanism (not implemented yet)
 * - Type Information Exchange (TIE) support
 *
 * @todo 当前版本的BaseUnknown类时线程不安全的
 */
#pragma once

#include "DictionaryManager.h"
#include "ObjectChain.h"
#include "SystemStatus.h"

#include "core_export.h"

#define DEExtendable BaseUnknown
#define DEDataExtendable BaseUnknown
#define DEInterfaceObject BaseUnknown

namespace DE {

  class MetaClass;
  class BaseUnknown;
  class DESysWeakRef;

  class DENull {
   public:
    [[nodiscard]] static MetaClass* MetaObject() {
      return nullptr;
    }
  };
  /**
   * @brief BaseUnknown类是所有使用DE元对象系统的类的基类。它提供了
   *
   * 1. 编译期的类类型和类名称
   * 2. 元对象的构建与获取、继承关系判断
   * 3. 接口实例的获取
   * 4. 生命周期和引用计数管理
   *
   * 所有使用DE元对象系统的类都必须继承自BaseUnknown类。
   */
  class CORE_EXPORT BaseUnknown {
    friend class DEGTMObject;

   public:
    enum ComponentState { Activated, Garbaged, Destroyed };
    /**
     * @brief 类的名称。除了TIE以外均与类名一致。由于为了向用户隐藏TIE的实现，TIE的类名定义为与其对应的接口名一致。
     */
    constexpr static std::string_view Name = "BaseUnknown";
    /**
     * @brief 类的类型。
     */
    constexpr static TypeOfClass Type = TypeOfClass::BaseUnknown;

    /**
     * @brief 构造函数，初始化引用计数为1
     */
    BaseUnknown();

    /**
     * @brief 查询接口，如果实现了该接口，则返回接口实例指针，否则返回nullptr。
     *
     * 遵循以下规则：
     * 1. 如果引用计数为0，属异常情况，返回Err_InvalidRefCount异常码，并不执行后续操作。
     * 2. 如果Query的接口名为BaseUnknown，则返回this指针。
     * 3. 如果此实例为拓展类的实例，且不是DEObject，则查询其Extendee是否有此接口
     * 4. 如果此实例没有对应的元对象实例，属异常情况，返回Err_Unknown，接口指针返回空
     * 5.
     * 如果此实例是实现类，尝试在其BaseUnknownData链表中查找已经创建过的TIE实例，如找到则返回该实例。该情况一般为TIEchain绑定的接口
     * 6.
     * 最后，在组件接口表中查询，如果能找到对应的条目，则使用对应的工厂函数指针创建一个绑定到this的TIE/BOA实例，并将this的引用计数加1，返回该实例。
     *    查询过程是递归的，即此实例找不到对应的接口，则去找此实例的父类有没有对应接口。而且，结果（工厂函数指针）会缓存到元对象实例的接口表中，下次查询时直接使用。
     *
     * @param iInterfaceName 接口名称
     * @param ioInstance 返回接口实例指针
     * @return SystemStatus 系统状态码，可能有OK, Err_Unknown, Err_InterfaceNotFound, Err_InvalidRefCount
     */
    virtual SystemStatus QueryInterface(const std::string_view& iInterfaceName, BaseUnknown*& ioInstance);

    template <typename T>
      requires(T::Type == TypeOfClass::Interface)
    SystemStatus QueryInterface(T*& ioInstance) {
      return QueryInterface(T::Name, static_cast<BaseUnknown*&>(ioInstance));
    }

    static SystemStatus QueryInterface(
        const std::string_view& iImplName, const std::string_view& iIntfName, BaseUnknown*& ioInstance
    );

    /**
     * @brief 另一个QueryInterface函数的别名，用于直接返回接口实例指针，如果没有找到，返回nullptr，而丢弃返回码
     * @see QueryInterface
     *
     * @param iInterfaceName 接口名称
     * @return Query的接口实例指针，一般为自身或某个TIE的实例。如果没有找到，则返回nullptr。
     */
    [[nodiscard]] virtual BaseUnknown* QueryInterface(const std::string_view& iInterfaceName);

    template <typename Interface>
      requires(std::derived_from<Interface, BaseUnknown> && Interface::Type == TypeOfClass::Interface)
    [[nodiscard]] Interface* QueryInterface() {
      return static_cast<Interface*>(QueryInterface(Interface::Name));
    }

    /**
     * @brief 手动增加引用计数，如果是拓展类，则递归调用Extendee的AddRef
     * @note 不适用于TIE和TIEchain，必须与Release()配对使用，由用户自己管理生命周期
     * @return 增加后的引用计数
     */
    virtual uint32_t AddRef();

    /**
     * @brief 手动递减引用计数，如果是拓展类，则递归调用Extendee的AddRef。
     *
     * 1. 如果是实现类且引用计数递减后为0，则释放BaseUnknownData.
     * 2. 如果是拓展类示例，则递归调用Extendee的Release，且如果引用计数递减后为0，则Extendee重置为nullptr
     * 3. 如果是CodeExtension实例，则返回递减后自身的引用计数
     * 4. 如果是DataExtension实例，则返回Extendee操作后的引用计数
     * 5. 如果是TransientExtension或CacheExtension示例，且引用计数递减后为0，则从Extendee的BaseUnknownData中移除自身。
     * 6. 如果自身引用计数递减后为0，则delete this
     *
     * @return 递减后的引用计数，DataExtension实例返回Extendee操作后的引用计数，此外返回自己的
     */
    virtual uint32_t Release();

    /**
     * @brief 判断引用计数是否为0
     */
    [[nodiscard]] virtual bool IsNull() const {
      return !_data.refCount;
    }

    /**
     * @brief 获取当前实例实际（而非某个父类指针）类型对应的MetaObject，如果没有则构建一个再返回
     */
    [[nodiscard]] virtual MetaClass* GetMeta() const;

    [[nodiscard]] virtual const char* IsA() const;

    /**
     * @brief 递归沿继承树向上查找与传入参数匹配的类名，如果找到的话则认为传入的类是当前类的父类。
     *
     * @param iClassName 类名
     * @return 当前类是否是iName的子类， 如果是则返回true，否则返回false
     */
    [[nodiscard]] virtual bool IsAKindOf(const char* iClassName) const {
      const MetaClass* meta = GetMeta();
      return meta ? meta->IsAKindOf(std::string_view(iClassName)) : 0;
    }

    /**
     * @brief 递归沿继承树向上查找与传入参数匹配的类名，如果找到的话则认为传入的类是当前类的父类。
     *
     * @param iClassName 类名
     * @return 当前类是否是iName的子类， 如果是则返回true，否则返回false
     */
    [[nodiscard]] virtual bool IsAKindOf(std::string_view iClassName) const {
      const MetaClass* meta = GetMeta();
      return meta ? meta->IsAKindOf(iClassName) : 0;
    }

    /**
     * @brief 递归沿继承树向上查找与传入参数匹配的MetaClass，如果找到的话则认为传入的类是当前类的父类。
     *
     * @param iClassName 类名
     * @return 当前类是否是iName的子类， 如果是则返回true，否则返回false
     */
    [[nodiscard]] virtual bool IsAKindOf(const MetaClass* iClassName) const {
      const MetaClass* meta = GetMeta();
      return meta ? meta->IsAKindOf(iClassName) : false;
    }

    template <typename T>
      requires std::is_base_of_v<BaseUnknown, T>
    [[nodiscard]] bool IsAKindOf() const {
      const MetaClass* meta = GetMeta();
      return meta ? meta->IsAKindOf(T::GetMetaStatic()) : false;
    }

    // only used for TIE
    [[nodiscard]] virtual bool IsSameTo(const BaseUnknown* iOther) const;

    [[nodiscard]] virtual bool IsDelegation() const {
      return false;
    }

    /**
     * @brief 静态获取类型对应的MetaObject，如果没有则构建一个再返回
     */
    [[nodiscard]] static MetaClass* GetMetaStatic();

    /**
     * @brief
     * 在当前实例对应的组件类（如果是组件类实例则指本身）的BaseUnknownData链表中查找WeakRef，如果找到则返回该WeakRef，否则创建一个新的WeakRef并添加到链表后再返回
     *
     * @return 当前实例对应的组件类的BaseUnknownData链表中的WeakRef
     */
    [[deprecated("We have 2 weak refs??")]] [[nodiscard]] virtual WeakRef* GetWeakRef() const;
    [[nodiscard]] virtual DESysWeakRef* GetComponentWeakRef();

    /**
     * @brief 获取当前实例对应的组件类
     *
     * 1. 如果没有MetaObject则返回nullptr
     * 2. 如果是实现类则返回自身
     * 3. 如果是CodeExtension则返回被拓展类
     * 4. 如果是其他类型的拓展类则根据iAllowExtension决定是否返回被拓展类，如果为true则返回自身
     * 5. 如果是实现类或者CodeExtension的TIE和TIEChain，则返回实现类实例
     * 6. 如果是其他拓展类的TIE和TIEchain，则返回被拓展类（被拓展类是实现类）
     *
     * @todo g_someStacklikeBadVar的存在很尴尬，需要重新设计
     * @param iDisallowExtension 是否允许返回拓展类
     * @return 当前实例对应的组件类
     */
    [[nodiscard]] virtual BaseUnknown* GetImpl(bool iDisallowExtension = false) const;

    /**
     * @brief 手动添加拓展类实例
     *
     * 如果iCheckValidity为true，则检查传入的拓展类是否合法，即
     * 1. this类是否是实现类且MetaObject存在，只有实现类才能添加拓展
     * 2. 传入的拓展类实例的元对象记录的被拓展类中有没有 this类或其父类，如果没有则不合法
     * 3. 是否是DataExtension或CacheExtension, 只有这两个类型的拓展类可以添加
     * 设置后将会把this绑定到拓展类实例，并把拓展类实例追加到自己的 BaseUnknownData链表中
     *
     * @param iExtension 拓展类实例
     * @param iCheckValidity 是否检查传入的拓展类是否合法
     * @return SystemStatus
     */
    virtual SystemStatus AddExtension(BaseUnknown* iExtension, bool iCheckValidity = false);

    /**
     * @brief 获取sharedObject，只用于CodeExtension的TIE存储CodeExtension实例
     *
     * @return BaseUnknown*
     */
    [[nodiscard]] virtual BaseUnknown* GetSharedObject() {
      return nullptr;
    }

    /**
     * @brief 设置sharedObject，只用于CodeExtension的TIE存储CodeExtension实例
     */
    virtual void SetSharedObject(BaseUnknown* iSharedObject) {
    }

    /**
     * @brief 获取真实类型的类名
     */
    [[nodiscard]] virtual std::string_view ClassName() const {
      return Name;
    }

    /**
     * @brief 获取真实类型
     */
    [[nodiscard]] virtual TypeOfClass ClassType() const {
      return GetMeta()->GetTypeOfClass();
    }

    [[nodiscard]] virtual bool IsEqual(const BaseUnknown* iObject) const;

   protected:
    /**
     * @brief 析构函数
     *
     * 1. 如果是实现类，则释放所有BaseUnknownData链表中的实例
     * 2. 如果是拓展类，则解除与实现类的双向绑定关系（存储在BaseUnknownData中）
     * 3. 被拓展类的引用计数减去this的引用计数后递减
     */
    virtual ~BaseUnknown();

    /**
     * @brief 存储拓展和TIE的内部机制需要的对象
     */
    BaseUnknownData _data;

   private:
    /**
     * Copy Constructor.
     * @param iObj The BaseUnknown instance to copy
     */
    // BaseUnknown(const BaseUnknown &iObj) = delete;

    // meta class pointer
    static MetaClass* _metaObject;

    friend class WeakRef;
    friend class BaseUnknownData;
    friend class TIEInternal;
    friend class BOAInternal;
  };

  // @todo (nekomiya) check offset

}  // namespace DE
template <>
struct std::hash<DE::BaseUnknown> {
  [[nodiscard]] std::size_t operator()(const DE::BaseUnknown& iInstance) const noexcept {
    return reinterpret_cast<size_t>(&iInstance);
  }
};

// @todo (Nekomiya) WHO FUCKING PLACED THESE MACROS HERE?!!  THEY NEVER BELONG TO SYSTEM!!
/**
 * @nodoc
 */
#define DEMacBuildGTM(implementation, interface) \
  SetInterface(reinterpret_cast<DEIGTMUnknown*>(CreateTIE_##interface##_##implementation(this)));

#define DEMacBuildGTMOfficiel(implementation, interface) \
  SetInterface(reinterpret_cast<DEIGTMUnknown*>(CreateTIE_##interface##_##implementation(this)));

#include "MacrosForBaseUnknown.h"
