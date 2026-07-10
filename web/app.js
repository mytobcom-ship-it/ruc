/**
 * RUC 맵매칭 GPS 시각화 (2026-07-10 최정우)
 *  - 주변 도로: moct_link (trip bbox + road_buffer_m, 기본 1000m)
 *  - GPS 빨강 / MATCHED 파랑 / SKIP 주황
 */
(function () {
  const MATCH_STATUS_MATCHED = 1;
  const MATCH_STATUS_SKIP = 3;

  let map, layerRoads, layerRoadsHalo, layerGpsLine, layerGps, layerMatched, layerSkip;
  let roadBufferM = 1000;
  let pollSec = 0;
  let pollTimer = null;
  let currentTripId = "";

  const statusEl = document.getElementById("status");
  const tripSelect = document.getElementById("tripSelect");
  const chkPoll = document.getElementById("chkPoll");

  function setStatus(msg) {
    statusEl.textContent = msg;
  }

  function initMap() {
    map = L.map("map", { preferCanvas: true });
    // 밝은 무료 베이스맵 — 도로 오버레이 가독성 (2026-07-10 최정우)
    L.tileLayer("https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png", {
      attribution: "&copy; OpenStreetMap &copy; CARTO",
      subdomains: "abcd",
      maxZoom: 20,
    }).addTo(map);
    map.setView([37.55, 126.98], 14);

    layerRoadsHalo = L.layerGroup().addTo(map);
    layerRoads = L.layerGroup().addTo(map);
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
  }

  function addRoadGeoJson(fc) {
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
        const lid = feat.properties && feat.properties.link_id;
        if (lid) layer.bindTooltip("link: " + lid, { sticky: true });
      },
    }).addTo(layerRoads);
  }

  function popupHtml(p) {
    return (
      "<b>seq</b> " + p.gps_seq + "<br>" +
      "<b>status</b> " + p.match_status +
      (p.intersect_len != null ? "<br><b>intersect</b> " + p.intersect_len + "m" : "") +
      "<br><b>gps_dt</b> " + p.gps_dt
    );
  }

  function drawPoints(points) {
    clearPointLayers();
    const gpsLatLngs = [];

    points.forEach(function (p) {
      if (p.gps_lat != null && p.gps_lon != null) {
        const ll = [p.gps_lat, p.gps_lon];
        gpsLatLngs.push(ll);
        L.circleMarker(ll, {
          radius: 5, color: "#b71c1c", fillColor: "#e53935", fillOpacity: 0.95, weight: 1,
        }).bindPopup(popupHtml(p)).addTo(layerGps);
      }

      const hasMatch = p.match_lat != null && p.match_lon != null;
      if (!hasMatch) return;

      const mll = [p.match_lat, p.match_lon];
      if (p.match_status === MATCH_STATUS_MATCHED) {
        L.circleMarker(mll, {
          radius: 6, color: "#0d47a1", fillColor: "#1e88e5", fillOpacity: 0.95, weight: 1,
        }).bindPopup(popupHtml(p)).addTo(layerMatched);
      } else if (p.match_status === MATCH_STATUS_SKIP) {
        L.circleMarker(mll, {
          radius: 6, color: "#e65100", fillColor: "#fb8c00", fillOpacity: 0.95, weight: 1,
        }).bindPopup(popupHtml(p)).addTo(layerSkip);
      }
    });

    if (gpsLatLngs.length >= 2) {
      L.polyline(gpsLatLngs, { color: "#e53935", weight: 2, opacity: 0.5, dashArray: "4,6" })
        .addTo(layerGpsLine);
    }
  }

  function fitToContent(points) {
    const bounds = L.latLngBounds([]);
    points.forEach(function (p) {
      if (p.gps_lat != null && p.gps_lon != null) bounds.extend([p.gps_lat, p.gps_lon]);
      if (p.match_lat != null && p.match_lon != null) bounds.extend([p.match_lat, p.match_lon]);
    });
    layerRoads.eachLayer(function (ly) {
      if (ly.getBounds) bounds.extend(ly.getBounds());
    });
    if (bounds.isValid()) map.fitBounds(bounds.pad(0.08));
  }

  function fillTripSelect(trips, selectedId) {
    const prev = selectedId || tripSelect.value || currentTripId;
    tripSelect.innerHTML = "";
    trips.forEach(function (t) {
      const opt = document.createElement("option");
      opt.value = t.trip_id;
      opt.textContent = t.trip_id + " (" + t.count + "pts)";
      tripSelect.appendChild(opt);
    });
    if (trips.length === 0) return "";
    if (prev && trips.some(function (t) { return t.trip_id === prev; })) {
      tripSelect.value = prev;
      return prev;
    }
    tripSelect.value = trips[0].trip_id;
    return trips[0].trip_id;
  }

  async function fetchTrips() {
    const res = await fetch("/api/trips?limit=50");
    return res.json();
  }

  async function refreshTrips(fit) {
    const trips = await fetchTrips();
    if (trips.length === 0) {
      currentTripId = "";
      clearPointLayers();
      clearRoadLayers();
      setStatus("prim_rawgps 데이터 없음 — 시뮬/맵매칭 기동 후 새로고침");
      return;
    }
    const hadTrip = !!currentTripId;
    const tripId = fillTripSelect(trips, currentTripId);
    const shouldFit = fit || !hadTrip || tripId !== currentTripId;
    await loadTrip(tripId, shouldFit);
  }

  async function loadTrips() {
    await refreshTrips(true);
  }

  async function loadTrip(tripId, fit) {
    if (!tripId) return;
    currentTripId = tripId;
    setStatus("로딩… " + tripId);
    const [roadsRes, ptsRes] = await Promise.all([
      fetch("/api/trip/" + encodeURIComponent(tripId) + "/roads?buffer=" + roadBufferM),
      fetch("/api/trip/" + encodeURIComponent(tripId) + "/points"),
    ]);
    const roads = await roadsRes.json();
    const points = await ptsRes.json();
    addRoadGeoJson(roads);
    drawPoints(points);
    if (fit) fitToContent(points);
    setStatus(
      "trip=" + tripId +
      " 도로=" + (roads.features ? roads.features.length : 0) +
      " 점=" + points.length +
      " 버퍼=" + roadBufferM + "m"
    );
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
    const cfgRes = await fetch("/api/config");
    const cfg = await cfgRes.json();
    roadBufferM = cfg.road_buffer_m || 1000;
    pollSec = cfg.poll_sec || 0;
    chkPoll.checked = pollSec > 0;
    if (chkPoll.checked) startPoll();

    await loadTrips();

    tripSelect.addEventListener("change", function () {
      loadTrip(tripSelect.value, true);
    });
    document.getElementById("btnReload").addEventListener("click", function () {
      refreshTrips(false);
    });
    chkPoll.addEventListener("change", function () {
      if (chkPoll.checked) startPoll();
      else stopPoll();
    });
  }

  boot();
})();
