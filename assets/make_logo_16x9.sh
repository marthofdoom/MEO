#!/usr/bin/env bash
# 16:9 logo for marth Enchanting Overhaul (1920x1080) — the banner's motif and
# house style (dark ground, amethyst glow, faceted gem, gold P052 type) recomposed
# for a 16:9 canvas: gem as a centered hero above stacked, centered typography.
# Use for splash / Nexus header / video thumbnail. Sibling of make_banner.sh.
set -euo pipefail
cd "$(dirname "$0")"
W=1920; H=1080
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# Brilliant-cut gem (side view, culet down), centered horizontally, upper third.
# Scaled ~1.8x the banner gem and re-centred on cx=960.
CX=960; TOP=250; GIRD=180; MIDY=330; BOT=560; HALFT=115; HALFG=170
L1="$((CX-HALFT)),${TOP}"        # table left
R1="$((CX+HALFT)),${TOP}"        # table right
L2="$((CX-HALFG)),${MIDY}"       # girdle left
R2="$((CX+HALFG)),${MIDY}"       # girdle right
CU="${CX},${BOT}"                # culet
OUTLINE="$L1 $R1 $R2 $CU $L2 $L1"
POLY="$L1 $R1 $R2 $CU $L2"

# 1) Base: near-black vertical gradient + soft amethyst radial glow (centered).
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#241a33-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The gem: blurred violet glow underlayer, translucent fill, faint inner
#    facets, crisp gold girdle + a bright table sparkle. Same recipe as the banner.
magick base.png \
  \( -clone 0 -fill none -stroke '#b98cff' -strokewidth 16 \
     -draw "polyline $OUTLINE" -blur 0x12 \) -compose screen -composite \
  -stroke none -fill 'rgba(150,110,210,0.22)' -draw "polygon $POLY" \
  -fill none -stroke '#6b5a86' -strokewidth 1.8 \
  -draw "line $L1 $CU  line $R1 $CU  line $L2 $R2  line ${CX},${TOP} $CU  line $L2 $CU  line $R2 $CU" \
  -stroke '#e8c87e' -strokewidth 3.6 -draw "polyline $OUTLINE" \
  -stroke '#c9a45c' -strokewidth 2.2 -draw "line $L1 $L2  line $R1 $R2" \
  -stroke none -fill '#f4dfa8' -draw "translate ${CX},${TOP} rotate 45 rectangle -7,-7 7,7" \
  gem.png

# 3) Typography — centered stack below the gem.
magick gem.png -gravity north \
  -font "$P052" \
  -fill '#9a8a5e' -pointsize 44 -kerning 22 -annotate +0+630 "m a r t h" \
  -fill '#eae1cb' -pointsize 128 -kerning 8 -annotate +0+688 "ENCHANTING" \
  -fill '#c9a45c' -pointsize 40 -kerning 34 -annotate +6+840 "O V E R H A U L" \
  -font "$P052I" -fill '#8d939e' -pointsize 30 -kerning 1 \
  -annotate +0+928 "socketable, leveling enchantment gems  —  gear that grows with you" \
  logo-16x9.png
rm -f base.png gem.png
echo "wrote logo-16x9.png (${W}x${H})"
