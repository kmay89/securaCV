// stlbox.mjs — read a committed STL's bounding box.
//
// This is what makes the figures self-maintaining rather than a parallel set
// of drawings that rot. A figure's massing envelope is checked against the
// real part's bounding box at generate time: change the CAD, re-export the
// STL, and the generator fails until the figure agrees again. Without this
// the fleet would drift into showing users a shape the printer no longer
// makes — which is exactly the confusion the figures exist to remove.
//
// Binary STL only: that is what render.sh exports (`--export-format binstl`)
// and what the repo commits. An ASCII STL is rejected loudly rather than
// silently measured as an empty box.

import { readFileSync } from 'node:fs';

const HEADER = 80;
const TRI = 50; // 12 floats + 2-byte attribute

/**
 * @returns {{min:[number,number,number], max:[number,number,number],
 *            size:[number,number,number], triangles:number}}
 */
export function stlBounds(path) {
  const buf = readFileSync(path);
  if (buf.length < HEADER + 4) throw new Error(`stlbox: ${path} is too short to be an STL`);
  if (buf.subarray(0, 6).toString('ascii').trim().toLowerCase().startsWith('solid')
      && !looksBinary(buf)) {
    throw new Error(`stlbox: ${path} is ASCII STL; the repo commits binary STLs`);
  }
  const count = buf.readUInt32LE(HEADER);
  const need = HEADER + 4 + count * TRI;
  if (buf.length < need) {
    throw new Error(`stlbox: ${path} claims ${count} triangles but is ${buf.length} bytes (needs ${need})`);
  }
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let t = 0; t < count; t++) {
    const base = HEADER + 4 + t * TRI + 12; // skip the facet normal
    for (let v = 0; v < 3; v++) {
      for (let c = 0; c < 3; c++) {
        const val = buf.readFloatLE(base + v * 12 + c * 4);
        if (val < min[c]) min[c] = val;
        if (val > max[c]) max[c] = val;
      }
    }
  }
  if (count === 0) throw new Error(`stlbox: ${path} has no triangles`);
  return {
    min, max, triangles: count,
    size: [max[0] - min[0], max[1] - min[1], max[2] - min[2]],
  };
}

// A binary STL's declared triangle count must match its byte length exactly;
// an ASCII file that happens to start with "solid" will not.
function looksBinary(buf) {
  if (buf.length < HEADER + 4) return false;
  return buf.length === HEADER + 4 + buf.readUInt32LE(HEADER) * TRI;
}
