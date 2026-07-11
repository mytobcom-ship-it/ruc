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
  let lastPoints = null;

  const statusEl = document.getElementById("status");
  const tripSelect = document.getElementById("tripSelect");
  const chkPoll = document.getElementById("chkPoll");
  const chkFollow = document.getElementById("chkFollow");

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
      maxZoom: 20,
    }).addTo(map);
    map.setView([37.55, 126.98], 14);

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
    return L.marker(latlng, {
      interactive: false,
      icon: L.divIcon({
        className: "road-dir-arrow",
        html:
          '<svg width="11" height="11" viewBox="0 0 11 11" ' +
          'style="transform:rotate(' + bearingDeg + 'deg);display:block">' +
          '<path d="M1.5 1.5 L9.5 5.5 L1.5 9.5 Z" fill="#1e40af" stroke="#ffffff" stroke-width="0.6"/>' +
          "</svg>",
        iconSize: [11, 11],
        iconAnchor: [5.5, 5.5],
      }),
    });
  }

  function rebuildRoadArrows() {
    layerRoadArrows.clearLayers();
    if (!lastRoadFc || !lastRoadFc.features || map.getZoom() < ARROW_ZOOM_MIN) return;

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
        const title = p.name ? p.name + " (" + lid + ")" : "link: " + lid;
        layer.bindTooltip(title, { sticky: true });
        const popup =
          "<b>link</b> " + lid +
          (p.name ? "<br><b>도로명</b> " + p.name : "") +
          (p.len != null ? "<br><b>len</b> " + p.len + "m" : "") +
          (p.st_nd_name ? "<br><b>출발</b> " + p.st_nd_name : (p.st_nd_id ? "<br><b>출발</b> " + p.st_nd_id : "")) +
          (p.ed_nd_name ? "<br><b>도착</b> " + p.ed_nd_name : (p.ed_nd_id ? "<br><b>도착</b> " + p.ed_nd_id : ""));
        layer.bindPopup(popup);
      },
    }).addTo(layerRoads);
    rebuildRoadArrows();
  }

  function popupHtml(p) {
    return (
      "<b>seq</b> " + p.gps_seq + "<br>" +
      "<b>status</b> " + p.match_status +
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

  function addPointMarker(latlng, style, label, tipClass, dir, popup, layer) {
    const m = L.circleMarker(latlng, style).bindPopup(popup);
    if (label) {
      m.bindTooltip(label, {
        permanent: true,
        direction: dir,
        className: tipClass,
        offset: dir === "top" ? [0, -8] : dir === "bottom" ? [0, 8] : dir === "left" ? [-8, 0] : [8, 0],
      });
    }
    m.addTo(layer);
    return m;
  }

  function renderPoints(points) {
    clearPointLayers();
    const gpsLatLngs = [];
    const showLabel = map.getZoom() >= SEQ_ZOOM_MIN;

    points.forEach(function (p) {
      if (p.gps_lat != null && p.gps_lon != null) {
        gpsLatLngs.push([p.gps_lat, p.gps_lon]);
      }
    });

    points.forEach(function (p) {
      const matchFailed = isMatchFailed(p.match_status);
      let gpsLl = null;
      if (p.gps_lat != null && p.gps_lon != null) {
        gpsLl = [p.gps_lat, p.gps_lon];
        addPointMarker(
          gpsLl,
          { radius: 6, color: "#b71c1c", fillColor: "#e53935", fillOpacity: 0.95, weight: 2 },
          showLabel ? "G" + p.gps_seq : null,
          "tip-gps",
          TIP_DIRS_GPS[p.gps_seq % TIP_DIRS_GPS.length],
          popupHtml(p),
          layerGps
        );
      }

      if (p.match_lat == null || p.match_lon == null) return;

      const trueMatchLl = [p.match_lat, p.match_lon];
      const isSkip = p.match_status === MATCH_STATUS_SKIP;
      const layer = isSkip ? layerSkip : layerMatched;
      addPointMarker(
        trueMatchLl,
        {
          radius: 7,
          color: isSkip ? "#e65100" : "#0d47a1",
          fillColor: isSkip ? "#fb8c00" : "#1e88e5",
          fillOpacity: 0.95,
          weight: 2,
        },
        showLabel ? "M" + p.gps_seq : null,
        tipClassName(isSkip ? "tip-skip" : "tip-match", matchFailed),
        TIP_DIRS_MATCH[p.gps_seq % TIP_DIRS_MATCH.length],
        popupHtml(p),
        layer
      );

      // INTERSECT_LEN: GPS(G) ↔ 세그먼트 교차점(MATCH_LAT/LON) 거리 시각화
      if (gpsLl && map.distance(gpsLl, trueMatchLl) > 0.8) {
        L.polyline([gpsLl, trueMatchLl], {
          color: "#94a3b8", weight: 1, opacity: 0.55, dashArray: "2,4",
        }).addTo(layerGpsLine);
      }
    });

    if (gpsLatLngs.length >= 2) {
      L.polyline(gpsLatLngs, { color: "#e53935", weight: 2, opacity: 0.55, dashArray: "4,6" })
        .addTo(layerGpsLine);
    }
  }

  function drawPoints(points) {
    lastPoints = points;
    renderPoints(points);
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
    if (bounds.isValid()) map.fitBounds(bounds.pad(0.12), { maxZoom: 17 });
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
    return !viewportHasPoints(points);
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
    lastPoints = null;
    clearPointLayers();
    clearRoadLayers();
    tripSelect.innerHTML = '<option value="">(trip 없음)</option>';
    if (statusMsg) setStatus(statusMsg, isError);
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
      addRoadGeoJson(roads);
      drawPoints(points);
      const tripChanged = tripId !== prevTripId;
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
    window.addEventListener("error", function (e) {
      setStatus("JS 오류: " + e.message, true);
    });
    map.on("zoomend", function () {
      rebuildRoadArrows();
      if (lastPoints) renderPoints(lastPoints);
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
