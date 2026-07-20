# RUC 맵매칭 시뮬레이터 (RawGpsSimSvr)

여러 대의 차량이 실제 도로(표준 노드·링크)를 따라 운행하는 상황을 모사하여,
**도로에서 약간 벗어난(노이즈가 섞인) 원시 GPS 좌표**를 약 1초 간격으로 생성하고
**3초마다** `matching.raw_gps_log` 테이블에 일괄 INSERT 하는 **C++ 데몬**입니다.

맵매칭 알고리즘(`MapMatchSvr`) 검증용 입력 데이터를 만드는 것이 목적입니다.

```
[차량 N대 · 1초 tick]
  network.moct_link 형상 → 경로 진행점 계산 → 도로 이탈 노이즈(가우시안) → WGS-84
        └ 메모리 버퍼 적재
[flush_sec(기본 3초)마다]
  버퍼 → matching.raw_gps_log 트랜잭션 일괄 INSERT (MATCH_STATUS=PENDING, TRIP_ID=NULL)
        └ 이후 MapMatchSvr 가 PENDING 행을 읽어 맵매칭
```

---

## 1. 디렉토리 구성

```
Simulator/
├── src/                    # C++ 소스 + Makefile
│   ├── (재사용) TypeDefine.h, log4z.*, Mutex.*, Condition.*,
│   │            IniReader.*, SQLAccessor.*, PostgrePool.*      ← MapMatchSvr/src 에서 복사
│   ├── Config.h SimTypes.h
│   ├── GeoUtil.*           # WKT 파싱, 거리/방위, 미터 오프셋
│   ├── RouteProvider.*     # 도로망 링크 조회(시작/연결) + SRID 자동감지
│   ├── Vehicle.*           # 차량 1대 주행/노이즈/운행이벤트
│   ├── SimServer.*         # 1초 tick 루프 + 3초 배치 INSERT
│   ├── AppMain.cpp         # 데몬 main (config/log/signal)
│   └── Makefile
├── bin/                    # 실행 산출물
│   ├── RawGpsSimSvr        # 컴파일된 데몬 (make install 시 생성)
│   ├── config.ini          # 환경설정
│   ├── query.sql           # SQL 문 모음
│   ├── run_svr.sh / kill_svr.sh / ps_svr.sh
│   └── log/
└── db/                     # DB 환경 구축 (다른 PC 재현용)
    ├── create_sim.sql      # matching 스키마 + raw_gps_log + 주석 + 권한
    └── setup.py            # 생성/점검 스크립트
```

---

## 2. 사전 조건

- PostgreSQL 18 + PostGIS, 그리고 `roadnet` DB 에 도로망(`network.moct_link`)이 적재되어 있어야 합니다.
  (없으면 `../roadnet/scripts/roadnet.sh` 로 먼저 생성/적재)
- 접속 계정: **`mytobcom` / `my664761`**
- 빌드 도구: `g++`(C++11), `libpq`(`/usr/pgsql-18`)

---

## 3. DB 환경 구축 (다른 PC 에서도 동일하게)

`matching` 스키마와 `matching.raw_gps_log` 테이블을 생성하고 `mytobcom` 권한을 부여합니다.

```bash
# 로컬(peer 인증, postgres 슈퍼유저, sudo 사용)
python3 db/setup.py

# 원격/TCP 접속
python3 db/setup.py --host localhost --port 5432 --super-user postgres

# 생성 후 mytobcom 접속 및 도로망 SRID/INSERT 권한까지 점검
python3 db/setup.py --check
```

수동 실행도 가능합니다.

```bash
sudo -u postgres psql -d roadnet -f db/create_sim.sql
```

> 좌표계 주의: 도로망 geom 은 적재 방식에 따라 EPSG:4326 또는 5179 일 수 있습니다.
> 시뮬레이터는 기동 시 SRID 를 자동 감지하고, 항상 WGS-84(4326)로 변환해 사용합니다.

---

## 4. 빌드

```bash
cd src
make install     # 컴파일 후 실행파일을 ../bin/ 으로 이동
```

---

## 5. 실행 / 종료 / 상태

```bash
cd bin
./run_svr.sh     # nohup 데몬 기동
./ps_svr.sh      # 상태 확인
./kill_svr.sh    # SIGTERM → 잔여 버퍼 flush 후 정상 종료
```

로그: `bin/log/RawGpsSimSvr_*.log`

---

## 6. 설정 (`bin/config.ini`)

| 섹션 | 키 | 설명 |
|------|----|------|
| `database` | host/port/name/userid/password | DB 접속 (기본 `roadnet` / `mytobcom`) |
| `sim` | `vehicles` | 동시 운행 차량 수 |
| `sim` | `flush_sec` | DB INSERT 주기(초) — 1초 tick 으로 모아 N초마다 적재 |
| `sim` | `max_samples` | 총 GPS 생성 상한 (0=무제한). 도달 시 flush 후 자동 종료 |
| `sim` | `idle_prob` | tick 당 정차(IDLE) 확률 |
| `area` | min/max lon·lat | 주행 영역(WGS-84 bbox). 기본: 서울 일부 |
| `route` | min_m / max_links | 1회 운행 경로 길이·링크 수 |
| `noise` | `sigma_m` / `max_m` | 도로 이탈 GPS 오차(가우시안) 표준편차·최대값 |
| `speed` | factor_min/max, default_max_kmh | 제한속도 대비 주행속도 비율 |

---

## 7. 동작 확인 (적재 결과 조회)

```sql
-- 최근 적재 건수/차량 수
SELECT match_status, count(*), count(DISTINCT device_key) AS devices
FROM matching.raw_gps_log GROUP BY 1;

-- 차량별 최근 좌표
SELECT device_key, trip_event, drive_status, lat, lon, speed_kmh, accuracy_m, gps_dt
FROM matching.raw_gps_log
ORDER BY gps_seq DESC LIMIT 20;
```

- 차량 인증키(`device_key`)는 실행마다 겹치지 않게 `SIM{8hex}{4자리}` 형식으로 발급됩니다.
- `trip_id` 는 NULL(맵매칭 후 위치검증서버가 채움), `match_status` 는 `PENDING` 으로 적재됩니다.
- 좌표는 실제 도로점에 평균 `sigma_m` 수준의 노이즈가 더해져 **도로 위에 정확히 놓이지 않습니다.**
