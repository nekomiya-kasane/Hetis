#pragma once

/**
 * @file DictionaryManager.h
 * @brief
 * @~chinese
 * DE架构的字典管理器，负责组件和接口的注册、查找和元数据管理
 *
 * 主要功能（均在运行时）：
 * - 管理组件和接口的注册信息
 * - 维护组件库和元对象类信息
 * - 处理组件创建和条件检查函数
 * - 支持从文件和目录加载字典数据
 * - 提供字符串池优化和委托关系管理（后者未实现）
 *
 * @~english
 * Dictionary manager for DE architecture, responsible for component and interface registration, lookup, and metadata
 * management
 *
 * Key features:
 * - Manages component and interface registration information
 * - Maintains component libraries and metaclass information
 * - Handles component creation and condition check functions
 * - Supports dictionary data loading from files and directories
 * - Provides string pool optimization and delegation relationship management
 */

#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core_export.h"

#include "MacrosForBaseUnknown.h"

namespace env {

  inline constexpr char8_t global$dictionary$schemas_directory[] = u8"global.dictionary.schemas-directory";

}

namespace DE {

  class MetaClass;

  using UniqueString = std::string_view;
  using PairedName = std::pair<std::string_view, std::string_view>;

  /**
   * @struct PairedNameHasher
   * @brief
   * @~chinese
   * 配对名称的哈希计算器，用于组件-接口对的哈希表存储，同时优化字典数据的存储和访问。由两个字符串的哈希值按位或得来
   *
   * @~english
   * Hasher for paired names, used for component-interface pair hash table storage
   * to optimizes dictionary data storage and access
   */
  struct PairedNameHasher {
    uint64_t operator()(const PairedName& iPairedName) const;
  };

  /**
   * @struct CompIntfRecord
   * @brief
   * @~chinese
   * 组件-接口记录，存储组件和接口的关联信息及其状态
   *
   * 主要功能：
   * - 记录组件和接口的关联关系
   * - 管理组件的加载状态和优先级
   * - 存储组件的创建函数和条件函数指针
   * - 维护组件的库依赖信息
   *
   * @note
   * - 默认状态为NotLoaded，表示尚未加载
   * - 对于与main函数一起加载的组件，不需要库名，可留空，其他需要动态加载的组件必须提供库名
   * - 创建函数指针在运行时由DictionaryFiller提供
   * - 条件函数可为空，为空则表示无需条件检查，如果需要检查，则需要在字典文件中提供检查函数的函数名和所在DLL名
   *
   * @~english
   * Component-interface record, stores association information and status between components and interfaces
   *
   * Key features:
   * - Records component and interface associations
   * - Manages component loading status and priority
   * - Stores component creation and condition functions
   * - Maintains component library dependency information
   *
   * @note
   * - Default status is NotLoaded, indicating that it has not been loaded yet.
   * - For components loaded together with the main function, no library name is required, which can be left empty.
   * Other dynamically loaded components must provide a library name.
   * - The creation function pointer is provided by DictionaryFiller at runtime.
   * - The condition function may be null, indicating that no condition check is needed.
   */
  struct CompIntfRecord {
    /**
     * @brief
     * @~chinese
     * 组件加载状态枚举
     *
     * - Loaded: 已加载
     * - NotLoaded: 未加载，这是字典加载记录的默认状态
     * - Unauthorized: 组件未经授权，即检查函数返回空值
     * - Unreachable: 无法加载对应的库，找不到
     *
     * @~english
     * Component loading status enumeration
     *
     * - Loaded: Loaded
     * - NotLoaded: Not loaded, which is the default status for dictionary loading records.
     * - Unauthorized: Component not authorized, i.e., the condition function returns a null value.
     * - Unreachable: The corresponding library cannot be loaded, or the symbol is still not loaded despite of the given
     * dll
     */
    enum RecordStatus : int8_t {
      LOADED = 1,
      NOT_LOADED = 0,
      UNAUTHORIZED = -1,
      UNREACHABLE = -2,
    };

    /**
     * @brief
     * @~chinese
     * 接口组件条目优先级枚举
     *
     * - Settled: 已确定的条目，即字典中的条目
     * - DirectedlyImplemented: 直接实现的组件，即TIE_Bind, BOA_Bind等直接声明的条目
     * - InDirectedlyImplemented:
     * 间接实现的组件，即通过继承关系和拓展关系被间接实现的条目，比如组件类间接实现了其实现的接口类的父类的接口
     * - Blank: 未指定优先级
     *
     * 优先级数值越小，优先级越高
     *
     * @note 高优先级的条目会覆盖低优先级的条目，同优先级的条目后来的会覆盖先前的。因此，需要避免同优先级的竞争条目
     *
     * @~english
     * Component priority enumeration
     *
     * - Settled: Settled, i.e., the entry in the dictionary.
     * - DirectedlyImplemented: Directly implemented component, i.e., entries directly declared by TIE_Bind, BOA_Bind,
     * etc.
     * - InDirectedlyImplemented: Indirectly implemented component, i.e., entries indirectly implemented through
     * inheritance and extension relationships, such as a component class implementing an interface class whose parent
     * class implements the interface.
     * - Blank: No priority specified.
     *
     * The smaller the value of the priority, the higher the priority.
     *
     * @note Higher-priority entries will override lower-priority entries. Therefore, it is necessary to avoid competing
     * entries with the same priority.
     */
    enum Priority : uint8_t {
      SETTLED = 1,
      DIRECTLY_IMPLEMENTED = 2,
      INDIRECTLY_IMPLEMENTED = 3,
      BLANK = 4,
    };

    UniqueString componentName;       ///< 组件名称 (Component name)
    UniqueString interfaceName;       ///< 接口名称 (Interface name)
    UniqueString libraryName;         ///< 库名称 (Library name)
    UniqueString precondFuncName;     ///< 前置条件函数名称 (Precondition function name)
    UniqueString precondFuncLibName;  ///< 前置条件函数库名称 (Precondition function library name)
    UniqueString frameworkName;       ///< 框架名称 (Framework name)
    UniqueString dictionaryName;      ///< 字典名称 (Dictionary name)

    void* creationFunc = nullptr;   ///< 组件创建函数指针 (Component creation function pointer)
    void* conditionFunc = nullptr;  ///< 条件检查函数指针 (Condition check function pointer)

    Priority priority = BLANK;         ///< 优先级，数值越小优先级越高 (Priority, lower value means higher priority)
    RecordStatus status = NOT_LOADED;  ///< 加载状态 (Loading status)
  };

  /**
   *
   */
  struct FactoryRecord {
    UniqueString objectName;
    UniqueString factoryName;

    UniqueString frameworkName;
    UniqueString dictionaryName;
  };

  /**
   *
   */
  struct ApplicationRecord {
    UniqueString componentName;
    UniqueString interfaceName;
    UniqueString dataTypeName;

    UniqueString frameworkName;
    UniqueString dictionaryName;
  };

  /**
   * @brief
   * @~chinese
   * 字典管理器类，负责字典文件里接口组件表的读取和运行时接口组件表的维护。
   *
   * 字典管理器是单例类
   *
   * @~english
   * Dictionary manager class, responsible for reading and maintaining the interface component table in the dictionary
   * file and at runtime
   *
   * The dictionary manager is a singleton class
   */
  class CORE_EXPORT DictionaryManager {
   public:
    /**
     * @brief 获取字典管理器的单例实例
     */
    static DictionaryManager& GetInstance();

    /**
     * @brief
     * @~chinese
     * 字典文件是否已读取
     *
     * @~english
     * Whether the dictionary file has been read
     */
    static bool initialized;

    /**
     * @brief
     * @~chinese
     * 构造函数，会通过InitDictionary读取字典文件
     *
     * @~english
     * Constructor, will read the dictionary file through InitDictionary
     */
    DictionaryManager();

    /**
     * @brief
     * @~chinese
     * 析构函数
     *
     * @~english
     * Destructor
     */
    ~DictionaryManager();

    /**
     * @brief
     * @~chinese
     * 读取字典文件
     *
     * @param iPath 字典文件路径 (Dictionary file path)
     *
     * @~english
     * Read the dictionary file
     *
     * @param iPath Path to the dictionary file
     */
    bool ReadFromFile(const std::filesystem::path& iPath);

    /**
     * @brief
     * @~chinese
     * 从目录读取目录下所有字典文件（非递归）
     *
     * @param iPath 字典文件所在目录 (Directory where the dictionary files are located)
     *
     * @~english
     * Read all dictionary files in the directory (non-recursive)
     *
     * @param iPath Directory where the dictionary files are located
     */
    bool ReadFromDirectory(const std::filesystem::path& iPath);

    /**
     * @brief
     * @~chinese
     * 写入字典文件
     *
     * @param iPath 字典文件路径
     *
     * @~english
     * Write the dictionary file
     *
     * @param iPath Path to the dictionary file
     */
    bool WriteToFile(const std::filesystem::path& iPath) const;

    /**
     * @brief
     * @~chinese
     * 添加字符串到字符串池，保证相同的字符串只存在一份
     *
     * @param iStr 字符串 (String)
     *
     * @~english
     * Add a string to the string pool, ensuring that the same string only exists once
     *
     * @param iStr String
     */
    UniqueString AddString(const std::string& iStr);
    /**
     * @brief
     * @~chinese
     * 添加字符串到字符串池，保证相同的字符串只存在一份
     *
     * @param iStr 字符串 (String)
     *
     * @~english
     * Add a string to the string pool, ensuring that the same string only exists once
     *
     * @param iStr String
     */
    UniqueString AddString(const char* iStr);
    /**
     * @brief
     * @~chinese
     * 添加字符串到字符串池，保证相同的字符串只存在一份（由于传入的是std::string_view，所以会复制一份）
     *
     * @param iStr 字符串 (String)
     *
     * @~english
     * Add a string to the string pool, ensuring that the same string only exists once (since std::string_view is passed
     * in, a copy will be made)
     *
     * @param iStr String
     */
    UniqueString AddString(const std::string_view& iStr);  // will copy the string

    /**
     * @brief
     * @~chinese
     * 添加组件接口记录
     *
     * @param iRecord 组件接口记录 (Component interface record)
     *
     * @~english
     * Add component interface record
     *
     * @param iRecord Component interface record
     */
    CompIntfRecord* AddCompIntfRecord(const CompIntfRecord& iRecord);

    /**
     * @brief
     * @~chinese
     * 获取组件接口记录
     *
     * @param iCompIntfNames 组件接口名称 (Component interface name)
     *
     * @~english
     * Get component interface record
     *
     * @param iCompIntfNames Component interface name
     */
    [[nodiscard]] CompIntfRecord* GetCompIntfRecord(const PairedName& iCompIntfNames) const;

    /**
     * @brief
     */
    FactoryRecord& AddFactoryRecord(const FactoryRecord& iRecord);

    /**
     * @brief
     */
    const std::vector<FactoryRecord>& GetAllFactoryRecords() const;

    /**
     * @brief
     */
    ApplicationRecord& AddApplicationRecord(const ApplicationRecord& iRecord);

    /**
     * @brief
     */
    const std::vector<ApplicationRecord>& GetAllApplicationRecords() const;

    /**
     * @brief
     * @~chinese
     * 把库的组件接口记录列表中的组件接口记录状态全部设置为Unreachable（内部使用）
     *
     * @param iLibName 库名称 (Library name)
     *
     * @~english
     * Set the status of component interface records in the list of component interface records in the library to
     * Unreachable
     *
     * @param iLibName Library name
     */
    bool MarkLibraryAsUnreachable(const std::string_view& iLibName);

    /**
     * @brief
     * @~chinese
     * 添加元对象类到元对象列表中，元对象列表全局唯一
     *
     * @param iName 类名称
     * @param iMeta 类对应的元对象实例
     *
     * @~english
     * Add a meta object class to the meta object list, which is globally unique
     *
     * @param iName Class name
     * @param iMeta Meta object instance corresponding to the class
     */
    bool AddMetaClass(const std::string_view& iName, MetaClass* iMeta);

    /**
     * @brief
     * @~chinese
     * 查找类的元对象类，如果不存在则创建一个并返回，因此除非内存满，该接口始终能返回非nullptr值。
     *
     * @param iName 类名称 (Class name)
     *
     * @~english
     * Find meta object class, return NULL if it does not exist
     *
     * @param iName Class name
     */
    MetaClass* GetMetaClass(const std::string_view& iName);

    /**
     * @brief
     * @~chinese
     * 查找类的元对象类，如果不存在则返回nullptr值。
     *
     * @param iName 类名称 (Class name)
     *
     * @~english
     * Find meta object class, return NULL if it does not exist
     */
    [[nodiscard]] MetaClass* FindMetaClass(const std::string_view& iName) const;

    // todo: temp solution, too bad
    bool GetAllComponents(std::string_view iIntfName, std::vector<MetaClass*>& oAllCompsOfIntf);

   protected:
    DISABLE_COPY_AND_MOVE_AND_ASSIGN_OPERATOR(DictionaryManager)

    /**
     * @brief
     * @~chinese
     * 初始化字典，即从字典文件中读取
     */
    void InitDictionary();

    std::unordered_set<std::string> _stringPool;  // only
    std::unordered_map<std::string_view, MetaClass*> _metaClasses;
    std::unordered_map<PairedName, CompIntfRecord*, PairedNameHasher> _compIntfRecords;
    std::unordered_map<std::string_view, std::vector<CompIntfRecord*>> _libraryRecords;
    std::vector<FactoryRecord> _factoryRecords;
    std::vector<ApplicationRecord> _applicationRecords;

    // framework names
    // library name -> framework
    std::unordered_set<std::string_view> _fwNames;
    std::unordered_map<std::string_view, std::string_view> _libFwName;

    //
    // for debugging
    //
   public:
    void AddMetaClassForTie(MetaClass* iMeta);
    std::unordered_set<MetaClass*> _tieMetas;
  };

  [[nodiscard]] CORE_EXPORT DictionaryManager& GetDictionaryManager();
  extern "C" CORE_EXPORT CompIntfRecord* GetCompIntfRecord(const PairedName& iCompIntfName);
  extern "C" CORE_EXPORT MetaClass* GetMetaClass(const char* iClassName);

}  // namespace DE
