/**
 * @file AutoVar.h
 * @brief
 * @~chinese
 * DE架构中的智能指针基类定义
 *
 * 本文件定义了DE架构中的智能指针基类AutoVar，用于：
 * - 自动管理DE对象的生命周期
 * - 提供类似COM智能指针的功能
 * - 支持移动语义以优化性能
 * - 确保线程安全的引用计数管理（未实现）
 *
 * @~english
 * Smart pointer base class definition for DE architecture
 *
 * This file defines the AutoVar base class for smart pointers in DE architecture, used for:
 * - Automatic lifecycle management of DE objects
 * - Providing COM-like smart pointer functionality
 * - Supporting move semantics for performance optimization
 * - Ensuring thread-safe reference counting
 *
 * @todo 当前版本未实现线程安全的引用计数管理
 */

#pragma once

#include "BaseUnknown.h"

#include "gin/runtime/assertion.h"

#include <stdio.h>

namespace DE {

  /**
   * @nodoc
   */
  constexpr BaseUnknown* NULL_var = nullptr;

  template <typename T>
  class AutoVar;

  /**
   * @brief
   * @~chinese
   * DE架构中的智能指针基类
   *
   * 该类提供了以下核心功能：
   * - 自动引用计数管理 (类似COM的AddRef/Release)
   * - 移动语义支持，优化性能
   *
   * @~english
   * Smart pointer base class for DE architecture
   *
   * This class provides the following core features:
   * - Automatic reference counting (similar to COM's AddRef/Release)
   * - Move semantics support for performance optimization
   */
  template <>
  class AutoVar<BaseUnknown> {
   public:
    using InterfaceType = BaseUnknown;
    using BaseInterfaceType = void;
    using Base = void;

    /**
     * @brief
     * @~chinese
     * 构造空的智能指针
     * 初始化一个指向nullptr的智能指针实例
     *
     * @~english
     * Construct an empty smart pointer
     * Initializes a smart pointer instance pointing to nullptr
     */
    AutoVar<BaseUnknown>() : pointer(nullptr) {
    }

    /**
     * @brief
     * @~chinese
     * 析构函数
     * 自动递减所持有对象的引用计数, 并将对象指针置为nullptr
     *
     * @~english
     * Destructor
     * Automatically releases the reference count of the held object
     */
    ~AutoVar() {
      if (pointer) {
        pointer->Release();
      }
      pointer = nullptr;
    }

    /**
     * @brief
     * @~chinese
     * 从原始指针构造智能指针
     * @param iPtr 要管理的BaseUnknown对象指针
     *
     * @~english
     * Construct smart pointer from raw pointer
     * @param iPtr Pointer to BaseUnknown object to manage
     */
    AutoVar(BaseUnknown* iPtr) : pointer(iPtr) {
      if (pointer) {
        pointer->AddRef();
      }
    }

    /**
     * @brief
     * @~chinese
     * 移动构造函数
     *
     * @~english
     * Move constructor
     */
    AutoVar(AutoVar&& iOther) noexcept : pointer(iOther.pointer) {
      iOther.pointer = nullptr;
    }

    /**
     * @brief
     * @~chinese
     * 复制构造函数
     * @param iRef 要复制的智能指针引用
     *
     * @~english
     * Copy constructor
     * @param iRef Reference to smart pointer to copy from
     */
    AutoVar(const AutoVar& iRef) : pointer(iRef.pointer) {
      if (pointer) {
        pointer->AddRef();
      }
    }

    /**
     * @brief
     * @~chinese
     * 复制赋值运算符
     * @param iRef 要复制的智能指针引用
     *
     * @~english
     * Copy assignment operator
     * @param iRef Reference to smart pointer to copy from
     */
    AutoVar& operator=(const AutoVar& iRef) {
      if (pointer == iRef.pointer) {
        return *this;
      }

      if (pointer) {
        pointer->Release();
      }
      pointer = iRef.pointer;
      if (pointer) {
        pointer->AddRef();
      }
      return *this;
    }

    /**
     * @brief
     * @~chinese
     * 移动赋值运算符
     *
     * @~english
     * Move assignment operator
     */
    AutoVar<BaseUnknown>& operator=(AutoVar<BaseUnknown>&& iOther) {
      M_ASSERT(this != &iOther);
      if (pointer == iOther.pointer) {
        iOther.~AutoVar<BaseUnknown>();
        return *this;
      }

      if (pointer) {
        pointer->Release();
      }
      pointer = iOther.pointer;
      iOther.pointer = nullptr;

      return *this;
    }

    /**
     * @brief
     * @~chinese
     * 获取内部管理的原始指针
     * @return 返回内部管理的BaseUnknown指针
     *
     * @~english
     * Get raw pointer
     * @return Returns the internally managed BaseUnknown pointer
     */
    auto* get() const {
      return static_cast<BaseUnknown*>(pointer);
    }

    /**
     * @brief
     * @~chinese
     * 逻辑非运算符
     * @return 如果指针为空则返回true
     *
     * @~english
     * Logical not operator
     * @return Returns true if pointer is null
     */
    bool operator!() const {
      if (!pointer) {
        return true;
      }

      BaseUnknown* i1 = nullptr;
      auto s1 = pointer->QueryInterface("BaseUnknown", i1);
      if (s1 != StatusCode::OK) {
        if (i1) {
          i1->Release();
        }
        return true;
      }

      auto isNull = i1->IsNull();
      i1->Release();

      return isNull;
    }

    /**
     * @brief
     * @~chinese
     * 类型转换运算符
     * @return 返回内部管理的BaseUnknown指针
     *
     * @~english
     * Type cast operator
     * @return Returns the internally managed BaseUnknown pointer
     */
    // todo: delete or add explicit to this
    operator BaseUnknown*() const {
      return static_cast<BaseUnknown*>(pointer);
    }

    /**
     * @brief
     * @~chinese
     * 指针成员运算符
     * @return 返回内部管理的BaseUnknown指针
     *
     * @~english
     * Pointer member operator
     * @return Returns the internally managed BaseUnknown pointer
     */
    BaseUnknown* operator->() const {
      if (!pointer) {
        throw std::runtime_error("Null var");
      }
      return pointer;
    }

    /**
     * @brief
     * @~chinese
     * 获取内部管理的原始指针，仅供内部使用
     * @return 返回内部管理的BaseUnknown指针
     *
     * @~english
     * Get raw pointer, for internal purpose
     * @return Returns the internally managed BaseUnknown pointer
     */
    BaseUnknown* GetPointer() const {
      return pointer;
    }

   protected:
    /**
     * @brief
     * @~chinese
     * 内部管理的原始指针
     *
     * @~english
     * Internally managed raw pointer
     */
    BaseUnknown* pointer;
  };

  template <typename T>
  class AutoVar : public AutoVar<typename T::Base> {
   public:
    using InterfaceType = T;
    using BaseInterfaceType = typename T::Base;
    using Base = AutoVar<typename T::Base>;

    // static_assert(std::derived_from<T, typename T::Base>, "");

    AutoVar() : AutoVar<typename T::Base>() {
    }

    AutoVar(BaseUnknown* iIncoming) : AutoVar<typename T::Base>() {
      TryReplaceWith(iIncoming);
    }

    explicit AutoVar(InterfaceType* iIncoming) : AutoVar<typename T::Base>(static_cast<typename T::Base*>(iIncoming)) {
      // We can trust that iBase is indeed of InterfaceType, so no need to TryReplaceWith
    }

    AutoVar(const AutoVar<T>& iIncoming) : AutoVar<typename T::Base>(static_cast<typename T::Base*>(iIncoming)) {
      TryReplaceWith(iIncoming);
    }

    AutoVar(const AutoVar<BaseUnknown>& iIncoming) : AutoVar<typename T::Base>() {
      TryReplaceWith(iIncoming);
    }

    T* operator->() const {
#ifdef DEBUG_MODE
      auto ret = dynamic_cast<T*>(AutoVar<BaseUnknown>::operator->());
      M_ASSERT(!this->pointer || ret);
      return (T*)AutoVar<BaseUnknown>::operator->();
#else
      return (T*)AutoVar<BaseUnknown>::operator->();
#endif
    }

    operator T*() const {
      return (T*)this->pointer;
    }

    T& operator*() const {
      return *(T*)this->pointer;
    }

    AutoVar& operator=(const AutoVar<BaseUnknown>& base) {
      TryReplaceWith(base.GetPointer());
      return *this;
    }

    AutoVar& operator=(BaseUnknown* base) {
      TryReplaceWith(base);
      return *this;
    }

   protected:
    void TryReplaceWith(BaseUnknown* iIncoming) {
      BaseUnknown* ret = nullptr;
      do {
        if (!iIncoming) {
          break;
        }
        if (iIncoming == this->pointer) {
          ret = this->pointer;  // nothing changes
        }

        auto meta = iIncoming->GetMeta();

        // The incoming instance itself is suitable for our interface, so just use it
        if (meta && meta->IsAKindOf(T::Name)) {
          iIncoming->AddRef();
          ret = iIncoming;
          break;
        }

        // Try finding the requested interface through extensions
        BaseUnknown* intf = nullptr;
        iIncoming->QueryInterface(T::Name, intf);
        ret = intf;
        break;
      } while (false);

      // nothing changes
      if (ret == this->pointer) {
        return;
      }
      if (this->pointer) {
        this->pointer->Release();  // release former one
      }
      this->pointer = ret;
    }
  };

  template <typename T>
  inline bool operator!=(std::nullptr_t, const AutoVar<T>& ptr) {
    return ptr.get() != nullptr;
  }

  template <typename T>
  inline bool operator==(std::nullptr_t, const AutoVar<T>& ptr) {
    return ptr.get() == nullptr;
  }

  template <typename T>
  inline bool operator!=(const AutoVar<T>& ptr, std::nullptr_t) {
    return ptr.get() != nullptr;
  }

  template <typename T>
  inline bool operator==(const AutoVar<T>& ptr, std::nullptr_t) {
    return ptr.get() == nullptr;
  }

  template <typename T, typename U>
  inline bool operator==(const AutoVar<T>& lf, const AutoVar<U>& rt) {
    auto lfPtr = (BaseUnknown*)lf;
    auto rtPtr = (BaseUnknown*)rt;
    if (!lfPtr || !rtPtr) {
      return lfPtr == rtPtr;
    }

    BaseUnknown *i1 = nullptr, *i2 = nullptr;
    auto s1 = lfPtr->QueryInterface("BaseUnknown", i1);
    auto s2 = rtPtr->QueryInterface("BaseUnknown", i2);
    if (s1 != StatusCode::OK || s2 != StatusCode::OK) {
      if (i1) {
        i1->Release();
      }
      if (i2) {
        i2->Release();
      }
      throw std::runtime_error("Unexpected behaviour");
    }

    if (i1) {
      i1->Release();
    }
    if (i2) {
      i2->Release();
    }
    return i1 == i2;
  }

  template <typename T, typename U>
  inline bool operator==(const AutoVar<T>& lf, const U* rt) {
    return lf == AutoVar<U>(rt);
  }

  template <typename T, typename U>
  inline bool operator==(const AutoVar<T>& lf, U* rt) {
    return lf == AutoVar<U>(rt);
  }

  template <typename T, typename U>
  inline bool operator==(const T* lf, const AutoVar<U>& rt) {
    return AutoVar<T>(lf) == rt;
  }

  template <typename T, typename U>
  inline bool operator==(T* lf, const AutoVar<U>& rt) {
    return AutoVar<T>(lf) == rt;
  }

  template <typename T, typename U>
  inline bool operator!=(const AutoVar<T>& lf, const AutoVar<U>& rt) {
    return !(lf == rt);
  }

  template <typename T, typename U>
  inline bool operator!=(const AutoVar<T>& lf, const U* rt) {
    return !(lf == rt);
  }

  template <typename T, typename U>
  inline bool operator!=(const T* lf, const AutoVar<U>& rt) {
    return !(lf == rt);
  }

  template <typename T, typename U>
  inline bool operator!=(const AutoVar<T>& lf, U* rt) {
    return !(lf == rt);
  }

  template <typename T, typename U>
  inline bool operator!=(T* lf, const AutoVar<U>& rt) {
    return !(lf == rt);
  }

}  // namespace DE