Scriptname MEO_MCM extends MCM_ConfigBase
; marth's Enchanting Overhaul — MCM registration shim.
;
; Deliberately empty. MCM Helper renders every control from
; Data/MCM/Config/MEO/config.json and persists each ModSetting to
; Data/MCM/Settings/MEO.ini. The MEO SKSE plugin reads that INI at load
; and re-reads it live whenever a menu closes, so no Papyrus logic lives
; here. This subclass exists only so SkyUI/MCM Helper have a concrete
; MCM_ConfigBase-derived quest script to register.
;
; The Debug page's "Version" readout is a build-stamped STATIC text value in
; config.json (generated from kMEOVersion in native/plugin.cpp) — MRO's style —
; so it renders with no ModSetting/Papyrus round-trip that could come up blank.
