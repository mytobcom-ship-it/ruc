# roadnet — PostgreSQL 도로 네트워크 DB

표준 노드·링크(MOCT) Shapefile을 PostgreSQL/PostGIS에 적재하기 위한 스키마·스크립트·문서입니다.

## 구조

```
roadnet/
├── sql/
│   └── create.sql          # DB + 스키마 + 테이블 + 권한
├── scripts/
│   ├── roadnet.sh          # setup / import / all (통합)
│   └── import.py           # Python import (Shapefile + DBF)
├── docs/
│   └── SCHEMA.md
└── requirements.txt
```

## DB·스키마

```
roadnet
└── network
    ├── moct_node
    ├── moct_link
    ├── multilink
    └── turn_info
```

## 빠른 시작

```bash
# 전체 (DB 생성 + 데이터 import)
bash roadnet/scripts/roadnet.sh

# 단계별
bash roadnet/scripts/roadnet.sh setup
bash roadnet/scripts/roadnet.sh import

# Python만 사용 (ogr2ogr 없을 때, 기본 fallback)
FORCE_PYTHON=1 bash roadnet/scripts/roadnet.sh import
```

의존 패키지:

```bash
pip install -r roadnet/requirements.txt
```

## 데이터 원본

| 파일 | 테이블 |
|------|--------|
| `data/MOCT_NODE.shp` | `network.moct_node` |
| `data/MOCT_LINK.shp` | `network.moct_link` |
| `data/MULTILINK.dbf` | `network.multilink` |
| `TURNINFO.dbf` | `network.turn_info` |

좌표계: **EPSG:5179** (GRS80 UTM-K)

상세 스키마: **[docs/SCHEMA.md](docs/SCHEMA.md)**
