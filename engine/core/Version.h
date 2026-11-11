// LegacyAVR - product version (v1.6.0).
//
// Bump together with CMakeLists.txt (project VERSION), engine/app/AVREngine/AVREngine.rc,
// vcpkg.json, ui/LegacyAVR.UI/LegacyAVR.UI.csproj and app.manifest. The UI test
// VersionTests.EngineAndDashboardVersionsMatch fails if they drift apart.
//
// Used by the configuration file: experimental features are honoured only by the version that
// saved them, so every upgrade starts with them off (see ConfigStore.cpp).
#pragma once

namespace avr {

inline constexpr const char* kVersionString = "1.6.0";

} // namespace avr
