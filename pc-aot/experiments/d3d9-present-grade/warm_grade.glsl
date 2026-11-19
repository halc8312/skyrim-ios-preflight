// SPDX-License-Identifier: GPL-3.0-only
// A small, authored display-space grade. The caller preserves source alpha.
vec3 skyrimWarmDisplay(vec3 c) {
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  return clamp(mix(vec3(luma), c, 0.90) * vec3(1.08, 1.0, 0.90), 0.0, 1.0);
}
