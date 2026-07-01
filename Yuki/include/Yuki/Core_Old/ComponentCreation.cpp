
#include "ComponentCreation.h"

#include "BaseUnknown.h"
#include "DEDataType.h"
#include "DEICreateInstance.h"

#include "gin/runtime/module_info.h"

#include "MacrosForExternalObjectCreation.h"

namespace DE {

  CORE_EXPORT SystemStatus DECreateClassInstances(
      const std::string_view iClassName, const std::string_view iIntfName, BaseUnknown*& oInstance
  ) {
    /* initialisation */
    oInstance = nullptr;

    auto* record = GetCompIntfRecord({iClassName, DEICreateInstance::Name});
    if (record && !record->creationFunc && !record->libraryName.empty()) {
      os::load_module(record->libraryName);
    }
    if (!record || !record->creationFunc) {
      return StatusCode::Err_NOINTERFACE;
    }

    BaseUnknown* intf = static_cast<CreationFunc>(record->creationFunc)(nullptr);
    auto* createInstance = dynamic_cast<DEICreateInstance*>(intf);
    if (!createInstance) {
      M_ASSERT(!intf);
      return StatusCode::Err_NOINTERFACE;
    }

    SystemStatus hr = createInstance->CreateInstance(oInstance);
    createInstance->Release();
    if (!oInstance || !IS_SUCCEEDED(hr)) {
      return StatusCode::Err_UNEXPECTED;
    }

    hr = oInstance->QueryInterface(iIntfName, oInstance);
    oInstance->Release();

    if (!oInstance) {
      return StatusCode::Err_NOINTERFACE;
    }

    return hr;
  }

}  // namespace DE