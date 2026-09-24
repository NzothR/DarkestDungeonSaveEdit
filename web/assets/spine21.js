// Minimal reader and canvas renderer for the Spine 2.1 binary data used by
// Darkest Dungeon. No game art or third-party runtime code is bundled here.

class BinaryReader {
  constructor(buffer) {
    this.view = new DataView(buffer);
    this.offset = 0;
    this.decoder = new TextDecoder();
  }
  need(count) {
    if (this.offset + count > this.view.byteLength) throw new Error("Truncated Spine skeleton");
  }
  byte() { this.need(1); return this.view.getUint8(this.offset++); }
  float() { this.need(4); const value = this.view.getFloat32(this.offset, false); this.offset += 4; return value; }
  int32() { this.need(4); const value = this.view.getInt32(this.offset, false); this.offset += 4; return value; }
  varint(signed = false) {
    let value = 0;
    for (let shift = 0; shift <= 28; shift += 7) {
      const part = this.byte();
      value |= (part & 127) << shift;
      if (!(part & 128)) return signed ? (value >>> 1) ^ -(value & 1) : value >>> 0;
    }
    throw new Error("Invalid Spine integer");
  }
  string() {
    const size = this.varint();
    if (!size) return null;
    if (size === 1) return "";
    this.need(size - 1);
    const value = this.decoder.decode(new Uint8Array(this.view.buffer, this.offset, size - 1));
    this.offset += size - 1;
    return value;
  }
  floats() { return Array.from({ length: this.varint() }, () => this.float()); }
  shorts() {
    return Array.from({ length: this.varint() }, () => (this.byte() << 8) | this.byte());
  }
  ints() { return Array.from({ length: this.varint() }, () => this.varint()); }
}

function parseAttachment(reader, fallbackName, nonessential) {
  const name = reader.string() || fallbackName;
  const type = reader.byte();
  if (type === 0) {
    return {
      type: "region", name, path: reader.string() || name,
      x: reader.float(), y: reader.float(), scaleX: reader.float(), scaleY: reader.float(),
      rotation: reader.float(), width: reader.float(), height: reader.float(), color: reader.int32(),
    };
  }
  if (type === 1) return { type: "box", name, vertices: reader.floats() };
  if (type === 2) {
    const path = reader.string() || name;
    const uvs = reader.floats(), triangles = reader.shorts(), vertices = reader.floats();
    const color = reader.int32(), hull = reader.varint();
    if (nonessential) { reader.ints(); reader.float(); reader.float(); }
    return { type: "mesh", name, path, uvs, triangles, vertices, color, hull };
  }
  if (type === 3) {
    const path = reader.string() || name;
    const uvs = reader.floats(), triangles = reader.shorts();
    const vertexCount = reader.varint();
    const vertices = [];
    let weights = 0;
    for (let index = 0; index < vertexCount; index++) {
      const boneCount = reader.float();
      const influences = [];
      for (let bone = 0; bone < boneCount; bone++) {
        influences.push({ bone: reader.float(), x: reader.float(), y: reader.float(), weight: reader.float() });
        weights += 3;
        index += 4;
      }
      vertices.push(influences);
    }
    const color = reader.int32(), hull = reader.varint();
    if (nonessential) { reader.ints(); reader.float(); reader.float(); }
    return { type: "skinnedMesh", name, path, uvs, triangles, vertices, weights, color, hull };
  }
  throw new Error(`Unsupported Spine attachment type ${type}`);
}

function parseSkin(reader, name, nonessential) {
  const slotCount = reader.varint();
  if (!slotCount) return null;
  const attachments = new Map();
  for (let slot = 0; slot < slotCount; slot++) {
    const slotIndex = reader.varint(), count = reader.varint();
    for (let index = 0; index < count; index++) {
      const attachmentName = reader.string();
      attachments.set(`${slotIndex}/${attachmentName}`,
        parseAttachment(reader, attachmentName, nonessential));
    }
  }
  return { name, attachments };
}

function curve(reader) {
  const kind = reader.byte();
  if (kind === 2) return [reader.float(), reader.float(), reader.float(), reader.float()];
  return kind === 1 ? "stepped" : "linear";
}

function frames(reader, count, values, curved) {
  const result = [];
  for (let index = 0; index < count; index++) {
    const frame = { time: reader.float(), values: Array.from({ length: values }, () => reader.float()) };
    if (curved && index + 1 < count) frame.curve = curve(reader);
    result.push(frame);
  }
  return result;
}

function parseAnimation(reader, name, skins, slots) {
  const timelines = [];
  let duration = 0;
  const add = (target, index, kind, data) => {
    timelines.push({ target, index, kind, frames: data });
    if (data.length) duration = Math.max(duration, data.at(-1).time);
  };
  for (let slot = reader.varint(); slot > 0; slot--) {
    const slotIndex = reader.varint();
    for (let count = reader.varint(); count > 0; count--) {
      const kind = reader.byte(), size = reader.varint();
      if (kind === 4) {
        const data = [];
        for (let i = 0; i < size; i++) {
          const frame = { time: reader.float(), color: reader.int32() };
          if (i + 1 < size) frame.curve = curve(reader);
          data.push(frame);
        }
        add("slot", slotIndex, "color", data);
      } else if (kind === 3) {
        add("slot", slotIndex, "attachment", Array.from({ length: size },
          () => ({ time: reader.float(), name: reader.string() })));
      } else throw new Error(`Unsupported Spine slot timeline ${kind}`);
    }
  }
  for (let bone = reader.varint(); bone > 0; bone--) {
    const boneIndex = reader.varint();
    for (let count = reader.varint(); count > 0; count--) {
      const kind = reader.byte(), size = reader.varint();
      if (kind === 1) add("bone", boneIndex, "rotate", frames(reader, size, 1, true));
      else if (kind === 0 || kind === 2)
        add("bone", boneIndex, kind === 0 ? "scale" : "translate", frames(reader, size, 2, true));
      else if (kind === 5 || kind === 6) add("bone", boneIndex, kind === 5 ? "flipX" : "flipY",
        Array.from({ length: size }, () => ({ time: reader.float(), value: !!reader.byte() })));
      else throw new Error(`Unsupported Spine bone timeline ${kind}`);
    }
  }
  for (let ik = reader.varint(); ik > 0; ik--) {
    const index = reader.varint(), size = reader.varint(), data = [];
    for (let i = 0; i < size; i++) {
      const frame = { time: reader.float(), mix: reader.float(), bend: reader.byte() };
      if (i + 1 < size) frame.curve = curve(reader);
      data.push(frame);
    }
    add("ik", index, "ik", data);
  }
  for (let skinCount = reader.varint(); skinCount > 0; skinCount--) {
    const skin = skins[reader.varint()];
    for (let slotCount = reader.varint(); slotCount > 0; slotCount--) {
      const slotIndex = reader.varint();
      for (let attachmentCount = reader.varint(); attachmentCount > 0; attachmentCount--) {
        const attachmentName = reader.string();
        const attachment = skin?.attachments.get(`${slotIndex}/${attachmentName}`);
        const size = reader.varint(), data = [];
        for (let i = 0; i < size; i++) {
          const time = reader.float(), end = reader.varint();
          const vertexCount = attachment?.type === "mesh" ? attachment.vertices.length
            : attachment?.type === "skinnedMesh" ? attachment.weights / 3 * 2 : 0;
          if (end) {
            const start = reader.varint();
            for (let vertex = start; vertex < start + end; vertex++) reader.float();
          }
          const frame = { time, vertexCount };
          if (i + 1 < size) frame.curve = curve(reader);
          data.push(frame);
        }
        add("ffd", slotIndex, "ffd", data);
      }
    }
  }
  const drawOrder = reader.varint();
  if (drawOrder) {
    const data = [];
    for (let i = 0; i < drawOrder; i++) {
      const changes = reader.varint(), order = Array(slots.length).fill(-1);
      const unchanged = [];
      let next = 0;
      for (let change = 0; change < changes; change++) {
        const slot = reader.varint();
        while (next < slot) unchanged.push(next++);
        order[next + reader.varint(true)] = next++;
      }
      while (next < slots.length) unchanged.push(next++);
      for (let at = order.length - 1; at >= 0; at--) if (order[at] < 0) order[at] = unchanged.pop();
      data.push({ time: reader.float(), order });
    }
    add("skeleton", 0, "drawOrder", data);
  }
  for (let count = reader.varint(); count > 0; count--) {
    const time = reader.float();
    reader.varint(); reader.varint(true); reader.float();
    if (reader.byte()) reader.string();
    duration = Math.max(duration, time);
  }
  return { name, timelines, duration };
}

export function parseSpine21(buffer) {
  const reader = new BinaryReader(buffer);
  const hash = reader.string(), version = reader.string();
  if (!version?.startsWith("2.1.")) throw new Error(`Unsupported Spine version ${version || "unknown"}`);
  const width = reader.float(), height = reader.float(), nonessential = !!reader.byte();
  if (nonessential) reader.string();
  const bones = Array.from({ length: reader.varint() }, () => ({
    name: reader.string(), parent: reader.varint() - 1,
    x: reader.float(), y: reader.float(), scaleX: reader.float(), scaleY: reader.float(),
    rotation: reader.float(), length: reader.float(), flipX: !!reader.byte(), flipY: !!reader.byte(),
    inheritScale: !!reader.byte(), inheritRotation: !!reader.byte(),
    ...(nonessential ? { color: reader.int32() } : {}),
  }));
  const ik = Array.from({ length: reader.varint() }, () => ({
    name: reader.string(), bones: Array.from({ length: reader.varint() }, () => reader.varint()),
    target: reader.varint(), mix: reader.float(), bend: reader.byte(),
  }));
  const slots = Array.from({ length: reader.varint() }, () => ({
    name: reader.string(), bone: reader.varint(), color: reader.int32(),
    attachment: reader.string(), additive: !!reader.byte(),
  }));
  const skins = [];
  const defaultSkin = parseSkin(reader, "default", nonessential);
  if (defaultSkin) skins.push(defaultSkin);
  for (let count = reader.varint(); count > 0; count--)
    skins.push(parseSkin(reader, reader.string(), nonessential));
  const events = Array.from({ length: reader.varint() }, () => ({
    name: reader.string(), integer: reader.varint(true), value: reader.float(), text: reader.string(),
  }));
  const animations = Array.from({ length: reader.varint() }, () => parseAnimation(reader, reader.string(), skins, slots));
  if (reader.offset !== reader.view.byteLength) throw new Error("Unexpected Spine skeleton data");
  return { hash, version, width, height, bones, ik, slots, skins, events, animations };
}

export function parseSpineAtlas(text) {
  const lines = text.replaceAll("\r", "").split("\n");
  const regions = new Map();
  for (let i = 0; i < lines.length;) {
    const line = lines[i++].trim();
    if (!line || line.includes(".png") && lines[i]?.trim().startsWith("size:")) continue;
    if (line.includes(":")) continue;
    const fields = {};
    while (i < lines.length && lines[i].trim().includes(":")) {
      const value = lines[i++].trim();
      const colon = value.indexOf(":");
      fields[value.slice(0, colon)] = value.slice(colon + 1).trim();
    }
    if (!fields.xy || !fields.size) continue;
    const pair = (key) => (fields[key] || "0,0").split(",").map((value) => Number(value.trim()));
    const [x, y] = pair("xy"), [width, height] = pair("size");
    const [originalWidth, originalHeight] = pair("orig"), [offsetX, offsetY] = pair("offset");
    regions.set(line, { x, y, width, height, originalWidth, originalHeight,
      offsetX, offsetY, rotate: fields.rotate === "true" });
  }
  return regions;
}

const radians = Math.PI / 180;

function interpolation(frame, next, time) {
  const fraction = Math.max(0, Math.min(1, (time - frame.time) / (next.time - frame.time || 1)));
  if (frame.curve === "stepped") return 0;
  if (!Array.isArray(frame.curve)) return fraction;
  const [x1, y1, x2, y2] = frame.curve;
  const cubic = (a, b, t) => 3 * a * t * (1 - t) ** 2 + 3 * b * t * t * (1 - t) + t ** 3;
  let low = 0, high = 1;
  for (let i = 0; i < 12; i++) {
    const middle = (low + high) / 2;
    if (cubic(x1, x2, middle) < fraction) low = middle;
    else high = middle;
  }
  return cubic(y1, y2, (low + high) / 2);
}

function sampled(timeline, time) {
  const data = timeline.frames;
  if (!data.length || time < data[0].time) return null;
  let index = data.length - 1;
  while (index > 0 && time < data[index].time) index--;
  const frame = data[index], next = data[index + 1];
  if (!next || !frame.values) return frame;
  const fraction = interpolation(frame, next, time);
  return { ...frame, values: frame.values.map((value, position) => {
    let difference = next.values[position] - value;
    if (timeline.kind === "rotate") {
      while (difference > 180) difference -= 360;
      while (difference < -180) difference += 360;
    }
    return value + difference * fraction;
  }) };
}

function poseAt(skeleton, time) {
  const animation = skeleton.animations.find((item) => item.name === "idle") || skeleton.animations[0];
  const animationTime = animation?.duration ? time % animation.duration : 0;
  const bones = skeleton.bones.map((bone) => ({ ...bone }));
  const attachments = skeleton.slots.map((slot) => slot.attachment);
  let order = skeleton.slots.map((_, index) => index);
  for (const timeline of animation?.timelines || []) {
    const frame = sampled(timeline, animationTime);
    if (!frame) continue;
    if (timeline.target === "bone") {
      const bone = bones[timeline.index];
      if (!bone) continue;
      if (timeline.kind === "rotate") bone.rotation += frame.values[0];
      else if (timeline.kind === "translate") {
        bone.x += frame.values[0]; bone.y += frame.values[1];
      } else if (timeline.kind === "scale") {
        bone.scaleX *= frame.values[0]; bone.scaleY *= frame.values[1];
      } else if (timeline.kind === "flipX" || timeline.kind === "flipY") bone[timeline.kind] = frame.value;
    } else if (timeline.kind === "attachment") attachments[timeline.index] = frame.name;
    else if (timeline.kind === "drawOrder") order = frame.order;
  }
  const updateWorld = () => {
    for (const bone of bones) {
      const parent = bones[bone.parent];
      bone.worldX = parent ? bone.x * parent.m00 + bone.y * parent.m01 + parent.worldX : bone.x;
      bone.worldY = parent ? bone.x * parent.m10 + bone.y * parent.m11 + parent.worldY : bone.y;
      bone.worldScaleX = parent && bone.inheritScale ? parent.worldScaleX * bone.scaleX : bone.scaleX;
      bone.worldScaleY = parent && bone.inheritScale ? parent.worldScaleY * bone.scaleY : bone.scaleY;
      bone.worldRotation = parent && bone.inheritRotation ? parent.worldRotation + bone.rotation : bone.rotation;
      bone.worldFlipX = !!(parent?.worldFlipX !== undefined ? parent.worldFlipX !== bone.flipX : bone.flipX);
      bone.worldFlipY = !!(parent?.worldFlipY !== undefined ? parent.worldFlipY !== bone.flipY : bone.flipY);
      const angle = bone.worldRotation * radians, cos = Math.cos(angle), sin = Math.sin(angle);
      bone.m00 = cos * bone.worldScaleX * (bone.worldFlipX ? -1 : 1);
      bone.m01 = -sin * bone.worldScaleY * (bone.worldFlipX ? -1 : 1);
      bone.m10 = sin * bone.worldScaleX * (bone.worldFlipY ? -1 : 1);
      bone.m11 = cos * bone.worldScaleY * (bone.worldFlipY ? -1 : 1);
    }
  };
  updateWorld();
  // Idle rigs can constrain one or two bones. Apply the setup constraint mix,
  // then rebuild world matrices so skinned vertices follow the constrained pose.
  for (const constraint of skeleton.ik) {
    const target = bones[constraint.target];
    if (!target || !constraint.mix) continue;
    if (constraint.bones.length === 1) {
      const bone = bones[constraint.bones[0]];
      if (!bone) continue;
      const parent = bones[bone.parent];
      const desired = Math.atan2(target.worldY - bone.worldY, target.worldX - bone.worldX) / radians;
      const local = desired - (bone.inheritRotation ? parent?.worldRotation || 0 : 0);
      bone.rotation += (local - bone.rotation) * constraint.mix;
      updateWorld();
    } else if (constraint.bones.length === 2) {
      const parent = bones[constraint.bones[0]], child = bones[constraint.bones[1]];
      if (!parent || !child) continue;
      const dx = target.worldX - parent.worldX, dy = target.worldY - parent.worldY;
      const first = Math.hypot(child.worldX - parent.worldX, child.worldY - parent.worldY);
      const second = Math.abs(child.length * child.worldScaleX);
      if (first < 1e-4 || second < 1e-4) continue;
      const distance = Math.hypot(dx, dy);
      const bend = constraint.bend === 255 ? -1 : 1;
      const inner = Math.acos(Math.max(-1, Math.min(1, (first * first + second * second - distance * distance) / (2 * first * second))));
      const outer = Math.acos(Math.max(-1, Math.min(1, (first * first + distance * distance - second * second) / (2 * first * distance || 1))));
      const desiredParent = Math.atan2(dy, dx) / radians - bend * outer / radians;
      const parentDelta = desiredParent - Math.atan2(child.worldY - parent.worldY, child.worldX - parent.worldX) / radians;
      parent.rotation += parentDelta * constraint.mix;
      child.rotation += (bend * (Math.PI - inner) / radians - child.rotation) * constraint.mix;
      updateWorld();
    }
  }
  return { bones, attachments, order };
}

function regionVertices(attachment, region, bone) {
  const scaleX = attachment.width / (region.originalWidth || region.width) * attachment.scaleX;
  const scaleY = attachment.height / (region.originalHeight || region.height) * attachment.scaleY;
  const x0 = -attachment.width * attachment.scaleX / 2 + region.offsetX * scaleX;
  const y0 = -attachment.height * attachment.scaleY / 2 + region.offsetY * scaleY;
  const x1 = x0 + region.width * scaleX, y1 = y0 + region.height * scaleY;
  const angle = attachment.rotation * radians, cos = Math.cos(angle), sin = Math.sin(angle);
  return [[x0, y0], [x0, y1], [x1, y1], [x1, y0]].map(([x, y]) => {
    const rx = x * cos - y * sin + attachment.x, ry = x * sin + y * cos + attachment.y;
    return [rx * bone.m00 + ry * bone.m01 + bone.worldX,
      rx * bone.m10 + ry * bone.m11 + bone.worldY];
  });
}

function meshVertices(attachment, bone, bones) {
  if (attachment.type === "mesh") {
    const vertices = [];
    for (let i = 0; i < attachment.vertices.length; i += 2) {
      const x = attachment.vertices[i], y = attachment.vertices[i + 1];
      vertices.push([x * bone.m00 + y * bone.m01 + bone.worldX,
        x * bone.m10 + y * bone.m11 + bone.worldY]);
    }
    return vertices;
  }
  return attachment.vertices.map((influences) => {
    let x = 0, y = 0;
    for (const influence of influences) {
      const parent = bones[influence.bone];
      if (!parent) continue;
      x += (influence.x * parent.m00 + influence.y * parent.m01 + parent.worldX) * influence.weight;
      y += (influence.x * parent.m10 + influence.y * parent.m11 + parent.worldY) * influence.weight;
    }
    return [x, y];
  });
}

function geometryAt(skeleton, atlas, image, time) {
  const pose = poseAt(skeleton, time), skins = skeleton.skins;
  const batches = [];
  const width = image.width, height = image.height;
  for (const index of pose.order) {
    const slot = skeleton.slots[index], bone = pose.bones[slot?.bone];
    if (!bone || !pose.attachments[index]) continue;
    const attachment = skins[0]?.attachments.get(`${index}/${pose.attachments[index]}`);
    if (!attachment || attachment.type === "box") continue;
    const region = atlas.get(attachment.path);
    if (!region) continue;
    let vertices, uvs, triangles;
    if (attachment.type === "region") {
      vertices = regionVertices(attachment, region, bone);
      const left = region.x / width, top = region.y / height;
      const right = (region.x + (region.rotate ? region.height : region.width)) / width;
      const bottom = (region.y + (region.rotate ? region.width : region.height)) / height;
      uvs = region.rotate
        ? [[right, bottom], [left, bottom], [left, top], [right, top]]
        : [[left, bottom], [left, top], [right, top], [right, bottom]];
      triangles = [0, 1, 2, 0, 2, 3];
    } else {
      vertices = meshVertices(attachment, bone, pose.bones);
      uvs = [];
      for (let i = 0; i < attachment.uvs.length; i += 2) {
        const u = attachment.uvs[i], v = attachment.uvs[i + 1];
        uvs.push(region.rotate
          ? [(region.x + v * region.height) / width, (region.y + (1 - u) * region.width) / height]
          : [(region.x + u * region.width) / width, (region.y + v * region.height) / height]);
      }
      triangles = attachment.triangles;
    }
    batches.push({ vertices, uvs, triangles, alpha: (slot.color & 255) / 255 * (attachment.color & 255) / 255 });
  }
  return batches;
}

function shader(gl, type, source) {
  const compiled = gl.createShader(type);
  gl.shaderSource(compiled, source);
  gl.compileShader(compiled);
  if (!gl.getShaderParameter(compiled, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(compiled));
  return compiled;
}

function createRenderer(canvas, image) {
  const gl = canvas.getContext("webgl", { alpha: true, premultipliedAlpha: false });
  if (!gl) throw new Error("WebGL is unavailable");
  const program = gl.createProgram();
  gl.attachShader(program, shader(gl, gl.VERTEX_SHADER,
    "attribute vec2 aPosition; attribute vec2 aUv; attribute float aAlpha; varying vec2 vUv; varying float vAlpha; void main(){gl_Position=vec4(aPosition,0.0,1.0);vUv=aUv;vAlpha=aAlpha;}"));
  gl.attachShader(program, shader(gl, gl.FRAGMENT_SHADER,
    "precision mediump float; uniform sampler2D uTexture; varying vec2 vUv; varying float vAlpha; void main(){vec4 color=texture2D(uTexture,vUv);gl_FragColor=vec4(color.rgb,color.a*vAlpha);}"));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
  const buffer = gl.createBuffer(), texture = gl.createTexture();
  gl.useProgram(program);
  gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  const stride = 5 * 4;
  for (const [name, size, offset] of [["aPosition", 2, 0], ["aUv", 2, 8], ["aAlpha", 1, 16]]) {
    const location = gl.getAttribLocation(program, name);
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, size, gl.FLOAT, false, stride, offset);
  }
  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.uniform1i(gl.getUniformLocation(program, "uTexture"), 0);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  return {
    draw(batches, camera) {
      const deviceRatio = Math.min(window.devicePixelRatio || 1, 2);
      const width = Math.max(1, Math.round(canvas.clientWidth * deviceRatio));
      const height = Math.max(1, Math.round(canvas.clientHeight * deviceRatio));
      if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
      gl.viewport(0, 0, width, height);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      const scale = Math.min(width * .82 / camera.width, height * .86 / camera.height);
      const data = [];
      for (const batch of batches) {
        for (const index of batch.triangles) {
          const point = batch.vertices[index], uv = batch.uvs[index];
          if (!point || !uv) continue;
          data.push((point[0] - camera.x) * scale * 2 / width,
            (point[1] - camera.y) * scale * 2 / height, uv[0], uv[1], batch.alpha);
        }
      }
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.DYNAMIC_DRAW);
      gl.drawArrays(gl.TRIANGLES, 0, data.length / 5);
    },
    stop() { gl.deleteBuffer(buffer); gl.deleteTexture(texture); gl.deleteProgram(program); },
  };
}

function cameraFor(batches) {
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  let weightedX = 0, totalArea = 0;
  for (const batch of batches) for (const [x, y] of batch.vertices) {
    if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
    minX = Math.min(minX, x); minY = Math.min(minY, y);
    maxX = Math.max(maxX, x); maxY = Math.max(maxY, y);
  }
  if (!Number.isFinite(minX)) throw new Error("No visible Spine attachments");
  for (const batch of batches) {
    for (let i = 0; i < batch.triangles.length; i += 3) {
      const a = batch.vertices[batch.triangles[i]];
      const b = batch.vertices[batch.triangles[i + 1]];
      const c = batch.vertices[batch.triangles[i + 2]];
      if (!a || !b || !c) continue;
      const area = Math.abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
      if (!Number.isFinite(area)) continue;
      const visibleArea = area * batch.alpha;
      weightedX += (a[0] + b[0] + c[0]) / 3 * visibleArea;
      totalArea += visibleArea;
    }
  }
  const width = Math.max(1, maxX - minX), height = Math.max(1, maxY - minY);
  const boundsCenterX = (minX + maxX) / 2;
  const visualCenterX = totalArea > 0 ? weightedX / totalArea : boundsCenterX;
  const horizontalOffset = Math.max(-width * .07, Math.min(width * .07, visualCenterX - boundsCenterX));
  return { x: boundsCenterX + horizontalOffset, y: (minY + maxY) / 2 + height * .05,
    width, height };
}

export async function mountSpine21(canvas, paths, signal) {
  const url = (path) => `/api/content-asset?path=${encodeURIComponent(path)}`;
  const [png, atlasResponse, skeletonResponse] = await Promise.all(
    [paths.sprite, paths.atlas, paths.skeleton].map((path) => fetch(url(path), { signal })));
  if (![png, atlasResponse, skeletonResponse].every((response) => response.ok))
    throw new Error("Idle asset unavailable");
  const [image, atlasText, skeletonBytes] = await Promise.all([
    createImageBitmap(await png.blob()), atlasResponse.text(), skeletonResponse.arrayBuffer(),
  ]);
  if (signal.aborted) { image.close(); return () => {}; }
  const skeleton = parseSpine21(skeletonBytes), atlas = parseSpineAtlas(atlasText);
  const initial = geometryAt(skeleton, atlas, image, 0);
  const camera = cameraFor(initial);
  const renderer = createRenderer(canvas, image);
  let frame = 0, stopped = false, start = performance.now();
  const draw = (now) => {
    if (stopped) return;
    if (document.visibilityState !== "hidden")
      renderer.draw(geometryAt(skeleton, atlas, image, (now - start) / 1000), camera);
    frame = requestAnimationFrame(draw);
  };
  frame = requestAnimationFrame(draw);
  return () => { stopped = true; cancelAnimationFrame(frame); renderer.stop(); image.close(); };
}
