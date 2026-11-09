#!/usr/bin/env python3
"""이미지 모델 독립 평가용 소규모 웹 이미지 수집 — Wikimedia Commons API(공개 라이선스)에서 클래스별 후보를 받는다.

받은 뒤 반드시 사람이 라벨을 검수(잘못된 장면 제거)하고 tools/image_eval.py로 평가한다.
출처·라이선스는 sources.csv에 기록한다. 학습 데이터(웹 수집)와 겹칠 가능성은 배제할 수 없으므로 문서에는 "웹 독립 세트(학습 세트와의 중복 미확인)"로 쓴다.

사용: python tools/fetch_eval_images.py [--out eval_images_web] [--per-query 4]
"""
import argparse
import csv
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

UA = "SmartGlassHA-eval/1.0 (student project; contact via GitHub issues)"
API = "https://commons.wikimedia.org/w/api.php"

QUERIES = {
    "conversation": ["two people talking face to face", "friends conversation cafe table", "elderly couple talking bench", "people chatting kitchen"],
    "danger": ["car approaching crosswalk pedestrian view", "bicycle rider close sidewalk", "electric scooter rider street close", "motorcycle passing pedestrian street"],
    "indoor": ["living room interior sofa", "home kitchen interior", "office interior desks", "cafe interior tables", "hospital corridor interior"],
    "negative": ["blurry out of focus photo", "concrete floor texture", "plain white wall", "ceiling tiles", "overexposed photo white", "dark underexposed photo"],
    "outdoor": ["park walking path trees", "city sidewalk pedestrians", "pedestrian crossing street", "bus stop street", "apartment buildings street"],
    "tv": ["watching television living room", "television screen living room sofa", "family watching tv", "tv on wall living room"],
}


def api(params):
    params = dict(params, format="json")
    url = API + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def search(query, limit):
    data = api({
        "action": "query", "generator": "search", "gsrsearch": query, "gsrnamespace": "6", "gsrlimit": str(limit * 3),
        "prop": "imageinfo", "iiprop": "url|extmetadata|mime", "iiurlwidth": "640",
    })
    out = []
    for page in (data.get("query", {}).get("pages", {}) or {}).values():
        ii = (page.get("imageinfo") or [{}])[0]
        mime = ii.get("mime", "")
        if mime not in ("image/jpeg", "image/png"):
            continue
        meta = ii.get("extmetadata", {}) or {}
        out.append({
            "title": page.get("title", ""), "thumb": ii.get("thumburl"), "page": ii.get("descriptionurl", ""),
            "license": (meta.get("LicenseShortName", {}) or {}).get("value", ""), "artist": (meta.get("Artist", {}) or {}).get("value", "")[:80],
        })
        if len(out) >= limit:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="eval_images_web")
    ap.add_argument("--per-query", type=int, default=4)
    args = ap.parse_args()
    root = Path(args.out)
    root.mkdir(exist_ok=True)
    rows = []
    total = 0
    for cls, qs in QUERIES.items():
        d = root / cls
        d.mkdir(exist_ok=True)
        n = 0
        for q in qs:
            try:
                items = search(q, args.per_query)
            except Exception as e:
                print(f"[{cls}] 검색 실패 '{q}': {e}")
                continue
            for it in items:
                if not it["thumb"]:
                    continue
                n += 1
                ext = ".png" if it["thumb"].lower().endswith(".png") else ".jpg"
                fn = d / f"{cls}_{n:02d}{ext}"
                try:
                    req = urllib.request.Request(it["thumb"], headers={"User-Agent": UA})
                    with urllib.request.urlopen(req, timeout=30) as r:
                        fn.write_bytes(r.read())
                except Exception as e:
                    print(f"[{cls}] 다운로드 실패 {it['thumb'][:60]}: {e}")
                    n -= 1
                    continue
                rows.append([cls, fn.name, q, it["title"], it["page"], it["license"], it["artist"]])
                total += 1
                time.sleep(0.3)
        print(f"[{cls}] {n}장")
    with open(root / "sources.csv", "w", encoding="utf-8-sig", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["class", "file", "query", "commons_title", "page_url", "license", "artist"])
        w.writerows(rows)
    print(f"총 {total}장 → {root}/ (sources.csv에 출처·라이선스 기록). 다음: 라벨 검수 후 python tools/image_eval.py {root} --app-rule")


if __name__ == "__main__":
    main()
