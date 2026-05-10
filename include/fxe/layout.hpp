#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fxe::layout {

  enum class LengthKind : std::uint8_t { undefined = 0, points = 1, percent = 2, auto_ = 3 };
  struct Length {
    LengthKind kind = LengthKind::undefined;
    float value = 0.0f;
  };

  enum class FlexDirection : std::uint8_t { column = 0, column_reverse, row, row_reverse };
  enum class FlexWrap : std::uint8_t { nowrap = 0, wrap, wrap_reverse };
  enum class Align : std::uint8_t {
    auto_ = 0,
    flex_start,
    center,
    flex_end,
    stretch,
    baseline,
    space_between,
    space_around,
    space_evenly
  };
  enum class Justify : std::uint8_t {
    flex_start = 0,
    center,
    flex_end,
    space_between,
    space_around,
    space_evenly
  };
  enum class PositionType : std::uint8_t { relative = 0, absolute };
  enum class Display : std::uint8_t { flex = 0, none };
  enum class Overflow : std::uint8_t { visible = 0, hidden, scroll };
  enum class Edge : std::uint8_t { left = 0, top, right, bottom };

  struct Style {
    // optional setters: kind=undefined means "do not set, leave Yoga default".
    Length width{}, height{};
    Length min_width{}, min_height{};
    Length max_width{}, max_height{};

    // padding/margin per edge; element value with kind=undefined leaves default.
    Length padding[4]{}; // indexed by Edge
    Length margin[4]{};
    Length position[4]{}; // top/right/bottom/left when position_type=absolute

    Length flex_basis{};

    std::optional<FlexDirection> flex_direction;
    std::optional<FlexWrap> flex_wrap;
    std::optional<Justify> justify_content;
    std::optional<Align> align_items;
    std::optional<Align> align_self;
    std::optional<Align> align_content;
    std::optional<PositionType> position_type;
    std::optional<Display> display;
    std::optional<Overflow> overflow;

    std::optional<float> flex;
    std::optional<float> flex_grow;
    std::optional<float> flex_shrink;
    std::optional<float> aspect_ratio;
    std::optional<float> gap;        // applied to YGGutterAll if no row/column gap
    std::optional<float> row_gap;    // YGGutterRow
    std::optional<float> column_gap; // YGGutterColumn
  };

  // Measure callback used by leaf nodes (text, image, custom).
  // available_width / available_height are NaN when unconstrained (Yoga
  // reports YGUndefined; we surface NaN to the user).
  struct MeasureResult {
    float width = 0.0f;
    float height = 0.0f;
  };
  using MeasureFn = std::function<MeasureResult(float available_width, float available_height)>;

  struct Node {
    Style style;
    std::vector<Node> children;
    MeasureFn measure;            // empty → no measure func
    bool measure_is_leaf = false; // when true and measure set, mark as leaf
  };

  struct Result {
    float x = 0.0f, y = 0.0f;
    float width = 0.0f, height = 0.0f;
    float padding_left = 0.0f, padding_top = 0.0f, padding_right = 0.0f, padding_bottom = 0.0f;
    std::vector<Result> children;
  };

  // Optional top-level constraint. Use std::nullopt for unconstrained.
  struct Constraint {
    std::optional<float> width;
    std::optional<float> height;
  };

  // Solve the tree. Throws std::runtime_error on internal Yoga config errors.
  // The returned Result tree mirrors `root`'s structure with two filters:
  //   * children whose style sets `display: none` are dropped from the
  //     parent's `children` list (no zero-sized placeholder is emitted),
  //   * `flexDirection: row-reverse` / `column-reverse` reverses the visual
  //     order while keeping main-start at the leading edge — the returned
  //     children stay in the input order so callers can index them naturally.
  Result solve(const Node& root, const Constraint& constraint = {});

} // namespace fxe::layout
