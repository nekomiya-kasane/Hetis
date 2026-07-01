#pragma once

#include "BaseUnknown.h"
#include "DEBaseUnknownTraits.h"
#include "DictionaryManager.h"
#include "ObjectChain.h"

#include "core_export.h"

namespace DE {

  /**
   *  @~chinese
   *
   *  @brief
   * 用于创建拓展的参数结构体。包含了拓展组件的名字和创建接口函数指针（可能是创建TIE的函数，也可能是创建BOA的函数）。
   */
  struct ArgsForExtension {
    std::string_view compName;
    DefaultCreationFunc compCreatorFunc = nullptr;
    CreationFunc compBaseCreatorFunc = nullptr;
  };

  /**
   * @brief 一系列用于为TIE对象填充信息的函数，包括填充引用计数、绑定的实现类对象等。仅供内部使用。
   */
  class CORE_EXPORT TIEInternal {
   public:
    static void SetLastCreatedTie(const BaseUnknown* iTie);
    static const BaseUnknown* GetLastCreatedTie();

    /**
     * @brief 为实现类对象填充TIE信息，TIE需要是新创建的
     *
     * 该函数填充了以下信息：
     * 1. TIE的引用计数初始化为1
     * 2. TIE的实现类对象指针指向传入的实现类对象
     * 3. 如果是TIEchain类型，则在是实现类对象里的BaseUnknownData链表中添加该TIEchain对象
     *
     * @code
     * [Original Usage]
     *   → TIETSTIGraphicPropTSTOmPointGPExt:
     *        Tie_Construct(this, _metaObject, &NecessaryData.ForTIE, 1, pt,
     *                      TSTOmPointGPExt::ClassId(),
     *                      TSTOmPointGPExt::MetaObject()->GetTypeOfClass(), ptstat,
     *                      TSTOmPointGPExt::CreateItself, delegue, &delegate);
     * @endcode
     *
     * @todo if the function is called when querying interface, then no need to check duplication now we just check in
     * every call
     *
     * @param ioTie TIE实例的指针，该TIE实例需要是新创建的。
     * @param iImplOrExtendee 与TIE绑定的实现类对象
     * @return 执行状态码
     */
    static SystemStatus Tie_Construct_Implementation(BaseUnknown* ioTie, BaseUnknown* iImplOrExtendee);

    /**
     * @brief 为数据拓展类对象填充TIE信息，TIE需要是新创建的
     *
     * 该函数填充了以下信息：
     * 1. TIE的引用计数初始化为1
     * 2. 如果没有被拓展类实例传入，则创建一个拓展类实例，Tie本身绑定到拓展类实例上。
     * 3.
     * 如果有拓展类实例，则将TIE添加到被拓展类实例里的BaseUnknownData链表中。Tie本身绑定到拓展类实例上。同时拓展类实例的引用计数递增
     *
     * @code
     * [Instance]
     *                 ExtIntf                   TIE knows to create ExtImpl
     *                    |                      and put it in ForTIE, by
     *            Inherit |                      looking into the type of
     *                    |               ↙---- ExtImpl, which is Extension.
     *                   TIE ...............................
     *                    |                                .
     *             ForTIE |                                .
     *                    |         ForExtension           .
     *                 ExtImpl <===================> Implementation (In)
     *                           ForImplementation[]
     *
     * [Meta]
     *  Some-Intf-Of-Implementation
     *      ↘ Query
     *      Implementation
     *          ↘ Query
     *            (Meta<Implementation> interface chain)
     *            (Meta<Derivation> dictionary) - i.e. transverse the inheritance tree
     *                 |
     *                 |    ↓ what's in the dictionary?
     *                 |
     *                 |--- interfaces implemented directly
     *                 └--- interfaces implemented via extensions <== we find them here!
     *
     *                      ↓ how to load the creation function for the tie
     *
     *                 load library -> the creation function will be registered via the instantiation of a static
     *                                 Filler class
     *                              -> <ExtImpl, ExtIntf> Filler will register the interface for its extendees as
     *                              well
     *
     *          ↘ Create
     *            (Create the extension's impl
     *            (Create the TIE for the extension and its interface)
     *            (link the extension's impl with what it extends)
     * @endcode
     *
     * @note 该函数允许拓展类是悬空的，即没有被拓展类传入。
     *
     * @param ioTie TIE实例的指针，该TIE实例需要是新创建的。
     * @param iImplOrExtendee 与TIE绑定的数据拓展类对象
     * @return 执行状态码
     */
    static SystemStatus Tie_Construct_DataExtension(
        BaseUnknown* ioTie, BaseUnknown* iImplOrExtendee, ArgsForExtension& iAdditionalArgs
    );

    /**
     * @brief 为Transient拓展类对象填充TIE信息，TIE需要是新创建的
     *
     * 与DataExtension行为相同。
     *
     * @see Tie_Construct_DataExtension
     *
     * @param ioTie TIE实例的指针，该TIE实例需要是新创建的。
     * @param iImplOrExtendee 与TIE绑定的Transient拓展类对象
     * @return 执行状态码
     */
    static SystemStatus Tie_Construct_TransientExtension(
        BaseUnknown* ioTie, BaseUnknown* iImplOrExtendee, ArgsForExtension& iAdditionalArgs
    );

    /**
     * @brief 为缓存拓展类对象填充TIE信息，TIE需要是新创建的
     *
     * 该函数填充了以下信息：
     * 1. TIE的引用计数设为1
     * 2. 构建新的缓存拓展类实例（每次调用都会构建）并绑定到TIE上
     * 3. TIE绑定到拓展类实例上
     *
     * @see Tie_Construct_DataExtension
     *
     * @note 如果是TIEchain的话，构建好的TIE会被添加到被拓展类下面的BaseUnknownData中的链表里
     *
     * @param ioTie TIE实例的指针，该TIE实例需要是新创建的。
     * @param iImplOrExtendee 与TIE绑定的Transient拓展类对象
     * @return 执行状态码
     */
    static SystemStatus Tie_Construct_CacheExtension(
        BaseUnknown* ioTie, BaseUnknown* iImplOrExtendee, ArgsForExtension& iAdditionalArgs
    );

    /**
     * @brief 为Code拓展类对象填充TIE信息，TIE需要是新创建的
     *
     * 该函数填充了以下信息：
     * 1. TIE的引用计数设为1
     * 2. 如果该TIE的实例没有shared对象，则构建Code拓展类实例并赋给shared，否则递增 shared对象的引用计数
     * 3. TIE绑定到拓展类实例上
     *
     * 因为shared对象是所有同类型TIE对象共享的，因此CodeExtension实例只有一份，并且这个TIE会反复动态绑定到不同的被拓展类实例上。
     *
     * CodeExtension[shared] <=> TIE <=> extendee[tieBase]
     *
     * @see Tie_Construct_DataExtension
     *
     * @note 如果是TIEchain的话，构建好的TIE会被添加到被拓展类下面的BaseUnknownData中的链表里
     *
     * @param ioTie TIE实例的指针，该TIE实例需要是新创建的。
     * @param iImplOrExtendee 与TIE绑定的Code拓展类对象
     * @return 执行状态码
     */
    static SystemStatus Tie_Construct_CodeExtension(
        BaseUnknown* ioTie, BaseUnknown* iImplOrExtendee, ArgsForExtension& iAdditionalArgs
    );

    /**
     * @brief 等同于使用TIE绑定的组件类实例QueryInterface，仅供内部使用
     *
     * 如果TIE没有绑定任何组件类实例，返回Err_HangingTie
     *
     * @param ioTie TIE实例的指针
     * @param iInterfaceName 要Query的接口名称
     * @param ioInterfaceInstanc 返回的接口实例

     * @return 执行状态码
     */
    static SystemStatus Tie_Query(
        BaseUnknown* iTie, const std::string_view& iInterfaceName, BaseUnknown*& ioInterfaceInstanc
    );

    /**
     * @brief 递增TIE和其绑定的组件类实例的引用计数，仅供内部使用
     *
     * 如果TIE没有绑定任何组件类实例，返回Err_HangingTie
     *
     * @param ioTie TIE实例的指针
     * @param ioRef 递增后的引用计数
     *
     * @return 执行状态码
     */
    static uint32_t Tie_AddRef(BaseUnknown* ioTie, uint32_t& ioCRef);

    /**
     * @brief 递减TIE和其绑定的组件类实例的引用计数，仅供内部使用
     *
     * 如果是TieChain且其绑定的组件类实例的引用计数为0，重置组件类实例指针为NULL
     *
     * @param ioTie TIE实例的指针
     * @param ioRef 递减后的引用计数
     * @param oDestruct 是否需要析构该TIE实例（当传入的TIE实例的引用计数执行后为0时为true）
     *
     * @return 传入TIE实例执行后的引用计数
     */
    static uint32_t Tie_Release(BaseUnknown* ioTie, uint32_t& ioCRef, bool& oDestruct);

    /**
     * @brief 在组件类下的BaseUnknownData中查找TIE，并递增其和其绑定的组件类实例的引用计数.
     *
     * @param iImpl 组件类实例
     * @param iInterfaceName 要查找的TIE的接口名称
     *
     * @return 返回的TIE实例，如果没找到，则返回NULL
     */
    static BaseUnknown* Tie_Link(BaseUnknown* iImpl, std::string_view iInterfaceName);

    /**
     * @brief AutoRxBinder
     * 结构体用于动态地改变一个CodeExtension的被拓展类对象。它通过保存和恢复上下文来支持在运行时切换对象的行为。
     *
     * 所有的AutoRxBinder实例必须且确实在函数的栈帧内创建，因此构成了一个自动的RAII机制或者说人工实现的栈结构，当函数返回时，AutoRxBinder会自动析构并恢复原始的实现。
     *
     * `_data`: 存储当前的BaseUnknown数据指针，用来获取或设置扩展的对象。
     * `_oldBaseImpl`: 保存旧的被拓展类对象指针，以便在需要时恢复。
     */
    struct AutoRxBinder {
      // Save context for tie call
      inline AutoRxBinder() {
      }
      /**
       * @brief 析构函数，析构时恢复原来绑定的被拓展类对象
       */
      inline ~AutoRxBinder() {
        _data->SetExtendee(_oldBaseImpl);
      }
      /**
       * @brief 保存当前上下文并绑定新的被拓展类对象
       */
      inline BaseUnknown* Run(BaseUnknown*& iSharedObject, BaseUnknown* iForTie /* new impl */) {
        if (iSharedObject) {
          _oldBaseImpl = iSharedObject->_data.GetExtendee();  // old impl
          _data = &(iSharedObject->_data);

          _data->SetExtendee(iForTie);
          return iSharedObject;
        }
        return iForTie;
      }

     private:
      BaseUnknownData* _data = nullptr;
      BaseUnknown* _oldBaseImpl;
    };

#ifdef TIE_USE_DYNAMIC_CAST
#  define Tie_Cast dynamic_cast<ImplClass*>
#else
#  define Tie_Cast reinterpret_cast<ImplClass*>
#endif

/**
 * @brief 宏定义，只在TIE中使用，用以获得TIE绑定的组件类对象的指针。封装该类只是为了方便处理CodeExtension的情况。
 *        因为只有CodeExtension的组件类对象才需要动态绑定，才是使用shared成员函数调用接口，其余的都是使用TieBase
 *        （存储在BaseUnknownData中的）来调用接口。
 *
 * @param SHARED_OBJECT 组件类的实例
 * @param FOR_TIE TIE的实例
 */
#define Tie_RxBind(SHARED_OBJECT, FOR_TIE)                                                                      \
  Tie_Cast(                                                                                                     \
      (ImplClass::Type == TypeOfClass::CodeExtension) ? TIEInternal::AutoRxBinder().Run(SHARED_OBJECT, FOR_TIE) \
                                                      : FOR_TIE                                                 \
  )
  };

}  // namespace DE

#define Tie_RxBind_Default() Tie_RxBind(_sharedObject, _data.GetTieBase())

//
// Declarations
//

/**
 * @brief 声明TIE类共同需要的函数，主要是元对象相关的函数。
 *
 * @param INTERFACE_NAME TIE实现的接口名称
 */
#define DeclareTIE(INTERFACE_NAME)                                                                          \
 private:                                                                                                   \
  /* such as for the extension object in a code extension relationship, all implementation shares the same  \
   * _sharedObject */                                                                                       \
  static BaseUnknown* _sharedObject;                                                                    \
  static MetaClass* _metaObject;                                                                        \
                                                                                                            \
 public:                                                                                                    \
  constexpr static TypeOfClass Type = TypeOfClass::TIE; /* may be TIEchain as well, modified at runtime */  \
                                                                                                            \
  using InterfaceClass = INTERFACE_NAME;                                                                    \
  using ComponentClass = ImplClass;                                                                         \
                                                                                                            \
  using Base = INTERFACE_NAME;                                                                              \
                                                                                                            \
  TIE_##INTERFACE_NAME();                                                                                   \
  static MetaClass* GetMetaStatic();                                                                 \
  /* virtual function for getting real meta object */                                                       \
  virtual MetaClass* GetMeta() const override {                                                   \
    return GetMetaStatic();                                                                              \
  }                                                                                                         \
                                                                                                            \
  inline constexpr virtual std::string_view ClassName() const override {                                    \
    return INTERFACE_NAME::Name;                                                                            \
  }                                                                                                         \
                                                                                                            \
  inline virtual TypeOfClass ClassType() const override {                                                   \
    return this->BaseUnknown::ClassType();                                                              \
  }                                                                                                         \
                                                                                                            \
  TIE_##INTERFACE_NAME(BaseUnknown* iImpl);                                                             \
  virtual ~TIE_##INTERFACE_NAME();                                                                          \
                                                                                                            \
 public:                                                                                                    \
  virtual SystemStatus QueryInterface(const std::string_view& iInterfaceName, BaseUnknown*& iInstance); \
  virtual uint32_t AddRef();                                                                                \
  virtual uint32_t Release();                                                                               \
                                                                                                            \
  inline virtual BaseUnknown* GetSharedObject() {                                                       \
    return static_cast<BaseUnknown*>(_sharedObject);                                                    \
  }                                                                                                         \
                                                                                                            \
  inline virtual void SetSharedObject(BaseUnknown* iSharedObject) {                                     \
    _sharedObject = iSharedObject;                                                                          \
  }

//
// Implementations
//

#define ImplementTIE_Common(INTERFACE_NAME)                                                                        \
                                                                                                                   \
  template <class ImplClass>                                                                                       \
  MetaClass* TIE_##INTERFACE_NAME<ImplClass>::_metaObject = nullptr;                                           \
  template <class ImplClass>                                                                                       \
  BaseUnknown* TIE_##INTERFACE_NAME<ImplClass>::_sharedObject = nullptr;                                       \
                                                                                                                   \
  template <class ImplClass>                                                                                       \
  TIE_##INTERFACE_NAME<ImplClass>::TIE_##INTERFACE_NAME() {                                                        \
    _data.SetTieBase(nullptr);                                                                                     \
    _data.refCount = 1;                                                                                            \
  }                                                                                                                \
                                                                                                                   \
  template <class ImplClass>                                                                                       \
  TIE_##INTERFACE_NAME<ImplClass>::~TIE_##INTERFACE_NAME() {                                                       \
    _data.SetTieBase(nullptr);                                                                                     \
  }                                                                                                                \
                                                                                                                   \
  template <class ImplClass>                                                                                       \
  MetaClass* TIE_##INTERFACE_NAME<ImplClass>::GetMetaStatic() {                                             \
    if (_metaObject) {                                                                                             \
      return _metaObject;                                                                                          \
    }                                                                                                              \
    /* the metaobject of tie classes will not be added into the metaclass dictionary cuz no one will query it. */  \
    _metaObject                                                                                                    \
        = new MetaClass(INTERFACE_NAME::Name, INTERFACE_NAME::GetMetaStatic(), TIE_##INTERFACE_NAME::Type); \
    _metaObject->SetImplementationClass(ImplClass::GetMetaStatic());                                            \
    GetDictionaryManager().AddMetaClassForTie(_metaObject);                                                        \
    return _metaObject;                                                                                            \
  }

#define ImplementTIE_Own(INTERFACE_NAME)                                            \
  template <class ImplClass>                                                        \
  TIE_##INTERFACE_NAME<ImplClass>::TIE_##INTERFACE_NAME(BaseUnknown* iImpl) {   \
    TIEInternal::SetLastCreatedTie(iImpl);                                          \
    if constexpr (ImplClass::Type == TypeOfClass::Implementation) {                 \
      TIEInternal::Tie_Construct_Implementation(this, iImpl);                       \
    }                                                                               \
    else {                                                                          \
      ArgsForExtension args = {                                                     \
        .compName = ImplClass::Name,                                                \
        .compCreatorFunc = (DefaultCreationFunc) & ImplClass::template Construct<>, \
      };                                                                            \
                                                                                    \
      if constexpr (ImplClass::Type == TypeOfClass::DataExtension) {                \
        TIEInternal::Tie_Construct_DataExtension(this, iImpl, args);                \
      }                                                                             \
      if constexpr (ImplClass::Type == TypeOfClass::TransientExtension) {           \
        TIEInternal::Tie_Construct_TransientExtension(this, iImpl, args);           \
      }                                                                             \
      if constexpr (ImplClass::Type == TypeOfClass::CacheExtension) {               \
        TIEInternal::Tie_Construct_CacheExtension(this, iImpl, args);               \
      }                                                                             \
      if constexpr (ImplClass::Type == TypeOfClass::CodeExtension) {                \
        TIEInternal::Tie_Construct_CodeExtension(this, iImpl, args);                \
      }                                                                             \
    }                                                                               \
  }                                                                                 \
                                                                                    \
  template <class ImplClass>                                                        \
  SystemStatus TIE_##INTERFACE_NAME<ImplClass>::QueryInterface(                     \
      const std::string_view& iInterfaceName, BaseUnknown*& iInstance           \
  ) {                                                                               \
    return TIEInternal::Tie_Query(this, iInterfaceName, iInstance);                 \
  }                                                                                 \
                                                                                    \
  template <class ImplClass>                                                        \
  uint32_t TIE_##INTERFACE_NAME<ImplClass>::AddRef() {                              \
    return TIEInternal::Tie_AddRef(this, _data.refCount);                           \
  }                                                                                 \
                                                                                    \
  template <class ImplClass>                                                        \
  uint32_t TIE_##INTERFACE_NAME<ImplClass>::Release() {                             \
    bool mustDestruct = false;                                                      \
    uint32_t ref = TIEInternal::Tie_Release(this, _data.refCount, mustDestruct);    \
    if (mustDestruct) delete this;                                                  \
    return ref;                                                                     \
  }

/**
 * @brief 实现TIE类共同需要的函数，主要是元对象相关的函数。
 *
 * @param INTERFACE_NAME TIE实现的接口名称
 */
#define ImplementTIE(INTERFACE_NAME)   \
  ImplementTIE_Common(INTERFACE_NAME); \
  ImplementTIE_Own(INTERFACE_NAME);

// Binds

/**
 * @brief
 * 绑定宏。以TIE形式绑定接口和组件，并注册到接口-组件表中。注册主要由RecordAppenderTie模板完成，且发生在其所在DLL加载时
 *
 * @param INTERFACE_NAME 接口名称
 * @param IMPLEMENTATION_NAME 实现类名称
 */
#define TIE_Bind(INTERFACE_NAME, IMPLEMENTATION_NAME)                                                         \
  static_assert(INTERFACE_NAME::Type == TypeOfClass::Interface, #INTERFACE_NAME " is not an interface");      \
  static_assert(                                                                                              \
      has_flag(TypeOfClass::Any_Component, IMPLEMENTATION_NAME::Type), #INTERFACE_NAME " is not a component"  \
  );                                                                                                          \
                                                                                                              \
  DLLEXPORT BaseUnknown* CreateTIE_##INTERFACE_NAME##_##IMPLEMENTATION_NAME(BaseUnknown* iBaseImpl) { \
    return reinterpret_cast<BaseUnknown*>(new TIE_##INTERFACE_NAME<IMPLEMENTATION_NAME>(iBaseImpl));      \
  }                                                                                                           \
                                                                                                              \
  namespace {                                                                                                 \
    DictionaryFiller RecordAppenderTie_##INTERFACE_NAME##_##IMPLEMENTATION_NAME = {                           \
      IMPLEMENTATION_NAME::GetMetaStatic(), INTERFACE_NAME::GetMetaStatic(),                            \
      &CreateTIE_##INTERFACE_NAME##_##IMPLEMENTATION_NAME                                                     \
    };                                                                                                        \
  }

#define TIE_Bind_Begin(IMPLEMENTATION_NAME) using __CURRENT_TYPE = IMPLEMENTATION_NAME;
#define TIE_Bind_Inplace(INTERFACE_NAME) TIE_Bind(INTERFACE_NAME, __CURRENT_TYPE);
#define TIE_Bind_End()

//------------------------------------------------------------------------------------------------------

/**
 * @brief
 * 绑定宏。以TIEchain形式绑定接口和组件，并注册到接口-组件表中。注册主要由RecordAppenderTie模板完成，且发生在其所在DLL加载时
 *
 * @param INTERFACE_NAME 接口名称
 * @param IMPLEMENTATION_NAME 实现类名称
 */
#define TIEchain_Bind(INTERFACE_NAME, IMPLEMENTATION_NAME)                                                             \
  static_assert(INTERFACE_NAME::Type == TypeOfClass::Interface, #INTERFACE_NAME " is not an interface");               \
  static_assert(                                                                                                       \
      has_flag(TypeOfClass::Any_Component, IMPLEMENTATION_NAME::Type), #INTERFACE_NAME " is not a component"           \
  );                                                                                                                   \
                                                                                                                       \
  DLLEXPORT BaseUnknown* CreateTIE_##INTERFACE_NAME##_##IMPLEMENTATION_NAME(BaseUnknown* iBaseImpl) {          \
    BaseUnknown* ext = TIEInternal::Tie_Link(iBaseImpl, #INTERFACE_NAME);                                          \
    if (!ext) {                                                                                                        \
      ext = (BaseUnknown*)new TIE_##INTERFACE_NAME<IMPLEMENTATION_NAME>(iBaseImpl);                                \
    }                                                                                                                  \
    return ext;                                                                                                        \
  }                                                                                                                    \
                                                                                                                       \
  DictionaryFiller RecordAppenderTie_##INTERFACE_NAME##_##IMPLEMENTATION_NAME = {                                      \
    IMPLEMENTATION_NAME::GetMetaStatic(), INTERFACE_NAME::GetMetaStatic(),                                       \
    (CreationFunc)(TIE_##INTERFACE_NAME<IMPLEMENTATION_NAME>::GetMetaStatic()->SetClassType(TypeOfClass::TIEchain), \
                   CreateTIE_##INTERFACE_NAME##_##IMPLEMENTATION_NAME)                                                 \
  };

#define TIEchain_Bind_Begin(IMPLEMENTATION_NAME) using __CURRENT_TYPE = IMPLEMENTATION_NAME;
#define TIEchain_Bind_Inplace(INTERFACE_NAME) TIEchain_Bind(INTERFACE_NAME, __CURRENT_TYPE);
#define TIEchain_Bind_End()

//------------------------------------------------------------------------------------------------------

namespace DE {
  /**
   * @~english
   * @brief Internal helper functions for the Backward Object Attachment (BOA) mechanism in DE.
   *
   * These static methods are used to construct various types of BOA-attached components,
   *
   * @~chinese
   * @brief DE中BOA机制的内部辅助函数。
   *
   * 这些静态方法用于构建各种类型的与BOA关联的组件.
   */
  class CORE_EXPORT BOAInternal {
   public:
    /**
     *  @~chinese
     *
     *  @brief 为实现类构建由自身或自身基类实现BOA接口并记录到其下BaseUnknownData链表中。
     *
     *  对于实现类来说，BOA就是它本身，因此该函数将把`iBaseImpl`对象添加到其下BaseUnknownData链表中，并返回其自身指针。添加到链表时会查重。
     *
     *  @param iBaseImpl 实现类指针
     *  @return 传入实现类对象的BOA接口指针（实际上就是实现类自身的指针）
     */
    [[nodiscard]] static BaseUnknown* BOA_Construct_Implementation(BaseUnknown* iBaseImpl);
    /**
     *  @~chinese
     *
     *  @brief 为实现类构建由缓存拓展实现的BOA接口并记录到其下BaseUnknownData链表中。
     *
     *  对于拓展来说，BOA就是它本身，但且该拓展是缓存拓展，因此缓存拓展的Dependee将记录为传入实现类指针，但缓存拓展实例不会被记录到实现类实例下面的BaseUnknownData链表中，函数直接返回拓展类实例的指针。
     *
     *  @param iBaseImpl 实现类指针
     *  @param iArgs Args for creating the extension
     *  @return 传入实现类对象的BOA接口指针（实际上就是拓展类实例指针，它拓展了传入的实现类对象）
     */
    [[nodiscard]] static BaseUnknown* BOA_Construct_CacheExtension(
        BaseUnknown* iBaseImpl, ArgsForExtension& iArgs
    );
    /**
     *  @~chinese
     *
     *  @brief 为实现类构建由数据拓展实现的BOA接口并记录到其下BaseUnknownData链表中。
     *
     *  对于拓展来说，BOA就是它本身。此函数会：
     *  1.
     * 从实现类实例下面的aseUnknownData链表中查找是否已经存在一个与传入的`iArgs`参数匹配的数据拓展对象，如果找到则直接返回该数据拓展对象的指针并增加其引用计数。
     *  2.
     * 如果没有找到，则创建一个新的数据拓展对象，并将其添加到实现类实例下面的BaseUnknownData链表中，然后返回新创建的数据拓展对象的指针，创建时初始始引用计数为1。
     *
     *  @param iBaseImpl 实现类指针
     *  @param iArgs Args for creating the extension
     *  @return 传入实现类对象的BOA接口指针（实际上就是拓展类实例指针，它拓展了传入的实现类对象）
     */
    [[nodiscard]] static BaseUnknown* BOA_Construct_DataExtension(
        BaseUnknown* iBaseImpl, ArgsForExtension& iArgs
    );
    /**
     *  @~chinese
     *
     *  @brief 为实现类构建由数据拓展实现的BOA接口并记录到其下BaseUnknownData链表中。
     *
     *  与数据拓展行为相同。
     *  @see BOA_Construct_DataExtension
     *
     *  @param iBaseImpl 实现类指针
     *  @param iArgs Args for creating the extension
     *  @return 传入实现类对象的BOA接口指针（实际上就是拓展类实例指针，它拓展了传入的实现类对象）
     */
    [[nodiscard]] static BaseUnknown* BOA_Construct_TransientExtension(
        BaseUnknown* iBaseImpl, ArgsForExtension& iArgs
    );
  };  // namespace BOAInternal

  /**
   * @brief 根据接口类型和组件类型创建BOA对象。
   *
   * 此函数根据提供的组件类型 `Component` 创建一个BOA实例。
   *
   * @param iBaseImpl 要与新创建的 BOA 关联的实现类对象。
   * @return 创建的 BOA 实例的指针，如果成功则返回非空值
   */
  template <class Interface, ImplementationClass Component>
    requires(Component::Type == TypeOfClass::Implementation)
  [[nodiscard]] BaseUnknown* CreateBOA(BaseUnknown* iBaseImpl) {
    static_assert(Component::Type == TypeOfClass::Implementation);
    return BOAInternal::BOA_Construct_Implementation(iBaseImpl);
  }

  template <class Interface, ExtensionClass Component>
  [[nodiscard]] BaseUnknown* CreateBOA(BaseUnknown* iBaseImpl) {
    ArgsForExtension args = {
      .compName = Component::Name,
      .compCreatorFunc = static_cast<DefaultCreationFunc>(&Component::template Construct<>),
    };

    if constexpr (std::constructible_from<Component, BaseUnknown*>) {
      args.compBaseCreatorFunc = [](BaseUnknown* iInstance) -> BaseUnknown* {
        return new Component(iInstance);
      };  // @todo (Nekomiya) ugly
    }

    if constexpr (Component::Type == TypeOfClass::CacheExtension) {
      return BOAInternal::BOA_Construct_CacheExtension(iBaseImpl, args);
    }
    if constexpr (Component::Type == TypeOfClass::DataExtension) {
      return BOAInternal::BOA_Construct_DataExtension(iBaseImpl, args);
    }
    if constexpr (Component::Type == TypeOfClass::TransientExtension) {
      return BOAInternal::BOA_Construct_TransientExtension(iBaseImpl, args);
    }
    const auto msg = std::string(Interface::Name) + " and " + std::string(Component::Name) + " didn't define a BOA.";
    M_ASSERT_MSG(false, msg.c_str());
    return nullptr;
  }

  /**
   * @brief 定义 RecordAppenderBOA 的模板实例化，用于自动填充字典。
   *
   * 通过模板实例化创建一个名为 `RecordAppenderBOA` 的 `DictionaryFiller` 对象，
   * 把接口类型 `InterfaceClass`
   * 和组件类型`ComponentClass`成对作为Key，相应的CreateBOA函数指针作为Value记录到字典中。这个对象在初始化时，
   * 同时初始化了相应的元对象（MetaObject）和用于创建 BOA 函数的指针。它服务于元对象系统和动态对象模型。
   *
   * @param InterfaceClass 接口类类型
   * @param ComponentClass 组件类类型
   */
  template <typename Interface, ComponentClass Component>
  DictionaryFiller RecordAppenderBOA
      = {Component::GetMetaStatic(), Interface::GetMetaStatic(), &CreateBOA<Interface, Component>};

}  // namespace DE

/* Macro to switch between BOA and TIE at build time */
#ifdef BOA_IS_TIE
#  define BOA_Bind(INTERFACE_NAME, COMPONENT_NAME) TIE_Bind(INTERFACE_NAME, COMPONENT_NAME)
#else
/**
 * @brief 绑定宏，用于在元对象系统中注册接口和组件的关联关系。该宏将接口类型 `InterfaceClass` 和组件类型
 * `ComponentClass` 注册到元对象系统，表示通过BOA方式将接口与组件绑定。
 */
#  define BOA_Bind(INTERFACE_NAME, COMPONENT_NAME)                                             \
    static_assert(InterfaceClass<INTERFACE_NAME>, #INTERFACE_NAME " is not an interface.");    \
    static_assert(ComponentClass<COMPONENT_NAME>, "The second class must be a component.");    \
    static_assert(!CodeExtensionClass<COMPONENT_NAME>, "A code extension cannot have a BOA."); \
                                                                                               \
    DictionaryFiller RecordAppenderBoa_##INTERFACE_NAME##_##COMPONENT_NAME = {                 \
      COMPONENT_NAME::GetMetaStatic(), INTERFACE_NAME::GetMetaStatic(),                  \
      &CreateBOA<##INTERFACE_NAME##, ##COMPONENT_NAME>                                         \
    };
#endif

#define BOA_Bind_By_Name(INTERFACE_NAME, COMPONENT_NAME)                                                \
  static_assert(InterfaceClass<INTERFACE_NAME>, #INTERFACE_NAME " is not an interface.");               \
  DictionaryFiller RecordAppenderBoa_##INTERFACE_NAME##_##COMPONENT_NAME = {                            \
    DictionaryManager::GetInstance().GetMetaClass(#COMPONENT_NAME), INTERFACE_NAME::GetMetaStatic(), \
    &CreateBOA<##INTERFACE_NAME##, ##COMPONENT_NAME>                                                    \
  };

#define BOA_Bind_Begin(COMPONENT_NAME) using __CURRENT_TYPE = COMPONENT_NAME;
#define BOA_Bind_Inplace(INTERFACE_NAME) BOA_Bind(INTERFACE_NAME, __CURRENT_TYPE);
#define BOA_Bind_End()
