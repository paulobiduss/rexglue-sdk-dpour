/**
 * @file        audio/downmix.h
 * @brief       Output-stage mix parameters: 5.1 to stereo fold and master gain
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
// DPOUR MIGRATION 2026-09-02 (upstream 3b5ef8a): taken verbatim.
#pragma once

namespace rex::audio {

/**
 * Weights applied when the guest's 5.1 render is folded to a stereo device.
 * Guest channel order is the XAudio default: fl fr fc lf bl br. The front
 * channels carry an implicit weight of 1.0, so `scale` alone sets the output
 * level.
 *
 * Defaults follow ITU-R BS.775 and Dolby Lo/Ro: center and surround at -3 dB,
 * LFE dropped. No published stereo downmix folds LFE, which is authored around
 * 10 dB hot by convention, so a title that wants it audible sets a weight that
 * undoes that offset rather than folding it at unity.
 */
struct StereoFold {
  float center = 0.70710678f;
  float surround = 0.70710678f;
  float lfe = 0.0f;
  float scale = 0.58578644f;  // 1/(1+0.707)
};

/// Safe to call from any thread. The output stage picks the new values up on
/// its next device callback.
void SetStereoFold(const StereoFold& fold);
StereoFold GetStereoFold();

/// Linear master gain applied to the stereo fold and to 5.1 passthrough alike.
/// 1.0 is unity. Above unity can clip, and the output stage clamps.
void SetOutputGain(float linear);
float GetOutputGain();

}  // namespace rex::audio
