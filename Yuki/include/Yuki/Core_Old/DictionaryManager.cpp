#include "DictionaryManager.h"

#include "MetaClass.h"
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/schema.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <filesystem>
#include <fstream>
#include <gin/io/terminal.h>
#include <gin/runtime/module_info.h>
#include <gin/utils/environment.h>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>

namespace DE {

  uint64_t PairedNameHasher::operator()(const PairedName& iPairedName) const {
    uint64_t i = 1, j = 1, c1 = 0, c2 = 0;
    for (const auto ch : iPairedName.first) {
      c1 += i++ * ch;
    }
    for (const auto ch : iPairedName.second) {
      c2 += j++ * ch;
    }
    return c1 ^ c2;
  }

  namespace {

    bool DeserializeDictionaryJsonFile(
        rapidjson::Document& iDoc, const std::string& iDocName, DictionaryManager& ioDictMgr
    ) {
      // TODO: add some terminal outputs for debugging

      // [dict.json example]
      //
      // {
      //   "type" : "dictionary",
      //   "items" : [ {
      //     "component" : "",
      //     "bindings" : [ {
      //       "interface" : "",
      //       "library" : "",
      //       "function" : "",
      //       "priority" : 0,
      //       "conditions" : [ {"function" : "", "library" : ""} ]
      //     } ]
      //   } ],
      //   "framework" : ""
      // }

      const auto& docNameView = ioDictMgr.AddString(iDocName);

      using namespace rapidjson;

      if (!iDoc.IsObject()) {
        M_ASSERT_UNREACHABLE();
        return false;
      }

      // 1. check required: type
      if (!iDoc.HasMember("type") || !iDoc["type"].IsString() || ustring(iDoc["type"].GetString()) != "dictionary") {
        M_ASSERT_UNREACHABLE();
        return false;
      }

      //    check required: items
      if (!iDoc.HasMember("items")) {
        return true;
      }

      if (!iDoc["items"].IsArray()) {
        return false;
      }

      //    check required: framework
      std::string_view framework = ioDictMgr.AddString("");
      if (iDoc.HasMember("framework")) {
        const auto& frameworkVal = iDoc["framework"];
        if (frameworkVal.IsString() && frameworkVal.GetStringLength()) {
          framework = ioDictMgr.AddString(frameworkVal.GetString());
        }
      }

      // 2. items
      const Value& items = iDoc["items"];
      for (SizeType i = 0; i < items.Size(); ++i) {
        auto& item = items[i];
        if (!item.IsObject() || !item.HasMember("component") || !item.HasMember("bindings")) {
          continue;
        }

        //  2.1 required: component
        auto& component = item["component"];
        if (!component.GetStringLength() || !component.GetString()) {
          continue;
        }

        //  2.2 required: bindings
        auto& bindings = item["bindings"];
        if (!bindings.IsArray()) {
          continue;
        }

        // [bindings example]
        //
        // "bindings": [
        //   {
        //     "interface": "",
        //     "library": "",
        //     "function": "",
        //     "priority": 0,
        //     "conditions": [
        //       {
        //         "function": "",
        //         "library": ""
        //       }
        //     ]
        //   }
        // ]

        for (SizeType j = 0; j < bindings.Size(); ++j) {
          // 2.2.1 check required: interface
          auto& binding = bindings[j];
          if (!binding.IsObject() || !binding.HasMember("interface")) {
            continue;
          }

          auto& interface = binding["interface"];
          if (!interface.IsString() || !interface.GetStringLength()) {
            continue;
          }

          // 2.2.2 framework, interface, dictionary, component
          CompIntfRecord record;
          record.componentName = ioDictMgr.AddString(component.GetString());
          record.interfaceName = ioDictMgr.AddString(interface.GetString());
          record.dictionaryName = docNameView;
          record.frameworkName = framework;
          record.conditionFunc = nullptr;
          record.creationFunc = nullptr;

          // 2.2.3 preconditions
          BEGIN_BREAKABLE_BLOCK(preconditions) {
            if (!binding.HasMember("conditions")) {
              break;
            }

            const auto& preconds = binding["conditions"];
            if (!preconds.IsArray()) {
              break;
            }

            // [conditions example]
            //
            // "conditions": [
            //   {
            //     "function": "",
            //     "library": ""
            //   }
            // ]

            const auto& precondsArr = preconds.GetArray();
            if (precondsArr.Size() > 1) {
              M_ASSERT_UNREACHABLE_MSG("TODO: we currently only support one precondition function");
            }
            for (SizeType k = 0; k < precondsArr.Size(); ++k) {
              if (k > 0) {
                M_ASSERT_UNREACHABLE_MSG("TODO: we currently only support one precondition function");
                break;
              }
              const auto& precond = precondsArr[k];
              // 2.2.3.1 function
              if (!precond.IsObject() || !precond.HasMember("function")) {
                continue;
              }
              const auto& precondFunc = precond["function"];
              if (!precondFunc.IsString() || !precondFunc.GetStringLength()) {
                continue;
              }
              record.precondFuncName = ioDictMgr.AddString(precondFunc.GetString());

              // 2.2.3.2 library
              if (!precond.HasMember("library")) {
                // TODO: in this situation, we should be sure that this precond function already exists in the current
                // runtime dictionary, so actually we can load them now
                continue;
              }
              const auto& precondFuncLib = precond["library"];
              if (!precondFuncLib.IsString() || !precondFuncLib.GetStringLength()) {
                continue;
              }
              record.precondFuncLibName = ioDictMgr.AddString(precondFuncLib.GetString());
            }
          }
          END_BREAKABLE_BLOCK(preconditions)

          // 2.2.4 library
          BEGIN_BREAKABLE_BLOCK(library) {
            bool belongToDefaultModules = !binding.HasMember("library");
            if (!belongToDefaultModules) {
              auto& library = binding["library"];
              if (!library.IsString()) {
                M_ASSERT_UNREACHABLE();
              }
              if (!library.GetStringLength()) {
                belongToDefaultModules = true;
              }

              if (!belongToDefaultModules) {
                record.libraryName = ioDictMgr.AddString(library.GetString());
              }
            }
            if (belongToDefaultModules) {
              // TODO: in this situation, we should be sure that this record already exists in the current runtime
              // dictionary, so actually we can load them now
            }
          }
          END_BREAKABLE_BLOCK(library)

          // 2.2.5 finally, add the record
          ioDictMgr.AddCompIntfRecord(record);
        }
      }

      return true;
    }

    bool DeserializeFactoryJsonFile(
        rapidjson::Document& iDoc, const std::string& iDocName, DictionaryManager& ioDictMgr
    ) {
      // [dict.json example]
      //
      // {
      //   "type" : "factory-spec",
      //   "items" : [
      //     {
      //       "object" : "",
      //       "factory": "",
      //     }
      //   ],
      //   "framework" : ""
      // }

      const auto& docNameView = ioDictMgr.AddString(iDocName);

      using namespace rapidjson;

      if (!iDoc.IsObject()) {
        M_ASSERT_UNREACHABLE();
        return false;
      }

      // 1. check required: type
      if (!iDoc.HasMember("type") || !iDoc["type"].IsString() || ustring(iDoc["type"].GetString()) != "factory-spec") {
        M_ASSERT_UNREACHABLE();
        return false;
      }

      //    check required: items
      if (!iDoc.HasMember("items")) {
        return true;
      }

      if (!iDoc["items"].IsArray()) {
        return false;
      }

      //    check required: framework
      std::string_view framework = ioDictMgr.AddString("");
      if (iDoc.HasMember("framework")) {
        const auto& frameworkVal = iDoc["framework"];
        if (frameworkVal.IsString() && frameworkVal.GetStringLength()) {
          framework = ioDictMgr.AddString(frameworkVal.GetString());
        }
      }

      // 2. items
      const Value& items = iDoc["items"];
      for (SizeType i = 0; i < items.Size(); ++i) {
        auto& item = items[i];
        if (!item.IsObject() || !item.HasMember("factory") || !item.HasMember("object")) {
          continue;
        }

        auto& object = item["object"];
        if (!object.IsString() || !object.GetStringLength()) {
          continue;
        }
        const auto objectName = ioDictMgr.AddString(object.GetString());

        auto& factory = item["factory"];
        if (!factory.IsString() || !factory.GetStringLength()) {
          continue;
        }
        const auto factoryName = ioDictMgr.AddString(factory.GetString());

        FactoryRecord record{
          .objectName = objectName,
          .factoryName = factoryName,
          .frameworkName = framework,
          .dictionaryName = docNameView,
        };

        ioDictMgr.AddFactoryRecord(record);
      }

      return true;
    }

    bool DeserializeApplicationJsonFile(
        rapidjson::Document& iDoc, const std::string& iDocName, DictionaryManager& ioDictMgr
    ) {
      // [dict.json example]
      //
      // {
      //   "type" : "application-spec",
      //   "items" : [
      //     {
      //       "interface" : "",
      //       "component": "",
      //       "data-type": "", // DocumentType, ReadableContainer, None
      //     }
      //   ],
      //   "framework" : ""
      // }

      const auto& docNameView = ioDictMgr.AddString(iDocName);

      using namespace rapidjson;

      if (!iDoc.IsObject()) {
        M_ASSERT_UNREACHABLE();
        return false;
      }

      // 1. check required: type
      if (!iDoc.HasMember("type") || !iDoc["type"].IsString()
          || ustring(iDoc["type"].GetString()) != "application-spec") {
        M_ASSERT_UNREACHABLE();
        return false;
      }

      //    check required: items
      if (!iDoc.HasMember("items")) {
        return true;
      }

      if (!iDoc["items"].IsArray()) {
        return false;
      }

      //    check required: framework
      std::string_view framework = ioDictMgr.AddString("");
      if (iDoc.HasMember("framework")) {
        const auto& frameworkVal = iDoc["framework"];
        if (frameworkVal.IsString() && frameworkVal.GetStringLength()) {
          framework = ioDictMgr.AddString(frameworkVal.GetString());
        }
      }

      // 2. items
      const Value& items = iDoc["items"];
      for (SizeType i = 0; i < items.Size(); ++i) {
        auto& item = items[i];
        if (!item.IsObject() || !item.HasMember("interface") || !item.HasMember("component")) {
          continue;
        }

        auto& component = item["component"];
        if (!component.IsString() || !component.GetStringLength()) {
          continue;
        }
        const auto componentName = ioDictMgr.AddString(component.GetString());

        auto& interface = item["interface"];
        if (!interface.IsString() || !interface.GetStringLength()) {
          continue;
        }
        const auto interfaceName = ioDictMgr.AddString(interface.GetString());

        auto& dataType = item["data-type"];
        if (!dataType.IsString() || !dataType.GetStringLength()) {
          continue;
        }
        const auto dataTypeName = ioDictMgr.AddString(dataType.GetString());

        ApplicationRecord record{
          .componentName = componentName,
          .interfaceName = interfaceName,
          .dataTypeName = dataTypeName,
          .frameworkName = framework,
          .dictionaryName = docNameView,
        };

        ioDictMgr.AddApplicationRecord(record);
      }

      return true;
    }

    bool GetFileContent(const std::filesystem::path& iPath, std::string& oContent) {
      const std::ifstream file(iPath, std::ios::in);
      if (!file.is_open()) {
        return false;
      }
      std::stringstream ss;
      ss << file.rdbuf();
      oContent = ss.str();
      return true;
    }

    // these implementations need to be improved with a better string class
    bool LoadDictionaryFile(const std::filesystem::path& iDictFilePath, DictionaryManager& ioDictMgr) {
      // step 1: read the dictionary file of JSON format
      std::string dictText;
      if (!GetFileContent(iDictFilePath, dictText)) {
        return false;
      }

      using namespace std::filesystem;
      using namespace rapidjson;

      try {
        // step 2: parse the file and dispatch it to different deserializing function
        Document dictDoc;

        dictDoc.Parse(dictText.c_str());
        if (dictDoc.HasParseError()) {
          std::println(
              std::cerr, "JSON Parse Error: {}, {}", dictDoc.GetErrorOffset(), static_cast<int>(dictDoc.GetParseError())
          );
          return false;
        }
        if (!dictDoc.HasMember("type") || !dictDoc["type"].IsString()) {
          std::println("JSON is not a dictionary file: {}", iDictFilePath.string());
          return false;
        }

        using DeserializationFunc = decltype(&DeserializeDictionaryJsonFile);
        DeserializationFunc deserializationFunc = nullptr;

        const std::string type = dictDoc["type"].GetString();
        if (type == "dictionary") {
          deserializationFunc = &DeserializeDictionaryJsonFile;
        }
        else if (type == "factory-spec") {
          deserializationFunc = &DeserializeFactoryJsonFile;
        }
        else if (type == "application-spec") {
          deserializationFunc = &DeserializeApplicationJsonFile;
        }
        else {
          std::println(std::cout, "Unknown dictionary type ({}): {}", iDictFilePath.string(), type);
          return false;
        }

        // step 3: apply schema
        auto schemaPath = env::get_path(env::global$dictionary$schemas_directory, path("dict_schema.json"));
        std::string schema;
        bool hasSchema = GetFileContent(schemaPath, schema);

        auto dictName = iDictFilePath.filename().string();

        if (!hasSchema) {
          return deserializationFunc(dictDoc, dictName, ioDictMgr);
        }

        Document schemaTextDoc;
        if (!schemaTextDoc.Parse(schema.c_str()).HasParseError()) {
          std::println("Schema JSON Parse Error");
          return deserializationFunc(dictDoc, dictName, ioDictMgr);
        }

        SchemaDocument schemaDoc(schemaTextDoc);
        if (SchemaValidator schemaValidator(schemaDoc); !dictDoc.Accept(schemaValidator)) {
          std::println("Schema validation failed");

          StringBuffer buf;

          buf.Clear();
          schemaValidator.GetInvalidSchemaPointer().StringifyUriFragment(buf);
          std::println("  Invalid schema pointer: {}", buf.GetString());
          std::println("  Invalid key word: {}", schemaValidator.GetInvalidSchemaKeyword());

          buf.Clear();
          schemaValidator.GetInvalidDocumentPointer().StringifyUriFragment(buf);
          std::println("  Invalid input location: {}", buf.GetString());

#ifdef COMPILER_MSVC
          buf.Clear();
          PrettyWriter writer(buf);
          schemaValidator.GetError().Accept(writer);
          std::println("  Details: {}", buf.GetString());
#endif
          return false;
        }

        return deserializationFunc(dictDoc, dictName, ioDictMgr);
      } catch (...) {
        M_ASSERT_UNREACHABLE();
      }
    }

  }  // namespace

  DictionaryManager& DictionaryManager::GetInstance() {
    static DictionaryManager s_manager;
    return s_manager;
  }

  bool DictionaryManager::initialized = false;

  DictionaryManager::DictionaryManager() {
    InitDictionary();
  }

  DictionaryManager::~DictionaryManager() = default;

  void DictionaryManager::InitDictionary() {
    if (const auto dir = env::get_path("global.dictionaryDirectory"); !!dir) {
      ReadFromDirectory(dir.value());
    }

    initialized = true;

    // after initializing dictionaries, generally we have already known all
    // ClassID's and names of classes except TIEs
  }

  bool DictionaryManager::ReadFromFile(const std::filesystem::path& iPath) {
    return LoadDictionaryFile(iPath, *this);
  }

  bool DictionaryManager::ReadFromDirectory(const std::filesystem::path& iPath) {
    bool res = true;
    if (!exists(iPath)) {
      std::println("Dictionary path not exists: {}", iPath.generic_string());
      return false;
    }
    for (const auto& entry : fs::directory_iterator(iPath)) {
      const auto& path = entry.path();
      std::cout << path << "\n";
      if (is_regular_file(path) && path.extension() == ".json") {
        std::println("Read dictionary file: {}", path.string());
        if (!ReadFromFile(path)) {
          res = false;
        }
      }
      else {
        // todo
        std::println("Skipped dictionary {}", path.string());
      }
    }
    return res;
  }

  bool DictionaryManager::WriteToFile(const std::filesystem::path& iPath) const {
    M_ASSERT_TODO_MSG("Write to JSON");
    return true;
  }

  std::string_view DictionaryManager::AddString(const std::string& iStr) {
    const auto& [ele, _] = _stringPool.insert(iStr);
    return *ele;
  }

  std::string_view DictionaryManager::AddString(const std::string_view& iStr) {
    const auto& [ele, _] = _stringPool.insert(std::string{iStr.cbegin(), iStr.cend()});
    return *ele;
  }

  std::string_view DictionaryManager::AddString(const char* iStr) {
    M_ASSERT(iStr);
    const auto& [ele, _] = _stringPool.insert(std::string{iStr});
    return *ele;
  }

  CompIntfRecord* DictionaryManager::AddCompIntfRecord(const CompIntfRecord& iRecord) {
    // 1. test if the record already exists, insert directly if not
    PairedName namePair = {iRecord.componentName, iRecord.interfaceName};
    const auto it = _compIntfRecords.find(namePair);
    if (it == _compIntfRecords.cend()) {
      auto newRecord = new CompIntfRecord(iRecord);

      newRecord->componentName = namePair.first = AddString(namePair.first);
      newRecord->interfaceName = namePair.second = AddString(namePair.second);
      newRecord->libraryName = AddString(iRecord.libraryName);
      newRecord->dictionaryName = AddString(iRecord.dictionaryName);
      newRecord->frameworkName = AddString(iRecord.frameworkName);
      newRecord->precondFuncLibName = AddString(iRecord.precondFuncLibName);
      newRecord->precondFuncName = AddString(iRecord.precondFuncName);

      _compIntfRecords.insert({namePair, newRecord});

#if PLATFORM_WINDOWS && (defined(DEBUG) || defined(_DEBUG))
      std::string symbolName;
      os::module_handle module_info;
      auto ret = os::get_module_symbol_info(newRecord->creationFunc, symbolName, module_info);
      if (!symbolName.empty() && ret == os::Error::Ok) {
        terminal::set_foreground(terminal::foreground_color::BrightBlue);
        std::print("Appending ");
        terminal::reset();
        std::print("record for ");
        terminal::set_foreground(terminal::foreground_color::BrightBlue);

        std::print("[{}, {}]\n", iRecord.componentName, iRecord.interfaceName);
        terminal::reset();
        std::print("    to ");
        terminal::set_foreground(terminal::foreground_color::BrightYellow);

        std::println("{}", symbolName);
        terminal::reset();
      }
#endif
      return newRecord;
    }

    // 2. update record
    CompIntfRecord* record = it->second;
    const bool hasCreationFunc = iRecord.creationFunc;
    const bool notSettled = iRecord.priority != CompIntfRecord::Priority::SETTLED;
    const bool isMorePrior = iRecord.priority <= record->priority;
    if (!record->creationFunc || (hasCreationFunc && notSettled && isMorePrior)) {
#if PLATFORM_WINDOWS && (defined(DEBUG) || defined(_DEBUG))
      // auto oldSymbolName = GetAddressFunctionName(record->creationFunc);
      std::string oldSymbolName;
      os::module_handle old_module_info;
      os::get_module_symbol_info(record->creationFunc, oldSymbolName, old_module_info);
      // auto newSymbolName = GetAddressFunctionName(iRecord.creationFunc);
      std::string newSymbolName;
      os::module_handle new_module_info;
      os::get_module_symbol_info(record->creationFunc, newSymbolName, new_module_info);

      if (!oldSymbolName.empty() && !newSymbolName.empty()) {
        terminal::set_foreground(terminal::foreground_color::BrightRed);
        std::print("Replacing ");
        terminal::reset();
        std::print("record for ");
        terminal::set_foreground(terminal::foreground_color::BrightBlue);

        std::print("[{}, {}]\n", iRecord.componentName, iRecord.interfaceName);
        terminal::reset();
        std::print("    from ");
        terminal::set_foreground(terminal::foreground_color::Green);

        std::print("{}\n", oldSymbolName);
        terminal::reset();
        std::print("    to ");
        terminal::set_foreground(terminal::foreground_color::BrightYellow);

        std::print("{}\n", newSymbolName);
        terminal::reset();
      }
      else if (!newSymbolName.empty()) {
        terminal::set_foreground(terminal::foreground_color::BrightGreen);

        std::print("Filling ");
        terminal::reset();
        std::print("record for ");
        terminal::set_foreground(terminal::foreground_color::BrightBlue);

        std::print("[{}, {}]\n", iRecord.componentName, iRecord.interfaceName);
        terminal::reset();
        std::print("    to ");
        terminal::set_foreground(terminal::foreground_color::BrightYellow);
        std::println("{}", newSymbolName);
        terminal::reset();
      }
#endif
      record->creationFunc = iRecord.creationFunc;
    }
    const bool hasConditionFunc = !!iRecord.conditionFunc;
    if (!record->conditionFunc || (hasConditionFunc && notSettled && isMorePrior)) {
      record->conditionFunc = iRecord.conditionFunc;
    }
    if (isMorePrior) {
      record->priority = iRecord.priority;
    }
    record->status = iRecord.status;

    // loaded record must provide the creator function
    M_ASSERT(record->status == CompIntfRecord::RecordStatus::LOADED ? !!record->creationFunc : true);
    // PS: library will not be updated since it is uniquely dependent on the dictionary context. This runtime has no
    // permission to modify it.

    return record;
  }

  CompIntfRecord* DictionaryManager::GetCompIntfRecord(const PairedName& iCompIntfNames) const {
    if (const auto it = _compIntfRecords.find(iCompIntfNames); it != _compIntfRecords.cend()) {
      return it->second;
    }
    return nullptr;
  }

  FactoryRecord& DictionaryManager::AddFactoryRecord(const FactoryRecord& iRecord) {
    return _factoryRecords.emplace_back(iRecord);
  }

  const std::vector<FactoryRecord>& DictionaryManager::GetAllFactoryRecords() const {
    return _factoryRecords;
  }

  ApplicationRecord& DictionaryManager::AddApplicationRecord(const ApplicationRecord& iRecord) {
    return _applicationRecords.emplace_back(iRecord);
  }

  const std::vector<ApplicationRecord>& DictionaryManager::GetAllApplicationRecords() const {
    return _applicationRecords;
  }

  bool DictionaryManager::MarkLibraryAsUnreachable(const std::string_view& iLibName) {
    const auto it = _libraryRecords.find(iLibName);
    if (it == _libraryRecords.cend()) {
      return true;
    }

    for (const auto record : it->second) {
      M_ASSERT(
          !record->creationFunc && !record->conditionFunc && record->status != CompIntfRecord::RecordStatus::LOADED
      );
      record->status = CompIntfRecord::RecordStatus::UNREACHABLE;
    }
    return true;
  }

  bool DictionaryManager::AddMetaClass(const std::string_view& iName, MetaClass* iMeta) {
    auto [it, res] = _metaClasses.insert({iName, iMeta});
    if (res) {
      const CompIntfRecord record = {
        .componentName = iName,
        .interfaceName = MetaClass::Name,
        .creationFunc = static_cast<void*>(iMeta),
        .conditionFunc = nullptr,
        .priority = CompIntfRecord::Priority::SETTLED,
        .status = CompIntfRecord::RecordStatus::LOADED,
      };
      AddCompIntfRecord(record);
    }
    return res;
  }

  MetaClass* DictionaryManager::GetMetaClass(const std::string_view& iName) {
    auto& meta = _metaClasses[iName];
    if (!meta) {
      meta = new MetaClass(iName, nullptr, TypeOfClass::NothingType);
    }

    return meta;
  }

  MetaClass* DictionaryManager::FindMetaClass(const std::string_view& iName) const {
    const auto meta = _metaClasses.find(iName);
    if (meta == _metaClasses.cend()) {
      return nullptr;
    }
    return meta->second;
  }

  void DictionaryManager::AddMetaClassForTie(MetaClass* iMeta) {
    _tieMetas.insert(iMeta);
  }

  bool DictionaryManager::GetAllComponents(std::string_view iIntfName, std::vector<MetaClass*>& oAllCompsOfIntf) {
    bool ret = false;
    for (const auto& pairedName : _compIntfRecords | std::views::keys) {
      const auto& [compName, intfName] = pairedName;
      if (intfName != iIntfName) {
        continue;
      }
      oAllCompsOfIntf.push_back(GetMetaClass(compName));
      ret = true;
    }
    return ret;
  }

  DictionaryManager& GetDictionaryManager() {
    return DictionaryManager::GetInstance();
  }

  extern "C" CompIntfRecord* GetCompIntfRecord(const PairedName& iCompIntfName) {
    return GetDictionaryManager().GetCompIntfRecord(iCompIntfName);
  }

  extern "C" MetaClass* GetMetaClass(const char* iClassName) {
    return GetDictionaryManager().GetMetaClass(iClassName);
  }

}  // namespace DE