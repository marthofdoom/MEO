Scriptname MEO_PouchScript extends ActiveMagicEffect
{Gem Pouch power handler.

P1a: validates the data pipeline end to end -- the power fires, this script runs,
and JsonUtil reads Data/SKSE/Plugins/MEO/meo_runtime.json (the generated catalog).
The full catalog-driven socket menu (list gems, pick worn item, insert/remove,
marker decode/apply for any of the 50 gem types) lands in P1b.}

Actor       Property PlayerRef     Auto
FormList    Property AllGems       Auto   ; MEO_AllGems (every gem x level form)
String      Property RuntimePath   Auto   ; "MEO/meo_runtime.json" (JsonUtil root = Data/SKSE/Plugins)
MagicEffect Property MarkerContact Auto   ; weapon effects
MagicEffect Property MarkerSelf    Auto   ; armor effects

Event OnEffectStart(Actor akTarget, Actor akCaster)
    Int total = AllGems.GetSize()
    Int catalog = JsonUtil.GetPathIntValue(RuntimePath, "._meta.count")

    If catalog <= 0
        Debug.MessageBox("MEO P1a ERROR: could not read " + RuntimePath + \
            "\nCheck Data/SKSE/Plugins/MEO/meo_runtime.json is installed and PapyrusUtil is active.")
        Return
    EndIf

    ; Prove nested reads work: pull one gem's data by path.
    Int fireIdx = JsonUtil.GetPathIntValue(RuntimePath, ".fireDamage.type_index")
    Int fireV   = JsonUtil.PathIntElements(RuntimePath, ".fireDamage.curve")[4]

    Debug.MessageBox("MEO P1a pipeline OK\n" + \
        "Gem forms in plugin: " + total + "\n" + \
        "Catalog gems (JSON): " + catalog + "\n" + \
        "Fire gem type_index=" + fireIdx + ", Level V magnitude=" + fireV + "\n" + \
        "(Socket menu arrives in P1b.)")
EndEvent
