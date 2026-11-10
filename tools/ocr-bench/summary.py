"""Tables of what the OCR benchmark read: the error rate (wrong characters among those wanted), the lines read
exactly, and the lines where nothing was read, by language, style, size and font. With a second file of results, the
same for it beside them, and how many lines read better or worse than there.
	python3 tools/ocr-bench/summary.py <results.tsv> [earlier results.tsv] [--fonts] [--worse]
--fonts adds the table by font, --worse lists the lines that read worse than in the earlier results."""

import sys
from collections import defaultdict


def load(path):
	rows = {}
	with open(path, encoding="utf-8") as lines:
		for line in lines:
			f = line.rstrip("\n").split("\t")
			if len(f) >= 9:
				rows[(f[0], f[7])] = {"picture": f[0], "lang": f[1], "font": f[2], "style": f[3], "size": int(f[4]), "error": float(f[5]), "want": f[7], "got": f[8]}
	return rows


def figures(rows):
	count = len(rows)
	return (
		f"errors {sum(r['error'] for r in rows) / count:6.1%}  exact {sum(r['error'] == 0 for r in rows) / count:6.1%}"
		f"  nothing {sum(r['got'] == '<nothing>' for r in rows) / count:6.1%}"
	)


def table(title, rows, key, earlier):
	print(title)
	groups = defaultdict(list)
	for at, row in rows.items():
		groups[key(row)].append(at)
	for name in sorted(groups):
		line = f"  {name!s:30} {len(groups[name]):5} lines  {figures([rows[at] for at in groups[name]])}"
		both = [at for at in groups[name] if at in earlier]
		if both:
			better = sum(rows[at]["error"] < earlier[at]["error"] for at in both)
			worse = sum(rows[at]["error"] > earlier[at]["error"] for at in both)
			line += f"   | earlier: {figures([earlier[at] for at in both])}   better {better:4}  worse {worse:4}"
		print(line)


def main():
	files = [a for a in sys.argv[1:] if not a.startswith("--")]
	if not files:
		sys.exit(__doc__)
	rows = load(files[0])
	earlier = load(files[1]) if len(files) > 1 else {}
	table("ALL", rows, lambda r: "all", earlier)
	table("LANGUAGE", rows, lambda r: r["lang"], earlier)
	table("STYLE", rows, lambda r: r["style"], earlier)
	table("SIZE (plain style)", {at: r for at, r in rows.items() if r["style"] == "plain"}, lambda r: r["size"], earlier)
	if "--fonts" in sys.argv:
		table("FONT", rows, lambda r: r["lang"] + ":" + r["font"], earlier)
	if "--worse" in sys.argv:
		for at, row in sorted(rows.items()):
			if at in earlier and row["error"] > earlier[at]["error"]:
				print(f"  WORSE {row['picture']}: {earlier[at]['error']:.2f} -> {row['error']:.2f}  wanted «{row['want']}», was «{earlier[at]['got']}», now «{row['got']}»")


main()
