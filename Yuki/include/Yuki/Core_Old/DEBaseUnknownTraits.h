/**
 * @file DEBaseUnknownTraits.h
 * @brief
 * @~chinese
 * DE架构中的类型特征定义
 *
 * 本文件定义了DE架构中各种组件类型的特征概念（C++20 concepts），用于：
 * - 在编译时进行类型检查和约束
 * - 区分不同类型的DE组件（接口、实现、扩展等）
 * - 确保类型安全和正确的组件继承关系
 *
 * @~english
 * Type traits definitions for the DE architecture
 *
 * This file defines type trait concepts (C++20) for various DE component types, used for:
 * - Compile-time type checking and constraints
 * - Distinguishing different types of DE components (interfaces, implementations, extensions, etc.)
 * - Ensuring type safety and correct component inheritance relationships
 */
#pragma once

#include "BaseUnknown.h"

namespace DE {

  /**
   * @brief
   * @~chinese
   * 检查类型是否继承自BaseUnknown
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type inherits from BaseUnknown
   * @tparam T Type to check
   */
  template <class T>
  concept BaseUnknownClass = std::is_base_of<BaseUnknown, T>::value || std::is_same_v<BaseUnknown, T>;

  /**
   * @brief
   * @~chinese
   * 检查类型是否不继承自BaseUnknown
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type does not inherit from BaseUnknown
   * @tparam T Type to check
   *
   * @see Also
   * - BaseUnknownClass<T>
   */
  template <class T>
  concept NonBaseUnknownClass = !BaseUnknownClass<T>;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为DE接口类
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a DE interface class
   * @tparam T Type to check
   */
  template <class T>
  concept InterfaceClass = BaseUnknownClass<T> && T::Type == TypeOfClass::Interface;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为DE实现类
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a DE implementation class
   * @tparam T Type to check
   */
  template <class T>
  concept ImplementationClass = BaseUnknownClass<T> && T::Type == TypeOfClass::Implementation;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为数据扩展类
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a data extension class
   * @tparam T Type to check
   */
  template <class T>
  concept DataExtensionClass = BaseUnknownClass<T> && T::Type == TypeOfClass::DataExtension;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为代码扩展类
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a code extension class
   * @tparam T Type to check
   */
  template <class T>
  concept CodeExtensionClass = BaseUnknownClass<T> && T::Type == TypeOfClass::CodeExtension;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为缓存扩展类
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a cache extension class
   * @tparam T Type to check
   */
  template <class T>
  concept CacheExtensionClass = BaseUnknownClass<T> && T::Type == TypeOfClass::CacheExtension;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为临时扩展类
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a transient extension class
   * @tparam T Type to check
   */
  template <class T>
  concept TransientExtensionClass = BaseUnknownClass<T> && T::Type == TypeOfClass::TransientExtension;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为TIE类（类型接口扩展）
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a TIE (Type Interface Extension) class
   * @tparam T Type to check
   */
  template <class T>
  concept TIEClass = BaseUnknownClass<T> && T::Type == TypeOfClass::TIE;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为任意类型的扩展类
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is any kind of extension class
   * @tparam T Type to check
   */
  template <class T>
  concept ExtensionClass
      = DataExtensionClass<T> || CodeExtensionClass<T> || CacheExtensionClass<T> || TransientExtensionClass<T>;

  /**
   * @brief
   * @~chinese
   * 检查类型是否为组件类（实现类或扩展类）
   * @tparam T 要检查的类型
   *
   * @~english
   * Checks if a type is a component class (implementation or extension)
   * @tparam T Type to check
   */
  template <class T>
  concept ComponentClass = ImplementationClass<T> || ExtensionClass<T>;

}  // namespace DE