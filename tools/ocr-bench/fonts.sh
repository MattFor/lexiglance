#!/usr/bin/env bash
# Downloads the fonts the OCR benchmark draws its pictures in: pixel, handwritten, brush and display faces for
# Japanese, Cyrillic and Korean, from Google Fonts at a fixed commit (Open Font License). They are not kept in the
# repository. Fonts already there are left alone.
#   bash tools/ocr-bench/fonts.sh <directory>
set -euo pipefail

target="${1:?usage: fonts.sh <directory>}"
base=https://raw.githubusercontent.com/google/fonts/5bb28733cec313b63fbd415b574b5ae35c8aa8b8/ofl
fonts=(
    # Japanese
    dotgothic16/DotGothic16-Regular.ttf yomogi/Yomogi-Regular.ttf zenkurenaido/ZenKurenaido-Regular.ttf
    yuseimagic/YuseiMagic-Regular.ttf delagothicone/DelaGothicOne-Regular.ttf rocknrollone/RocknRollOne-Regular.ttf
    reggaeone/ReggaeOne-Regular.ttf pottaone/PottaOne-Regular.ttf hachimarupop/HachiMaruPop-Regular.ttf
    kaiseidecol/KaiseiDecol-Regular.ttf yujisyuku/YujiSyuku-Regular.ttf kleeone/KleeOne-Regular.ttf
    mochiypopone/MochiyPopOne-Regular.ttf stick/Stick-Regular.ttf rampartone/RampartOne-Regular.ttf
    trainone/TrainOne-Regular.ttf zenantique/ZenAntique-Regular.ttf kiwimaru/KiwiMaru-Regular.ttf
    # Cyrillic (Press Start 2P has Greek too)
    pressstart2p/PressStart2P-Regular.ttf lobster/Lobster-Regular.ttf marckscript/MarckScript-Regular.ttf
    badscript/BadScript-Regular.ttf rubikmonoone/RubikMonoOne-Regular.ttf ruslandisplay/RuslanDisplay-Regular.ttf
    russoone/RussoOne-Regular.ttf amaticsc/AmaticSC-Regular.ttf pangolin/Pangolin-Regular.ttf
    kellyslab/KellySlab-Regular.ttf
    # Korean
    nanumpenscript/NanumPenScript-Regular.ttf blackhansans/BlackHanSans-Regular.ttf gaegu/Gaegu-Regular.ttf
    jua/Jua-Regular.ttf dohyeon/DoHyeon-Regular.ttf gugi/Gugi-Regular.ttf singleday/SingleDay-Regular.ttf
    nanumbrushscript/NanumBrushScript-Regular.ttf eastseadokdo/EastSeaDokdo-Regular.ttf
    bagelfatone/BagelFatOne-Regular.ttf dongle/Dongle-Regular.ttf
)

mkdir -p "$target"
for font in "${fonts[@]}"; do
    file="$target/${font##*/}"
    if [ ! -s "$file" ]; then
        echo "downloading ${font##*/}"
        curl -fsSL --retry 3 -o "$file" "$base/$font" || {
            rm -f "$file"
            echo "not downloaded: $font" >&2
        }
    fi
done
