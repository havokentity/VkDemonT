// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "BscCatalog.h"

#include "../core/Tracy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace pt::stars {

namespace {
constexpr double kPi    = 3.14159265358979323846;
constexpr double kRad2Deg = 180.0 / kPi;

// All known macOS/x86/arm targets are little-endian; the BSC5 file
// shipped with this engine is the canonical Harvard distribution which
// is also LE. We assume LE without runtime checks; on a hypothetical
// big-endian build the parse would silently produce wrong numbers and
// the loader would reject most stars on the sanity guards below.
template <typename T>
T read_le(const std::uint8_t*& p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}
}  // namespace

std::vector<Star> LoadBsc5(const std::string& path, std::string* err) {
    PT_ZONE_SCOPED_N("stars::LoadBsc5");
    std::vector<Star> stars;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "failed to open " + path;
        return stars;
    }
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 28) {
        if (err) *err = "file shorter than BSC5 header";
        return stars;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(buf.data()), size);

    const std::uint8_t* p = buf.data();
    const std::int32_t star0  = read_le<std::int32_t>(p);  // unused
    const std::int32_t star1  = read_le<std::int32_t>(p);  // unused
    const std::int32_t starn  = read_le<std::int32_t>(p);  // negative if J2000 floats present
    const std::int32_t stnum  = read_le<std::int32_t>(p);  // unused
    const std::int32_t mprop  = read_le<std::int32_t>(p);  // unused
    const std::int32_t nmag   = read_le<std::int32_t>(p);  // unused
    const std::int32_t nbent  = read_le<std::int32_t>(p);
    (void)star0; (void)star1; (void)stnum; (void)mprop; (void)nmag;

    if (nbent != 32) {
        if (err) *err = "unexpected record size " + std::to_string(nbent);
        return stars;
    }
    const std::size_t count = static_cast<std::size_t>(std::abs(starn));
    if (28 + count * 32 > buf.size()) {
        if (err) *err = "file truncated";
        return stars;
    }

    stars.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* r = buf.data() + 28 + i * 32;
        // float xno (4) | double ra_rad (8) | double dec_rad (8) |
        // char[2] sp | int16 mag*100 | float ra_pm | float dec_pm
        double ra_rad, dec_rad;
        std::int16_t mag_x100;
        std::memcpy(&ra_rad,    r + 4,  sizeof(double));
        std::memcpy(&dec_rad,   r + 12, sizeof(double));
        std::memcpy(&mag_x100,  r + 22, sizeof(std::int16_t));

        // Sanity gates -- the catalog has a handful of placeholder
        // entries with garbage RA/Dec and impossible magnitudes.
        if (!std::isfinite(ra_rad) || !std::isfinite(dec_rad)) continue;
        if (ra_rad < -1.0 || ra_rad > 2.0 * kPi + 1.0) continue;
        if (dec_rad < -kPi || dec_rad > kPi) continue;
        const float vmag = float(mag_x100) * 0.01f;
        if (vmag < -2.0f || vmag > 9.0f) continue;

        // Normalise RA to [0, 360).
        double ra_deg = ra_rad * kRad2Deg;
        ra_deg = std::fmod(ra_deg, 360.0);
        if (ra_deg < 0.0) ra_deg += 360.0;
        const double dec_deg = std::clamp(dec_rad * kRad2Deg, -90.0, 90.0);

        Star s{};
        s.ra_deg  = float(ra_deg);
        s.dec_deg = float(dec_deg);
        s.vmag    = vmag;
        stars.push_back(s);
    }

    std::sort(stars.begin(), stars.end(),
              [](const Star& a, const Star& b) { return a.vmag < b.vmag; });
    return stars;
}

// Apparent magnitude -> V-band irradiance at the observer, in W/m^2.
//
// Two cited pieces and no free parameters:
//   Pogson, N. R. (1856), MNRAS 17, 12 -- one magnitude step is a factor
//     10^0.4 = 2.51188643 in flux, by definition.
//   Zero point -- a V = 0 source (Vega) delivers 3.19e-9 W/m^2 over the V
//     band. Bessell, Castelli & Plez (1998), A&A 333, 231: f_lambda =
//     3.63e-11 W/m^2/nm at 545 nm, times the V band's 88 nm FWHM. The
//     independent 3640 Jy route gives 3.21e-9, agreeing to within 1%.
//
// RADIOMETRIC because the engine is: kPtSolarIrradiance is 1360.8 W/m^2
// (Kopp & Lean 2011) and skies are carried in W/m^2/sr. Quoting stars in
// lux would be wrong by the luminous-efficacy factor (~700 for a Vega-like
// spectrum), which is a bigger error than the one it would be fixing.
//
// The gain that used to sit on the end of this expression (kFluxScale =
// 4.0, "picked so the naked-eye limit reads as ~50/255") is gone. It was
// never a photometric quantity: it existed to paper over RasteriseJ2000Map
// below, which splatted a Gaussian at PEAK amplitude instead of conserving
// energy, so a star's total flux came out proportional to flux * sigma^2
// and had to be re-tuned by hand whenever sigma or the map resolution
// moved. With the splat normalised, the correct scale is simply the real
// one -- and it works because the engine's sky is already in real units
// (kPtSolarIrradiance = 1360.8 W/m^2, skies in W/m^2/sr). Both ends of the
// range then come out on their own: against a true ~2.9e-7 W/m^2/sr
// moonless night sky Vega is ~1200x the background, and against a ~15
// W/m^2/sr noon zenith the identical star is ~2e-5 of it -- invisible,
// exactly as in life.
float MagnitudeToIrradianceWm2(float vmag) {
    constexpr float kVegaZeroPointWm2 = 3.19e-9f;  // Bessell et al. 1998
    constexpr float kPogson           = 2.51188643f;
    return kVegaZeroPointWm2 * std::pow(kPogson, -vmag);
}

// Angular sigma of the point-spread function, radians.
//
// Independent of magnitude, which is the correction: a star is a point
// source, so how far it spreads on the sky is a property of the OPTICS,
// not of its brightness. The previous tiered values (1.4e-3 for the
// brightest tier down to 0.75e-3) made a bright star carry
// (1.4/0.75)^2 = 3.5x more total energy than its magnitude specified,
// on top of already being brighter for the right reason -- the magnitude
// scale was being applied twice, once honestly and once by accident.
//
// 1.5 arcmin is the human eye's optical PSF for a point source (the
// figure the old implementation comment already quoted as "1-2 arcmin"
// before tiering away from it). Bright stars still read as bigger on
// screen, because with a normalised PSF their wings clear the display
// threshold further out -- which is the actual physical mechanism, and
// is why bright stars look bigger in a photograph too.
//
// `sampling_floor_rad` widens sigma when the consumer's sampling grid is
// coarser than the optical PSF: the map bake passes its texel pitch, the
// planet path passes the on-screen pixel angle. Because the PSF is
// normalised, widening it lowers the peak by exactly the area ratio and
// leaves total energy untouched -- it anti-aliases without changing
// brightness. (Space Graphics Toolkit reaches the same place empirically
// with `scale = saturate(size/sizeMin); colour *= scale*scale`; this is
// that identity, derived rather than tuned.)
float PsfSigmaRad(float sampling_floor_rad) {
    constexpr float kEyeOpticalPsfRad = 4.36e-4f;  // 1.5 arcmin
    const float floor_rad = (std::isfinite(sampling_floor_rad) &&
                             sampling_floor_rad > 0.0f)
                          ? sampling_floor_rad : 0.0f;
    return std::max(kEyeOpticalPsfRad, floor_rad);
}

void BvToLinearSrgbTint(float bv, float out_rgb[3]) {
    // Step 1: B-V -> blackbody colour temperature.
    //   Ballesteros, F. J. (2012), "New insights into black bodies",
    //   Europhysics Letters 97, 34008, eq. (14):
    //     T = 4600 K * ( 1/(0.92 (B-V) + 1.7) + 1/(0.92 (B-V) + 0.62) )
    // Sanity anchor: the Sun's B-V = 0.65 returns 5779 K against its
    // true 5772 K effective temperature.
    //
    // Clamp the input to the range over which the relation was fitted
    // for main-sequence stars; outside it the second denominator can
    // reach zero and the expression diverges. -0.4 is bluer than any
    // O-type star, +2.0 redder than any planet or M-type giant.
    const double x  = 0.92 * double(std::clamp(bv, -0.4f, 2.0f));
    const double T  = 4600.0 * (1.0 / (x + 1.7) + 1.0 / (x + 0.62));
    // Step 2: T -> CIE 1931 chromaticity on the Planckian locus.
    //   Kim, Y., Moon, B.-C. and Kim, D.-S. (2002), "Design of advanced
    //   color temperature control system for HDTV applications",
    //   J. Korean Phys. Soc. 41(6), 865-871 -- the cubic approximations
    //   reproduced in CIE 15:2004 practice and in Wikipedia's
    //   "Planckian locus" article. Valid 1667 K .. 25000 K.
    const double Tc = std::clamp(T, 1667.0, 25000.0);
    const double t1 = 1.0e3 / Tc;
    const double t2 = t1 * t1;
    const double t3 = t2 * t1;
    double cx;
    if (Tc < 4000.0) {
        cx = -0.2661239 * t3 - 0.2343589 * t2 + 0.8776956 * t1 + 0.179910;
    } else {
        cx = -3.0258469 * t3 + 2.1070379 * t2 + 0.2226347 * t1 + 0.240390;
    }
    const double x2 = cx * cx;
    const double x3 = x2 * cx;
    double cy;
    if (Tc < 2222.0) {
        cy = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * cx - 0.20219683;
    } else if (Tc < 4000.0) {
        cy = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * cx - 0.16748867;
    } else {
        cy =  3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * cx - 0.37001483;
    }
    // xyY (Y = 1) -> XYZ -> linear sRGB / Rec.709 (IEC 61966-2-1 matrix,
    // D65 white). Negative components mean the Planckian chromaticity fell
    // outside the sRGB gamut; clamp rather than desaturate, the excursion
    // is fractions of a percent over the temperature range planets span.
    const double Y = 1.0;
    const double X = (cx / std::max(cy, 1e-6)) * Y;
    const double Z = ((1.0 - cx - cy) / std::max(cy, 1e-6)) * Y;
    double r =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double b =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    r = std::max(r, 0.0); g = std::max(g, 0.0); b = std::max(b, 0.0);
    // Normalise to unit Rec.709 luminance so the tint carries hue only --
    // MagnitudeToIrradianceWm2 owns brightness, and a caller multiplying the two
    // must get back exactly the magnitude it asked for.
    const double lum = std::max(0.2126 * r + 0.7152 * g + 0.0722 * b, 1e-6);
    out_rgb[0] = float(r / lum);
    out_rgb[1] = float(g / lum);
    out_rgb[2] = float(b / lum);
}

void RasteriseJ2000Map(const std::vector<Star>& stars,
                       std::uint32_t W, std::uint32_t H,
                       std::vector<float>& out) {
    PT_ZONE_SCOPED_N("stars::RasteriseJ2000Map");
    out.assign(std::size_t(W) * H * 4, 0.0f);
    if (stars.empty() || W == 0 || H == 0) return;

    auto color_for_index = [](std::uint32_t i) {
        const float palette[3][3] = {
            {0.85f, 0.92f, 1.10f},     // hot blue-white
            {1.00f, 0.98f, 0.95f},     // neutral white (most common)
            {1.05f, 0.85f, 0.70f},     // warm amber (red giants)
        };
        const std::uint32_t h = (i * 2654435761u) >> 30;  // 0..3
        const int idx = (h < 1) ? 0 : (h < 3) ? 1 : 2;
        return std::tuple<float, float, float>{
            palette[idx][0], palette[idx][1], palette[idx][2]};
    };

    constexpr double kPi    = 3.14159265358979323846;
    constexpr double kDeg2R = kPi / 180.0;
    const double dphi   = (2.0 * kPi) / double(W);  // RA per u-step (rad)
    const double dtheta = kPi / double(H);          // colatitude per v-step (rad)

    // One PSF for every star: the splat width is an optical property, not
    // a per-star one (see PsfSigmaRad). Floored at 0.75 texel so a star
    // landing between texel centres is still resolved by the discrete
    // grid; the normalisation below keeps its energy exact either way.
    const float r_ang  = PsfSigmaRad(0.75f * float(dtheta));
    const float r_ang2 = r_ang * r_ang;
    constexpr int K    = 4;     // truncate Gaussian past 4 sigma

    for (std::size_t i = 0; i < stars.size(); ++i) {
        const Star& s = stars[i];
        // Total V-band irradiance this star delivers, in W/m^2. Every
        // texel it touches shares exactly this much between them -- that
        // is the contract the normalisation below enforces.
        const double E_wm2 = double(MagnitudeToIrradianceWm2(s.vmag));

        // Star direction in J2000: same convention as the shader looks
        // up later (atan2(j.y, j.x) -> RA, asin(j.z) -> dec).
        const double ra  = s.ra_deg  * kDeg2R;
        const double dec = s.dec_deg * kDeg2R;
        const double cdec = std::cos(dec);
        const double sdec = std::sin(dec);
        const double sx = cdec * std::cos(ra);
        const double sy = cdec * std::sin(ra);
        const double sz = sdec;

        // Equirectangular center texel.
        const float u = s.ra_deg / 360.0f;
        const float v = float(0.5 - s.dec_deg / 180.0);   // dec=+90 -> v=0
        const float fx = u * float(W);
        const float fy = v * float(H);

        const auto [r_tint, g_tint, b_tint] = color_for_index(std::uint32_t(i));

        // Splat extent in texel space. dec direction is uniform; ra
        // direction widens by 1/cos(dec) so a star near a pole still
        // covers its full angular footprint. Cap at a half-sphere to
        // avoid degenerate sweeps when the star is at the celestial
        // pole exactly (the entire row is "within range" angularly,
        // and we'd visit W texels per star).
        const float half_v = float(K) * r_ang / float(dtheta);
        // Longitude half-extent widens by 1/cos(dec) because equirectangular
        // columns converge toward the poles.
        //
        // There used to be a `max(cos(dec), 0.05)` floor here, nominally "~3
        // deg from pole". It TRUNCATED the sweep: above |dec| 87.13 deg the
        // visited span was narrower than the star's true 4-sigma footprint.
        // Energy stayed exact -- the normalisation below divides by whatever
        // was actually visited -- but the PEAK inflated, because the same
        // energy was packed into fewer texels. Measured against a full sweep:
        // 1.16x at Polaris, 6.27x at dec 89.9 on the production 8192x4096 map.
        //
        // The floor was also redundant. half_u is already bounded by W/2
        // below, which is the CORRECT bound: a star close enough to the pole
        // genuinely spans every longitude, and W/2 sweeps exactly that once
        // and no more. Removing the floor makes the near-pole case correct
        // and leaves the worst case unchanged.
        const double cos_dec = std::max(std::cos(dec), 1e-9);  // 1e-9 guards 1/0 only
        const float half_u = std::min(
            float(K) * r_ang / (float(dphi) * float(cos_dec)),
            float(W) * 0.5f);

        const int iy0 = std::max(0, int(std::floor(fy - half_v)));
        const int iy1 = std::min(int(H) - 1, int(std::ceil (fy + half_v)));
        const int ix0 = int(std::floor(fx - half_u));
        const int ix1 = int(std::ceil (fx + half_u));

        // Walk the footprint once per pass. Traversing twice beats
        // buffering it: the bake runs once at startup, and the footprint
        // is a few hundred texels even in the worst case (near a pole,
        // where 1/cos(dec) stretches the row span).
        auto for_each_texel = [&](auto&& fn) {
            for (int y = iy0; y <= iy1; ++y) {
                // texel-center direction in J2000 for this row of texels
                const double theta_t = (double(y) + 0.5) * dtheta;     // colatitude
                const double sint    = std::sin(theta_t);
                const double cost    = std::cos(theta_t);              // = sin(dec_t)
                // True solid angle of a texel in this row. The sin(theta)
                // is why the poles do not accumulate spurious energy: an
                // equirectangular texel there covers almost no sky.
                const double omega   = dphi * dtheta * sint;
                for (int xRaw = ix0; xRaw <= ix1; ++xRaw) {
                    int x = xRaw;
                    while (x < 0)         x += int(W);
                    while (x >= int(W))   x -= int(W);
                    // u = ra/(2pi); the rasteriser maps RA=0 -> u=0, so the
                    // texel-center azimuth is phi_t = u*2pi *without* a -pi
                    // recentre. (An earlier draft subtracted pi here, which
                    // pointed every texel exactly opposite its star and made
                    // the whole map zero.)
                    const double phi_t  = (double(x) + 0.5) * dphi;
                    const double tx = sint * std::cos(phi_t);
                    const double ty = sint * std::sin(phi_t);
                    const double tz = cost;
                    // Angular distance via dot product. cos(angle) = s . t.
                    // For small angles, angle^2 ~= 2 * (1 - cos(angle)).
                    const double dotv = std::clamp(sx * tx + sy * ty + sz * tz,
                                                   -1.0, 1.0);
                    const double ang2 = 2.0 * (1.0 - dotv);
                    if (ang2 > double(r_ang2) * double(K * K)) continue;
                    const double w = std::exp(-ang2 / double(r_ang2));
                    if (w < 1e-4) continue;
                    fn(x, y, w, omega);
                }
            }
        };

        // Pass 1: integrate the un-normalised Gaussian over the texels it
        // actually lands on, each weighted by its true solid angle.
        double wsum_omega = 0.0;
        for_each_texel([&](int, int, double w, double omega) {
            wsum_omega += w * omega;
        });
        // A star whose entire footprint fell outside the map (or below the
        // weight cutoff) has nowhere to put its energy. Dropping it is the
        // only choice that does not silently inflate a neighbour.
        if (!(wsum_omega > 0.0)) continue;

        // Pass 2: write radiance in W/m^2/sr such that, summed over the
        // footprint with solid-angle weights, the star delivers exactly
        // E_wm2. Normalising against the DISCRETE sum rather than the
        // analytic pi*sigma^2 is what makes this exact no matter how
        // sigma compares to the texel pitch, where the Gaussian is
        // truncated, or how hard the equirectangular grid is distorting.
        const double norm = E_wm2 / wsum_omega;
        for_each_texel([&](int x, int y, double w, double) {
            const double L = norm * w;
            std::size_t off = (std::size_t(y) * W + std::size_t(x)) * 4;
            out[off + 0] += float(L * double(r_tint));
            out[off + 1] += float(L * double(g_tint));
            out[off + 2] += float(L * double(b_tint));
            out[off + 3]  = 1.0f;
        });
    }
}

}  // namespace pt::stars
