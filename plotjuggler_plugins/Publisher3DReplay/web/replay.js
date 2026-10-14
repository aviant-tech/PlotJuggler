// 3D replay of a PlotJuggler time cursor. PlotJuggler sends the trajectory once
// ({type: "trajectory"}) and then every move of its time tracker ({type: "time"}).
// Times are PlotJuggler's absolute time in seconds; Cesium needs a JulianDate,
// so they are offset from an arbitrary epoch.
"use strict";

const EPOCH = Cesium.JulianDate.fromIso8601("2000-01-01T00:00:00Z");
// height of the model origin above the ground at rest, when the model does not tell
const DEFAULT_GROUND_CLEARANCE = 0.25;
const PATH_MIN_INTERVAL = 0.2;
// a motor with no command for this long is stopped (PlotJuggler drops its NaN samples)
const MOTOR_TIMEOUT = 0.5;
// props turn at a rate the eye can follow, not their real one: rad/s at command 0 and 1
const PROP_RATE_MIN = 10;
const PROP_RATE_MAX = 50;
// Notus 4 winch, body forward-left-up [m]: where the line leaves the receptacle,
// the package origin when it is locked under the belly, and its line attachment
const WINCH_LINE_EXIT = new Cesium.Cartesian3(0.082, 0, -0.165);
const PACKAGE_STOWED = new Cesium.Cartesian3(0.082, 0, -0.304);
const PACKAGE_ATTACH_HEIGHT = 0.12;
// shorter than this the line is in, and the package locked
const WINCH_STOWED_LENGTH = 0.05;

// msg/VehicleStatus.msg NAVIGATION_STATE_*
const NAV_STATES = {
  0: "Manual", 1: "Altitude", 2: "Position", 3: "Mission", 4: "Hold", 5: "Return",
  6: "Position slow", 10: "Acro", 12: "Descend", 13: "Termination", 14: "Offboard",
  15: "Stabilized", 17: "Takeoff", 18: "Land", 19: "Follow target", 20: "Precision land",
  21: "Orbit", 22: "VTOL takeoff", 31: "Mission RTL",
};

function toJulian(t) {
  return Cesium.JulianDate.addSeconds(EPOCH, t, new Cesium.JulianDate());
}

// NED -> ENU and body FLU (the model frame) -> FRD (PX4 body frame)
const R_ENU_NED = new Cesium.Matrix3(0, 1, 0, 1, 0, 0, 0, 0, -1);
const Q_FRD_FLU = Cesium.Quaternion.fromRotationMatrix(new Cesium.Matrix3(1, 0, 0, 0, -1, 0, 0, 0, -1));

/// position: {t, v: [lat, lon, alt, ...]}, heightOffset added to alt.
/// Returns the property and its sample positions.
function buildPositionProperty(position, heightOffset) {
  const property = new Cesium.SampledPositionProperty();
  const times = [];
  const values = [];
  for (let i = 0; i < position.t.length; i++) {
    const v = position.v;
    times.push(toJulian(position.t[i]));
    values.push(Cesium.Cartesian3.fromDegrees(v[3 * i + 1], v[3 * i], v[3 * i + 2] + heightOffset));
  }
  property.addSamples(times, values);
  return { property, positions: values };
}

/// The model's orientation in ECEF: attitude {t, v: [w, x, y, z, ...]} is PX4's
/// body FRD -> NED rotation; the local NED frame is taken at the vehicle position.
function buildOrientationProperty(attitude, positionProperty, firstPosition) {
  const property = new Cesium.SampledProperty(Cesium.Quaternion);
  const times = [];
  const values = [];
  const enu = new Cesium.Matrix4();
  const rotation = new Cesium.Matrix3();
  let previous;
  for (let i = 0; i < attitude.t.length; i++) {
    const v = attitude.v;
    const time = toJulian(attitude.t[i]);
    const position = positionProperty.getValue(time) || firstPosition;
    Cesium.Transforms.eastNorthUpToFixedFrame(position, Cesium.Ellipsoid.WGS84, enu);
    Cesium.Matrix4.getMatrix3(enu, rotation);
    Cesium.Matrix3.multiply(rotation, R_ENU_NED, rotation);
    const qEcefNed = Cesium.Quaternion.fromRotationMatrix(rotation);
    const qNedFrd = new Cesium.Quaternion(v[4 * i + 1], v[4 * i + 2], v[4 * i + 3], v[4 * i]);
    const q = Cesium.Quaternion.multiply(qEcefNed, qNedFrd, new Cesium.Quaternion());
    Cesium.Quaternion.multiply(q, Q_FRD_FLU, q);
    Cesium.Quaternion.normalize(q, q);
    // keep the sign continuous so interpolation takes the short way round
    if (previous && Cesium.Quaternion.dot(previous, q) < 0) {
      Cesium.Quaternion.negate(q, q);
    }
    previous = q;
    times.push(time);
    values.push(q);
  }
  property.addSamples(times, values);
  return property;
}

/// Index of the last sample at or before t, -1 if none.
function sampleIndexAt(times, t) {
  let low = 0;
  let high = times.length - 1;
  if (high < 0 || t < times[0]) return -1;
  while (low < high) {
    const mid = (low + high + 1) >> 1;
    if (times[mid] <= t) low = mid; else high = mid - 1;
  }
  return low;
}

function nodeMatrix(node) {
  if (node.matrix) return Cesium.Matrix4.fromColumnMajorArray(node.matrix);
  const t = node.translation || [0, 0, 0];
  const r = node.rotation || [0, 0, 0, 1];
  const s = node.scale || [1, 1, 1];
  return Cesium.Matrix4.fromTranslationQuaternionRotationScale(
    new Cesium.Cartesian3(t[0], t[1], t[2]), new Cesium.Quaternion(r[0], r[1], r[2], r[3]),
    new Cesium.Cartesian3(s[0], s[1], s[2]));
}

/// What the page needs to know about a .glb:
///  clearance: how far its lowest point is below its origin, from the bounds of
///    the POSITION accessors (glTF requires them), through the node transforms
///  rotors: [{node, motor, direction}] from node extras {motor, direction}
///    (tools/sdf_to_glb.py writes them), to spin the node about its local Z
function parseGlb(arrayBuffer) {
  const view = new DataView(arrayBuffer);
  if (view.getUint32(0, true) !== 0x46546c67) return undefined;  // "glTF"
  const jsonLength = view.getUint32(12, true);
  const gltf = JSON.parse(new TextDecoder().decode(new Uint8Array(arrayBuffer, 20, jsonLength)));
  const nodes = gltf.nodes || [];
  let lowest = Infinity;
  const rotors = [];
  const corner = new Cesium.Cartesian3();
  const visit = (index, parent) => {
    const node = nodes[index];
    const matrix = Cesium.Matrix4.multiply(parent, nodeMatrix(node), new Cesium.Matrix4());
    const extras = node.extras || {};
    if (node.name && Number.isInteger(extras.motor)) {
      rotors.push({ node: node.name, motor: extras.motor, direction: extras.direction < 0 ? -1 : 1 });
    }
    const mesh = node.mesh !== undefined && gltf.meshes[node.mesh];
    for (const primitive of (mesh && mesh.primitives) || []) {
      const accessor = gltf.accessors[primitive.attributes.POSITION];
      if (!accessor || !accessor.min || !accessor.max) continue;
      for (let i = 0; i < 8; i++) {
        corner.x = (i & 1 ? accessor.max : accessor.min)[0];
        corner.y = (i & 2 ? accessor.max : accessor.min)[1];
        corner.z = (i & 4 ? accessor.max : accessor.min)[2];
        lowest = Math.min(lowest, Cesium.Matrix4.multiplyByPoint(matrix, corner, corner).y);
      }
    }
    for (const child of node.children || []) visit(child, matrix);
  };
  const scene = gltf.scenes ? gltf.scenes[gltf.scene || 0] : { nodes: nodes.map((_, i) => i) };
  for (const index of scene.nodes || []) visit(index, Cesium.Matrix4.IDENTITY);
  return { clearance: Number.isFinite(lowest) ? -lowest : undefined, rotors };
}

/// The command of one motor at t: its last sample, 0 if it is older than MOTOR_TIMEOUT.
function motorCommand(motor, t) {
  const index = sampleIndexAt(motor.t, t);
  return index >= 0 && t - motor.t[index] <= MOTOR_TIMEOUT ? motor.v[index] : 0;
}

/// Linear interpolation of a {t, v} series with one value per sample, clamped at the ends.
function interpolate(series, t) {
  const index = sampleIndexAt(series.t, t);
  if (index < 0) return series.v[0];
  if (index >= series.t.length - 1) return series.v[series.t.length - 1];
  const f = (t - series.t[index]) / (series.t[index + 1] - series.t[index]);
  return series.v[index] + f * (series.v[index + 1] - series.v[index]);
}

function decimate(position, minInterval) {
  const result = [];
  let last = -Infinity;
  for (let i = 0; i < position.t.length; i++) {
    if (position.t[i] - last >= minInterval || i === position.t.length - 1) {
      result.push(i);
      last = position.t[i];
    }
  }
  return result;
}

if (typeof module !== "undefined") {
  module.exports = {
    toJulian, buildPositionProperty, buildOrientationProperty, sampleIndexAt, parseGlb, motorCommand, interpolate,
  };
}

if (typeof window !== "undefined") {
  const statusEl = document.getElementById("status");
  const timeEl = document.getElementById("time");
  const modeEl = document.getElementById("mode");
  const followEl = document.getElementById("follow");
  const pathEl = document.getElementById("path");

  let viewer;
  let ionToken;
  let vehicle;
  let path;
  let trajectory;
  let currentTime;
  let loading = Promise.resolve();

  async function createViewer(token) {
    if (token) Cesium.Ion.defaultAccessToken = token;
    ionToken = token;
    let terrainProvider;
    let baseLayer;
    try {
      terrainProvider = await Cesium.createWorldTerrainAsync({ requestVertexNormals: true, requestWaterMask: true });
    } catch (error) {
      console.warn("Cesium ion unavailable, using a flat globe and OpenStreetMap", error);
      terrainProvider = new Cesium.EllipsoidTerrainProvider();
      baseLayer = new Cesium.ImageryLayer(new Cesium.OpenStreetMapImageryProvider({ url: "https://tile.openstreetmap.org/" }));
    }
    viewer = new Cesium.Viewer("viewer", {
      terrainProvider,
      ...(baseLayer ? { baseLayer } : {}),
      animation: false,
      timeline: false,
      geocoder: false,
      homeButton: false,
      sceneModePicker: false,
      navigationHelpButton: false,
      infoBox: false,
      selectionIndicator: false,
      fullscreenButton: false,
      shouldAnimate: false,
      // only render when the time or the camera changes
      requestRenderMode: true,
      maximumRenderTimeChange: Infinity,
    });
    viewer.scene.globe.depthTestAgainstTerrain = true;
    viewer.clock.clockRange = Cesium.ClockRange.UNBOUNDED;
  }

  async function modelInfo(uri) {
    try {
      const response = await fetch(uri);
      const info = parseGlb(await response.arrayBuffer());
      if (info) return { clearance: info.clearance ?? DEFAULT_GROUND_CLEARANCE, rotors: info.rotors };
    } catch (error) {
      console.warn("Cannot read the model", error);
    }
    return { clearance: DEFAULT_GROUND_CLEARANCE, rotors: [] };
  }

  async function groundOffset(position, clearance) {
    const start = Cesium.Cartographic.fromDegrees(position.v[1], position.v[0]);
    let groundHeight = 0;
    if (!(viewer.terrainProvider instanceof Cesium.EllipsoidTerrainProvider)) {
      try {
        const [sampled] = await Cesium.sampleTerrainMostDetailed(viewer.terrainProvider, [start]);
        groundHeight = sampled.height || 0;
      } catch (error) {
        console.warn("Terrain sampling failed", error);
      }
    }
    // assume the log starts on the ground
    return groundHeight - position.v[2] + clearance;
  }

  async function loadTrajectory(message) {
    if (viewer && message.ion_token !== ionToken) {
      // the ion token can only be set before the viewer is created
      location.reload();
      return;
    }
    if (!viewer) await createViewer(message.ion_token);
    viewer.entities.removeAll();
    vehicle = undefined;
    trajectory = undefined;
    if (message.missing) {
      statusEl.textContent = message.missing;
      return;
    }
    statusEl.textContent = "Loading...";

    const modelUri = message.model || "model.glb";
    const model = await modelInfo(modelUri);
    const offset = await groundOffset(message.position, model.clearance);
    const { property: positionProperty, positions } = buildPositionProperty(message.position, offset);
    const orientationProperty = buildOrientationProperty(message.attitude, positionProperty, positions[0]);
    const t = message.position.t;

    vehicle = viewer.entities.add({
      availability: new Cesium.TimeIntervalCollection([
        new Cesium.TimeInterval({ start: toJulian(t[0]), stop: toJulian(t[t.length - 1]) }),
      ]),
      position: positionProperty,
      orientation: orientationProperty,
      model: {
        uri: modelUri,
        minimumPixelSize: 48,
        maximumScale: 2000,
        nodeTransformations: propTransformations(model.rotors, message.motors),
      },
      viewFrom: new Cesium.Cartesian3(0, -12, 4),
    });
    path = viewer.entities.add({
      polyline: {
        positions: decimate(message.position, PATH_MIN_INTERVAL).map((i) => positions[i]),
        width: 6,
        material: new Cesium.PolylineGlowMaterialProperty({ glowPower: 0.15, color: Cesium.Color.YELLOW }),
      },
      show: pathEl.checked,
    });
    if (message.winch) {
      const packageHeight = message.package ? (await modelInfo("package.glb")).clearance : 0;
      addWinch(message.winch, message.package, packageHeight, positionProperty, orientationProperty);
    }
    trajectory = message;
    statusEl.textContent = "";

    if (currentTime === undefined) currentTime = t[0];
    setTime(currentTime);
    if (followEl.checked) {
      viewer.trackedEntity = vehicle;
    } else {
      await viewer.zoomTo(path);
    }
  }

  // props: the angle of each spinning node, advanced in wall-clock time
  let spinning = [];
  let lastFrame;

  function propTransformations(rotors, motors) {
    spinning = [];
    if (!motors) return undefined;
    const transformations = {};
    for (const rotor of rotors) {
      const motor = motors[rotor.motor];
      if (!motor) continue;
      // its own rotation: the default is the shared Quaternion.IDENTITY
      const prop = { ...rotor, series: motor, angle: 0,
                     trs: new Cesium.TranslationRotationScale(undefined, new Cesium.Quaternion(0, 0, 0, 1)) };
      spinning.push(prop);
      transformations[rotor.node] = new Cesium.CallbackProperty(() => prop.trs, false);
    }
    return new Cesium.PropertyBag(transformations);
  }

  function spinProps(now) {
    const dt = lastFrame === undefined ? 0 : Math.min((now - lastFrame) / 1000, 0.1);
    lastFrame = now;
    let moved = false;
    if (trajectory && currentTime !== undefined) {
      for (const prop of spinning) {
        const command = motorCommand(prop.series, currentTime);
        if (command <= 0.01) continue;
        const rate = PROP_RATE_MIN + (PROP_RATE_MAX - PROP_RATE_MIN) * Math.min(command, 1);
        prop.angle = (prop.angle + prop.direction * rate * dt) % (2 * Math.PI);
        Cesium.Quaternion.fromAxisAngle(Cesium.Cartesian3.UNIT_Z, prop.angle, prop.trs.rotation);
        moved = true;
      }
    }
    if (moved) viewer.scene.requestRender();
    requestAnimationFrame(spinProps);
  }
  requestAnimationFrame(spinProps);

  /// The winch line hanging straight down from the receptacle, with the
  /// package on it until the hook released it, then left where it landed.
  function addWinch(winch, pack, packageHeight, positionProperty, orientationProperty) {
    const scratch = { position: new Cesium.Cartesian3(), orientation: new Cesium.Quaternion(),
                      matrix: new Cesium.Matrix3(), offset: new Cesium.Cartesian3() };
    const bodyPoint = (time, offset, result) => {
      const position = positionProperty.getValue(time, scratch.position);
      const orientation = orientationProperty.getValue(time, scratch.orientation);
      if (!position || !orientation) return undefined;
      Cesium.Matrix3.fromQuaternion(orientation, scratch.matrix);
      Cesium.Matrix3.multiplyByVector(scratch.matrix, offset, scratch.offset);
      return Cesium.Cartesian3.add(position, scratch.offset, result);
    };
    // the line end, no lower than above the ground (by what hangs from it)
    const lineEnd = (time, exit, above = 0) => {
      const length = interpolate(winch, Cesium.JulianDate.secondsDifference(time, EPOCH));
      const cartographic = Cesium.Cartographic.fromCartesian(exit);
      const ground = viewer.scene.globe.getHeight(cartographic);
      cartographic.height = Math.max(cartographic.height - length, ground === undefined ? -Infinity : ground + above);
      return Cesium.Cartographic.toCartesian(cartographic);
    };
    const lineOut = (time) =>
      interpolate(winch, Cesium.JulianDate.secondsDifference(time, EPOCH)) > WINCH_STOWED_LENGTH;

    viewer.entities.add({
      polyline: {
        positions: new Cesium.CallbackProperty((time) => {
          const exit = lineOut(time) && bodyPoint(time, WINCH_LINE_EXIT, new Cesium.Cartesian3());
          return exit ? [exit, lineEnd(time, exit)] : [];
        }, false),
        width: 2,
        material: Cesium.Color.fromCssColorString("#c0301f"),
      },
    });
    viewer.entities.add({
      position: new Cesium.CallbackProperty((time) => {
        const exit = lineOut(time) && bodyPoint(time, WINCH_LINE_EXIT, new Cesium.Cartesian3());
        return exit ? lineEnd(time, exit) : undefined;
      }, false),
      point: { pixelSize: 6, color: Cesium.Color.fromCssColorString("#c0301f") },
    });
    if (!pack) return;

    const released = pack.released === null ? Infinity : pack.released;
    const hanging = PACKAGE_ATTACH_HEIGHT + packageHeight;
    const releasedAt = toJulian(released);
    let landed;
    const isReleased = (time) => Cesium.JulianDate.secondsDifference(time, EPOCH) >= released;
    viewer.entities.add({
      position: new Cesium.CallbackProperty((time) => {
        if (isReleased(time)) {
          // where the line put it down
          if (!landed) {
            const exit = bodyPoint(releasedAt, WINCH_LINE_EXIT, new Cesium.Cartesian3());
            landed = exit && packageOnLine(lineEnd(releasedAt, exit, hanging));
          }
          return landed;
        }
        if (!lineOut(time)) return bodyPoint(time, PACKAGE_STOWED, new Cesium.Cartesian3());
        const exit = bodyPoint(time, WINCH_LINE_EXIT, new Cesium.Cartesian3());
        return exit && packageOnLine(lineEnd(time, exit, hanging));
      }, false),
      orientation: new Cesium.CallbackProperty(
        (time, result) => orientationProperty.getValue(isReleased(time) ? releasedAt : time, result), false),
      model: { uri: "package.glb", minimumPixelSize: 12 },
    });
  }

  /// The package origin when it hangs from the line end
  function packageOnLine(end) {
    const cartographic = Cesium.Cartographic.fromCartesian(end);
    cartographic.height -= PACKAGE_ATTACH_HEIGHT;
    return Cesium.Cartographic.toCartesian(cartographic);
  }

  function setTime(t) {
    currentTime = t;
    timeEl.textContent = "t = " + t.toFixed(3) + " s";
    if (!viewer || !trajectory) return;
    viewer.clock.currentTime = toJulian(t);

    const pos = trajectory.position.t;
    statusEl.textContent = t < pos[0] || t > pos[pos.length - 1] ? "Outside of the flight data" : "";
    const nav = trajectory.nav_state;
    const index = nav ? sampleIndexAt(nav.t, t) : -1;
    modeEl.textContent = index >= 0 ? NAV_STATES[nav.v[index]] || "nav_state " + nav.v[index] : "";
    viewer.scene.requestRender();
  }

  followEl.addEventListener("change", () => {
    if (!viewer || !vehicle) return;
    viewer.trackedEntity = followEl.checked ? vehicle : undefined;
    viewer.scene.requestRender();
  });
  pathEl.addEventListener("change", () => {
    if (!path) return;
    path.show = pathEl.checked;
    viewer.scene.requestRender();
  });

  function connect() {
    const socket = new WebSocket("ws://" + location.host + "/");
    socket.onopen = () => { statusEl.textContent = ""; };
    socket.onmessage = (event) => {
      const message = JSON.parse(event.data);
      if (message.type === "trajectory") {
        loading = loading.then(() => loadTrajectory(message)).catch((error) => {
          console.error(error);
          statusEl.textContent = "Failed to load the trajectory: " + error;
        });
      } else if (message.type === "time") {
        setTime(message.t);
      }
    };
    socket.onclose = () => {
      statusEl.textContent = "Not connected to PlotJuggler (enable \"3D Replay\" in its Publishers)";
      setTimeout(connect, 1000);
    };
  }
  connect();
}
