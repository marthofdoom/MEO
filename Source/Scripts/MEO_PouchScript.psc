Scriptname MEO_PouchScript extends ActiveMagicEffect
{P0 validation driver for the "item is the database" architecture.

Operates on the worn RIGHT-HAND weapon (WornObject handSlot 1, slotMask 0).
Each cast reads the current socket state back from the item's own runtime
enchantment (decoding the hidden marker effect), reports it, then advances:
  none -> Fire I -> II -> III -> IV -> V -> removed -> none ...
This exercises insert, level-up (rebuild), removal, and readback. Persistence
across save/load and re-equip is verified by casting, doing the console action,
and casting again to confirm the decoded level survived.}

Actor       Property PlayerRef  Auto
MagicEffect Property FireEffect Auto   ; vanilla EnchFireDamageFFContact
MagicEffect Property Marker     Auto   ; MEO_MarkerMGEF (inert; carries the code)
Form[]      Property GemForms   Auto   ; MEO_GemFire1..5 (wired, unused in P0)

Int   Property GemTypeFire = 1 Auto    ; marker code = GemTypeFire*8 + level
Float Property MaxCharge  = 5000.0 Auto

; P0 PLACEHOLDER magnitudes (index = level). The real curve anchors Level I to
; the Requiem base and Level V to the Requiem max per effect (DESIGN §3); these
; 5/10/15/20/25 values only exist to make the validation harness legible.
Float[] Function FireMags()
    Float[] m = new Float[6]
    m[0] = 0.0
    m[1] = 5.0
    m[2] = 10.0
    m[3] = 15.0
    m[4] = 20.0
    m[5] = 25.0
    Return m
EndFunction

Event OnEffectStart(Actor akTarget, Actor akCaster)
    RunStep()
EndEvent

; --- Decode: read the marker magnitude off the worn enchantment. 0 = no MEO gem.
Int Function DecodeLevel(Enchantment ench)
    If ench == None
        Return 0
    EndIf
    Int n = ench.GetNumEffects()
    Int i = 0
    While i < n
        If ench.GetNthEffectMagicEffect(i) == Marker
            Float mag = ench.GetNthEffectMagnitude(i)
            Int code = Math.Floor(mag + 0.5) as Int
            Return code - (GemTypeFire * 8)
        EndIf
        i += 1
    EndWhile
    Return 0
EndFunction

Function RunStep()
    Form wpn = PlayerRef.GetEquippedWeapon(False)   ; right hand
    If wpn == None
        Debug.MessageBox("MEO P0: equip a weapon in your RIGHT hand first.")
        Return
    EndIf

    Enchantment cur = WornObject.GetEnchantment(PlayerRef, 1, 0)
    Int level = DecodeLevel(cur)

    String action
    If level <= 0
        Apply(1)
        action = "INSERT -> Fire I"
    ElseIf level < 5
        Apply(level + 1)
        action = "LEVEL UP -> Fire " + RomanOf(level + 1)
    Else
        Remove(wpn)
        action = "REMOVE (socket emptied)"
    EndIf

    ; Read back AFTER the action to prove the item is the source of truth.
    Enchantment after = WornObject.GetEnchantment(PlayerRef, 1, 0)
    Int newLevel = DecodeLevel(after)
    Int effCount = 0
    Float fireMag = 0.0
    If after != None
        effCount = after.GetNumEffects()
        Int i = 0
        While i < effCount
            If after.GetNthEffectMagicEffect(i) == FireEffect
                fireMag = after.GetNthEffectMagnitude(i)
            EndIf
            i += 1
        EndWhile
    EndIf

    ; Rebuilding the enchantment under an already-equipped item does not
    ; refresh the live weapon display, effect magnitude, or name until the item
    ; is re-equipped. Force it so leveling/rename take effect immediately.
    RefreshWeapon(wpn)

    Debug.MessageBox("MEO P0 -- " + action + "\n" + \
        "Weapon: " + wpn.GetName() + "\n" + \
        "Decoded gem: " + LabelOf(newLevel) + "\n" + \
        "Fire magnitude: " + fireMag + "\n" + \
        "Enchant effect count: " + effCount)
EndFunction

Function RefreshWeapon(Form wpn)
    PlayerRef.UnequipItem(wpn, False, True)   ; abPreventEquip=False, abSilent=True
    PlayerRef.EquipItem(wpn, False, True)     ; abPreventRemoval=False, abSilent=True
EndFunction

Function Apply(Int level)
    Float[] fm = FireMags()
    MagicEffect[] effects = new MagicEffect[2]
    Float[] mags = new Float[2]
    Int[] areas  = new Int[2]
    Int[] durs   = new Int[2]

    effects[0] = FireEffect
    mags[0]    = fm[level]
    effects[1] = Marker
    mags[1]    = (GemTypeFire * 8 + level) as Float

    WornObject.CreateEnchantment(PlayerRef, 1, 0, MaxCharge, effects, mags, areas, durs)
    WornObject.SetDisplayName(PlayerRef, 1, 0, \
        PlayerRef.GetEquippedWeapon(False).GetName() + " [Fire " + RomanOf(level) + "]", True)
EndFunction

Function Remove(Form wpn)
    WornObject.SetEnchantment(PlayerRef, 1, 0, None, 0.0)
    WornObject.SetDisplayName(PlayerRef, 1, 0, wpn.GetName(), True)
EndFunction

String Function LabelOf(Int level)
    If level <= 0
        Return "(none)"
    EndIf
    Return "Fire " + RomanOf(level)
EndFunction

String Function RomanOf(Int level)
    If level == 1
        Return "I"
    ElseIf level == 2
        Return "II"
    ElseIf level == 3
        Return "III"
    ElseIf level == 4
        Return "IV"
    ElseIf level == 5
        Return "V"
    EndIf
    Return level as String
EndFunction
