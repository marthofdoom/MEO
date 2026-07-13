Scriptname MEO_MCM extends MCM_ConfigBase
; marth's Enchanting Overhaul — MCM registration shim.
;
; Deliberately empty. MCM Helper renders every control from
; Data/MCM/Config/MEO/config.json and persists each ModSetting to
; Data/MCM/Settings/MEO.ini. The MEO SKSE plugin reads that INI at load
; and re-reads it live whenever a menu closes, so no Papyrus logic lives
; here. This subclass exists only so SkyUI/MCM Helper have a concrete
; MCM_ConfigBase-derived quest script to register (the proven pattern in
; this load order — e.g. HighlightQuestMarkers_MCM ships an identical
; zero-logic stub).
