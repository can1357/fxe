#pragma once

#include <fxe/types.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <utility>

#include <fxe/math.hpp>

namespace fxe {
  // -------------------------------------------------------------------------------------
  // Plain RGBA8 colour types. Layout-compatible with the original GFW r8g8b8/r8g8b8a8 so
  // serialised vertex memory stays bit-identical.
  // -------------------------------------------------------------------------------------
  struct r8g8b8 {
    u8 r = 0, g = 0, b = 0;
    constexpr r8g8b8() = default;
    constexpr r8g8b8(u8 r_, u8 g_, u8 b_) : r(r_), g(g_), b(b_) {}
    constexpr r8g8b8(u32 hex) : r((hex >> 16) & 0xff), g((hex >> 8) & 0xff), b(hex & 0xff) {}
    constexpr auto operator<=>(const r8g8b8&) const = default;
  };

  struct r8g8b8a8 {
    u8 r = 0, g = 0, b = 0, a = 0;
    constexpr r8g8b8a8() = default;
    constexpr r8g8b8a8(u8 r_, u8 g_, u8 b_, u8 a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
    constexpr r8g8b8a8(r8g8b8 rgb, u8 a_ = 255) : r(rgb.r), g(rgb.g), b(rgb.b), a(a_) {}
    constexpr explicit r8g8b8a8(u32 rgba)
        : r((rgba >> 24) & 0xff), g((rgba >> 16) & 0xff), b((rgba >> 8) & 0xff), a(rgba & 0xff) {}
    constexpr auto operator<=>(const r8g8b8a8&) const = default;

    [[nodiscard]] constexpr u32 rgba() const noexcept {
      return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a;
    }
    [[nodiscard]] constexpr math::vec4 xyzw(float scale = 1.0f) const noexcept {
      return {r * scale / 255.0f, g * scale / 255.0f, b * scale / 255.0f, a * scale / 255.0f};
    }
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr r8g8b8a8 operator*(r8g8b8a8 o) const noexcept {
      return {u8((u16(r) * o.r) / 255), u8((u16(g) * o.g) / 255), u8((u16(b) * o.b) / 255),
              u8((u16(a) * o.a) / 255)};
    }
    constexpr r8g8b8a8& operator*=(r8g8b8a8 o) noexcept {
      return *this = *this * o;
    }

    // Move every channel toward 0 by a fraction of itself.
    [[nodiscard]] constexpr r8g8b8a8 darken(float t) const noexcept {
      t = math::fclamp(t, 0.0f, 1.0f);
      return {u8(float(r) * (1.0f - t)), u8(float(g) * (1.0f - t)), u8(float(b) * (1.0f - t)), a};
    }
    // Move every channel toward max(r,g,b) by a fraction of the gap, alpha untouched.
    [[nodiscard]] constexpr r8g8b8a8 lighten(float t) const noexcept {
      t = math::fclamp(t, 0.0f, 1.0f);
      float m = math::fmax(float(r), math::fmax(float(g), float(b)));
      return {u8(float(r) + t * (m - float(r))), u8(float(g) + t * (m - float(g))),
              u8(float(b) + t * (m - float(b))), a};
    }

    // Rec. 601 luminance, 0..255 for an RGBA8.
    static constexpr math::vec4 luminance_coeffs{0.299f, 0.587f, 0.114f, 0.0f};
    [[nodiscard]] constexpr float luminance() const noexcept {
      return luminance_coeffs.x * float(r) + luminance_coeffs.y * float(g) +
             luminance_coeffs.z * float(b);
    }
    // Rescale RGB so the resulting luminance matches `target` (0..255), clamping channels
    // to 255. Mirrors the original GFW r8g8b8a8::luminance(float) modulator.
    [[nodiscard]] constexpr r8g8b8a8 luminance(float target) const noexcept {
      float current = luminance();
      if (current <= 0.001f) {
        u8 v = u8(math::fclamp(target, 0.0f, 255.0f));
        return {v, v, v, a};
      }
      float k = target / current;
      auto scale = [k](u8 c) { return u8(math::fmin(float(c) * k, 255.0f)); };
      return {scale(r), scale(g), scale(b), a};
    }
  };

  // -------------------------------------------------------------------------------------
  // Named colours — full SVG/CSS palette plus a couple project-specific extras.
  // -------------------------------------------------------------------------------------
  inline constexpr r8g8b8a8 transparent{0, 0, 0, 0};
  inline constexpr r8g8b8a8 sr_red{r8g8b8{0xd80c3c}};
  inline constexpr r8g8b8a8 sr_black{r8g8b8{0x282828}};
  inline constexpr r8g8b8a8 aliceblue{r8g8b8{0xf0f8ff}};
  inline constexpr r8g8b8a8 antiquewhite{r8g8b8{0xfaebd7}};
  inline constexpr r8g8b8a8 aqua{r8g8b8{0x00ffff}};
  inline constexpr r8g8b8a8 aquamarine{r8g8b8{0x7fffd4}};
  inline constexpr r8g8b8a8 azure{r8g8b8{0xf0ffff}};
  inline constexpr r8g8b8a8 beige{r8g8b8{0xf5f5dc}};
  inline constexpr r8g8b8a8 bisque{r8g8b8{0xffe4c4}};
  inline constexpr r8g8b8a8 black{r8g8b8{0x000000}};
  inline constexpr r8g8b8a8 blanchedalmond{r8g8b8{0xffebcd}};
  inline constexpr r8g8b8a8 blue{r8g8b8{0x0000ff}};
  inline constexpr r8g8b8a8 blueviolet{r8g8b8{0x8a2be2}};
  inline constexpr r8g8b8a8 brown{r8g8b8{0xa52a2a}};
  inline constexpr r8g8b8a8 burlywood{r8g8b8{0xdeb887}};
  inline constexpr r8g8b8a8 cadetblue{r8g8b8{0x5f9ea0}};
  inline constexpr r8g8b8a8 chartreuse{r8g8b8{0x7fff00}};
  inline constexpr r8g8b8a8 chocolate{r8g8b8{0xd2691e}};
  inline constexpr r8g8b8a8 coral{r8g8b8{0xff7f50}};
  inline constexpr r8g8b8a8 cornflowerblue{r8g8b8{0x6495ed}};
  inline constexpr r8g8b8a8 cornsilk{r8g8b8{0xfff8dc}};
  inline constexpr r8g8b8a8 crimson{r8g8b8{0xdc143c}};
  inline constexpr r8g8b8a8 cyan{r8g8b8{0x00ffff}};
  inline constexpr r8g8b8a8 darkblue{r8g8b8{0x00008b}};
  inline constexpr r8g8b8a8 darkcyan{r8g8b8{0x008b8b}};
  inline constexpr r8g8b8a8 darkgoldenrod{r8g8b8{0xb8860b}};
  inline constexpr r8g8b8a8 darkgray{r8g8b8{0xa9a9a9}};
  inline constexpr r8g8b8a8 darkgreen{r8g8b8{0x006400}};
  inline constexpr r8g8b8a8 darkgrey{r8g8b8{0xa9a9a9}};
  inline constexpr r8g8b8a8 darkkhaki{r8g8b8{0xbdb76b}};
  inline constexpr r8g8b8a8 darkmagenta{r8g8b8{0x8b008b}};
  inline constexpr r8g8b8a8 darkolivegreen{r8g8b8{0x556b2f}};
  inline constexpr r8g8b8a8 darkorange{r8g8b8{0xff8c00}};
  inline constexpr r8g8b8a8 darkorchid{r8g8b8{0x9932cc}};
  inline constexpr r8g8b8a8 darkred{r8g8b8{0x8b0000}};
  inline constexpr r8g8b8a8 darksalmon{r8g8b8{0xe9967a}};
  inline constexpr r8g8b8a8 darkseagreen{r8g8b8{0x8fbc8f}};
  inline constexpr r8g8b8a8 darkslateblue{r8g8b8{0x483d8b}};
  inline constexpr r8g8b8a8 darkslategray{r8g8b8{0x2f4f4f}};
  inline constexpr r8g8b8a8 darkslategrey{r8g8b8{0x2f4f4f}};
  inline constexpr r8g8b8a8 darkturquoise{r8g8b8{0x00ced1}};
  inline constexpr r8g8b8a8 darkviolet{r8g8b8{0x9400d3}};
  inline constexpr r8g8b8a8 deeppink{r8g8b8{0xff1493}};
  inline constexpr r8g8b8a8 deepskyblue{r8g8b8{0x00bfff}};
  inline constexpr r8g8b8a8 dimgray{r8g8b8{0x696969}};
  inline constexpr r8g8b8a8 dimgrey{r8g8b8{0x696969}};
  inline constexpr r8g8b8a8 dodgerblue{r8g8b8{0x1e90ff}};
  inline constexpr r8g8b8a8 firebrick{r8g8b8{0xb22222}};
  inline constexpr r8g8b8a8 floralwhite{r8g8b8{0xfffaf0}};
  inline constexpr r8g8b8a8 forestgreen{r8g8b8{0x228b22}};
  inline constexpr r8g8b8a8 fuchsia{r8g8b8{0xff00ff}};
  inline constexpr r8g8b8a8 gainsboro{r8g8b8{0xdcdcdc}};
  inline constexpr r8g8b8a8 ghostwhite{r8g8b8{0xf8f8ff}};
  inline constexpr r8g8b8a8 goldenrod{r8g8b8{0xdaa520}};
  inline constexpr r8g8b8a8 gold{r8g8b8{0xffd700}};
  inline constexpr r8g8b8a8 gray{r8g8b8{0x808080}};
  inline constexpr r8g8b8a8 green{r8g8b8{0x008000}};
  inline constexpr r8g8b8a8 greenyellow{r8g8b8{0xadff2f}};
  inline constexpr r8g8b8a8 grey{r8g8b8{0x808080}};
  inline constexpr r8g8b8a8 honeydew{r8g8b8{0xf0fff0}};
  inline constexpr r8g8b8a8 hotpink{r8g8b8{0xff69b4}};
  inline constexpr r8g8b8a8 indianred{r8g8b8{0xcd5c5c}};
  inline constexpr r8g8b8a8 indigo{r8g8b8{0x4b0082}};
  inline constexpr r8g8b8a8 ivory{r8g8b8{0xfffff0}};
  inline constexpr r8g8b8a8 khaki{r8g8b8{0xf0e68c}};
  inline constexpr r8g8b8a8 lavenderblush{r8g8b8{0xfff0f5}};
  inline constexpr r8g8b8a8 lavender{r8g8b8{0xe6e6fa}};
  inline constexpr r8g8b8a8 lawngreen{r8g8b8{0x7cfc00}};
  inline constexpr r8g8b8a8 lemonchiffon{r8g8b8{0xfffacd}};
  inline constexpr r8g8b8a8 lightblue{r8g8b8{0xadd8e6}};
  inline constexpr r8g8b8a8 lightcoral{r8g8b8{0xf08080}};
  inline constexpr r8g8b8a8 lightcyan{r8g8b8{0xe0ffff}};
  inline constexpr r8g8b8a8 lightgoldenrodyellow{r8g8b8{0xfafad2}};
  inline constexpr r8g8b8a8 lightgray{r8g8b8{0xd3d3d3}};
  inline constexpr r8g8b8a8 lightgreen{r8g8b8{0x90ee90}};
  inline constexpr r8g8b8a8 lightgrey{r8g8b8{0xd3d3d3}};
  inline constexpr r8g8b8a8 lightpink{r8g8b8{0xffb6c1}};
  inline constexpr r8g8b8a8 lightsalmon{r8g8b8{0xffa07a}};
  inline constexpr r8g8b8a8 lightseagreen{r8g8b8{0x20b2aa}};
  inline constexpr r8g8b8a8 lightskyblue{r8g8b8{0x87cefa}};
  inline constexpr r8g8b8a8 lightslategray{r8g8b8{0x778899}};
  inline constexpr r8g8b8a8 lightslategrey{r8g8b8{0x778899}};
  inline constexpr r8g8b8a8 lightsteelblue{r8g8b8{0xb0c4de}};
  inline constexpr r8g8b8a8 lightyellow{r8g8b8{0xffffe0}};
  inline constexpr r8g8b8a8 lime{r8g8b8{0x00ff00}};
  inline constexpr r8g8b8a8 limegreen{r8g8b8{0x32cd32}};
  inline constexpr r8g8b8a8 linen{r8g8b8{0xfaf0e6}};
  inline constexpr r8g8b8a8 magenta{r8g8b8{0xff00ff}};
  inline constexpr r8g8b8a8 maroon{r8g8b8{0x800000}};
  inline constexpr r8g8b8a8 mediumaquamarine{r8g8b8{0x66cdaa}};
  inline constexpr r8g8b8a8 mediumblue{r8g8b8{0x0000cd}};
  inline constexpr r8g8b8a8 mediumorchid{r8g8b8{0xba55d3}};
  inline constexpr r8g8b8a8 mediumpurple{r8g8b8{0x9370db}};
  inline constexpr r8g8b8a8 mediumseagreen{r8g8b8{0x3cb371}};
  inline constexpr r8g8b8a8 mediumslateblue{r8g8b8{0x7b68ee}};
  inline constexpr r8g8b8a8 mediumspringgreen{r8g8b8{0x00fa9a}};
  inline constexpr r8g8b8a8 mediumturquoise{r8g8b8{0x48d1cc}};
  inline constexpr r8g8b8a8 mediumvioletred{r8g8b8{0xc71585}};
  inline constexpr r8g8b8a8 midnightblue{r8g8b8{0x191970}};
  inline constexpr r8g8b8a8 mintcream{r8g8b8{0xf5fffa}};
  inline constexpr r8g8b8a8 mistyrose{r8g8b8{0xffe4e1}};
  inline constexpr r8g8b8a8 moccasin{r8g8b8{0xffe4b5}};
  inline constexpr r8g8b8a8 navajowhite{r8g8b8{0xffdead}};
  inline constexpr r8g8b8a8 navy{r8g8b8{0x000080}};
  inline constexpr r8g8b8a8 oldlace{r8g8b8{0xfdf5e6}};
  inline constexpr r8g8b8a8 olive{r8g8b8{0x808000}};
  inline constexpr r8g8b8a8 olivedrab{r8g8b8{0x6b8e23}};
  inline constexpr r8g8b8a8 orange{r8g8b8{0xffa500}};
  inline constexpr r8g8b8a8 orangered{r8g8b8{0xff4500}};
  inline constexpr r8g8b8a8 orchid{r8g8b8{0xda70d6}};
  inline constexpr r8g8b8a8 palegoldenrod{r8g8b8{0xeee8aa}};
  inline constexpr r8g8b8a8 palegreen{r8g8b8{0x98fb98}};
  inline constexpr r8g8b8a8 paleturquoise{r8g8b8{0xafeeee}};
  inline constexpr r8g8b8a8 palevioletred{r8g8b8{0xdb7093}};
  inline constexpr r8g8b8a8 papayawhip{r8g8b8{0xffefd5}};
  inline constexpr r8g8b8a8 peachpuff{r8g8b8{0xffdab9}};
  inline constexpr r8g8b8a8 peru{r8g8b8{0xcd853f}};
  inline constexpr r8g8b8a8 pink{r8g8b8{0xffc0cb}};
  inline constexpr r8g8b8a8 plum{r8g8b8{0xdda0dd}};
  inline constexpr r8g8b8a8 powderblue{r8g8b8{0xb0e0e6}};
  inline constexpr r8g8b8a8 purple{r8g8b8{0x800080}};
  inline constexpr r8g8b8a8 rebeccapurple{r8g8b8{0x663399}};
  inline constexpr r8g8b8a8 red{r8g8b8{0xff0000}};
  inline constexpr r8g8b8a8 rosybrown{r8g8b8{0xbc8f8f}};
  inline constexpr r8g8b8a8 royalblue{r8g8b8{0x4169e1}};
  inline constexpr r8g8b8a8 saddlebrown{r8g8b8{0x8b4513}};
  inline constexpr r8g8b8a8 salmon{r8g8b8{0xfa8072}};
  inline constexpr r8g8b8a8 sandybrown{r8g8b8{0xf4a460}};
  inline constexpr r8g8b8a8 seagreen{r8g8b8{0x2e8b57}};
  inline constexpr r8g8b8a8 seashell{r8g8b8{0xfff5ee}};
  inline constexpr r8g8b8a8 sienna{r8g8b8{0xa0522d}};
  inline constexpr r8g8b8a8 silver{r8g8b8{0xc0c0c0}};
  inline constexpr r8g8b8a8 skyblue{r8g8b8{0x87ceeb}};
  inline constexpr r8g8b8a8 slateblue{r8g8b8{0x6a5acd}};
  inline constexpr r8g8b8a8 slategray{r8g8b8{0x708090}};
  inline constexpr r8g8b8a8 slategrey{r8g8b8{0x708090}};
  inline constexpr r8g8b8a8 snow{r8g8b8{0xfffafa}};
  inline constexpr r8g8b8a8 springgreen{r8g8b8{0x00ff7f}};
  inline constexpr r8g8b8a8 steelblue{r8g8b8{0x4682b4}};
  inline constexpr r8g8b8a8 tan{r8g8b8{0xd2b48c}};
  inline constexpr r8g8b8a8 teal{r8g8b8{0x008080}};
  inline constexpr r8g8b8a8 thistle{r8g8b8{0xd8bfd8}};
  inline constexpr r8g8b8a8 tomato{r8g8b8{0xff6347}};
  inline constexpr r8g8b8a8 turquoise{r8g8b8{0x40e0d0}};
  inline constexpr r8g8b8a8 violet{r8g8b8{0xee82ee}};
  inline constexpr r8g8b8a8 wheat{r8g8b8{0xf5deb3}};
  inline constexpr r8g8b8a8 white{r8g8b8{0xffffff}};
  inline constexpr r8g8b8a8 whitesmoke{r8g8b8{0xf5f5f5}};
  inline constexpr r8g8b8a8 yellow{r8g8b8{0xffff00}};
  inline constexpr r8g8b8a8 yellowgreen{r8g8b8{0x9acd32}};

  // Name-keyed lookup table for JS / config bindings. Sorted alphabetically (stable).
  // Use color_by_name(sv) to fetch a colour by its uppercase name; returns transparent
  // when the lookup misses.
  using named_color = std::pair<const char*, r8g8b8a8>;
  inline constexpr std::array<named_color, 150> color_table{{
      {"ALICEBLUE", aliceblue},
      {"ANTIQUEWHITE", antiquewhite},
      {"AQUA", aqua},
      {"AQUAMARINE", aquamarine},
      {"AZURE", azure},
      {"BEIGE", beige},
      {"BISQUE", bisque},
      {"BLACK", black},
      {"BLANCHEDALMOND", blanchedalmond},
      {"BLUE", blue},
      {"BLUEVIOLET", blueviolet},
      {"BROWN", brown},
      {"BURLYWOOD", burlywood},
      {"CADETBLUE", cadetblue},
      {"CHARTREUSE", chartreuse},
      {"CHOCOLATE", chocolate},
      {"CORAL", coral},
      {"CORNFLOWERBLUE", cornflowerblue},
      {"CORNSILK", cornsilk},
      {"CRIMSON", crimson},
      {"CYAN", cyan},
      {"DARKBLUE", darkblue},
      {"DARKCYAN", darkcyan},
      {"DARKGOLDENROD", darkgoldenrod},
      {"DARKGRAY", darkgray},
      {"DARKGREEN", darkgreen},
      {"DARKGREY", darkgrey},
      {"DARKKHAKI", darkkhaki},
      {"DARKMAGENTA", darkmagenta},
      {"DARKOLIVEGREEN", darkolivegreen},
      {"DARKORANGE", darkorange},
      {"DARKORCHID", darkorchid},
      {"DARKRED", darkred},
      {"DARKSALMON", darksalmon},
      {"DARKSEAGREEN", darkseagreen},
      {"DARKSLATEBLUE", darkslateblue},
      {"DARKSLATEGRAY", darkslategray},
      {"DARKSLATEGREY", darkslategrey},
      {"DARKTURQUOISE", darkturquoise},
      {"DARKVIOLET", darkviolet},
      {"DEEPPINK", deeppink},
      {"DEEPSKYBLUE", deepskyblue},
      {"DIMGRAY", dimgray},
      {"DIMGREY", dimgrey},
      {"DODGERBLUE", dodgerblue},
      {"FIREBRICK", firebrick},
      {"FLORALWHITE", floralwhite},
      {"FORESTGREEN", forestgreen},
      {"FUCHSIA", fuchsia},
      {"GAINSBORO", gainsboro},
      {"GHOSTWHITE", ghostwhite},
      {"GOLD", gold},
      {"GOLDENROD", goldenrod},
      {"GRAY", gray},
      {"GREEN", green},
      {"GREENYELLOW", greenyellow},
      {"GREY", grey},
      {"HONEYDEW", honeydew},
      {"HOTPINK", hotpink},
      {"INDIANRED", indianred},
      {"INDIGO", indigo},
      {"IVORY", ivory},
      {"KHAKI", khaki},
      {"LAVENDER", lavender},
      {"LAVENDERBLUSH", lavenderblush},
      {"LAWNGREEN", lawngreen},
      {"LEMONCHIFFON", lemonchiffon},
      {"LIGHTBLUE", lightblue},
      {"LIGHTCORAL", lightcoral},
      {"LIGHTCYAN", lightcyan},
      {"LIGHTGOLDENRODYELLOW", lightgoldenrodyellow},
      {"LIGHTGRAY", lightgray},
      {"LIGHTGREEN", lightgreen},
      {"LIGHTGREY", lightgrey},
      {"LIGHTPINK", lightpink},
      {"LIGHTSALMON", lightsalmon},
      {"LIGHTSEAGREEN", lightseagreen},
      {"LIGHTSKYBLUE", lightskyblue},
      {"LIGHTSLATEGRAY", lightslategray},
      {"LIGHTSLATEGREY", lightslategrey},
      {"LIGHTSTEELBLUE", lightsteelblue},
      {"LIGHTYELLOW", lightyellow},
      {"LIME", lime},
      {"LIMEGREEN", limegreen},
      {"LINEN", linen},
      {"MAGENTA", magenta},
      {"MAROON", maroon},
      {"MEDIUMAQUAMARINE", mediumaquamarine},
      {"MEDIUMBLUE", mediumblue},
      {"MEDIUMORCHID", mediumorchid},
      {"MEDIUMPURPLE", mediumpurple},
      {"MEDIUMSEAGREEN", mediumseagreen},
      {"MEDIUMSLATEBLUE", mediumslateblue},
      {"MEDIUMSPRINGGREEN", mediumspringgreen},
      {"MEDIUMTURQUOISE", mediumturquoise},
      {"MEDIUMVIOLETRED", mediumvioletred},
      {"MIDNIGHTBLUE", midnightblue},
      {"MINTCREAM", mintcream},
      {"MISTYROSE", mistyrose},
      {"MOCCASIN", moccasin},
      {"NAVAJOWHITE", navajowhite},
      {"NAVY", navy},
      {"OLDLACE", oldlace},
      {"OLIVE", olive},
      {"OLIVEDRAB", olivedrab},
      {"ORANGE", orange},
      {"ORANGERED", orangered},
      {"ORCHID", orchid},
      {"PALEGOLDENROD", palegoldenrod},
      {"PALEGREEN", palegreen},
      {"PALETURQUOISE", paleturquoise},
      {"PALEVIOLETRED", palevioletred},
      {"PAPAYAWHIP", papayawhip},
      {"PEACHPUFF", peachpuff},
      {"PERU", peru},
      {"PINK", pink},
      {"PLUM", plum},
      {"POWDERBLUE", powderblue},
      {"PURPLE", purple},
      {"REBECCAPURPLE", rebeccapurple},
      {"RED", red},
      {"ROSYBROWN", rosybrown},
      {"ROYALBLUE", royalblue},
      {"SADDLEBROWN", saddlebrown},
      {"SALMON", salmon},
      {"SANDYBROWN", sandybrown},
      {"SEAGREEN", seagreen},
      {"SEASHELL", seashell},
      {"SIENNA", sienna},
      {"SILVER", silver},
      {"SKYBLUE", skyblue},
      {"SLATEBLUE", slateblue},
      {"SLATEGRAY", slategray},
      {"SLATEGREY", slategrey},
      {"SNOW", snow},
      {"SPRINGGREEN", springgreen},
      {"SR_BLACK", sr_black},
      {"SR_RED", sr_red},
      {"STEELBLUE", steelblue},
      {"TAN", tan},
      {"TEAL", teal},
      {"THISTLE", thistle},
      {"TOMATO", tomato},
      {"TURQUOISE", turquoise},
      {"VIOLET", violet},
      {"WHEAT", wheat},
      {"WHITE", white},
      {"WHITESMOKE", whitesmoke},
      {"YELLOW", yellow},
      {"YELLOWGREEN", yellowgreen},
  }};

  // Case-sensitive (uppercase only) binary-search lookup.
  [[nodiscard]] r8g8b8a8 color_by_name(std::string_view name) noexcept;
} // namespace fxe
