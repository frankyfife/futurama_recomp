// Quality presets of the wizard and the graphics tab (ROADMAP M3c):
//   Classic   640x480, render scale 1, MSAA off, anisotropy off - reproduces the M3 images (xemu regression)
//   Standard  native resolution (auto), MSAA 4x, anisotropy 16x, alpha-to-coverage on
//   Maximum   native resolution (auto), supersampling 2x (exact 2x2 box) + MSAA 8x = 32 samples per pixel,
//             anisotropy 16x, alpha-to-coverage on, gamma-correct resolve ([render] linear_aa)
// Review M3c finding 1: Maximum used render scale 1.5, whose filtered downsample drew text and 2D softer than native
// MSAA 8x at 2.6 times the GPU time. Finding 21: the presets also respect the video memory of query-gpu
// ("dedicated_mb"): the back buffers of a step must fit the D3D11 limit per resource (max(128 MB, min(VRAM/4,
// 2 GB))) and half of the video memory in total; otherwise the next step is taken - Maximum: 2x/8x, 2x/4x,
// 1x/8x, 1x/4x; Standard: 4x, 2x, off. Unknown memory: no supersampling. The size is the largest 4:3 area of the
// largest connected monitor (fullscreen; a window is smaller). Values are clamped to what the GPU reports
// (query-gpu "msaa", "max_anisotropy") and to the schema's choices and ranges; keys the schema lacks are skipped.
#pragma once

#include "core/config_schema.h"
#include "core/gpu_info.h"

#include <QList>
#include <QPair>
#include <QSize>
#include <QString>

#include <functional>

namespace fl {

enum class QualityPreset { Classic, Standard, Maximum, Custom };

inline constexpr char kKeyResolution[] = "render.resolution";
inline constexpr char kKeyRenderScale[] = "render.render_scale";
inline constexpr char kKeyMsaa[] = "render.msaa";
inline constexpr char kKeyAlphaToCoverage[] = "render.alpha_to_coverage";
inline constexpr char kKeyAnisotropy[] = "render.anisotropy";
inline constexpr char kKeyLinearAa[] = "render.linear_aa";

struct PresetValues {
    QList<QPair<QString, QString>> values;  // "section.key" -> INI text
    qint64 videoMemoryMb = 0;               // estimated back-buffer memory of the chosen step (0 = unknown size)
    bool limitedByMemory = false;           // a higher step did not fit the video memory
    QString value(const QString& fullName) const;
};

// Back-buffer memory in MB of the game at `output` (the 4:3 image) with render scale `scale` and `samples` per
// pixel: two persistent back buffers + the depth buffer (all multisampled), the resolve texture and the output
// image (GFX_CONTRACT.md 12.3). `largestMb` = the largest single resource.
qint64 BackBufferMemoryMb(QSize output, double scale, int samples, qint64* largestMb = nullptr);
// True if those buffers fit `dedicatedMb` of video memory (rules above). dedicatedMb <= 0: unknown -> false.
bool FitsVideoMemory(QSize output, double scale, int samples, qint64 dedicatedMb);
// The 4:3 output size the presets plan for: the largest 4:3 area of the largest output of `gpu` (2880x2160 if
// the GPU query lists none).
QSize PresetOutputSize(const GpuInfo& gpu);

QString PresetId(QualityPreset preset);  // "classic", "standard", "maximum", "custom"

// The preset's values for this schema and GPU.
PresetValues ComputePreset(QualityPreset preset, const ConfigSchema& schema, const GpuInfo& gpu);

// The preset whose values all match `current` (Custom if none does). `current` returns the effective
// INI text of a "section.key".
QualityPreset DetectPreset(const ConfigSchema& schema, const GpuInfo& gpu,
                           const std::function<QString(const KeyDef&)>& current);

// Largest numeric choice of `key` that is <= `wanted` and allowed by `allowed` (empty = no limit).
// Returns the "off"-like choice (non-numeric or 1) if none fits.
QString ClampNumericChoice(const KeyDef& key, int wanted, const QList<int>& allowed);

}  // namespace fl
