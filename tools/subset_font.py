"""地名に出てくる文字（JIS X 0208 の全部＝第 1・第 2 水準の漢字、かな、記号）に絞った太字フォントを作る。

    python tools/subset_font.py <NotoSansJP[wght].ttf> fonts/NotoSansJP-Bold-jis.ttf

観測点の名前（約 4400 か所）や震央地名は前もって全部はわからないので、JIS の文字を全部入れる。
元のフォント: https://github.com/google/fonts/tree/main/ofl/notosansjp （SIL OFL 1.1）
"""
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

src, out = sys.argv[1:3]
chars = set(chr(c) for c in range(0x20, 0x7F))
for hi in range(0xA1, 0xFF):          # EUC-JP の 2 バイト（JIS X 0208）を全部
    for lo in range(0xA1, 0xFF):
        try:
            chars.update(bytes([hi, lo]).decode("euc_jp"))
        except UnicodeDecodeError:
            pass
chars.update("〜－・：￥―＊※℃")
f = instancer.instantiateVariableFont(TTFont(src), {"wght": 700})
opt = subset.Options(); opt.layout_features = []; opt.notdef_outline = True; opt.name_IDs = ["*"]; opt.name_languages = ["*"]
opt.hinting = False
s = subset.Subsetter(opt); s.populate(text="".join(chars)); s.subset(f)
f.save(out)
print(len(chars), "chars ->", out)
