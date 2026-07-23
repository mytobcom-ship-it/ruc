/**
 * RUC 맵매칭 GPS 시각화 (2026-07-10 최정우)
 *  - 주변 도로: prim_link_info (PSF WGS84, trip bbox + road_buffer_m)
 *  - 도로 진행방향: LineString 꼭짓점 순서(f_node → t_node) 화살표
 *  - GPS 빨강 / MATCHED 파랑 / SKIP 주황
 */
(function () {
  const MATCH_STATUS_PENDING = 0;
  const MATCH_STATUS_MATCHED = 1;
  const MATCH_STATUS_SKIP = 3;
  const MATCH_STATUS_ERROR = 4;
  // Simulator/src/SimTypes.h SIM_TRIP_EVENT_END — 운행의 마지막 샘플에만 붙는 이벤트.
  //   신규테스트 진행률 판정에서 "지금까지 들어온 행이 전부 매칭됨" 만으로 완료를 판단하면,
  //   Simulator 가 아직 나머지 데이터를 생성 중인데도(2초 간격 분할 삽입) 초반 몇 건만
  //   빠르게 매칭돼 조기에 "완료"로 오판하는 문제가 있었다 — END 이벤트가 실제로 들어와
  //   있는지까지 같이 확인해야 진짜 끝을 알 수 있다 (2026-07-22 최정우 추가)
  const TRIP_EVENT_END = 2;
  const RETEST_POLL_MS = 1500;
  const RETEST_TIMEOUT_MS = 60000;
  // 신규테스트: Simulator 가 GPS 를 생성(최대 수십 초)한 뒤 MapMatchSvr 가 매칭하므로
  //   재테스트보다 완료까지 여유를 더 둔다. route.min_m=2000/max_links=20 조합은 실제로
  //   운행 1건이 2분 이상 걸리는 경우도 있어(2026-07-22 vehicles=3 전환 후 실측: 129초 시점에도
  //   미완주) 90초는 부족했다 — 5분으로 상향 (2026-07-22 최정우 수정)
  const NEWTEST_TIMEOUT_MS = 300000;
  // ROAD_TYPE(prim_link_info) — 시설 유형. 잠수교 등 특정 교량명은 별도 코드 없이 교량(1)에 포함, name 으로 구분 (2026-07-21 최정우 추가)
  const ROAD_TYPE_LABELS = { 0: "일반도로", 1: "교량", 2: "터널", 3: "고가도로", 4: "지하차도" };
  const ARROW_ZOOM_MIN = 14;
  const ARROW_SPACING_M = 45;
  const ARROW_MAX_PER_PATH = 6;
  const SEQ_ZOOM_MIN = 13;

  let map, layerRoads, layerRoadsHalo, layerRoadArrows;
  let roadBufferM = 1000;
  let pollSec = 0;
  let pollTimer = null;
  let currentTripId = "";
  let lastRoadFc = null;
  let lastRoadSig = "";
  let showLabelState = null;
  let arrowsVisibleState = null;
  let lastTripsSig = "";
  let lastTripsData = [];
  // trip(차량)별 렌더 컨텍스트 — 여러 trip을 동시에 지도에 겹쳐 표시하기 위해, 예전엔 전역이던
  //   레이어그룹·gps_seq 추적 Map을 trip마다 따로 둔다. 지도에 실제로 그려지는 trip 집합은
  //   renderedTripIds() = checkedDisplayTripIds ∪ {포커스 trip(tripSelect.value)} — 포커스 trip은
  //   재테스트/삭제 대상이라 항상 지도에도 보여야 하므로 체크리스트와 무관하게 항상 포함한다
  //   (2026-07-23 최정우 추가)
  let tripCtxById = new Map();
  let checkedDisplayTripIds = new Set();
  let tripColorById = new Map();
  const TRIP_COLOR_PALETTE = ["#2563eb", "#16a34a", "#dc2626", "#9333ea", "#ea580c", "#0891b2", "#65a30d", "#db2777"];

  const statusEl = document.getElementById("status");
  const tripSelect = document.getElementById("tripSelect");
  const vehicleCountSelect = document.getElementById("vehicleCount");
  const chkPoll = document.getElementById("chkPoll");
  const chkFollow = document.getElementById("chkFollow");
  const ctxMenu = document.getElementById("ctxMenu");
  const ctxCopy = document.getElementById("ctxCopy");
  const progressWrap = document.getElementById("progressBarWrap");
  const progressFill = document.getElementById("progressBarFill");
  // 다중 차량 동시표시 체크리스트 — 포커스 trip(tripSelect)은 그대로 두고, 지도에 겹쳐 보여줄
  //   차량을 추가로 고르는 별도 패널 (2026-07-23 최정우 추가)
  const btnDisplayTrips = document.getElementById("btnDisplayTrips");
  const displayTripsPanel = document.getElementById("displayTripsPanel");
  const displayTripsList = document.getElementById("displayTripsList");
  const legendTrips = document.getElementById("legendTrips");

  // 버튼 클릭 진행률 바 — 완료 시점을 알 수 있으면(재테스트) 실제 %, 모르면(신규테스트·삭제·
  //   새로고침) 좌우로 흐르는 불확정 애니메이션으로 표시 (2026-07-22 최정우 추가)
  function showProgressIndeterminate() {
    if (!progressWrap) return;
    progressWrap.classList.add("active", "indeterminate");
  }
  function showProgressPercent(pct) {
    if (!progressWrap) return;
    progressWrap.classList.add("active");
    progressWrap.classList.remove("indeterminate");
    progressFill.style.width = Math.max(0, Math.min(100, pct)) + "%";
  }
  function hideProgress() {
    if (!progressWrap) return;
    progressWrap.classList.remove("active", "indeterminate");
    progressFill.style.width = "0%";
  }
  // 작업이 "성공적으로" 끝났을 때만 호출 — 진행률 바를 100%(끝)까지 채운 채 잠깐 멈춰
  //   눈으로 완료를 확인할 수 있게 한 다음 hideProgress() 로 넘어간다. 실패·중단 시에는
  //   호출하지 않아 (완료 아님을 암시하지 않도록) 바로 hideProgress() 로 사라진다
  //   (2026-07-22 최정우 추가)
  function finishProgress() {
    if (!progressWrap) return Promise.resolve();
    progressWrap.classList.add("active");
    progressWrap.classList.remove("indeterminate");
    progressFill.style.width = "100%";
    return new Promise(function (resolve) { setTimeout(resolve, 400); });
  }

  function setStatus(msg, isError) {
    statusEl.textContent = msg;
    statusEl.style.color = isError ? "#b91c1c" : "#64748b";
  }

  async function fetchJson(url) {
    const sep = url.indexOf("?") >= 0 ? "&" : "?";
    const res = await fetch(url + sep + "_=" + Date.now(), { cache: "no-store" });
    const ct = res.headers.get("content-type") || "";
    if (!res.ok) {
      if (res.status === 404) {
        throw new Error("API 404 — http://VM_IP:8088/ 로 접속 (80번은 다른 사이트)");
      }
      throw new Error("HTTP " + res.status + " " + url);
    }
    if (ct.indexOf("json") < 0) {
      throw new Error("API 응답 오류 — :8088 포트로 접속하세요");
    }
    return res.json();
  }

  function isFollowLatest() {
    return !chkFollow || chkFollow.checked;
  }

  function initMap() {
    // 도로선(halo+본선 500+개)·점 마커(최대 120개, tooltip 포함)를 개별 SVG DOM 대신
    //   캔버스 한 장에 그림 — 확대 직후 수백 개 DOM 을 재배치하며 몇 프레임에 걸쳐
    //   "따라잡던" 것이 미세한 지도 드리프트로 보이던 원인. 점·연결선은 전용 pane
    //   (panePoints) 을 쓰기 때문에 { renderer: L.canvas() } 만으로는
    //   적용 안 되고 preferCanvas 를 켜야 pane 별 렌더러도 캔버스로 생성됨 (2026-07-21 최정우 수정)
    map = L.map("map", { preferCanvas: true });
    // 밝은 무료 베이스맵 — 도로 오버레이 가독성 (2026-07-10 최정우)
    L.tileLayer("https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png", {
      attribution: "&copy; OpenStreetMap &copy; CARTO",
      subdomains: "abcd",
      // 타일 네이티브 한계(20) 이상은 업스케일로 확대 허용 (2026-07-15 최정우 수정)
      maxNativeZoom: 20,
      maxZoom: 24,
    }).addTo(map);
    map.setView([37.55, 126.98], 14);

    // 마커·연결선(점선+히트선)을 같은 pane 에 둔다 — 서로 다른 pane 을 쓰면 각 pane 이
    //   전체 지도 크기의 투명 캔버스 하나로 렌더링되어(Leaflet canvas 렌더러 특성), z-index 가
    //   더 높은 pane 의 캔버스가 지도 전체 영역에서 마우스오버 이벤트 자체를 가로채 버려
    //   아래 pane(연결선)은 좌표가 실제로 겹치지 않는 곳에서도 mousemove 를 절대 못 받는다
    //   (DOM 상 "그 지점에서 가장 위에 있는 엘리먼트"가 항상 위 pane 의 캔버스이기 때문).
    //   → INTERSECT_LEN 점선에 마우스오버해도 툴팁이 전혀 안 뜨던 원인.
    //   같은 pane(캔버스)을 쓰면 Leaflet 이 도형 추가 순서대로 히트테스트 우선순위를 매기므로,
    //   연결선을 먼저 그리고 마커를 나중에 그리면 마커 위에서는 마커가, 연결선 위에서는
    //   연결선이 정상적으로 우선한다 (2026-07-22 최정우 수정)
    map.createPane("panePoints");
    map.getPane("panePoints").style.zIndex = 450;

    layerRoadsHalo = L.layerGroup().addTo(map);
    layerRoads = L.layerGroup().addTo(map);
    layerRoadArrows = L.layerGroup().addTo(map);
  }

  // trip 팔레트 색 — trip이 /api/trips 목록에 처음 등장한 순서로 고정 할당, 목록에서 사라져도
  //   재등장 시 같은 색을 유지하도록 트립이 사라지지 않는 한 지우지 않는다 (2026-07-23 최정우 추가)
  function getTripColor(tripId) {
    if (!tripColorById.has(tripId)) {
      tripColorById.set(tripId, TRIP_COLOR_PALETTE[tripColorById.size % TRIP_COLOR_PALETTE.length]);
    }
    return tripColorById.get(tripId);
  }

  // trip별 렌더 컨텍스트 생성/파괴 — 레이어그룹은 선택된 동안만 지도에 붙어있음 (2026-07-23 최정우 추가)
  function ensureTripCtx(tripId) {
    let ctx = tripCtxById.get(tripId);
    if (ctx) return ctx;
    ctx = {
      color: getTripColor(tripId),
      gpsLineLayer: L.layerGroup().addTo(map),
      gpsLayer: L.layerGroup().addTo(map),
      matchedLayer: L.layerGroup().addTo(map),
      skipLayer: L.layerGroup().addTo(map),
      pointLayerBySeq: new Map(),
      gpsTrailSegments: new Map(),
    };
    tripCtxById.set(tripId, ctx);
    return ctx;
  }

  function destroyTripCtx(tripId) {
    const ctx = tripCtxById.get(tripId);
    if (!ctx) return;
    map.removeLayer(ctx.gpsLineLayer);
    map.removeLayer(ctx.gpsLayer);
    map.removeLayer(ctx.matchedLayer);
    map.removeLayer(ctx.skipLayer);
    tripCtxById.delete(tripId);
  }

  function destroyAllTripCtx() {
    Array.from(tripCtxById.keys()).forEach(destroyTripCtx);
  }

  function clearRoadLayers() {
    layerRoads.clearLayers();
    layerRoadsHalo.clearLayers();
    layerRoadArrows.clearLayers();
  }

  function extractLinePaths(geom) {
    const paths = [];
    if (!geom) return paths;
    if (geom.type === "LineString") {
      paths.push(geom.coordinates.map(function (c) { return [c[1], c[0]]; }));
    } else if (geom.type === "MultiLineString") {
      geom.coordinates.forEach(function (line) {
        paths.push(line.map(function (c) { return [c[1], c[0]]; }));
      });
    }
    return paths;
  }

  function segmentBearingDeg(a, b) {
    const lat1 = a[0] * Math.PI / 180;
    const lat2 = b[0] * Math.PI / 180;
    const dLon = (b[1] - a[1]) * Math.PI / 180;
    const y = Math.sin(dLon) * Math.cos(lat2);
    const x = Math.cos(lat1) * Math.sin(lat2) - Math.sin(lat1) * Math.cos(lat2) * Math.cos(dLon);
    return (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
  }

  function pathLengthM(path) {
    let len = 0;
    for (let i = 1; i < path.length; i++) len += map.distance(path[i - 1], path[i]);
    return len;
  }

  function pointAlongPath(path, distM) {
    let acc = 0;
    for (let i = 1; i < path.length; i++) {
      const seg = map.distance(path[i - 1], path[i]);
      if (acc + seg >= distM) {
        const t = (distM - acc) / seg;
        return {
          latlng: [
            path[i - 1][0] + (path[i][0] - path[i - 1][0]) * t,
            path[i - 1][1] + (path[i][1] - path[i - 1][1]) * t,
          ],
          bearing: segmentBearingDeg(path[i - 1], path[i]),
        };
      }
      acc += seg;
    }
    return null;
  }

  function makeArrowMarker(latlng, bearingDeg) {
    // 삼각형 기본 방향은 동쪽(→). 방위각(정북=0°, 시계방향)에 맞추려면 -90° 보정 (2026-07-15 최정우 수정)
    var rotDeg = bearingDeg - 90;
    return L.marker(latlng, {
      interactive: false,
      icon: L.divIcon({
        className: "road-dir-arrow",
        html:
          '<svg width="11" height="11" viewBox="0 0 11 11" ' +
          'style="transform:rotate(' + rotDeg + 'deg);display:block">' +
          '<path d="M1.5 1.5 L9.5 5.5 L1.5 9.5 Z" fill="#1e40af" stroke="#ffffff" stroke-width="0.6"/>' +
          "</svg>",
        iconSize: [11, 11],
        iconAnchor: [5.5, 5.5],
      }),
    });
  }

  // 화살표 레이어 표시/숨김만 토글 — 줌마다 재계산·재생성하지 않음 (2026-07-21 최정우 수정)
  function syncArrowVisibility() {
    const arrowsOn = map.getZoom() >= ARROW_ZOOM_MIN;
    if (arrowsOn === arrowsVisibleState) return;
    arrowsVisibleState = arrowsOn;
    if (arrowsOn) {
      if (!map.hasLayer(layerRoadArrows)) map.addLayer(layerRoadArrows);
    } else if (map.hasLayer(layerRoadArrows)) {
      map.removeLayer(layerRoadArrows);
    }
  }

  // 화살표 위치는 실제 거리(m) 기준이라 줌과 무관 — 도로 데이터가 바뀔 때만 재계산 (2026-07-21 최정우 수정)
  function rebuildRoadArrows() {
    layerRoadArrows.clearLayers();
    syncArrowVisibility();
    if (!lastRoadFc || !lastRoadFc.features) return;

    lastRoadFc.features.forEach(function (feat) {
      const paths = extractLinePaths(feat.geometry);
      paths.forEach(function (path) {
        if (path.length < 2) return;
        const totalM = pathLengthM(path);
        if (totalM < 8) {
          const mid = Math.floor((path.length - 1) / 2);
          const mk = makeArrowMarker(
            [(path[mid][0] + path[mid + 1][0]) / 2, (path[mid][1] + path[mid + 1][1]) / 2],
            segmentBearingDeg(path[mid], path[mid + 1])
          );
          mk.addTo(layerRoadArrows);
          return;
        }
        const count = Math.min(ARROW_MAX_PER_PATH, Math.max(1, Math.floor(totalM / ARROW_SPACING_M)));
        const step = totalM / (count + 1);
        for (let n = 1; n <= count; n++) {
          const pt = pointAlongPath(path, step * n);
          if (pt) makeArrowMarker(pt.latlng, pt.bearing).addTo(layerRoadArrows);
        }
      });
    });
  }

  function addRoadGeoJson(fc) {
    lastRoadFc = fc;
    clearRoadLayers();
    const styleHalo = { color: "#ffffff", weight: 5, opacity: 0.9 };
    const styleRoad = { color: "#1e3a8a", weight: 3, opacity: 0.82 };
    L.geoJSON(fc, {
      style: styleHalo,
      interactive: false,
    }).addTo(layerRoadsHalo);
    L.geoJSON(fc, {
      style: styleRoad,
      // 점 마커·연결선과 같은 pane(panePoints) 에 둔다 — Leaflet canvas 렌더러는 pane 마다
      // 지도 전체 크기의 캔버스 하나를 만드는데, 다른 pane(overlayPane 등)에 두면 z-index
      // 가 더 높은 panePoints 캔버스가 지도 전체 영역의 마우스오버 이벤트를 가로채 버려
      // 도로선은 마우스가 도로 위에 있어도 이벤트를 전혀 못 받는다 (2026-07-15 GPS↔연결선에서
      // 같은 문제를 겪었던 것과 동일 원인) (2026-07-22 최정우 수정)
      pane: "panePoints",
      onEachFeature: function (feat, layer) {
        const p = feat.properties || {};
        const lid = p.link_id || "";
        // 시설 유형(터널·지하차도 등) 은 도로명 옆에 바로 보이도록 title 에 포함 (2026-07-21 최정우 추가)
        const facility = ROAD_TYPE_LABELS[p.road_type];
        const facilityTag = (facility && p.road_type !== 0) ? " [" + facility + "]" : "";
        const title = (p.name ? p.name + " (" + lid + ")" : "link: " + lid) + facilityTag;
        layer.bindTooltip(title, { sticky: true });
        const popup =
          "<b>link</b> " + lid +
          (p.name ? "<br><b>도로명</b> " + p.name : "") +
          "<br><b>시설물</b> " + (facility || "정보없음") +
          (p.len != null ? "<br><b>len</b> " + p.len + "m" : "") +
          (p.st_nd_name ? "<br><b>출발</b> " + p.st_nd_name : (p.st_nd_id ? "<br><b>출발</b> " + p.st_nd_id : "")) +
          (p.ed_nd_name ? "<br><b>도착</b> " + p.ed_nd_name : (p.ed_nd_id ? "<br><b>도착</b> " + p.ed_nd_id : ""));
        layer.bindPopup(popup);
        // 마우스오버 시 현재 정보(시설물 포함) 즉시 팝업 표시 — 점 마커와 동일 UX (2026-07-21 최정우 추가)
        layer.on("mouseover", function () { this.openPopup(); });
        layer.on("mouseout", function () { this.closePopup(); });
        // 같은 pane 안에서는 나중에 그린 도형이 마우스오버를 우선 차지한다 — 도로가 갱신될
        // 때마다(trip 이 바뀔 때마다) 새로 추가되므로, 이미 떠있는 마커·연결선보다 뒤로
        // 보내둬야 마커 위에서는 항상 마커가 우선한다 (2026-07-22 최정우 추가)
        layer.bringToBack();
      },
    }).addTo(layerRoads);
    rebuildRoadArrows();
  }

  function popupHtml(p, tripId) {
    return (
      // 여러 trip이 동시에 지도에 겹칠 때 이 점이 어느 차량 것인지 구분하기 위해 trip_id 표시
      // (2026-07-23 최정우 추가)
      "<b>trip</b> " + tripId + "<br>" +
      "<b>seq</b> " + p.gps_seq + "<br>" +
      "<b>status</b> " + p.match_status +
      // 매칭 LinkID·도로명 (엔진 저장값) 표시 (2026-07-15 최정우 수정)
      (p.match_link_id ? "<br><b>match link</b> " + p.match_link_id +
        (p.match_link_name ? " (" + p.match_link_name + ")" : "") : "") +
      (p.intersect_len != null ? "<br><b>intersect_len</b> " + p.intersect_len + "m (GPS↔세그먼트)" : "") +
      // SKIP·ERROR 원인 추정 — MATCH_REASON 컬럼이 없어 기존 값(link_id·intersect_len·accuracy_m)만으로
      // server.py 가 추정한 값. 엔진이 실제 기록한 사유가 아니므로 항상 "추정" 문구 포함 (2026-07-21 최정우 추가)
      (p.match_reason ? "<br><b>원인(추정)</b> " + p.match_reason : "") +
      "<br><b>gps_dt</b> " + p.gps_dt
    );
  }

  const TIP_DIRS_GPS = ["top", "right", "bottom", "left"];
  const TIP_DIRS_MATCH = ["bottom", "right", "left", "top"];

  function isMatchFailed(status) {
    return status === MATCH_STATUS_SKIP || status === MATCH_STATUS_ERROR;
  }

  function tipClassName(base, failed) {
    return failed ? base + " tip-fail" : base;
  }

  function addPointMarker(latlng, style, label, tipClass, dir, popup, layer, labelVisible) {
    // 마커는 전용 pane(panePoints)에 배치 → 연결선보다 항상 위, 중앙 마우스오버 정상 동작 (2026-07-15 최정우 수정)
    const markerStyle = Object.assign({ pane: "panePoints", bubblingMouseEvents: false }, style);
    const m = L.circleMarker(latlng, markerStyle).bindPopup(popup);
    // 마우스오버 시 popup 표시(매칭 LinkID·seq·상태 등), 벗어나면 닫힘 (2026-07-15 최정우 추가)
    m.on("mouseover", function () { this.openPopup(); });
    m.on("mouseout", function () { this.closePopup(); });
    // 툴팁은 항상 바인딩해두고 opacity 로만 표시/숨김 — 줌 변경 시 마커 재생성 없이 토글 가능 (2026-07-21 최정우 수정)
    m.bindTooltip(label, {
      permanent: true,
      direction: dir,
      className: tipClass,
      offset: dir === "top" ? [0, -8] : dir === "bottom" ? [0, 8] : dir === "left" ? [-8, 0] : [8, 0],
      opacity: labelVisible ? 0.9 : 0,
    });
    m.addTo(layer);
    return m;
  }

  // 점 하나의 렌더링에 영향을 주는 필드만 모아 서명 생성 — 값이 안 바뀌면 마커를 다시 그리지 않음 (2026-07-21 최정우 추가)
  function pointSig(p) {
    return [
      p.match_status, p.gps_lat, p.gps_lon, p.match_lat, p.match_lon,
      p.intersect_len, p.match_link_id, p.gps_dt,
    ].join("|");
  }

  function removePointEntry(entry, ctx) {
    if (entry.gpsMarker) ctx.gpsLayer.removeLayer(entry.gpsMarker);
    if (entry.matchMarker) (entry.isSkip ? ctx.skipLayer : ctx.matchedLayer).removeLayer(entry.matchMarker);
    if (entry.connDash) ctx.gpsLineLayer.removeLayer(entry.connDash);
    if (entry.connHit) ctx.gpsLineLayer.removeLayer(entry.connHit);
  }

  function buildPointEntry(p, showLabel, ctx, tripId) {
    const matchFailed = isMatchFailed(p.match_status);
    const entry = { sig: pointSig(p), isSkip: p.match_status === MATCH_STATUS_SKIP };
    const gpsLl = (p.gps_lat != null && p.gps_lon != null) ? [p.gps_lat, p.gps_lon] : null;
    const trueMatchLl = (p.match_lat != null && p.match_lon != null) ? [p.match_lat, p.match_lon] : null;

    // INTERSECT_LEN: GPS(G) ↔ 세그먼트 교차점(MATCH_LAT/LON) 거리 시각화 + 마우스오버 툴팁(m).
    // 마커보다 먼저 그린다 — 같은 pane(캔버스) 안에서는 나중에 그린 도형이 마우스오버 우선순위를
    // 가져가므로, 연결선을 먼저·마커를 나중에 그려야 마커 위에서는 마커가, 연결선 위(마커가 없는
    // 구간)에서는 연결선이 정상적으로 히트테스트를 잡는다 (2026-07-15 원설계, 2026-07-22 최정우 수정)
    if (gpsLl && trueMatchLl && map.distance(gpsLl, trueMatchLl) > 0.8) {
      // INTERSECT_LEN(DB, m) 우선, 없으면 화면상 거리로 계산
      const distM = (p.intersect_len != null)
        ? p.intersect_len
        : Math.round(map.distance(gpsLl, trueMatchLl));
      // SKIP·ERROR(신뢰 못 하는 매칭)는 교차선도 그 사실이 보이게 주황 계열로 구분 — 지금까지는
      // MATCHED 와 같은 회색선으로 그려서, 좁은 구역에 여러 점이 몰리면(예: 클램프로 SKIP된 점)
      // 정상 매칭 교차선과 뒤섞여 어떤 선이 신뢰 가능한 매칭인지 구분이 안 됐다 (2026-07-21 최정우 수정)
      const tipText = "seq " + p.gps_seq + " · INTERSECT_LEN " + distM + " m (GPS↔매칭)"
        + (matchFailed ? " · SKIP·저신뢰" : "");
      // 표시용 점선
      entry.connDash = L.polyline([gpsLl, trueMatchLl], {
        pane: "panePoints",
        color: matchFailed ? "#c2410c" : "#334155",
        weight: matchFailed ? 4 : 3,
        opacity: matchFailed ? 0.98 : 0.92,
        dashArray: "4,4",
      })
        .bindTooltip(tipText, {
          sticky: true,
          direction: "top",
          className: matchFailed ? "tip-intersect tip-fail" : "tip-intersect",
          opacity: 0.98,
        })
        .addTo(ctx.gpsLineLayer);
      // 마우스오버 히트영역(투명) — 툴팁 트리거 인식률 보강
      entry.connHit = L.polyline([gpsLl, trueMatchLl], {
        pane: "panePoints", color: "#000000", weight: 12, opacity: 0,
      })
        .bindTooltip(tipText, {
          sticky: true,
          direction: "top",
          className: matchFailed ? "tip-intersect tip-fail" : "tip-intersect",
          opacity: 0.98,
        })
        .addTo(ctx.gpsLineLayer);
    }

    // 마커는 연결선 다음에 그린다 — 같은 pane 안에서 나중에 그린 도형이 마우스오버를 우선
    // 차지하므로, 마커 중앙에서는 항상 마커(팝업)가 연결선 히트영역보다 우선하게 된다.
    // 테두리(color)는 trip 팔레트 색으로 통일해 "어느 차량인지"를 나타내고, 채우기(fillColor)는
    // 기존처럼 매칭상태를 나타낸다 — 이중 인코딩으로 여러 차량이 겹쳐도 구분 가능 (2026-07-23 최정우 추가)
    if (gpsLl) {
      entry.gpsMarker = addPointMarker(
        gpsLl,
        { radius: 6, color: ctx.color, fillColor: "#e53935", fillOpacity: 0.95, weight: 3 },
        "G" + p.gps_seq,
        "tip-gps",
        TIP_DIRS_GPS[p.gps_seq % TIP_DIRS_GPS.length],
        popupHtml(p, tripId),
        ctx.gpsLayer,
        showLabel
      );
    }

    if (trueMatchLl) {
      const layer = entry.isSkip ? ctx.skipLayer : ctx.matchedLayer;
      entry.matchMarker = addPointMarker(
        trueMatchLl,
        {
          radius: 7,
          color: ctx.color,
          fillColor: entry.isSkip ? "#fb8c00" : "#1e88e5",
          fillOpacity: 0.95,
          weight: 3,
        },
        "M" + p.gps_seq,
        tipClassName(entry.isSkip ? "tip-skip" : "tip-match", matchFailed),
        TIP_DIRS_MATCH[p.gps_seq % TIP_DIRS_MATCH.length],
        popupHtml(p, tripId),
        layer,
        showLabel
      );
    }
    return entry;
  }

  // GPS 궤적 점선 — 인접한 두 GPS 점 사이 구간마다 개별 polyline(가시용 점선 + 투명 히트라인)을
  //   그려서, 구간 위에 마우스오버하면 두 좌표간 직선거리(m)를 툴팁으로 보여준다. seq 쌍을 key로
  //   구간 단위 diff 해서 바뀐 구간만 다시 그림(불필요한 SVG path 재작성 방지).
  //   가시용 선(dash)은 interactive:false — 실제 마우스오버는 굵은 투명 히트라인(hit)이 받는다.
  //   반드시 마커보다 먼저 그려야 한다: 같은 pane(panePoints) 안에서는 나중에 그린 도형이
  //   마우스오버 우선순위를 가져가므로, 히트라인을 먼저 그려야 GPS점 정중앙에서는 마커가,
  //   구간 중간(마커가 없는 곳)에서는 히트라인이 정상적으로 우선순위를 갖는다 (2026-07-22 최정우 수정)
  function rebuildGpsTrail(points, ctx) {
    const validPoints = points.filter(function (p) { return p.gps_lat != null && p.gps_lon != null; });
    const seen = new Set();
    for (let i = 0; i < validPoints.length - 1; i++) {
      const a = validPoints[i], b = validPoints[i + 1];
      const key = a.gps_seq + "-" + b.gps_seq;
      seen.add(key);
      const sig = a.gps_lat + "," + a.gps_lon + "|" + b.gps_lat + "," + b.gps_lon;
      const existing = ctx.gpsTrailSegments.get(key);
      if (existing && existing.sig === sig) continue;
      if (existing) {
        ctx.gpsLineLayer.removeLayer(existing.dash);
        ctx.gpsLineLayer.removeLayer(existing.hit);
      }
      const aLl = [a.gps_lat, a.gps_lon], bLl = [b.gps_lat, b.gps_lon];
      const distM = Math.round(map.distance(aLl, bLl));
      const tipText = "seq " + a.gps_seq + " → " + b.gps_seq + " · " + distM + " m (GPS 연결 직선거리)";
      const dash = L.polyline([aLl, bLl], {
        pane: "panePoints", color: "#e53935", weight: 2, opacity: 0.55, dashArray: "4,6",
        interactive: false,
      }).addTo(ctx.gpsLineLayer);
      const hit = L.polyline([aLl, bLl], {
        pane: "panePoints", color: "#000000", weight: 12, opacity: 0,
      })
        .bindTooltip(tipText, { sticky: true, direction: "top", className: "tip-gpstrail", opacity: 0.98 })
        .addTo(ctx.gpsLineLayer);
      ctx.gpsTrailSegments.set(key, { dash: dash, hit: hit, sig: sig });
    }
    ctx.gpsTrailSegments.forEach(function (seg, key) {
      if (seen.has(key)) return;
      ctx.gpsLineLayer.removeLayer(seg.dash);
      ctx.gpsLineLayer.removeLayer(seg.hit);
      ctx.gpsTrailSegments.delete(key);
    });
  }

  // gps_seq 별로 이전 렌더링과 비교해 바뀐 점만 다시 그림 — 폴링마다 전체 재생성하던 깜빡임 제거 (2026-07-21 최정우 수정)
  // trip별 컨텍스트(ctx)를 받아 그 trip의 레이어·추적 Map에만 반영한다 (2026-07-23 최정우 수정 — 다중 trip 대응)
  function updatePoints(points, ctx, tripId) {
    const showLabel = map.getZoom() >= SEQ_ZOOM_MIN;
    showLabelState = showLabel;
    // 마커보다 먼저 그린다 — z-order 우선순위 규칙(위 rebuildGpsTrail 주석 참고) (2026-07-22 최정우 수정)
    rebuildGpsTrail(points, ctx);
    const seen = new Set();
    points.forEach(function (p) {
      seen.add(p.gps_seq);
      const sig = pointSig(p);
      const existing = ctx.pointLayerBySeq.get(p.gps_seq);
      if (existing && existing.sig === sig) return;
      if (existing) removePointEntry(existing, ctx);
      ctx.pointLayerBySeq.set(p.gps_seq, buildPointEntry(p, showLabel, ctx, tripId));
    });
    // trip 리셋 등으로 사라진 seq 가 있으면 정리 (평소엔 비어있는 no-op)
    ctx.pointLayerBySeq.forEach(function (entry, seq) {
      if (seen.has(seq)) return;
      removePointEntry(entry, ctx);
      ctx.pointLayerBySeq.delete(seq);
    });
  }

  // 줌 변경 시 라벨 표시/숨김만 토글 — 마커 재생성 없음. 현재 지도에 그려진 모든 trip 컨텍스트에
  //   적용해야 한다 (2026-07-21 최정우 추가, 2026-07-23 최정우 수정 — 다중 trip 대응)
  function setLabelsVisible(visible) {
    tripCtxById.forEach(function (ctx) {
      ctx.pointLayerBySeq.forEach(function (entry) {
        if (entry.gpsMarker) {
          const tt = entry.gpsMarker.getTooltip();
          if (tt) tt.setOpacity(visible ? 0.9 : 0);
        }
        if (entry.matchMarker) {
          const tt = entry.matchMarker.getTooltip();
          if (tt) tt.setOpacity(visible ? 0.9 : 0);
        }
      });
    });
  }

  function fitToContent(points) {
    map.invalidateSize(true);
    const bounds = L.latLngBounds([]);
    points.forEach(function (p) {
      if (p.gps_lat != null && p.gps_lon != null) bounds.extend([p.gps_lat, p.gps_lon]);
      if (p.match_lat != null && p.match_lon != null) bounds.extend([p.match_lat, p.match_lon]);
    });
    layerRoads.eachLayer(function (ly) {
      if (ly.getBounds) bounds.extend(ly.getBounds());
    });
    if (bounds.isValid()) map.fitBounds(bounds.pad(0.12), { maxZoom: 19 });
  }

  function shouldAutoFit(forceFit) {
    // 배경 폴링 중에는 trip 이 바뀌어도 자동 fit 하지 않음 — "최신 Trip" 상태에서
    //   새 trip 이 뜰 때마다 지도가 다른 위치로 튀던 원인. 수동 새로고침/trip 선택/
    //   최초 로딩 등 forceFit 이 명시적으로 요청된 경우에만 이동 (2026-07-21 최정우 수정)
    return !!forceFit;
  }

  // trip 목록(순번·pts 카운트)이 이전과 동일하면 <select> 를 다시 그리지 않음 — 값이 안 바뀌어도
  //   매 폴링마다 옵션을 전부 지웠다 새로 만들던 것이 콤보박스 깜빡임의 원인이었음 (2026-07-21 최정우 수정)
  function tripsSignature(trips) {
    return trips.map(function (t) { return t.trip_id + ":" + t.count; }).join(",");
  }

  function fillTripSelect(trips, selectedId, followLatest) {
    const prev = followLatest ? "" : (selectedId || tripSelect.value || currentTripId);
    const sig = tripsSignature(trips);
    if (sig !== lastTripsSig) {
      lastTripsSig = sig;
      tripSelect.innerHTML = "";
      trips.forEach(function (t) {
        const opt = document.createElement("option");
        opt.value = t.trip_id;
        opt.textContent = t.trip_id + " (" + t.count + "pts)";
        tripSelect.appendChild(opt);
      });
    }
    if (trips.length === 0) return "";
    if (followLatest) {
      tripSelect.value = trips[0].trip_id;
      return trips[0].trip_id;
    }
    if (prev && trips.some(function (t) { return t.trip_id === prev; })) {
      tripSelect.value = prev;
      return prev;
    }
    tripSelect.value = trips[0].trip_id;
    return trips[0].trip_id;
  }

  function clearAllDisplay(statusMsg, isError) {
    currentTripId = "";
    lastRoadFc = null;
    lastRoadSig = "";
    lastTripsSig = "";
    checkedDisplayTripIds.clear();
    destroyAllTripCtx();
    clearRoadLayers();
    updateLegendTrips();
    tripSelect.innerHTML = '<option value="">(trip 없음)</option>';
    if (statusMsg) setStatus(statusMsg, isError);
  }

  // 도로 링크 집합이 이전과 동일하면 다시 그리지 않음 — trip bbox 는 폴링 사이 거의 안 바뀌므로
  //   매번 도로선 전체를 재생성하던 깜빡임을 제거 (2026-07-21 최정우 추가)
  function roadsSignature(fc) {
    return fc.features.map(function (f) { return f.properties.link_id; }).sort().join(",");
  }

  function updateRoadsIfChanged(fc) {
    const sig = roadsSignature(fc);
    if (sig === lastRoadSig && lastRoadFc) return false;
    lastRoadSig = sig;
    addRoadGeoJson(fc);
    return true;
  }

  async function fetchTrips() {
    return fetchJson("/api/trips?limit=50");
  }

  // /api/trips 는 이미 MIN(gps_dt) DESC 로 정렬돼 오므로, device_key 별 첫 등장이 곧 그 차량의
  //   최신 trip — waitForNewTestData(892~896행 부근)가 진행률 추적에 쓰는 것과 동일한 계산을
  //   "지도에 표시할 차량 집합"에도 재사용한다 (2026-07-23 최정우 추가)
  function computeLatestPerDevice(trips) {
    const seenDevices = new Set();
    const result = new Set();
    trips.forEach(function (t) {
      if (seenDevices.has(t.device_key)) return;
      seenDevices.add(t.device_key);
      result.add(t.trip_id);
    });
    return result;
  }

  // 실제 지도에 렌더링할 trip 집합 = 체크리스트로 고른 trip 들 ∪ 포커스 trip(tripSelect 값).
  //   포커스 trip 은 재테스트·삭제 대상이라 체크리스트와 무관하게 항상 지도에 보여야 한다
  //   (2026-07-23 최정우 추가)
  function renderedTripIds() {
    const ids = new Set(checkedDisplayTripIds);
    if (currentTripId) ids.add(currentTripId);
    return ids;
  }

  async function refreshTrips(forceFit) {
    try {
      const trips = await fetchTrips();
      lastTripsData = trips;
      if (trips.length === 0) {
        clearAllDisplay("DB 데이터 없음 — 삭제 후 정상 (엔진 기동 시 새 데이터 생성)", true);
        return;
      }
      const prevTripId = currentTripId;
      const followLatest = isFollowLatest();
      const tripId = fillTripSelect(trips, currentTripId, followLatest);
      currentTripId = tripId;
      // "최신 Trip" 자동추적 시 체크리스트도 device_key 별 최신 trip으로 동기화. 수동 모드에서는
      //   목록에서 사라진(삭제된) trip만 체크리스트에서 정리 (2026-07-23 최정우 추가)
      const validIds = new Set(trips.map(function (t) { return t.trip_id; }));
      if (followLatest) {
        checkedDisplayTripIds = computeLatestPerDevice(trips);
      } else {
        Array.from(checkedDisplayTripIds).forEach(function (id) {
          if (!validIds.has(id)) checkedDisplayTripIds.delete(id);
        });
      }
      renderDisplayTripsPanel(trips);
      // trip 이 바뀌었다는 이유만으로는 화면을 이동하지 않음 — "최신 Trip" 체크 상태에서
      //   배경 폴링 중 새 trip 이 뜰 때마다 지도 위치가 튀던 원인. 화면에 아직 아무것도
      //   없던 최초 상태에서만 자동 fit (2026-07-21 최정우 수정)
      await syncMapTrips(renderedTripIds(), false, forceFit || !prevTripId);
    } catch (err) {
      setStatus("API 오류: " + err.message + " (웹/DB 기동 확인)", true);
    }
  }

  async function loadTrips() {
    await refreshTrips(true);
  }

  // 여러 trip을 동시에 지도에 렌더링하는 진입점 — 예전 loadTrip(단일 trip) 대체.
  //   1) tripIds 집합과 현재 tripCtxById 를 diff 해서 사라진 trip은 레이어 파괴, 새 trip은 생성
  //   2) 결합 API(/api/trips/points, /api/trips/prim-roads)로 한 번에 조회
  //   3) trip별로 updatePoints, 도로는 공유 레이어 하나로 렌더링 (2026-07-23 최정우 추가)
  async function syncMapTrips(tripIds, manualFit, forceFit) {
    const idsArr = Array.from(tripIds);
    const existingIds = Array.from(tripCtxById.keys());
    existingIds.forEach(function (id) { if (!tripIds.has(id)) destroyTripCtx(id); });
    idsArr.forEach(ensureTripCtx);
    updateLegendTrips();

    if (idsArr.length === 0) {
      clearRoadLayers();
      lastRoadFc = null;
      lastRoadSig = "";
      return;
    }

    const isBackgroundPoll = !manualFit && !forceFit;
    if (!isBackgroundPoll) setStatus("로딩… " + idsArr.join(", "));
    try {
      const idsParam = encodeURIComponent(idsArr.join(","));
      const [roads, pointsByTrip, primInfo] = await Promise.all([
        fetchJson("/api/trips/prim-roads?trip_ids=" + idsParam + "&buffer=" + roadBufferM),
        fetchJson("/api/trips/points?trip_ids=" + idsParam),
        fetchJson("/api/prim/info"),
      ]);
      if (!roads.features) throw new Error("roads 응답 형식 오류");
      updateRoadsIfChanged(roads);
      const allPoints = [];
      idsArr.forEach(function (tripId) {
        const points = pointsByTrip[tripId] || [];
        allPoints.push.apply(allPoints, points);
        updatePoints(points, tripCtxById.get(tripId), tripId);
      });
      if (shouldAutoFit(!!manualFit || !!forceFit)) {
        fitToContent(allPoints);
      }
      const now = new Date();
      const ts = now.toLocaleTimeString("ko-KR", { hour12: false });
      if (idsArr.length === 1) {
        const tripId = idsArr[0];
        const points = pointsByTrip[tripId] || [];
        setStatus(
          "[" + ts + "] trip=" + tripId +
          " prim도로=" + roads.features.length +
          " (전체 " + (primInfo.count || "?") + ")" +
          " GPS=" + points.filter(function (p) { return p.gps_lat != null; }).length +
          " 매칭=" + points.filter(function (p) { return p.match_lat != null; }).length
        );
      } else {
        setStatus(
          "[" + ts + "] " + idsArr.length + "대 표시 중 · prim도로=" + roads.features.length +
          " · 총 GPS=" + allPoints.filter(function (p) { return p.gps_lat != null; }).length +
          " 매칭=" + allPoints.filter(function (p) { return p.match_lat != null; }).length
        );
      }
    } catch (err) {
      setStatus("로딩 실패: " + err.message, true);
    }
  }

  // 클립보드 API 우선, 비보안 컨텍스트(HTTP·비-localhost) 대비 execCommand 폴백 (2026-07-21 최정우 추가)
  function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).catch(function () { copyTextFallback(text); });
    } else {
      copyTextFallback(text);
    }
  }

  function copyTextFallback(text) {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.focus();
    ta.select();
    try { document.execCommand("copy"); } catch (e) { /* 무시 — 클립보드 접근 불가 환경 */ }
    document.body.removeChild(ta);
  }

  function hideCtxMenu() {
    ctxMenu.style.display = "none";
  }

  // Trip id 콤보박스 우클릭 → "복사하기" 커스텀 메뉴 표시 (2026-07-21 최정우 추가)
  function setupTripContextMenu() {
    tripSelect.addEventListener("contextmenu", function (e) {
      if (!tripSelect.value) return;
      e.preventDefault();
      ctxMenu.style.left = e.clientX + "px";
      ctxMenu.style.top = e.clientY + "px";
      ctxMenu.style.display = "block";
    });
    ctxCopy.addEventListener("click", function () {
      copyText(tripSelect.value);
      hideCtxMenu();
      setStatus("Trip ID 복사됨: " + tripSelect.value);
    });
    document.addEventListener("click", function (e) {
      if (e.target !== ctxCopy) hideCtxMenu();
    });
    document.addEventListener("scroll", hideCtxMenu, true);
    window.addEventListener("blur", hideCtxMenu);
  }

  // 재매칭이 실제로 끝날 때까지(PENDING 건이 0이 될 때까지) 짧은 간격으로 확인 —
  //   고정 지연(예: 2초)으로 추측하면 PC 성능·MapMatchSvr 재시작 여부에 따라 완료 전에
  //   부분 결과를 "최종"으로 보여주고, 이후 일반 폴링이 나머지를 여러 번에 걸쳐 찔끔찔끔
  //   반영하면서 "완료했는데도 계속 깜빡이는" 것처럼 보이는 문제의 원인이었다.
  //   완료되면 즉시 멈추므로 PC 성능과 무관하게 동작한다 (2026-07-21 최정우 수정)
  // 반환값: 처리가 실제로 100% 완료됐는지(true) — 호출부에서 이 경우에만
  //   진행률 바를 끝까지 채운 채 보여준다 (2026-07-22 최정우 추가)
  async function waitForRetestCompletion(tripId, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      if (currentTripId !== tripId) return false;		// 사용자가 다른 trip 으로 이동 — 추적 중단
      let points;
      try {
        points = await fetchJson("/api/trip/" + encodeURIComponent(tripId) + "/points");
      } catch (err) {
        setStatus("재테스트 상태 확인 실패: " + err.message, true);
        return false;
      }
      const pending = points.filter(function (p) { return p.match_status === MATCH_STATUS_PENDING; }).length;
      // 완료 시점을 알 수 있는 경우이므로, 진행률 바에 실제 처리 비율(%) 표시 (2026-07-22 최정우 추가)
      showProgressPercent(points.length > 0 ? ((points.length - pending) / points.length) * 100 : 0);
      await syncMapTrips(renderedTripIds(), false, false);
      if (pending === 0)
      {
        setStatus("재테스트 완료: " + tripId + " (" + points.length + "건)");
        return true;
      }
      if (Date.now() >= deadline)
      {
        setStatus("재테스트 시간 초과 — 미완료 " + pending + "건 (자동 갱신을 켜면 이후 계속 반영됩니다)", true);
        return false;
      }
      await new Promise(function (resolve) { setTimeout(resolve, RETEST_POLL_MS); });
    }
  }

  // 선택된 Trip의 기존 수신 GPS(prim_rawgps)로 맵매칭 재테스트 — 매칭 결과만 초기화해
  //   MapMatchSvr 가 동일 좌표를 재폴링·재매칭하도록 함 (2026-07-21 최정우 추가)
  function setupRetestButton() {
    const btn = document.getElementById("btnRetest");
    btn.addEventListener("click", async function () {
      if (!currentTripId) return;
      const tripId = currentTripId;
      btn.disabled = true;
      // config.ini 변경 시 서버가 재테스트 전 MapMatchSvr 를 재시작하므로 다소 오래 걸릴 수 있음
      //   (2026-07-21 최정우 추가)
      setStatus("재테스트 요청 중… " + tripId + " (설정 변경 시 MapMatchSvr 재시작 포함, 최대 1분)");
      // 요청 직후(재매칭 시작 전)엔 아직 진행률(%)을 알 수 없어 불확정 표시로 시작 —
      //   waitForRetestCompletion 진입 후 실제 처리 비율로 전환됨 (2026-07-22 최정우 추가)
      showProgressIndeterminate();
      try {
        const res = await fetch("/api/trip/" + encodeURIComponent(tripId) + "/retest", {
          method: "POST",
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || ("HTTP " + res.status));
        setStatus(
          "재테스트 시작: " + tripId +
          (data.restarted ? " (설정 변경 감지 → MapMatchSvr 재시작 후" : " (") +
          " " + data.reset + "건 재매칭 중…)"
        );
        // 완료될 때까지 짧은 간격으로 확인 — 고정 지연 대신 실제 완료 시점에 맞춤 (2026-07-21 최정우 수정)
        const completed = await waitForRetestCompletion(tripId, RETEST_TIMEOUT_MS);
        if (completed) await finishProgress();
      } catch (err) {
        setStatus("재테스트 실패: " + err.message, true);
      } finally {
        btn.disabled = false;
        hideProgress();
      }
    });
  }

  // "삭제" 버튼 — 선택된 Trip을 PRIM_RAWGPS 에서 완전히 삭제(되돌릴 수 없음).
  //   삭제 후 지도·목록도 즉시 초기화하고 남은 Trip 중 최신 것을 다시 불러온다 (2026-07-22 최정우 추가)
  // "지도초기화" 버튼 — 지도에 그려진 내용(점·도로·화살표)만 지우고 초기 화면(위치·줌)으로
  //   되돌림. Trip 선택·데이터는 안 건드림 — 순수 화면 리셋 용도 (2026-07-22 최정우 추가)
  function setupResetMapButton() {
    const btn = document.getElementById("btnResetMap");
    if (!btn) return;
    btn.addEventListener("click", async function () {
      btn.disabled = true;
      showProgressIndeterminate();
      try {
        destroyAllTripCtx();
        clearRoadLayers();
        // 시그니처도 같이 리셋 — 안 하면 다음 갱신 때 "안 바뀜"으로 판단해 도로가 다시 안 그려짐
        lastRoadFc = null;
        lastRoadSig = "";
        map.invalidateSize(true);
        map.setView([37.55, 126.98], 14);
        setStatus("지도 초기화 완료");
        await finishProgress();
      } finally {
        btn.disabled = false;
        hideProgress();
      }
    });
  }

  function setupDeleteButton() {
    const btn = document.getElementById("btnDelete");
    if (!btn) return;
    btn.addEventListener("click", async function () {
      if (!currentTripId) return;
      const tripId = currentTripId;
      if (!window.confirm("Trip \"" + tripId + "\"을(를) DB에서 완전히 삭제하시겠습니까?\n되돌릴 수 없습니다."))
        return;
      btn.disabled = true;
      setStatus("삭제 중… " + tripId);
      showProgressIndeterminate();
      try {
        const res = await fetch("/api/trip/" + encodeURIComponent(tripId) + "/delete", {
          method: "POST",
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || ("HTTP " + res.status));
        // 지도·목록 초기화 후 남은 Trip 목록 다시 조회 (2026-07-22 최정우 추가)
        clearAllDisplay("삭제 완료: " + tripId + " (" + data.deleted + "건) — 목록 갱신 중");
        await refreshTrips(true);
        await finishProgress();
      } catch (err) {
        setStatus("삭제 실패: " + err.message, true);
      } finally {
        btn.disabled = false;
        hideProgress();
      }
    });
  }

  // 신규테스트: 엔진 기동 확인만으로는 화면에 반영되지 않는다 — Simulator 의 GPS 생성과
  //   MapMatchSvr 의 매칭이 모두 비동기 백그라운드 작업이라, 기동 확인 시점엔 새 trip 이
  //   아직 DB에 한 건도 없을 수 있다. 차량이 여러 대(vehicleCount)면 동시에 여러 trip 이
  //   새로 시작되므로, 그중 하나만 보고 "완료"로 판단하면 안 된다 — 재시작 전 존재하던
  //   trip_id 목록과 비교해 새로 나타난 device_key 마다 첫 trip 을 찾아 전부 END+매칭완료
  //   될 때까지 같이 추적한다 (2026-07-22 최정우 추가, 2026-07-22 최정우 수정 — 차량 여러대 대응)
  //   반환값: expectedVehicles 대 전부 100% 매칭 완료됐는지(true)
  async function waitForNewTestData(prevTripIds, expectedVehicles, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    const newTripByDevice = new Map();		// device_key -> trip_id (첫 등장한 것만 고정)
    const doneTripIds = new Set();
    // 새 trip 이 처음 나타난 시점에 딱 한 번만 그 범위로 이동·확대하고, 그 이후엔
    // refreshTrips 를 forceFit 없이 호출한다 — 매 폴링마다 강제로 fitBounds 하면
    // 매칭이 끝날 때까지 사용자가 확대·이동해도 계속 원래 범위로 되돌아가 버려
    // 지도를 조작할 수 없는 것처럼 보였다 (재테스트 버튼은 원래도 forceFit 없이 폴링함)
    // (2026-07-22 최정우 수정)
    let fittedOnce = false;
    for (;;) {
      let trips;
      try {
        trips = await fetchTrips();
      } catch (err) {
        trips = [];
      }
      trips.forEach(function (t) {
        if (!prevTripIds.has(t.trip_id) && !newTripByDevice.has(t.device_key)) {
          newTripByDevice.set(t.device_key, t.trip_id);
        }
      });

      // 지도에 실제로 표시되는 trip 은 기존 "최신 Trip" 팔로우 로직에 맡기고, 여기서는
      // 완료 판정·진행률 집계만 전 차량에 걸쳐 따로 수행한다
      await refreshTrips(false);

      if (newTripByDevice.size > 0) {
        const allPoints = [];
        let sumRatio = 0;
        for (const tripId of newTripByDevice.values()) {
          if (doneTripIds.has(tripId)) { sumRatio += 1; continue; }
          let points;
          try {
            points = await fetchJson("/api/trip/" + encodeURIComponent(tripId) + "/points");
          } catch (err) {
            points = null;
          }
          if (!points || points.length === 0) continue;
          allPoints.push.apply(allPoints, points);
          const pending = points.filter(function (p) { return p.match_status === MATCH_STATUS_PENDING; }).length;
          // Simulator 가 분할 삽입 중이면 지금까지 들어온 몇 건만 빠르게 매칭돼도
          // 아직 끝난 게 아니다 — 마지막 샘플(END 이벤트)까지 들어와서 매칭됐을 때만 완료로 본다
          const hasEndEvent = points.some(function (p) { return p.trip_event === TRIP_EVENT_END; });
          if (pending === 0 && hasEndEvent) {
            doneTripIds.add(tripId);
            sumRatio += 1;
          } else {
            sumRatio += (points.length - pending) / points.length;
          }
        }
        if (!fittedOnce && allPoints.length > 0) {
          fitToContent(allPoints);
          fittedOnce = true;
        }
        showProgressPercent((sumRatio / expectedVehicles) * 100);
        setStatus(
          "신규테스트 진행 중… " + doneTripIds.size + "/" + expectedVehicles + "대 완주" +
          " (" + newTripByDevice.size + "대 시작됨)"
        );

        if (doneTripIds.size >= expectedVehicles && newTripByDevice.size >= expectedVehicles) {
          setStatus("신규테스트 완료: " + expectedVehicles + "대 모두 완주");
          return true;
        }
      }
      if (Date.now() >= deadline) {
        setStatus(
          "신규테스트 진행 시간 초과 — " + doneTripIds.size + "/" + expectedVehicles +
          "대 완주 (자동 갱신을 켜면 이후 계속 반영됩니다)", true
        );
        return false;
      }
      await new Promise(function (resolve) { setTimeout(resolve, RETEST_POLL_MS); });
    }
  }

  // "신규테스트" 버튼 — MapMatchSvr → Simulator 순서로 1초 확인 + 최대 3회 재시도 기동.
  //   웹 자신은 이미 이 요청을 처리 중이므로 재기동 대상에서 제외. Simulator 는 기동될 때마다
  //   새 trip_id 를 발급하므로, 결과적으로 새 테스트 주행이 시작된다 (2026-07-21 최정우 추가,
  //   2026-07-22 최정우 수정 — 명칭을 실제 역할(신규 테스트)에 맞게 변경)
  function setupStartEnginesButton() {
    const btn = document.getElementById("btnStartEngines");
    if (!btn) return;
    btn.addEventListener("click", async function () {
      btn.disabled = true;
      // 재시작 전 존재하던 trip_id 전부 — 재시작 후 여기 없는 trip_id 가 나타나면 "신규"로 판별
      // (차량 대수만큼 동시에 새 trip 이 시작되므로, 예전처럼 currentTripId 하나만으로는
      // 판별 불가) (2026-07-22 최정우 수정 — 차량 여러대 대응)
      let prevTripIds;
      try {
        prevTripIds = new Set((await fetchTrips()).map(function (t) { return t.trip_id; }));
      } catch (err) {
        prevTripIds = new Set();
      }
      const vehicleCount = vehicleCountSelect ? parseInt(vehicleCountSelect.value, 10) : NaN;
      const requestBody = (vehicleCount > 0) ? { vehicles: vehicleCount } : {};
      setStatus("신규테스트 요청 중… (MapMatchSvr → Simulator, 최대 3회 재시도)");
      showProgressIndeterminate();
      try {
        const res = await fetch("/api/system/start-engines", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(requestBody),
        });
        const data = await res.json();
        if (data.ok) {
          const appliedVehicles = data.vehicles || vehicleCount || 1;
          setStatus("신규테스트 기동 완료(" + appliedVehicles + "대) — 새 trip 데이터 생성·매칭 대기 중…");
          // "최신 Trip"이 이전에(예: Trip 콤보박스 직접 선택 등으로) 꺼진 채 남아있으면
          // 새로고침해도 브라우저가 체크박스 상태를 그대로 복원해 대기 로직이 아예 동작하지
          // 않는다 — "신규테스트"는 새 trip 을 만들고 지켜보려는 의도이므로 여기서는
          // 무조건 켠다 (2026-07-22 최정우 추가)
          if (chkFollow) chkFollow.checked = true;
          const completed = await waitForNewTestData(prevTripIds, appliedVehicles, NEWTEST_TIMEOUT_MS);
          if (completed) await finishProgress();
        } else {
          // failed_stage 가 없으면(예: 바이너리 자체가 없음) 로그 마지막 줄로 대체 표시 (2026-07-21 최정우 추가)
          const logLines = (data.log || "").trim().split("\n");
          const lastLine = logLines[logLines.length - 1] || "";
          const reason = data.failed_stage ? (data.failed_stage + " 기동 실패") : lastLine;
          setStatus("신규테스트 실패: " + reason, true);
        }
      } catch (err) {
        setStatus("신규테스트 요청 실패: " + err.message, true);
      } finally {
        btn.disabled = false;
        hideProgress();
      }
    });
  }

  function stopPoll() {
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
  }

  function startPoll() {
    stopPoll();
    if (!pollSec || pollSec <= 0) return;
    pollTimer = setInterval(function () {
      refreshTrips(false);
    }, pollSec * 1000);
  }

  // 차량 동시운행 대수 콤보박스 — /api/config 의 sim_vehicles_min/max 로 옵션을 채우고
  // 현재 Simulator/bin/config.ini 값으로 초기 선택 — "신규테스트" 클릭 시 이 값을 그대로
  // 전달해 Simulator 재시작 전에 config.ini 에 반영한다 (2026-07-22 최정우 추가)
  function initVehicleCountSelect(cfg) {
    if (!vehicleCountSelect) return;
    const min = cfg.sim_vehicles_min || 1;
    const max = cfg.sim_vehicles_max || 10;
    vehicleCountSelect.innerHTML = "";
    for (let n = min; n <= max; n++) {
      const opt = document.createElement("option");
      opt.value = String(n);
      opt.textContent = n + "대";
      vehicleCountSelect.appendChild(opt);
    }
    const current = cfg.sim_vehicles || min;
    vehicleCountSelect.value = String(Math.min(max, Math.max(min, current)));
  }

  let lastDisplayPanelSig = "";

  function hideDisplayTripsPanel() {
    if (displayTripsPanel) displayTripsPanel.style.display = "none";
  }

  // 체크리스트 패널 목록 채우기 — trip 목록 시그니처가 안 바뀌면 다시 그리지 않음(체크박스
  //   깜빡임 방지, 기존 fillTripSelect 의 lastTripsSig 패턴과 동일). 포커스 trip(tripSelect
  //   값)은 항상 지도에 보여야 하므로 체크박스를 비활성화한 채 항상 체크 상태로 표시한다
  //   (2026-07-23 최정우 추가)
  function renderDisplayTripsPanel(trips) {
    if (!displayTripsList) return;
    const sig = tripsSignature(trips);
    if (sig === lastDisplayPanelSig) {
      // 목록 구조는 안 바뀌었어도 체크 상태(자동추적 등)는 갱신
      Array.from(displayTripsList.querySelectorAll("input[type=checkbox]")).forEach(function (cb) {
        if (cb.disabled) return;
        cb.checked = checkedDisplayTripIds.has(cb.value);
      });
      return;
    }
    lastDisplayPanelSig = sig;
    displayTripsList.innerHTML = "";
    trips.forEach(function (t) {
      const row = document.createElement("label");
      row.className = "display-trip-row";
      const isFocus = t.trip_id === currentTripId;
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.value = t.trip_id;
      cb.checked = isFocus || checkedDisplayTripIds.has(t.trip_id);
      cb.disabled = isFocus;
      cb.addEventListener("change", function () {
        if (chkFollow) chkFollow.checked = false;
        if (cb.checked) checkedDisplayTripIds.add(t.trip_id);
        else checkedDisplayTripIds.delete(t.trip_id);
        syncMapTrips(renderedTripIds(), true, false);
      });
      const swatch = document.createElement("span");
      swatch.className = "trip-swatch";
      swatch.style.background = getTripColor(t.trip_id);
      const text = document.createElement("span");
      text.textContent = t.trip_id + " (" + t.device_key + ") · " + t.count + "pts" + (isFocus ? " · 포커스" : "");
      row.appendChild(cb);
      row.appendChild(swatch);
      row.appendChild(text);
      displayTripsList.appendChild(row);
    });
  }

  // 현재 지도에 실제로 그려진 trip 들의 색상 범례 — syncMapTrips 가 컨텍스트를 만들고 나면 호출
  //   (2026-07-23 최정우 추가)
  function updateLegendTrips() {
    if (!legendTrips) return;
    legendTrips.innerHTML = "";
    Array.from(tripCtxById.keys()).forEach(function (tripId) {
      const meta = lastTripsData.find(function (t) { return t.trip_id === tripId; });
      const div = document.createElement("div");
      const swatch = document.createElement("span");
      swatch.className = "trip-swatch";
      swatch.style.background = getTripColor(tripId);
      div.appendChild(swatch);
      div.appendChild(document.createTextNode(tripId + (meta ? " (" + meta.device_key + ")" : "")));
      legendTrips.appendChild(div);
    });
  }

  function setupDisplayTripsPanel() {
    if (!btnDisplayTrips || !displayTripsPanel) return;
    btnDisplayTrips.addEventListener("click", function (e) {
      e.stopPropagation();
      const opening = displayTripsPanel.style.display !== "block";
      displayTripsPanel.style.display = opening ? "block" : "none";
    });
    document.addEventListener("click", function (e) {
      if (displayTripsPanel.contains(e.target) || e.target === btnDisplayTrips) return;
      hideDisplayTripsPanel();
    });
    document.addEventListener("scroll", hideDisplayTripsPanel, true);
    window.addEventListener("blur", hideDisplayTripsPanel);
  }

  async function boot() {
    initMap();
    setupTripContextMenu();
    window.addEventListener("error", function (e) {
      setStatus("JS 오류: " + e.message, true);
    });
    // 줌 변경 시 마커 전체를 재생성하지 않고 화살표·라벨 표시만 토글 — 깜빡임/포커스 흔들림 방지 (2026-07-21 최정우 수정)
    map.on("zoomend", function () {
      syncArrowVisibility();
      const showLabel = map.getZoom() >= SEQ_ZOOM_MIN;
      if (showLabel !== showLabelState) {
        showLabelState = showLabel;
        setLabelsVisible(showLabel);
      }
    });
    try {
      await new Promise(function (resolve) { map.whenReady(resolve); });
      const cfg = await fetchJson("/api/config");
      roadBufferM = cfg.road_buffer_m || 1000;
      pollSec = cfg.poll_sec || 0;
      initVehicleCountSelect(cfg);
      if (chkPoll && chkPoll.checked) startPoll();
      await loadTrips();
    } catch (err) {
      setStatus("시작 실패: " + err.message, true);
    }

    tripSelect.addEventListener("change", function () {
      if (chkFollow) chkFollow.checked = false;
      currentTripId = tripSelect.value;
      // 포커스 trip이 바뀌면 체크리스트의 disabled/checked 표시도 즉시 갱신 (2026-07-23 최정우 추가)
      lastDisplayPanelSig = "";
      renderDisplayTripsPanel(lastTripsData);
      syncMapTrips(renderedTripIds(), true, true);
    });
    // 다른 버튼들과 동일하게, 조회가 끝날 때까지 중복 클릭 방지용으로 비활성화 (2026-07-22 최정우 추가)
    document.getElementById("btnReload").addEventListener("click", async function () {
      const btn = this;
      btn.disabled = true;
      showProgressIndeterminate();
      try {
        await refreshTrips(true);
        await finishProgress();
      } finally {
        btn.disabled = false;
        hideProgress();
      }
    });
    setupRetestButton();
    setupResetMapButton();
    setupDeleteButton();
    setupStartEnginesButton();
    setupDisplayTripsPanel();
    if (chkPoll) {
      chkPoll.addEventListener("change", function () {
        if (chkPoll.checked) startPoll();
        else stopPoll();
      });
    }
    if (chkFollow) {
      chkFollow.addEventListener("change", function () {
        refreshTrips(true);
      });
    }
  }

  boot();
})();
