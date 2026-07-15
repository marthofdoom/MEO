Scriptname MEO_MCM extends MCM_ConfigBase
; marth's Enchanting Overhaul — MCM registration shim.
;
; MCM Helper renders every control from Data/MCM/Config/MEO/config.json and
; persists each ModSetting to Data/MCM/Settings/MEO.ini. The MEO SKSE plugin
; reads that INI at load and re-reads it live whenever a menu closes, so no
; tuning logic lives here. This subclass exists so SkyUI/MCM Helper have a
; concrete MCM_ConfigBase-derived quest script to register.
;
; The one bit of logic: the Debug page's read-only "DLL version" readout. It is
; bound (in config.json) to the sDLLVersion:Debug string ModSetting, which
; OnConfigOpen refreshes from the DLL's own GetDLLVersion() native every time the
; menu opens — so it always shows the ACTUALLY LOADED MEO.dll. Pushing it live on
; each open (rather than baking it into config.json) means a stale/mismatched DLL
; under a freshly-updated config surfaces here instead of hiding.

; Implemented in MEO.dll (registered in SKSEPluginLoad -> RegisterPapyrus).
String Function GetDLLVersion() Global Native

Event OnConfigOpen()
	SetModSettingString("sDLLVersion:Debug", GetDLLVersion())
EndEvent
