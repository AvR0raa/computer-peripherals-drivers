"""一次性迁移：protocol_db/db.json → protocol_db/brands/<slug>.json（一品牌一文件）。"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DB = ROOT / "protocol_db"
OLD = DB / "db.json"
BRAND_DIR = DB / "brands"

BRAND_SLUGS = {
    "Logitech": "logitech", "Razer": "razer", "SteelSeries": "steelseries",
    "Corsair": "corsair", "ASUS": "asus", "HyperX": "hyperx", "Creative": "creative",
    "MSI": "msi", "Cooler Master": "coolermaster", "Redragon": "redragon",
    "CHERRY": "cherry", "Dareu": "dareu", "MCHOSE": "mchose", "ATK": "atk",
    "attack shark": "attackshark", "A4Tech": "a4tech", "Microsoft": "microsoft",
    "Apple": "apple", "Keychron": "keychron", "Holtek (OEM)": "holtek",
    "Sennheiser": "sennheiser", "Plantronics/Poly": "plantronics", "Sharkoon": "sharkoon",
    "ITE": "internal-ite", "Elan": "internal-elan", "Windows VHF": "internal-vhf",
}

db = json.loads(OLD.read_text(encoding="utf-8"))
BRAND_DIR.mkdir(exist_ok=True)

# 品牌 VID → 归属文件
vid_files: dict[str, str] = {}
device_files: dict[str, str] = {}
for vid, brand in db["brands"].items():
    slug = BRAND_SLUGS.get(brand.get("name"), re.sub(r"\W+", "-", brand.get("name", "x")).lower())
    vid_files[vid] = slug
for vidpid, entry in db["devices"].items():
    slug = BRAND_SLUGS.get(entry.get("brand"), "unknown")
    device_files[vidpid] = slug

# 按文件聚合
files: dict[str, dict] = {}
for vid, slug in vid_files.items():
    files.setdefault(slug, {"brands": {}, "devices": {}})["brands"][vid] = db["brands"][vid]
for vidpid, slug in device_files.items():
    files.setdefault(slug, {"brands": {}, "devices": {}})["devices"][vidpid] = db["devices"][vidpid]

for slug, data in files.items():
    data["_comment"] = (f"品牌文件（一品牌一文件）。$schema=protocol-db/v2；"
                        f"合并加载由 core.fingerprint.load_db 完成。")
    data["$schema"] = "protocol-db/v2"
    (BRAND_DIR / f"{slug}.json").write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

# pending + 杂项 → _meta.json
meta = {"pending_brands": db.get("pending_brands", []),
        "_comment": db.get("_comment", "")}
(BRAND_DIR / "_meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n",
                                      encoding="utf-8")
OLD.rename(DB / "db.json.legacy")
print(f"迁移完成: {len(files)} 个品牌文件, {len(db['devices'])} 台设备")
for f in sorted(BRAND_DIR.glob("*.json")):
    print(" ", f.name)
