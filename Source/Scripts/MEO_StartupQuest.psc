Scriptname MEO_StartupQuest extends Quest
{P0 startup: grant the Gem Pouch power to the player once.}

Actor Property PlayerRef Auto
Spell  Property GemPouchPower Auto

Event OnInit()
    Grant()
EndEvent

Function Grant()
    If PlayerRef && GemPouchPower && !PlayerRef.HasSpell(GemPouchPower)
        PlayerRef.AddSpell(GemPouchPower, False)
        Debug.Notification("MEO P0: Gem Pouch power added (Powers menu).")
    EndIf
EndFunction
