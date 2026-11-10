"""Draws the OCR benchmark's pictures: lines of Japanese, Russian, Korean and Greek in many fonts, at sizes from 10 to
96 px and in the styles games and videos use (outlined, shadowed, low contrast, over a busy scene), with what each
line says. Needs Pillow, NumPy and fontconfig (fc-query, fc-match).
	python3 tools/ocr-bench/gen.py <fonts directory> <pictures directory>
Writes the pictures, truth.tsv (picture, the line's box, language, font, style, size, text) and quick.tsv (a fifth of
it, for trying a change)."""

import os
import subprocess
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS, OUT = sys.argv[1], sys.argv[2]
# As large as the area the daemon reads around the pointer at 100% scaling.
W, H = 1056, 480
MAX_LINE = 760

SENTENCES = {
	"ja": [
		"今日はいい天気ですね。", "図書館で本を借りました。", "彼女は毎朝コーヒーを飲む。", "冒険者よ、武器を選べ！",
		"セーブデータを読み込みますか？", "夕焼けがとても綺麗だった。", "新しい仲間が加わりました。", "この道をまっすぐ進んでください。",
		"魔王を倒すために旅に出る。", "食べ物を残してはいけません。", "電車が遅れているようです。", "約束の時間に間に合わない。",
	],
	"ru": [
		"Сегодня очень хорошая погода.", "Он взял книгу в библиотеке.", "Выберите оружие и начните игру.",
		"Сохранить изменения перед выходом?", "Новый спутник присоединился к отряду.", "Мы должны идти дальше на север.",
		"Эта дорога ведёт к старому замку.", "Спасибо за вашу помощь, друзья.",
	],
	"ko": [
		"오늘은 날씨가 정말 좋네요.", "도서관에서 책을 빌렸습니다.", "새로운 동료가 합류했습니다.", "무기를 선택하고 모험을 떠나세요.",
		"저장된 게임을 불러오시겠습니까?", "이 길을 따라 곧장 가세요.",
	],
	"el": [
		"Σήμερα έχει πολύ καλό καιρό.", "Πήρε ένα βιβλίο από τη βιβλιοθήκη.", "Διάλεξε όπλο και ξεκίνα το παιχνίδι.",
		"Ο δρόμος οδηγεί στο παλιό κάστρο.",
	],
}

# Ordinary fonts of the system, to tell a change that helps odd fonts from one that harms plain ones: the name in the
# results, what fontconfig knows the font as, its languages, and a pixel font's own size (0: not one). Those not
# installed are left out.
SYSTEM = [
	("NotoSansJP", "Noto Sans CJK JP:style=Regular", "ja", 0),
	("NotoSansJP-Bold", "Noto Sans CJK JP:style=Bold", "ja", 0),
	("NotoSansJP-Thin", "Noto Sans CJK JP:style=Thin", "ja", 0),
	("NotoSerifJP", "Noto Serif CJK JP:style=Regular", "ja", 0),
	("NotoSerifJP-Black", "Noto Serif CJK JP:style=Black", "ja", 0),
	("NotoSansKR", "Noto Sans CJK KR:style=Regular", "ko", 0),
	("NotoSerifKR", "Noto Serif CJK KR:style=Regular", "ko", 0),
	("DejaVuSans", "DejaVu Sans:style=Book", "ru el", 0),
	("LiberationSerif", "Liberation Serif:style=Regular", "ru el", 0),
	("ComicSans", "Comic Sans MS:style=Regular", "ru el", 0),
	("Impact", "Impact:style=Regular", "ru el", 0),
	("CourierNew", "Courier New:style=Regular", "ru el", 0),
	("Z003", "Z003", "ru", 0),
	("Monocraft", "Monocraft", "ru", 8),
]
# Pixel fonts among the downloaded ones, and the size they are drawn at before being enlarged without smoothing.
PIXEL = {"DotGothic16-Regular": 16, "PressStart2P-Regular": 8}

SIZES = [10, 12, 14, 16, 20, 24, 32, 48, 64, 96]
STYLES = ["dark", "outline", "shadow", "lowcontrast", "panel", "colour"]
STYLE_SIZES = [20, 36]
FILL = {
	"plain": (30, 30, 30), "dark": (230, 230, 230), "lowcontrast": (165, 165, 165), "colour": (255, 214, 80),
	"shadow": (255, 255, 255), "panel": (240, 240, 240), "outline": (255, 255, 255),
}


def system_font(pattern):
	"""The file and face fontconfig has for a font, or nothing when it would hand out another one instead."""
	found = subprocess.run(["fc-match", "-f", "%{family}|%{file}|%{index}", pattern], capture_output=True, text=True).stdout
	family, _, rest = found.partition("|")
	path, _, index = rest.rpartition("|")
	return (path, int(index)) if pattern.split(":")[0] in family.split(",") else None


def charset(path):
	out = subprocess.run(["fc-query", "--format", "%{charset}\n", path], capture_output=True, text=True).stdout
	ranges = []
	for part in out.split():
		a, _, b = part.partition("-")
		ranges.append((int(a, 16), int(b or a, 16)))
	return ranges


def covers(ranges, text):
	return all(any(a <= ord(c) <= b for a, b in ranges) for c in text if c != " ")


def busy(seed):
	"""A scene of blurred colours, as behind a game's text."""
	rng = np.random.default_rng(seed)
	coarse = Image.fromarray(rng.integers(30, 226, (H // 60 + 2, W // 60 + 2, 3), dtype=np.uint8)).resize((W, H), Image.BICUBIC)
	fine = Image.fromarray(rng.integers(0, 256, (H // 10 + 2, W // 10 + 2, 3), dtype=np.uint8)).resize((W, H), Image.BICUBIC)
	return Image.blend(coarse, fine, 0.25)


def background(style, seed):
	if style == "plain":
		return Image.new("RGB", (W, H), (245, 245, 245))
	if style == "dark":
		return Image.new("RGB", (W, H), (24, 26, 31))
	if style == "lowcontrast":
		return Image.new("RGB", (W, H), (120, 120, 120))
	if style == "colour":
		return Image.new("RGB", (W, H), (88, 28, 28))
	if style == "shadow":
		ramp = np.linspace(0.0, 1.0, H)[:, None, None]
		top, bottom = np.array([40, 70, 140]), np.array([90, 140, 200])
		return Image.fromarray(np.broadcast_to(top + (bottom - top) * ramp, (H, W, 3)).astype(np.uint8))
	scene = busy(seed)
	# A text box over the scene, as visual novels have.
	return Image.blend(scene, Image.new("RGB", (W, H), (10, 10, 20)), 0.65) if style == "panel" else scene


def mask_of(text, path, index, size, native):
	"""The text's coverage; a pixel font is drawn at its own size and enlarged without smoothing."""
	scale = size // native if native and size % native == 0 else 1
	font = ImageFont.truetype(path, size // scale, index=index)
	left, top, right, bottom = font.getbbox(text)
	image = Image.new("L", (right - left + 4, bottom - top + 4), 0)
	draw = ImageDraw.Draw(image)
	if native:
		draw.fontmode = "1"
	draw.text((2 - left, 2 - top), text, font=font, fill=255)
	return image.resize((image.width * scale, image.height * scale), Image.NEAREST) if scale > 1 else image


def fit(text, lang, path, index, size, native):
	"""The text, cut short at a word (a character in Japanese) until it fits the line; its end mark is kept."""
	mark, body = text[-1], text[:-1]
	while True:
		mask = mask_of(body + mark, path, index, size, native)
		if mask.width <= MAX_LINE or len(body) <= 2:
			return body + mark, mask
		body = body[:-1] if lang == "ja" or " " not in body else body.rsplit(" ", 1)[0]


def paint(canvas, mask, x, y, style, size):
	if style == "outline":
		reach = max(1, size // 12)
		canvas.paste((0, 0, 0), (x, y), mask.filter(ImageFilter.MaxFilter(2 * reach + 1)))
	if style == "shadow":
		offset = max(1, size // 14)
		canvas.paste((10, 10, 30), (x + offset, y + offset), mask)
	canvas.paste(FILL[style], (x, y), mask)


def fonts():
	"""Name: file, face, languages, own size and the characters it has (None: all that its languages need)."""
	found = {}
	for name, pattern, langs, native in SYSTEM:
		if at := system_font(pattern):
			found[name] = (*at, langs, native, None)
	for folder, _, names in sorted(os.walk(FONTS)):
		for name in sorted(names):
			path = os.path.join(folder, name)
			ranges = charset(path)
			langs = [lang for lang, lines in SENTENCES.items() if sum(covers(ranges, line) for line in lines) >= 3]
			# A Japanese or Korean font's Cyrillic and Greek are full-width extras nobody sets text in.
			langs = ["ja"] if "ja" in langs else ["ko"] if "ko" in langs else langs
			key = os.path.splitext(name)[0]
			if langs:
				found[key] = (path, 0, " ".join(langs), PIXEL.get(key, 0), ranges)
	return found


def main():
	os.makedirs(OUT, exist_ok=True)
	rows = []
	pictures = 0
	for key, (path, index, langs, native, ranges) in fonts().items():
		for lang in langs.split():
			lines = [s for s in SENTENCES[lang] if ranges is None or covers(ranges, s)]
			jobs = [("plain", size) for size in SIZES] + [(style, size) for style in STYLES for size in STYLE_SIZES]
			for style, size in jobs:
				if native and size < native:
					continue
				name = f"{key}_{style}_{size}_{lang}.png"
				seed = zlib.crc32(name.encode())
				canvas = background(style, seed)
				# Far enough apart that no line is taken for the next one's continuation.
				count = 3 if size <= 36 else 2 if size <= 64 else 1
				pitch = H // count
				for row in range(count):
					text, mask = fit(lines[(seed + row) % len(lines)], lang, path, index, size, native)
					box = mask.getbbox()
					if box is None:
						continue
					x, y = 40, row * pitch + (pitch - mask.height) // 2
					paint(canvas, mask, x, y, style, size)
					rows.append((seed, [name, x + box[0], y + box[1], box[2] - box[0], box[3] - box[1], lang, key, style, size, text]))
				canvas.save(os.path.join(OUT, name), compress_level=1)
				pictures += 1
	for file, keep in (("truth.tsv", lambda seed: True), ("quick.tsv", lambda seed: seed % 5 == 0)):
		with open(os.path.join(OUT, file), "w", encoding="utf-8") as out:
			out.writelines("\t".join(map(str, row)) + "\n" for seed, row in rows if keep(seed))
	print(f"{pictures} pictures, {len(rows)} lines in {len({row[6] for _, row in rows})} fonts")


main()
