#!/usr/bin/env bash
# Nexus banner for marth Enchanting Overhaul (1300x372).
# Motif: a faceted enchantment gem with an arcane glow — socket, level, reforge.
# House style matches marth Resurgence Overhaul (dark ground, gold type, P052).
set -euo pipefail
W=1300; H=372
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# Brilliant-cut gem (side view, culet down), sat on the right of the banner.
OUTLINE="1015,108 1145,108 1197,158 1080,304 963,158 1015,108"
POLY="1015,108 1145,108 1197,158 1080,304 963,158"

# 1) Base: near-black vertical gradient + soft amethyst radial glow.
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#241a33-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The gem: a blurred violet glow underlayer, translucent fill, faint inner
#    facets, then a crisp gold girdle + a bright table sparkle.
magick base.png \
  \( -clone 0 -fill none -stroke '#b98cff' -strokewidth 11 \
     -draw "polyline $OUTLINE" -blur 0x8 \) -compose screen -composite \
  -stroke none -fill 'rgba(150,110,210,0.22)' -draw "polygon $POLY" \
  -fill none -stroke '#6b5a86' -strokewidth 1.4 \
  -draw "line 1015,108 1080,304  line 1145,108 1080,304  line 963,158 1197,158  line 1080,108 1080,304  line 963,158 1080,304  line 1197,158 1080,304" \
  -stroke '#e8c87e' -strokewidth 2.6 -draw "polyline $OUTLINE" \
  -stroke '#c9a45c' -strokewidth 1.6 -draw "line 1015,108 963,158  line 1145,108 1197,158" \
  -stroke none -fill '#f4dfa8' -draw "translate 1080,108 rotate 45 rectangle -5,-5 5,5" \
  gem.png

# 3) Typography (left-aligned so it clears the gem).
magick gem.png \
  -font "$P052" -gravity northwest \
  -fill '#9a8a5e' -pointsize 30 -kerning 14 -annotate +72+58 "m a r t h" \
  -fill '#eae1cb' -pointsize 84 -kerning 5 -annotate +68+92 "ENCHANTING" \
  -fill '#c9a45c' -pointsize 26 -kerning 21 -annotate +76+206 "O V E R H A U L" \
  -font "$P052I" -fill '#8d939e' -pointsize 22 -kerning 1 \
  -annotate +74+270 "socketable, leveling enchantment gems  —  gear that grows with you" \
  nexus-banner.png
rm -f base.png gem.png
echo "wrote nexus-banner.png (${W}x${H})"
