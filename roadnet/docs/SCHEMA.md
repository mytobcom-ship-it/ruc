# roadnet PostgreSQL 스키마 문서

표준 노드·링크(MOCT) Shapefile을 PostgreSQL/PostGIS에 적재하기 위한 DB·스키마·테이블 설계 문서입니다.

---

## 1. 데이터베이스 개요

| 항목 | 값 |
|------|-----|
| **DB명** | `roadnet` |
| **스키마** | `network` |
| **좌표계** | EPSG:5186 (GRS80 TM Central Belt 2010) |
| **원본 데이터** | `/data/MOCT_LINK.*`, `/data/MOCT_NODE.*`, `/data/MULTILINK.dbf`, `/TURNINFO.dbf` |
| **PostGIS** | 필수 (`CREATE EXTENSION postgis`) |

### ER 관계

```
network.moct_node (node_id)
       ↑                    ↑
       │ f_node             │ t_node
network.moct_link (link_id)
       ↑                    ↑
       │ link_id            │ st_link / ed_link
network.multilink          network.turn_info (node_id, turn_id)
```

---

## 2. 테이블: `network.moct_node`

**원본:** `MOCT_NODE.shp` + `MOCT_NODE.dbf`  
**레코드 수:** 1,178,457건  
**설명:** 도로 네트워크의 노드(교차점, IC, JC, 분기점 등)

| 컬럼명 | PostgreSQL 타입 | NULL | PK/FK | Shapefile 필드 | 설명 |
|--------|-----------------|------|-------|----------------|------|
| `node_id` | `varchar(10)` | NOT NULL | **PK** | `NODE_ID` | 노드 고유 ID (10자리). 링크의 `f_node`/`t_node`가 참조 |
| `node_type` | `varchar(3)` | YES | | `NODE_TYPE` | 노드 유형 코드 (아래 코드표 참조) |
| `node_name` | `varchar(50)` | YES | | `NODE_NAME` | 노드 명칭 (교차로명, IC명 등) |
| `turn_p` | `varchar(1)` | YES | | `TURN_P` | 회전 가능 여부 (`0`: 불가, `1`: 가능) |
| `updatedate` | `varchar(8)` | YES | | `UPDATEDATE` | 데이터 갱신일 (`YYYYMMDD`) |
| `remark` | `varchar(30)` | YES | | `REMARK` | 비고 |
| `hist_type` | `varchar(8)` | YES | | `HIST_TYPE` | 이력 유형 코드 (신규/변경/유지 등) |
| `histremark` | `varchar(30)` | YES | | `HISTREMARK` | 이력 관련 비고 |
| `geom` | `geometry(Point, 5186)` | YES | GIST | `.shp` | 노드 좌표 (EPSG:5186) |

### `node_type` 코드 (실제 데이터 기준)

| 코드 | 설명 | 비고 |
|------|------|------|
| `101` | 교차로 | 전체의 약 68% |
| `102` | JC (분기점) | |
| `103` | SA / 휴게소 | |
| `104` | IC (나들목) | |
| `105` | TG (톨게이트) | |
| `106` | 기타 | |
| `107` | 시·군도 노드 등 | |

---

## 3. 테이블: `network.moct_link`

**원본:** `MOCT_LINK.shp` + `MOCT_LINK.dbf`  
**레코드 수:** 1,555,150건  
**설명:** 도로 네트워크의 링크(도로 구간). 맵매칭·소통정보의 기본 단위

| 컬럼명 | PostgreSQL 타입 | NULL | PK/FK | Shapefile 필드 | 설명 |
|--------|-----------------|------|-------|----------------|------|
| `link_id` | `varchar(10)` | NOT NULL | **PK** | `LINK_ID` | 링크 고유 ID (10자리). 소통정보·맵매칭 조인 키 |
| `f_node` | `varchar(10)` | YES | **FK** → `moct_node.node_id` | `F_NODE` | 시작 노드 ID |
| `t_node` | `varchar(10)` | YES | **FK** → `moct_node.node_id` | `T_NODE` | 종료 노드 ID |
| `lanes` | `integer` | YES | | `LANES` | 차로 수 |
| `road_rank` | `varchar(3)` | YES | | `ROAD_RANK` | 도로 등급 코드 (아래 코드표 참조) |
| `road_type` | `varchar(3)` | YES | | `ROAD_TYPE` | 도로 유형 (`000`: 일반, `001`~`004`: 터널·교량 등) |
| `road_no` | `varchar(5)` | YES | | `ROAD_NO` | 대표 노선번호 (예: `32`, `5`) |
| `road_name` | `varchar(30)` | YES | | `ROAD_NAME` | 도로명 (EUC-KR) |
| `road_use` | `varchar(1)` | YES | | `ROAD_USE` | 도로 사용 구분 (`0`: 일반, `1`: 전용) |
| `multi_link` | `varchar(1)` | YES | | `MULTI_LINK` | 다중 노선 여부 (`0`: 단일, `1`: 복수 → `multilink` 참조) |
| `connect` | `varchar(3)` | YES | | `CONNECT` | 연결로 여부 (`0`: 아님, `1`: 연결로) |
| `max_spd` | `integer` | YES | | `MAX_SPD` | 최고 제한속도 (km/h) |
| `rest_veh` | `varchar(3)` | YES | | `REST_VEH` | 통행 제한 차량 코드 |
| `rest_w` | `integer` | YES | | `REST_W` | 요일별 통행 제한 코드 |
| `rest_h` | `integer` | YES | | `REST_H` | 시간대별 통행 제한 코드 |
| `c_its` | `varchar(1)` | YES | | `C-ITS` | C-ITS 구간 여부 |
| `length` | `numeric(18,12)` | YES | | `LENGTH` | 링크 길이 (m) |
| `updatedate` | `varchar(8)` | YES | | `UPDATEDATE` | 데이터 갱신일 (`YYYYMMDD`) |
| `remark` | `varchar(30)` | YES | | `REMARK` | 비고 |
| `hist_type` | `varchar(8)` | YES | | `HIST_TYPE` | 이력 유형 코드 |
| `histremark` | `varchar(30)` | YES | | `HISTREMARK` | 이력 관련 비고 |
| `geom` | `geometry(LineString, 5186)` | YES | GIST | `.shp` | 링크 형상 (EPSG:5186) |

### `road_rank` 코드 (RUCSvr DataFormat.h / 실제 데이터)

| 코드 | 설명 | 실제 건수(약) |
|------|------|--------------|
| `101` | 고속도로 | 9,258 |
| `102` | 도시고속화도로 | 977 |
| `103` | 일반국도 | 45,694 |
| `104` | 특별·광역시도 | 79,302 |
| `105` | 국가지원지방도 | 13,738 |
| `106` | 지방도 | 45,914 |
| `107` | 시·군도 | 305,117 |
| `108` | 기타 | (데이터에 없음) |

### `road_type` 코드

| 코드 | 설명 |
|------|------|
| `000` | 일반 도로 |
| `001` | 교량 |
| `002` | 터널 |
| `003` | 고가도로 |
| `004` | 지하차도 |

### `hist_type` 코드

| 코드 | 설명 |
|------|------|
| `LINK0001` | 신규 |
| `LINK0003` | 속성 변경 |
| `LINK0004` | 삭제 |
| `LINK0005` | 노드 변경 |
| `LINK0006` | 형상 변경 |
| `LINK1007` | 변경 없음 (유지) |

### `multi_link` 값

| 값 | 설명 | 건수 |
|----|------|------|
| `0` | 단일 노선 | 1,536,885 |
| `1` | 다중 노선 (→ `multilink` 테이블 참조) | 18,265 |

---

## 4. 테이블: `network.multilink`

**원본:** `MULTILINK.dbf` (geometry 없음)  
**레코드 수:** 18,916건 (고유 `link_id` 18,247개)  
**설명:** 한 링크 구간에 노선번호가 2개 이상 겹칠 때의 부가 노선 정보

| 컬럼명 | PostgreSQL 타입 | NULL | PK/FK | DBF 필드 | 설명 |
|--------|-----------------|------|-------|----------|------|
| `link_id` | `varchar(10)` | NOT NULL | **PK**, **FK** → `moct_link.link_id` | `LINK_ID` | 링크 ID |
| `multi_id` | `smallint` | NOT NULL | **PK** | `MULTI_ID` | 다중 노선 순번 (`1`, `2`, `3` …) |
| `road_rank` | `varchar(3)` | YES | | `ROAD_RANK` | 도로 등급 |
| `road_type` | `varchar(3)` | YES | | `ROAD_TYPE` | 도로 유형 |
| `road_no` | `varchar(5)` | YES | | `ROAD_NO` | 추가 노선번호 |
| `road_name` | `varchar(30)` | YES | | `ROAD_NAME` | 도로명 |
| `remark` | `varchar(30)` | YES | | `REMARK` | 비고 |

### 사용 예

`link_id = 2980021700` (충서로) — 동일 구간, 노선 2개:

| multi_id | road_no | road_name |
|----------|---------|-----------|
| 1 | 32 | 충서로 |
| 2 | 45 | 충서로 |

---

## 5. 테이블: `network.turn_info`

**원본:** `TURNINFO.dbf` (geometry 없음, 프로젝트 루트)  
**레코드 수:** 44,218건 (고유 `node_id` 40,420개)  
**설명:** 노드(교차점)에서 **진입 링크 → 진출 링크** 회전 허용/제한 정보

| 컬럼명 | PostgreSQL 타입 | NULL | PK/FK | DBF 필드 | 설명 |
|--------|-----------------|------|-------|----------|------|
| `node_id` | `varchar(10)` | NOT NULL | **PK** | `NODE_ID` | 회전이 발생하는 노드 ID (→ `moct_node`) |
| `turn_id` | `smallint` | NOT NULL | **PK** | `TURN_ID` | 노드 내 회전 정보 순번 |
| `st_link` | `varchar(10)` | NOT NULL | | `ST_LINK` | 진입(시작) 링크 ID (→ `moct_link`) |
| `ed_link` | `varchar(10)` | NOT NULL | | `ED_LINK` | 진출(종료) 링크 ID (→ `moct_link`) |
| `turn_type` | `varchar(3)` | YES | | `TURN_TYPE` | 회전 유형 코드 |
| `turn_oper` | `varchar(1)` | YES | | `TURN_OPER` | 회전 허용/제한 (`0`: 허용, `1`: 제한) |
| `remark` | `varchar(30)` | YES | | `REMARK` | 비고 |

### `turn_type` 코드 (실제 데이터)

| 코드 | 설명 | 건수(약) |
|------|------|----------|
| `011` | 직진 | 27,825 |
| `101` | 좌회전 | 13,077 |
| `103` | 유턴 | 2,282 |
| `102` | 우회전 | 1,008 |
| `001`~`003`, `012` | 기타 | 26 |

### `turn_oper` 코드

| 코드 | 설명 | 건수(약) |
|------|------|----------|
| `0` | 회전 허용 | 44,205 |
| `1` | 회전 제한 | 13 |

### 사용 예

노드 `1790007102`에서 링크 `1790687601` → `1790687501` 직진:

> **참고:** FK 제약은 두지 않음 (원본 44,218건 중 69건이 moct_link/node에 없는 ID 참조)

```sql
SELECT * FROM network.turn_info
WHERE node_id = '1790007102';
-- st_link=1790687601, ed_link=1790687501, turn_type=011, turn_oper=0
```

---

## 6. 인덱스

| 인덱스명 | 테이블 | 컬럼 | 유형 |
|----------|--------|------|------|
| `pk_moct_node` | `moct_node` | `node_id` | PRIMARY KEY |
| `pk_moct_link` | `moct_link` | `link_id` | PRIMARY KEY |
| `pk_multilink` | `multilink` | `(link_id, multi_id)` | PRIMARY KEY |
| `pk_turn_info` | `turn_info` | `(node_id, turn_id)` | PRIMARY KEY |
| `idx_moct_link_f_node` | `moct_link` | `f_node` | B-tree |
| `idx_moct_link_t_node` | `moct_link` | `t_node` | B-tree |
| `idx_moct_link_road_rank` | `moct_link` | `road_rank` | B-tree |
| `idx_moct_link_multi_link` | `moct_link` | `multi_link` | B-tree |
| `idx_moct_link_geom` | `moct_link` | `geom` | GIST |
| `idx_moct_node_geom` | `moct_node` | `geom` | GIST |
| `idx_multilink_link_id` | `multilink` | `link_id` | B-tree |
| `idx_turn_info_node_id` | `turn_info` | `node_id` | B-tree |
| `idx_turn_info_st_link` | `turn_info` | `st_link` | B-tree |
| `idx_turn_info_ed_link` | `turn_info` | `ed_link` | B-tree |
| `idx_turn_info_st_ed` | `turn_info` | `(st_link, ed_link)` | B-tree |

---

## 7. 설치 및 import

```bash
# 전체 (DB·스키마·테이블·권한 + 데이터 import)
bash roadnet/scripts/roadnet.sh

# 단계별
bash roadnet/scripts/roadnet.sh setup    # create.sql
bash roadnet/scripts/roadnet.sh import   # import.py

# Python import 직접 실행
pip install -r roadnet/requirements.txt
python3 roadnet/scripts/import.py --data-dir data --turninfo TURNINFO.dbf --user postgres
```

### import 순서 (`import.py` 내부)

1. `create.sql` — DB·스키마·테이블·`mytobcom` 권한 (`roadnet.sh setup`)
2. `moct_node`
3. `moct_link`
4. `multilink`
5. `turn_info`

---

## 8. Oracle NTIC 대응 관계

| PostgreSQL | Oracle (기존) |
|------------|---------------|
| `roadnet` (DB) | `NTIC` (서비스명) |
| `network.moct_link` | `NTIC_COM.T_COB_MOCT_LINK_M` |
| `network.moct_node` | `NTIC_COM.T_COB_MOCT_NODE_M` |
| `network.multilink` | (Oracle 별도 테이블 또는 VIEW) |
| `network.turn_info` | MOCT 회전 정보 (TURNINFO) |

---

## 9. 향후 확장

| 스키마 | 용도 | 상태 |
|--------|------|------|
| `network` | 표준 노드·링크 | ✅ 적재 완료 |
| `matching` | GPS·맵매칭 | ❌ 미포함 (`NTIC_ALQ.T_ALQ_*`, DDL 문서 대기) |
| `traffic` | 소통정보 | ❌ 미포함 (`NTIC_HSM.T_HSS_*`) |

---

## 10. 참고 쿼리

```sql
-- 링크 + 시작/종료 노드 조인
SELECT l.link_id, l.road_name, l.road_no,
       fn.node_id AS f_node_id, tn.node_id AS t_node_id,
       ST_AsText(l.geom) AS wkt
FROM network.moct_link l
JOIN network.moct_node fn ON l.f_node = fn.node_id
JOIN network.moct_node tn ON l.t_node = tn.node_id
LIMIT 10;

-- 다중 노선 링크
SELECT l.link_id, l.road_name, m.multi_id, m.road_no, m.road_name AS multi_road_name
FROM network.moct_link l
JOIN network.multilink m ON l.link_id = m.link_id
WHERE l.multi_link = '1'
LIMIT 10;

-- SRID 확인
SELECT ST_SRID(geom), COUNT(*) FROM network.moct_link GROUP BY 1;
```
