#pragma once

// ReSharper disable once CppUnusedIncludeDirective
#include "DictionaryFiller.h"

namespace DE {

  // @todo (Nekomiya) void* is far from type safety. rewrite this pile of shit someday.

  using ExternalObjectCreatorFunc = void* (*)(void*);

  inline auto CastToExternalCreatorFunc(void* iCreator) {
    return reinterpret_cast<ExternalObjectCreatorFunc>(iCreator);
  }

  inline static constexpr std::string_view CREATE_INSTANCE_INTERFACE = "CreateInterface";

  [[nodiscard]] inline ustring GetExternalObjectCreatorFuncName(std::string_view iClass, bool iIsCommand = false) {
    return ustring(iIsCommand ? "CreateClassInstance_" : "CreateClassInstance_Command_") + iClass;
  }

  /**
   * @brief Defines the command creation function.
   * @param CLASSNAME Class of the command to create and used as input of the @href DECreateExternalObject function.
   */
#define DefineExternalObjectCreator_2(ALIASED_NAME, CLASSNAME)                                                     \
  namespace {                                                                                                      \
    extern "C" void* CreateClassInstance_##ALIASED_NAME(void* iArgs) {                                             \
      return new CLASSNAME();                                                                                      \
    }                                                                                                              \
                                                                                                                   \
    static_assert(std::same_as<ExternalObjectCreatorFunc, decltype(&CreateClassInstance_##ALIASED_NAME)>);         \
                                                                                                                   \
    DictionaryFiller CreateExternalObject_##CLASSNAME_DictionaryFiller(                                            \
        #CLASSNAME, CREATE_INSTANCE_INTERFACE, reinterpret_cast<CreationFunc>(&CreateClassInstance_##ALIASED_NAME) \
    );                                                                                                             \
  }

#define DefineExternalObjectCreator_1(CLASSNAME) DefineExternalObjectCreator_2(CLASSNAME, CLASSNAME)

#define Internal_DefineExternalObjectCreator_Impl(...)                                                     \
  M_MACRO_OVERRIDING_HELPER(                                                                               \
      M_MACRO_OVERRIDING_IMPL_2(__VA_ARGS__, DefineExternalObjectCreator_2, DefineExternalObjectCreator_1) \
  )

#define DefineExternalObjectCreator(...) \
  M_MACRO_OVERRIDING_HELPER(Internal_DefineExternalObjectCreator_Impl(__VA_ARGS__)(__VA_ARGS__))

  /**
   * @brief Defines the command creation function with an argument.
   *
   * @param CLASSNAME Class of the command to create and used as input of the @href DECreateExternalObject function.
   * @param ARGUMENT_TYPE Argument type expected by the function
   */
#define DefineExternalObjectCreatorWithArg_3(ALIASED_NAME, CLASSNAME, ARGUMENT_TYPE)                       \
  namespace {                                                                                              \
    extern "C" void* CreateClassInstance_##ALIASED_NAME(ARGUMENT_TYPE* iArgs) {                            \
      return new CLASSNAME(iArgs);                                                                         \
    }                                                                                                      \
                                                                                                           \
    static_assert(std::same_as<ExternalObjectCreatorFunc, decltype(&CreateClassInstance_##ALIASED_NAME)>); \
                                                                                                           \
    DictionaryFiller CreateExternalObject_##CLASSNAME_DictionaryFiller(                                    \
        #CLASSNAME, reinterpret_cast<CreationFunc>(&CreateClassInstance_##ALIASED_NAME)                    \
    );                                                                                                     \
  }

#define DefineExternalObjectCreatorWithArg_2(CLASSNAME, ARGUMENT_TYPE) \
  DefineExternalObjectCreatorWithArg_3(CLASSNAME, CLASSNAME, ARGUMENT_TYPE)

#define DefineExternalObjectCreatorWithArg_1(CLASSNAME, ARGUMENT_TYPE) \
  M_MACRO_PLACEHOLDER_ERROR_2(CLASSNAME, ARGUMENT_TYPE)

#define Internal_DefineExternalObjectCreatorWithArg_Impl(...)                                  \
  M_MACRO_OVERRIDING_HELPER(M_MACRO_OVERRIDING_IMPL_3(                                         \
      __VA_ARGS__, DefineExternalObjectCreatorWithArg_3, DefineExternalObjectCreatorWithArg_2, \
      DefineExternalObjectCreatorWithArg_1                                                     \
  ))

#define DefineExternalObjectCreatorWithArg(...) \
  M_MACRO_OVERRIDING_HELPER(Internal_DefineExternalObjectCreatorWithArg_Impl(__VA_ARGS__)(__VA_ARGS__))

}  // namespace DE