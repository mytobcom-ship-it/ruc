/**
 * RUC 맵매칭 GPS 시각화 (2026-07-10 최정우)
 *  - 주변 도로: prim_link_info (PSF WGS84, trip bbox + road_buffer_m)
 *  - 도로 진행방향: LineString 꼭짓점 순서(f_node → t_node) 화살표
 *  - GPS 빨강 / MATCHED 파랑 / SKIP 주황
 */
(function () {
  const MATCH_STATUS_MATCHED = 1;
  const MATCH_STATUS_SKIP = 3;
  const MATCH_STATUS_ERROR = 4;
  // ROAD_TYPE(prim_link_info) — 시설 유형. 잠수교 등 특정 교량명은 별도 코드 없이 교량(1)에 포함, name 으로 구분 (2026-07-21 최정우 추가)
  const ROAD_TYPE_LABELS = { 0: "일반도로", 1: "교량", 2: "터널", 3: "고가도로", 4: "지하차도" };
  const ARROW_ZOOM_MIN = 14;
  const ARROW_SPACING_M = 45;
  const ARROW_MAX_PER_PATH = 6;
  const SEQ_ZOOM_MIN = 13;

  let map, layerRoads, layerRoadsHalo, layerRoadArrows, layerGpsLine, layerGps, layerMatched, layerSkip;
  let roadBufferM = 1000;
  let pollSec = 0;
  let pollTimer = null;
  let currentTripId = "";
  let lastRoadFc = null;
  let lastRoadSig = "";
  // 점(gps_seq)별로 이미 그려진 마커/연결선을 추적 → 폴링·줌마다 전체 재생성하지 않고
  //   변경분만 갱신해 깜빡임·포커스 흔들림을 줄인다 (2026-07-21 최정우 추가)
  let pointLayerBySeq = new Map();
  let gpsTrailLine = null;
  let showLabelState = null;
  let arrowsVisibleState = null;

  const statusEl = document.getElementById("status");
  const tripSelect = document.getElementById("tripSelect");
  const chkPoll = document.getElementById("chkPoll");
  const chkFollow = document.getElementById("chkFollow");
  const ctxMenu = document.getElementById("ctxMenu");
  const ctxCopy = document.getElementById("ctxCopy");

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
    map = L.map("map");
    // 밝은 무료 베이스맵 — 도로 오버레이 가독성 (2026-07-10 최정우)
    L.tileLayer("https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png", {
      attribution: "&copy; OpenStreetMap &copy; CARTO",
      subdomains: "abcd",
      // 타일 네이티브 한계(20) 이상은 업스케일로 확대 허용 (2026-07-15 최정우 수정)
      maxNativeZoom: 20,
      maxZoom: 24,
    }).addTo(map);
    map.setView([37.55, 126.98], 14);

    // 마커가 항상 연결선(INTERSECT 히트선) 위에 오도록 전용 pane 생성 (2026-07-15 최정우 추가)
    //   기존엔 넓은 투명 히트선(weight 12)이 마커보다 나중에 그려져 마커 중앙 마우스오버를 가로챔
    map.createPane("paneConnectors");
    map.getPane("paneConnectors").style.zIndex = 420;
    map.createPane("panePoints");
    map.getPane("panePoints").style.zIndex = 450;

    layerRoadsHalo = L.layerGroup().addTo(map);
    layerRoads = L.layerGroup().addTo(map);
    layerRoadArrows = L.layerGroup().addTo(map);
    layerGpsLine = L.layerGroup().addTo(map);
    layerGps = L.layerGroup().addTo(map);
    layerMatched = L.layerGroup().addTo(map);
    layerSkip = L.layerGroup().addTo(map);
  }

  function clearPointLayers() {
    layerGpsLine.clearLayers();
    layerGps.clearLayers();
    layerMatched.clearLayers();
    layerSkip.clearLayers();
    pointLayerBySeq = new Map();
    gpsTrailLine = null;
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
      },
    }).addTo(layerRoads);
    rebuildRoadArrows();
  }

  function popupHtml(p) {
    return (
      "<b>seq</b> " + p.gps_seq + "<br>" +
      "<b>status</b> " + p.match_status +
      // 매칭 LinkID·도로명 (엔진 저장값) 표시 (2026-07-15 최정우 수정)
      (p.match_link_id ? "<br><b>match link</b> " + p.match_link_id +
        (p.match_link_name ? " (" + p.match_link_name + ")" : "") : "") +
      (p.intersect_len != null ? "<br><b>intersect_len</b> " + p.intersect_len + "m (GPS↔세그먼트)" : "") +
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

  function removePointEntry(entry) {
    if (entry.gpsMarker) layerGps.removeLayer(entry.gpsMarker);
    if (entry.matchMarker) (entry.isSkip ? layerSkip : layerMatched).removeLayer(entry.matchMarker);
    if (entry.connDash) layerGpsLine.removeLayer(entry.connDash);
    if (entry.connHit) layerGpsLine.removeLayer(entry.connHit);
  }

  function buildPointEntry(p, showLabel) {
    const matchFailed = isMatchFailed(p.match_status);
    const entry = { sig: pointSig(p), isSkip: p.match_status === MATCH_STATUS_SKIP };
    let gpsLl = null;
    if (p.gps_lat != null && p.gps_lon != null) {
      gpsLl = [p.gps_lat, p.gps_lon];
      entry.gpsMarker = addPointMarker(
        gpsLl,
        { radius: 6, color: "#b71c1c", fillColor: "#e53935", fillOpacity: 0.95, weight: 2 },
        "G" + p.gps_seq,
        "tip-gps",
        TIP_DIRS_GPS[p.gps_seq % TIP_DIRS_GPS.length],
        popupHtml(p),
        layerGps,
        showLabel
      );
    }

    if (p.match_lat == null || p.match_lon == null) return entry;

    const trueMatchLl = [p.match_lat, p.match_lon];
    const layer = entry.isSkip ? layerSkip : layerMatched;
    entry.matchMarker = addPointMarker(
      trueMatchLl,
      {
        radius: 7,
        color: entry.isSkip ? "#e65100" : "#0d47a1",
        fillColor: entry.isSkip ? "#fb8c00" : "#1e88e5",
        fillOpacity: 0.95,
        weight: 2,
      },
      "M" + p.gps_seq,
      tipClassName(entry.isSkip ? "tip-skip" : "tip-match", matchFailed),
      TIP_DIRS_MATCH[p.gps_seq % TIP_DIRS_MATCH.length],
      popupHtml(p),
      layer,
      showLabel
    );

    // INTERSECT_LEN: GPS(G) ↔ 세그먼트 교차점(MATCH_LAT/LON) 거리 시각화 + 마우스오버 툴팁(m) (2026-07-15 최정우 수정)
    if (gpsLl && map.distance(gpsLl, trueMatchLl) > 0.8) {
      // INTERSECT_LEN(DB, m) 우선, 없으면 화면상 거리로 계산
      const distM = (p.intersect_len != null)
        ? p.intersect_len
        : Math.round(map.distance(gpsLl, trueMatchLl));
      const tipText = "seq " + p.gps_seq + " · INTERSECT_LEN " + distM + " m (GPS↔매칭)";
      // 표시용 점선 (연결선 pane → 마커 아래)
      entry.connDash = L.polyline([gpsLl, trueMatchLl], {
        pane: "paneConnectors", color: "#94a3b8", weight: 1, opacity: 0.55, dashArray: "2,4",
      }).addTo(layerGpsLine);
      // 마우스오버 히트영역(투명) — sticky 툴팁으로 거리(m) 표시 (마커 아래 pane, 폭 축소)
      entry.connHit = L.polyline([gpsLl, trueMatchLl], {
        pane: "paneConnectors", color: "#000000", weight: 8, opacity: 0,
      })
        .bindTooltip(tipText, { sticky: true, direction: "top", className: "tip-intersect", opacity: 0.95 })
        .addTo(layerGpsLine);
    }
    return entry;
  }

  // GPS 궤적 점선 — 매번 새로 만들지 않고 좌표만 갱신 (2026-07-21 최정우 수정)
  function rebuildGpsTrail(points) {
    const gpsLatLngs = [];
    points.forEach(function (p) {
      if (p.gps_lat != null && p.gps_lon != null) gpsLatLngs.push([p.gps_lat, p.gps_lon]);
    });
    if (gpsLatLngs.length < 2) {
      if (gpsTrailLine) {
        layerGpsLine.removeLayer(gpsTrailLine);
        gpsTrailLine = null;
      }
      return;
    }
    if (gpsTrailLine) {
      gpsTrailLine.setLatLngs(gpsLatLngs);
    } else {
      gpsTrailLine = L.polyline(gpsLatLngs, {
        pane: "paneConnectors", color: "#e53935", weight: 2, opacity: 0.55, dashArray: "4,6",
      }).addTo(layerGpsLine);
    }
  }

  // gps_seq 별로 이전 렌더링과 비교해 바뀐 점만 다시 그림 — 폴링마다 전체 재생성하던 깜빡임 제거 (2026-07-21 최정우 수정)
  function updatePoints(points) {
    const showLabel = map.getZoom() >= SEQ_ZOOM_MIN;
    showLabelState = showLabel;
    const seen = new Set();
    points.forEach(function (p) {
      seen.add(p.gps_seq);
      const sig = pointSig(p);
      const existing = pointLayerBySeq.get(p.gps_seq);
      if (existing && existing.sig === sig) return;
      if (existing) removePointEntry(existing);
      pointLayerBySeq.set(p.gps_seq, buildPointEntry(p, showLabel));
    });
    // trip 리셋 등으로 사라진 seq 가 있으면 정리 (평소엔 비어있는 no-op)
    pointLayerBySeq.forEach(function (entry, seq) {
      if (seen.has(seq)) return;
      removePointEntry(entry);
      pointLayerBySeq.delete(seq);
    });
    rebuildGpsTrail(points);
  }

  // 줌 변경 시 라벨 표시/숨김만 토글 — 마커 재생성 없음 (2026-07-21 최정우 추가)
  function setLabelsVisible(visible) {
    pointLayerBySeq.forEach(function (entry) {
      if (entry.gpsMarker) {
        const tt = entry.gpsMarker.getTooltip();
        if (tt) tt.setOpacity(visible ? 0.9 : 0);
      }
      if (entry.matchMarker) {
        const tt = entry.matchMarker.getTooltip();
        if (tt) tt.setOpacity(visible ? 0.9 : 0);
      }
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

  function viewportHasPoints(points) {
    if (!points || points.length === 0) return false;
    const bounds = map.getBounds();
    return points.some(function (p) {
      if (p.gps_lat != null && p.gps_lon != null && bounds.contains([p.gps_lat, p.gps_lon])) return true;
      if (p.match_lat != null && p.match_lon != null && bounds.contains([p.match_lat, p.match_lon])) return true;
      return false;
    });
  }

  function shouldAutoFit(points, tripChanged, forceFit) {
    if (forceFit || tripChanged) return true;
    // "최신 Trip" 체크 해제 시(수동 탐색 중)는 폴링으로 뷰를 강제 이동시키지 않음
    //   — 확대해서 보는 도중 포커스가 튀는 문제 방지 (2026-07-21 최정우 수정)
    return isFollowLatest() && !viewportHasPoints(points);
  }

  function fillTripSelect(trips, selectedId, followLatest) {
    const prev = followLatest ? "" : (selectedId || tripSelect.value || currentTripId);
    tripSelect.innerHTML = "";
    trips.forEach(function (t) {
      const opt = document.createElement("option");
      opt.value = t.trip_id;
      opt.textContent = t.trip_id + " (" + t.count + "pts)";
      tripSelect.appendChild(opt);
    });
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
    clearPointLayers();
    clearRoadLayers();
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

  async function refreshTrips(forceFit) {
    try {
      const trips = await fetchTrips();
      if (trips.length === 0) {
        clearAllDisplay("DB 데이터 없음 — 삭제 후 정상 (엔진 기동 시 새 데이터 생성)", true);
        return;
      }
      const prevTripId = currentTripId;
      const followLatest = isFollowLatest();
      const tripId = fillTripSelect(trips, currentTripId, followLatest);
      const tripChanged = !!tripId && tripId !== prevTripId;
      await loadTrip(tripId, false, forceFit || tripChanged);
    } catch (err) {
      setStatus("API 오류: " + err.message + " (웹/DB 기동 확인)", true);
    }
  }

  async function loadTrips() {
    await refreshTrips(true);
  }

  async function loadTrip(tripId, manualFit, forceFit) {
    if (!tripId) return;
    const prevTripId = currentTripId;
    currentTripId = tripId;
    setStatus("로딩… " + tripId);
    try {
      const [roads, points, primInfo] = await Promise.all([
        fetchJson("/api/trip/" + encodeURIComponent(tripId) + "/prim-roads?buffer=" + roadBufferM),
        fetchJson("/api/trip/" + encodeURIComponent(tripId) + "/points"),
        fetchJson("/api/prim/info"),
      ]);
      if (!roads.features) {
        throw new Error("roads 응답 형식 오류");
      }
      if (!Array.isArray(points)) {
        throw new Error("points 응답 형식 오류");
      }
      if (points.length === 0) {
        clearAllDisplay("선택 trip 데이터 없음 (DB 삭제됨) — 목록 갱신 중", true);
        await refreshTrips(false);
        return;
      }
      const tripChanged = tripId !== prevTripId;
      // trip 이 바뀌면 gps_seq 체계가 리셋되므로 이전 마커를 전부 정리 (2026-07-21 최정우 추가)
      if (tripChanged) clearPointLayers();
      updateRoadsIfChanged(roads);
      updatePoints(points);
      if (shouldAutoFit(points, tripChanged, !!manualFit || !!forceFit)) {
        fitToContent(points);
      }
      const now = new Date();
      const ts = now.toLocaleTimeString("ko-KR", { hour12: false });
      setStatus(
        "[" + ts + "] trip=" + tripId +
        " prim도로=" + roads.features.length +
        " (전체 " + (primInfo.count || "?") + ")" +
        " GPS=" + points.filter(function (p) { return p.gps_lat != null; }).length +
        " 매칭=" + points.filter(function (p) { return p.match_lat != null; }).length +
        " 버퍼=" + roadBufferM + "m"
      );
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
        // 엔진 재폴링·재매칭 대기 후 결과 갱신 (2026-07-21 최정우 추가)
        setTimeout(function () {
          if (currentTripId === tripId) loadTrip(tripId, false, false);
        }, 2000);
      } catch (err) {
        setStatus("재테스트 실패: " + err.message, true);
      } finally {
        btn.disabled = false;
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
      if (pollSec > 0 && chkPoll) chkPoll.checked = true;
      if (chkPoll && chkPoll.checked) startPoll();
      await loadTrips();
    } catch (err) {
      setStatus("시작 실패: " + err.message, true);
    }

    tripSelect.addEventListener("change", function () {
      if (chkFollow) chkFollow.checked = false;
      loadTrip(tripSelect.value, true, true);
    });
    document.getElementById("btnReload").addEventListener("click", function () {
      refreshTrips(true);
    });
    setupRetestButton();
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
