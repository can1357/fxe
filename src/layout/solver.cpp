#include <fxe/layout.hpp>
#include <algorithm>

#include <yoga/Yoga.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fxe::layout {

  namespace {
    using scalar_setter = void (*)(YGNodeRef, float);
    using percent_setter = void (*)(YGNodeRef, float);
    using auto_setter = void (*)(YGNodeRef);
    using edge_scalar_setter = void (*)(YGNodeRef, YGEdge, float);
    using edge_percent_setter = void (*)(YGNodeRef, YGEdge, float);
    using edge_auto_setter = void (*)(YGNodeRef, YGEdge);

    struct measure_context {
      MeasureFn fn;
    };

    struct length_setters {
      scalar_setter set_points;
      percent_setter set_percent;
      auto_setter set_auto;
      std::string_view field_name;
    };

    struct edge_length_setters {
      edge_scalar_setter set_points;
      edge_percent_setter set_percent;
      edge_auto_setter set_auto;
      std::string_view field_name;
    };

    float round3(float value) {
      return std::round(value * 1000.0f) / 1000.0f;
    }

    YGConfigRef g_config() {
      static YGConfigRef config = []() -> YGConfigRef {
        YGConfigRef cfg = YGConfigNew();
        if (cfg == nullptr)
          throw std::runtime_error("layout: failed to allocate Yoga config");
        YGConfigSetPointScaleFactor(cfg, 0.0f);
        return cfg;
      }();
      return config;
    }

    YGFlexDirection to_yg(FlexDirection value) {
      switch (value) {
      case FlexDirection::column:
        return YGFlexDirectionColumn;
      case FlexDirection::column_reverse:
        return YGFlexDirectionColumnReverse;
      case FlexDirection::row:
        return YGFlexDirectionRow;
      case FlexDirection::row_reverse:
        return YGFlexDirectionRowReverse;
      }
      throw std::runtime_error("layout: invalid flex direction");
    }

    YGWrap to_yg(FlexWrap value) {
      switch (value) {
      case FlexWrap::nowrap:
        return YGWrapNoWrap;
      case FlexWrap::wrap:
        return YGWrapWrap;
      case FlexWrap::wrap_reverse:
        return YGWrapWrapReverse;
      }
      throw std::runtime_error("layout: invalid flex wrap");
    }

    YGAlign to_yg(Align value) {
      switch (value) {
      case Align::auto_:
        return YGAlignAuto;
      case Align::flex_start:
        return YGAlignFlexStart;
      case Align::center:
        return YGAlignCenter;
      case Align::flex_end:
        return YGAlignFlexEnd;
      case Align::stretch:
        return YGAlignStretch;
      case Align::baseline:
        return YGAlignBaseline;
      case Align::space_between:
        return YGAlignSpaceBetween;
      case Align::space_around:
        return YGAlignSpaceAround;
      case Align::space_evenly:
        return YGAlignSpaceEvenly;
      }
      throw std::runtime_error("layout: invalid align");
    }

    YGJustify to_yg(Justify value) {
      switch (value) {
      case Justify::flex_start:
        return YGJustifyFlexStart;
      case Justify::center:
        return YGJustifyCenter;
      case Justify::flex_end:
        return YGJustifyFlexEnd;
      case Justify::space_between:
        return YGJustifySpaceBetween;
      case Justify::space_around:
        return YGJustifySpaceAround;
      case Justify::space_evenly:
        return YGJustifySpaceEvenly;
      }
      throw std::runtime_error("layout: invalid justify content");
    }

    YGPositionType to_yg(PositionType value) {
      switch (value) {
      case PositionType::relative:
        return YGPositionTypeRelative;
      case PositionType::absolute:
        return YGPositionTypeAbsolute;
      }
      throw std::runtime_error("layout: invalid position type");
    }

    YGDisplay to_yg(Display value) {
      switch (value) {
      case Display::flex:
        return YGDisplayFlex;
      case Display::none:
        return YGDisplayNone;
      }
      throw std::runtime_error("layout: invalid display");
    }

    YGOverflow to_yg(Overflow value) {
      switch (value) {
      case Overflow::visible:
        return YGOverflowVisible;
      case Overflow::hidden:
        return YGOverflowHidden;
      case Overflow::scroll:
        return YGOverflowScroll;
      }
      throw std::runtime_error("layout: invalid overflow");
    }

    YGEdge to_yg(Edge edge) {
      switch (edge) {
      case Edge::left:
        return YGEdgeLeft;
      case Edge::top:
        return YGEdgeTop;
      case Edge::right:
        return YGEdgeRight;
      case Edge::bottom:
        return YGEdgeBottom;
      }
      throw std::runtime_error("layout: invalid edge");
    }

    void apply_length(YGNodeRef node, const Length& value, const length_setters& setters) {
      switch (value.kind) {
      case LengthKind::undefined:
        return;
      case LengthKind::points:
        setters.set_points(node, value.value);
        return;
      case LengthKind::percent:
        setters.set_percent(node, value.value);
        return;
      case LengthKind::auto_:
        if (setters.set_auto == nullptr) {
          throw std::runtime_error(std::string("layout: 'auto' not supported on ") +
                                   std::string(setters.field_name));
        }
        setters.set_auto(node);
        return;
      }
      throw std::runtime_error("layout: invalid length kind");
    }

    void apply_edge_length(YGNodeRef node, YGEdge edge, const Length& value,
                           const edge_length_setters& setters) {
      switch (value.kind) {
      case LengthKind::undefined:
        return;
      case LengthKind::points:
        setters.set_points(node, edge, value.value);
        return;
      case LengthKind::percent:
        setters.set_percent(node, edge, value.value);
        return;
      case LengthKind::auto_:
        if (setters.set_auto == nullptr) {
          throw std::runtime_error(std::string("layout: 'auto' not supported on ") +
                                   std::string(setters.field_name));
        }
        setters.set_auto(node, edge);
        return;
      }
      throw std::runtime_error("layout: invalid length kind");
    }

    void apply_style(YGNodeRef node, const Style& style) {
      apply_length(
          node, style.width,
          {&YGNodeStyleSetWidth, &YGNodeStyleSetWidthPercent, &YGNodeStyleSetWidthAuto, "width"});
      apply_length(node, style.height,
                   {&YGNodeStyleSetHeight, &YGNodeStyleSetHeightPercent, &YGNodeStyleSetHeightAuto,
                    "height"});
      apply_length(node, style.min_width,
                   {&YGNodeStyleSetMinWidth, &YGNodeStyleSetMinWidthPercent, nullptr, "min_width"});
      apply_length(
          node, style.min_height,
          {&YGNodeStyleSetMinHeight, &YGNodeStyleSetMinHeightPercent, nullptr, "min_height"});
      apply_length(node, style.max_width,
                   {&YGNodeStyleSetMaxWidth, &YGNodeStyleSetMaxWidthPercent, nullptr, "max_width"});
      apply_length(
          node, style.max_height,
          {&YGNodeStyleSetMaxHeight, &YGNodeStyleSetMaxHeightPercent, nullptr, "max_height"});
      apply_length(node, style.flex_basis,
                   {&YGNodeStyleSetFlexBasis, &YGNodeStyleSetFlexBasisPercent,
                    &YGNodeStyleSetFlexBasisAuto, "flex_basis"});

      apply_edge_length(
          node, YGEdgeLeft, style.padding[static_cast<std::size_t>(Edge::left)],
          {&YGNodeStyleSetPadding, &YGNodeStyleSetPaddingPercent, nullptr, "padding.left"});
      apply_edge_length(
          node, YGEdgeTop, style.padding[static_cast<std::size_t>(Edge::top)],
          {&YGNodeStyleSetPadding, &YGNodeStyleSetPaddingPercent, nullptr, "padding.top"});
      apply_edge_length(
          node, YGEdgeRight, style.padding[static_cast<std::size_t>(Edge::right)],
          {&YGNodeStyleSetPadding, &YGNodeStyleSetPaddingPercent, nullptr, "padding.right"});
      apply_edge_length(
          node, YGEdgeBottom, style.padding[static_cast<std::size_t>(Edge::bottom)],
          {&YGNodeStyleSetPadding, &YGNodeStyleSetPaddingPercent, nullptr, "padding.bottom"});

      apply_edge_length(node, YGEdgeLeft, style.margin[static_cast<std::size_t>(Edge::left)],
                        {&YGNodeStyleSetMargin, &YGNodeStyleSetMarginPercent,
                         &YGNodeStyleSetMarginAuto, "margin.left"});
      apply_edge_length(node, YGEdgeTop, style.margin[static_cast<std::size_t>(Edge::top)],
                        {&YGNodeStyleSetMargin, &YGNodeStyleSetMarginPercent,
                         &YGNodeStyleSetMarginAuto, "margin.top"});
      apply_edge_length(node, YGEdgeRight, style.margin[static_cast<std::size_t>(Edge::right)],
                        {&YGNodeStyleSetMargin, &YGNodeStyleSetMarginPercent,
                         &YGNodeStyleSetMarginAuto, "margin.right"});
      apply_edge_length(node, YGEdgeBottom, style.margin[static_cast<std::size_t>(Edge::bottom)],
                        {&YGNodeStyleSetMargin, &YGNodeStyleSetMarginPercent,
                         &YGNodeStyleSetMarginAuto, "margin.bottom"});

      // Public API indexes absolute positions as left/top/right/bottom. Yoga uses
      // the same edges, but the field array is not in the same order as CSS shorthands.
      apply_edge_length(
          node, to_yg(Edge::left), style.position[static_cast<std::size_t>(Edge::left)],
          {&YGNodeStyleSetPosition, &YGNodeStyleSetPositionPercent, nullptr, "position.left"});
      apply_edge_length(
          node, to_yg(Edge::top), style.position[static_cast<std::size_t>(Edge::top)],
          {&YGNodeStyleSetPosition, &YGNodeStyleSetPositionPercent, nullptr, "position.top"});
      apply_edge_length(
          node, to_yg(Edge::right), style.position[static_cast<std::size_t>(Edge::right)],
          {&YGNodeStyleSetPosition, &YGNodeStyleSetPositionPercent, nullptr, "position.right"});
      apply_edge_length(
          node, to_yg(Edge::bottom), style.position[static_cast<std::size_t>(Edge::bottom)],
          {&YGNodeStyleSetPosition, &YGNodeStyleSetPositionPercent, nullptr, "position.bottom"});

      if (style.flex_direction.has_value())
        YGNodeStyleSetFlexDirection(node, to_yg(*style.flex_direction));
      if (style.flex_wrap.has_value())
        YGNodeStyleSetFlexWrap(node, to_yg(*style.flex_wrap));
      if (style.justify_content.has_value())
        YGNodeStyleSetJustifyContent(node, to_yg(*style.justify_content));
      if (style.align_items.has_value())
        YGNodeStyleSetAlignItems(node, to_yg(*style.align_items));
      if (style.align_self.has_value())
        YGNodeStyleSetAlignSelf(node, to_yg(*style.align_self));
      if (style.align_content.has_value())
        YGNodeStyleSetAlignContent(node, to_yg(*style.align_content));
      if (style.position_type.has_value())
        YGNodeStyleSetPositionType(node, to_yg(*style.position_type));
      if (style.display.has_value())
        YGNodeStyleSetDisplay(node, to_yg(*style.display));
      if (style.overflow.has_value())
        YGNodeStyleSetOverflow(node, to_yg(*style.overflow));

      if (style.flex.has_value())
        YGNodeStyleSetFlex(node, *style.flex);
      if (style.flex_grow.has_value())
        YGNodeStyleSetFlexGrow(node, *style.flex_grow);
      if (style.flex_shrink.has_value())
        YGNodeStyleSetFlexShrink(node, *style.flex_shrink);
      if (style.aspect_ratio.has_value() && *style.aspect_ratio > 0.0f) {
        YGNodeStyleSetAspectRatio(node, *style.aspect_ratio);
      }

      // CSS-style fallback: `gap` seeds both axes, then explicit row/column gaps override.
      if (style.gap.has_value())
        YGNodeStyleSetGap(node, YGGutterAll, *style.gap);
      if (style.row_gap.has_value())
        YGNodeStyleSetGap(node, YGGutterRow, *style.row_gap);
      if (style.column_gap.has_value())
        YGNodeStyleSetGap(node, YGGutterColumn, *style.column_gap);
    }

    float to_measure_input(float value, YGMeasureMode mode) {
      return mode == YGMeasureModeUndefined ? std::numeric_limits<float>::quiet_NaN() : value;
    }

    YGSize measure_bridge(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                          YGMeasureMode height_mode) {
      const auto* context = static_cast<const measure_context*>(YGNodeGetContext(node));
      if (context == nullptr)
        return YGSize{0.0f, 0.0f};
      const MeasureResult measured =
          context->fn(to_measure_input(width, width_mode), to_measure_input(height, height_mode));
      return YGSize{measured.width, measured.height};
    }

    bool is_reverse(const Node& node) {
      return node.style.flex_direction.has_value() &&
             (*node.style.flex_direction == FlexDirection::row_reverse ||
              *node.style.flex_direction == FlexDirection::column_reverse);
    }

    void populate_node(YGNodeRef yg_node, const Node& node,
                       std::vector<std::unique_ptr<measure_context>>& contexts) {
      // Translate row-reverse / column-reverse into normal direction + reversed
      // child insertion. Yoga's reverse implementation packs at the trailing
      // edge (right / bottom); the fxe-ui contract reverses the visual order
      // while keeping main-start at the leading edge.
      Style adjusted = node.style;
      const bool reversed = is_reverse(node);
      if (adjusted.flex_direction.has_value()) {
        if (*adjusted.flex_direction == FlexDirection::row_reverse)
          adjusted.flex_direction = FlexDirection::row;
        else if (*adjusted.flex_direction == FlexDirection::column_reverse)
          adjusted.flex_direction = FlexDirection::column;
      }
      apply_style(yg_node, adjusted);

      // Yoga only permits measure functions on leaves. Keep the structural tree
      // truthful: nodes with children stay container nodes even if measure_is_leaf is set.
      if (node.measure && node.children.empty()) {
        // A measure callback's reported size is authoritative for both axes.
        // Without an explicit cross-axis size, default `align-items: stretch`
        // would override the measured cross dimension. Opt out so the
        // returned size sticks unless the caller explicitly chose a different
        // alignment.
        if (!adjusted.align_self.has_value())
          YGNodeStyleSetAlignSelf(yg_node, YGAlignFlexStart);
        auto context = std::make_unique<measure_context>();
        context->fn = node.measure;
        YGNodeSetContext(yg_node, context.get());
        YGNodeSetMeasureFunc(yg_node, &measure_bridge);
        contexts.push_back(std::move(context));
      }

      const std::size_t count = node.children.size();
      for (std::size_t index = 0; index < count; ++index) {
        const std::size_t source_index = reversed ? (count - 1 - index) : index;
        YGNodeRef child = YGNodeNew();
        if (child == nullptr)
          throw std::runtime_error("layout: failed to allocate Yoga node");
        try {
          populate_node(child, node.children[source_index], contexts);
          YGNodeInsertChild(yg_node, child, index);
        } catch (...) {
          YGNodeFreeRecursive(child);
          throw;
        }
      }
    }

    bool is_display_none(const Node& node) {
      return node.style.display.has_value() && *node.style.display == Display::none;
    }

    Result collect_result(YGNodeConstRef yg_node, const Node& node, float parent_pad_left,
                          float parent_pad_top) {
      Result result;
      result.x = round3(YGNodeLayoutGetLeft(yg_node));
      result.y = round3(YGNodeLayoutGetTop(yg_node));
      result.width = round3(YGNodeLayoutGetWidth(yg_node));
      result.height = round3(YGNodeLayoutGetHeight(yg_node));
      result.padding_left = round3(YGNodeLayoutGetPadding(yg_node, YGEdgeLeft));
      result.padding_top = round3(YGNodeLayoutGetPadding(yg_node, YGEdgeTop));
      result.padding_right = round3(YGNodeLayoutGetPadding(yg_node, YGEdgeRight));
      result.padding_bottom = round3(YGNodeLayoutGetPadding(yg_node, YGEdgeBottom));

      // W3C contract: absolute children with explicit insets are positioned
      // relative to the parent's padding edge, not its border edge. Yoga
      // reports raw inset offsets; add the parent padding back in so the
      // public API is content-box-relative.
      if (node.style.position_type.has_value() &&
          *node.style.position_type == PositionType::absolute) {
        if (node.style.position[static_cast<std::size_t>(Edge::left)].kind != LengthKind::undefined)
          result.x += parent_pad_left;
        if (node.style.position[static_cast<std::size_t>(Edge::top)].kind != LengthKind::undefined)
          result.y += parent_pad_top;
      }

      const std::size_t count = node.children.size();
      const bool reversed = is_reverse(node);
      result.children.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        const std::size_t source_index = reversed ? (count - 1 - i) : i;
        const Node& child_node = node.children[source_index];
        if (is_display_none(child_node))
          continue;
        YGNodeConstRef child_yg = YGNodeGetChild(const_cast<YGNodeRef>(yg_node), i);
        result.children.push_back(
            collect_result(child_yg, child_node, result.padding_left, result.padding_top));
      }
      // For reversed parents the visit order above walks Yoga child slots in
      // node-reverse order, so `result.children` lines up backwards relative
      // to `node.children`. Flip it so the public API matches input order.
      if (reversed)
        std::reverse(result.children.begin(), result.children.end());
      return result;
    }
  } // namespace

  Result solve(const Node& root, const Constraint& constraint) {
    YGNodeRef root_yg = YGNodeNewWithConfig(g_config());
    if (root_yg == nullptr)
      throw std::runtime_error("layout: failed to allocate Yoga root node");

    std::vector<std::unique_ptr<measure_context>> measure_contexts;
    try {
      populate_node(root_yg, root, measure_contexts);
      const float owner_width = constraint.width.value_or(YGUndefined);
      const float owner_height = constraint.height.value_or(YGUndefined);
      YGNodeCalculateLayout(root_yg, owner_width, owner_height, YGDirectionLTR);
      Result result = collect_result(root_yg, root, 0.0f, 0.0f);
      YGNodeFreeRecursive(root_yg);
      return result;
    } catch (...) {
      YGNodeFreeRecursive(root_yg);
      throw;
    }
  }

} // namespace fxe::layout
