#include "bind_layout.hpp"

#include <cctype>
#include <cmath>
#include <exception>
#include <fxe/font.hpp>
#include <fxe/layout.hpp>
#include <fxe/primitives.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;
    namespace layout = fxe::layout;

    constexpr int k_left = static_cast<int>(layout::Edge::left);
    constexpr int k_top = static_cast<int>(layout::Edge::top);
    constexpr int k_right = static_cast<int>(layout::Edge::right);
    constexpr int k_bottom = static_cast<int>(layout::Edge::bottom);

    struct CallState {
      v8::Isolate* iso = nullptr;
      v8::Global<v8::Value> pending_exception;
    };

    double num(Local<Context> ctx, Local<Value> v, double def = 0.0) {
      return v->NumberValue(ctx).FromMaybe(def);
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    std::string value_repr(Isolate* iso, Local<Context> ctx, Local<Value> value) {
      Local<String> detail;
      if (value->ToDetailString(ctx).ToLocal(&detail))
        return utf8(iso, detail);
      return utf8(iso, value);
    }

    bool get_prop(Local<Context> ctx, Local<Object> obj, Local<String> key, Local<Value>& out,
                  bool& present) {
      present = obj->Has(ctx, key).FromMaybe(false);
      if (!present)
        return true;
      return obj->Get(ctx, key).ToLocal(&out);
    }

    bool parse_finite_number(Isolate* iso, Local<Context> ctx, Local<Value> value, const char* what,
                             float& out) {
      if (!value->IsNumber())
        return throw_type_error(iso, "Layout.solve: {} must be a number", what);
      const double v = num(ctx, value);
      if (!std::isfinite(v))
        return throw_type_error(iso, "Layout.solve: {} must be finite", what);
      out = static_cast<float>(v);
      return true;
    }

    bool parse_optional_number_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                    Local<String> key, const char* what,
                                    std::optional<float>& out) {
      Local<Value> value;
      bool present = false;
      if (!get_prop(ctx, obj, key, value, present))
        return false;
      if (!present || value->IsUndefined())
        return true;
      float parsed = 0.0f;
      if (!parse_finite_number(iso, ctx, value, what, parsed))
        return false;
      out = parsed;
      return true;
    }

    bool parse_percent_string(const std::string& s, float& out) {
      if (s.size() < 2 || s.back() != '%')
        return false;
      size_t i = 0;
      if (s[i] == '-') {
        ++i;
        if (i == s.size() - 1)
          return false;
      }
      if (i >= s.size() - 1 || !std::isdigit(static_cast<unsigned char>(s[i])))
        return false;
      while (i < s.size() - 1 && std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
      if (i < s.size() - 1) {
        if (s[i] != '.')
          return false;
        ++i;
        if (i >= s.size() - 1 || !std::isdigit(static_cast<unsigned char>(s[i])))
          return false;
        while (i < s.size() - 1 && std::isdigit(static_cast<unsigned char>(s[i])))
          ++i;
      }
      if (i != s.size() - 1)
        return false;
      try {
        const float parsed = std::stof(s.substr(0, s.size() - 1));
        if (!std::isfinite(parsed))
          return false;
        out = parsed;
        return true;
      } catch (...) {
        return false;
      }
    }

    bool parse_length(Isolate* iso, Local<Context> ctx, Local<Value> value, layout::Length& out) {
      if (value->IsNumber()) {
        const double points = num(ctx, value);
        if (!std::isfinite(points))
          return throw_type_error(iso, "unsupported length: {}", value_repr(iso, ctx, value));
        out.kind = layout::LengthKind::points;
        out.value = static_cast<float>(points);
        return true;
      }
      if (value->IsString()) {
        auto s = value.As<String>();
        if (s == "auto"_v8) {
          out.kind = layout::LengthKind::auto_;
          out.value = 0.0f;
          return true;
        }
        const std::string raw = utf8(iso, s);
        float percent = 0.0f;
        if (parse_percent_string(raw, percent)) {
          out.kind = layout::LengthKind::percent;
          out.value = percent;
          return true;
        }
      }
      return throw_type_error(iso, "unsupported length: {}", value_repr(iso, ctx, value));
    }

    bool parse_display(Isolate* iso, Local<String> s, std::optional<layout::Display>& out) {
      if (s == "flex"_v8)
        out = layout::Display::flex;
      else if (s == "none"_v8)
        out = layout::Display::none;
      else
        return throw_type_error(iso, "Layout.solve: unsupported display '{}'", utf8(iso, s));
      return true;
    }

    bool parse_flex_direction(Isolate* iso, Local<String> s,
                              std::optional<layout::FlexDirection>& out) {
      if (s == "row"_v8)
        out = layout::FlexDirection::row;
      else if (s == "column"_v8)
        out = layout::FlexDirection::column;
      else if (s == "row-reverse"_v8)
        out = layout::FlexDirection::row_reverse;
      else if (s == "column-reverse"_v8)
        out = layout::FlexDirection::column_reverse;
      else
        return throw_type_error(iso, "Layout.solve: unsupported flexDirection '{}'", utf8(iso, s));
      return true;
    }

    bool parse_flex_wrap(Isolate* iso, Local<String> s, std::optional<layout::FlexWrap>& out) {
      if (s == "nowrap"_v8)
        out = layout::FlexWrap::nowrap;
      else if (s == "wrap"_v8)
        out = layout::FlexWrap::wrap;
      else if (s == "wrap-reverse"_v8)
        out = layout::FlexWrap::wrap_reverse;
      else
        return throw_type_error(iso, "Layout.solve: unsupported flexWrap '{}'", utf8(iso, s));
      return true;
    }

    bool parse_justify(Isolate* iso, Local<String> s, std::optional<layout::Justify>& out) {
      if (s == "flex-start"_v8)
        out = layout::Justify::flex_start;
      else if (s == "flex-end"_v8)
        out = layout::Justify::flex_end;
      else if (s == "center"_v8)
        out = layout::Justify::center;
      else if (s == "space-between"_v8)
        out = layout::Justify::space_between;
      else if (s == "space-around"_v8)
        out = layout::Justify::space_around;
      else if (s == "space-evenly"_v8)
        out = layout::Justify::space_evenly;
      else
        return throw_type_error(iso, "Layout.solve: unsupported justifyContent '{}'", utf8(iso, s));
      return true;
    }

    bool parse_align(Isolate* iso, Local<String> s, std::optional<layout::Align>& out,
                     const char* field, bool allow_space_values) {
      if (s == "auto"_v8)
        out = layout::Align::auto_;
      else if (s == "flex-start"_v8)
        out = layout::Align::flex_start;
      else if (s == "flex-end"_v8)
        out = layout::Align::flex_end;
      else if (s == "center"_v8)
        out = layout::Align::center;
      else if (s == "stretch"_v8)
        out = layout::Align::stretch;
      else if (s == "baseline"_v8)
        out = layout::Align::baseline;
      else if (allow_space_values && s == "space-between"_v8)
        out = layout::Align::space_between;
      else if (allow_space_values && s == "space-around"_v8)
        out = layout::Align::space_around;
      else if (allow_space_values && s == "space-evenly"_v8)
        out = layout::Align::space_evenly;
      else
        return throw_type_error(iso, "Layout.solve: unsupported {} '{}'", field, utf8(iso, s));
      return true;
    }

    bool parse_position_type(Isolate* iso, Local<String> s,
                             std::optional<layout::PositionType>& out) {
      if (s == "relative"_v8)
        out = layout::PositionType::relative;
      else if (s == "absolute"_v8)
        out = layout::PositionType::absolute;
      else
        return throw_type_error(iso, "Layout.solve: unsupported position '{}'", utf8(iso, s));
      return true;
    }

    bool parse_overflow(Isolate* iso, Local<String> s, std::optional<layout::Overflow>& out) {
      if (s == "visible"_v8)
        out = layout::Overflow::visible;
      else if (s == "hidden"_v8)
        out = layout::Overflow::hidden;
      else if (s == "scroll"_v8)
        out = layout::Overflow::scroll;
      else
        return throw_type_error(iso, "Layout.solve: unsupported overflow '{}'", utf8(iso, s));
      return true;
    }

    template <typename Parser, typename T>
    bool parse_optional_string_enum_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                         Local<String> key, Parser&& parser, T& out) {
      Local<Value> value;
      bool present = false;
      if (!get_prop(ctx, obj, key, value, present))
        return false;
      if (!present || value->IsUndefined())
        return true;
      if (!value->IsString())
        return throw_type_error(iso, "Layout.solve: {} must be a string", utf8(iso, key));
      return parser(iso, value.As<String>(), out);
    }

    bool parse_optional_length_prop(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                                    Local<String> key, layout::Length& out) {
      Local<Value> value;
      bool present = false;
      if (!get_prop(ctx, obj, key, value, present))
        return false;
      if (!present || value->IsUndefined())
        return true;
      return parse_length(iso, ctx, value, out);
    }

    bool parse_box_edges(Isolate* iso, Local<Context> ctx, Local<Object> obj, Local<String> all_key,
                         Local<String> x_key, Local<String> y_key, Local<String> top_key,
                         Local<String> right_key, Local<String> bottom_key, Local<String> left_key,
                         layout::Length (&edges)[4]) {
      Local<Value> value;
      bool present = false;
      layout::Length all{};
      layout::Length x{};
      layout::Length y{};
      layout::Length top{};
      layout::Length right{};
      layout::Length bottom{};
      layout::Length left{};
      bool has_all = false;
      bool has_x = false;
      bool has_y = false;
      bool has_top = false;
      bool has_right = false;
      bool has_bottom = false;
      bool has_left = false;

      if (!get_prop(ctx, obj, all_key, value, present))
        return false;
      if (present && !value->IsUndefined()) {
        if (!parse_length(iso, ctx, value, all))
          return false;
        has_all = true;
      }
      if (!get_prop(ctx, obj, x_key, value, present))
        return false;
      if (present && !value->IsUndefined()) {
        if (!parse_length(iso, ctx, value, x))
          return false;
        has_x = true;
      }
      if (!get_prop(ctx, obj, y_key, value, present))
        return false;
      if (present && !value->IsUndefined()) {
        if (!parse_length(iso, ctx, value, y))
          return false;
        has_y = true;
      }
      if (!get_prop(ctx, obj, top_key, value, present))
        return false;
      if (present && !value->IsUndefined()) {
        if (!parse_length(iso, ctx, value, top))
          return false;
        has_top = true;
      }
      if (!get_prop(ctx, obj, right_key, value, present))
        return false;
      if (present && !value->IsUndefined()) {
        if (!parse_length(iso, ctx, value, right))
          return false;
        has_right = true;
      }
      if (!get_prop(ctx, obj, bottom_key, value, present))
        return false;
      if (present && !value->IsUndefined()) {
        if (!parse_length(iso, ctx, value, bottom))
          return false;
        has_bottom = true;
      }
      if (!get_prop(ctx, obj, left_key, value, present))
        return false;
      if (present && !value->IsUndefined()) {
        if (!parse_length(iso, ctx, value, left))
          return false;
        has_left = true;
      }

      if (has_all) {
        edges[k_left] = all;
        edges[k_top] = all;
        edges[k_right] = all;
        edges[k_bottom] = all;
      }
      if (has_x) {
        edges[k_left] = x;
        edges[k_right] = x;
      }
      if (has_y) {
        edges[k_top] = y;
        edges[k_bottom] = y;
      }
      if (has_top)
        edges[k_top] = top;
      if (has_right)
        edges[k_right] = right;
      if (has_bottom)
        edges[k_bottom] = bottom;
      if (has_left)
        edges[k_left] = left;
      return true;
    }

    bool parse_style(Isolate* iso, Local<Context> ctx, Local<Value> value, layout::Style& out) {
      if (value.IsEmpty() || value->IsUndefined())
        return true;
      if (!value->IsObject() || value->IsNull())
        return throw_type_error(iso, "Layout.solve: style must be an object");
      auto obj = value.As<Object>();

      if (!parse_optional_string_enum_prop(iso, ctx, obj, "display"_v8(iso), parse_display,
                                           out.display) ||
          !parse_optional_length_prop(iso, ctx, obj, "width"_v8(iso), out.width) ||
          !parse_optional_length_prop(iso, ctx, obj, "height"_v8(iso), out.height) ||
          !parse_optional_length_prop(iso, ctx, obj, "minWidth"_v8(iso), out.min_width) ||
          !parse_optional_length_prop(iso, ctx, obj, "minHeight"_v8(iso), out.min_height) ||
          !parse_optional_length_prop(iso, ctx, obj, "maxWidth"_v8(iso), out.max_width) ||
          !parse_optional_length_prop(iso, ctx, obj, "maxHeight"_v8(iso), out.max_height) ||
          !parse_box_edges(iso, ctx, obj, "padding"_v8(iso), "paddingX"_v8(iso), "paddingY"_v8(iso),
                           "paddingTop"_v8(iso), "paddingRight"_v8(iso), "paddingBottom"_v8(iso),
                           "paddingLeft"_v8(iso), out.padding) ||
          !parse_box_edges(iso, ctx, obj, "margin"_v8(iso), "marginX"_v8(iso), "marginY"_v8(iso),
                           "marginTop"_v8(iso), "marginRight"_v8(iso), "marginBottom"_v8(iso),
                           "marginLeft"_v8(iso), out.margin) ||
          !parse_optional_string_enum_prop(iso, ctx, obj, "flexDirection"_v8(iso),
                                           parse_flex_direction, out.flex_direction) ||
          !parse_optional_string_enum_prop(iso, ctx, obj, "flexWrap"_v8(iso), parse_flex_wrap,
                                           out.flex_wrap) ||
          !parse_optional_string_enum_prop(iso, ctx, obj, "justifyContent"_v8(iso), parse_justify,
                                           out.justify_content) ||
          !parse_optional_string_enum_prop(
              iso, ctx, obj, "alignItems"_v8(iso),
              [](Isolate* i, Local<String> s, std::optional<layout::Align>& out_align) {
                return parse_align(i, s, out_align, "alignItems", true);
              },
              out.align_items) ||
          !parse_optional_string_enum_prop(
              iso, ctx, obj, "alignSelf"_v8(iso),
              [](Isolate* i, Local<String> s, std::optional<layout::Align>& out_align) {
                return parse_align(i, s, out_align, "alignSelf", true);
              },
              out.align_self) ||
          !parse_optional_string_enum_prop(
              iso, ctx, obj, "alignContent"_v8(iso),
              [](Isolate* i, Local<String> s, std::optional<layout::Align>& out_align) {
                return parse_align(i, s, out_align, "alignContent", true);
              },
              out.align_content) ||
          !parse_optional_number_prop(iso, ctx, obj, "flex"_v8(iso), "flex", out.flex) ||
          !parse_optional_number_prop(iso, ctx, obj, "flexGrow"_v8(iso), "flexGrow",
                                      out.flex_grow) ||
          !parse_optional_number_prop(iso, ctx, obj, "flexShrink"_v8(iso), "flexShrink",
                                      out.flex_shrink) ||
          !parse_optional_length_prop(iso, ctx, obj, "flexBasis"_v8(iso), out.flex_basis) ||
          !parse_optional_number_prop(iso, ctx, obj, "gap"_v8(iso), "gap", out.gap) ||
          !parse_optional_number_prop(iso, ctx, obj, "rowGap"_v8(iso), "rowGap", out.row_gap) ||
          !parse_optional_number_prop(iso, ctx, obj, "columnGap"_v8(iso), "columnGap",
                                      out.column_gap) ||
          !parse_optional_string_enum_prop(iso, ctx, obj, "position"_v8(iso), parse_position_type,
                                           out.position_type) ||
          !parse_optional_length_prop(iso, ctx, obj, "top"_v8(iso), out.position[k_top]) ||
          !parse_optional_length_prop(iso, ctx, obj, "right"_v8(iso), out.position[k_right]) ||
          !parse_optional_length_prop(iso, ctx, obj, "bottom"_v8(iso), out.position[k_bottom]) ||
          !parse_optional_length_prop(iso, ctx, obj, "left"_v8(iso), out.position[k_left]) ||
          !parse_optional_number_prop(iso, ctx, obj, "aspectRatio"_v8(iso), "aspectRatio",
                                      out.aspect_ratio) ||
          !parse_optional_string_enum_prop(iso, ctx, obj, "overflow"_v8(iso), parse_overflow,
                                           out.overflow)) {
        return false;
      }
      return true;
    }

    bool parse_constraint(Isolate* iso, Local<Context> ctx, Local<Value> value,
                          layout::Constraint& out) {
      if (value.IsEmpty() || value->IsUndefined() || value->IsNull())
        return true;
      if (!value->IsObject())
        return throw_type_error(iso, "Layout.solve: constraint must be an object");
      auto obj = value.As<Object>();
      return parse_optional_number_prop(iso, ctx, obj, "width"_v8(iso), "constraint.width",
                                        out.width) &&
             parse_optional_number_prop(iso, ctx, obj, "height"_v8(iso), "constraint.height",
                                        out.height);
    }

    bool set_pending_exception(CallState* state, Local<Value> ex) {
      if (state->pending_exception.IsEmpty())
        state->pending_exception.Reset(state->iso, ex);
      return false;
    }

    bool expect_object(Isolate* iso, Local<Value> value, const char* what, Local<Object>& out) {
      if (!value->IsObject() || value->IsNull())
        return throw_type_error(iso, "Layout.solve: {} must be an object", what);
      out = value.As<Object>();
      return true;
    }

    bool build_node(Isolate* iso, Local<Context> ctx, Local<Value> value, CallState* state,
                    layout::Node& out) {
      Local<Object> obj;
      if (!expect_object(iso, value, "root/child node", obj))
        return false;

      Local<Value> style_value;
      bool present = false;
      if (!get_prop(ctx, obj, "style"_v8(iso), style_value, present))
        return false;
      if (present && !style_value->IsUndefined()) {
        if (!parse_style(iso, ctx, style_value, out.style))
          return false;
      }

      Local<Value> children_value;
      present = false;
      if (!get_prop(ctx, obj, "children"_v8(iso), children_value, present))
        return false;
      if (present && !children_value->IsUndefined() && !children_value->IsNull()) {
        if (!children_value->IsArray())
          return throw_type_error(iso, "Layout.solve: children must be an array");
        auto children = children_value.As<Array>();
        out.children.reserve(children->Length());
        for (u32 i = 0; i < children->Length(); ++i) {
          Local<Value> child_value;
          if (!children->Get(ctx, i).ToLocal(&child_value))
            return false;
          auto& child = out.children.emplace_back();
          if (!build_node(iso, ctx, child_value, state, child))
            return false;
        }
      }

      Local<Value> measure_value;
      present = false;
      if (!get_prop(ctx, obj, "measure"_v8(iso), measure_value, present))
        return false;
      if (!present || measure_value->IsUndefined() || measure_value->IsNull())
        return true;
      Local<Object> measure_obj;
      if (!expect_object(iso, measure_value, "measure", measure_obj))
        return false;

      Local<Value> kind_value;
      if (!measure_obj->Get(ctx, "kind"_v8(iso)).ToLocal(&kind_value) || !kind_value->IsString())
        return throw_type_error(iso, "Layout.solve: measure.kind must be a string");
      auto kind_str = kind_value.As<String>();
      out.measure_is_leaf = true;

      if (kind_str == "text"_v8) {
        Local<Value> text_value;
        Local<Value> font_size_value;
        if (!measure_obj->Get(ctx, "text"_v8(iso)).ToLocal(&text_value) || !text_value->IsString())
          return throw_type_error(iso, "Layout.solve: text measure.text must be a string");
        if (!measure_obj->Get(ctx, "fontSize"_v8(iso)).ToLocal(&font_size_value))
          return false;
        float font_size = 0.0f;
        if (!parse_finite_number(iso, ctx, font_size_value, "measure.fontSize", font_size))
          return false;
        const std::string text = utf8(iso, text_value);
        out.measure = [text, font_size](float, float) {
          const auto v = primitives::calc_text(text, get_font_info(), font_size);
          return layout::MeasureResult{v.x, v.y};
        };
        return true;
      }

      if (kind_str == "image"_v8) {
        Local<Value> width_value;
        Local<Value> height_value;
        if (!measure_obj->Get(ctx, "width"_v8(iso)).ToLocal(&width_value) ||
            !measure_obj->Get(ctx, "height"_v8(iso)).ToLocal(&height_value)) {
          return false;
        }
        float width = 0.0f;
        float height = 0.0f;
        if (!parse_finite_number(iso, ctx, width_value, "measure.width", width) ||
            !parse_finite_number(iso, ctx, height_value, "measure.height", height)) {
          return false;
        }
        out.measure = [width, height](float, float) {
          return layout::MeasureResult{width, height};
        };
        return true;
      }

      if (kind_str == "js"_v8) {
        Local<Value> fn_value;
        if (!measure_obj->Get(ctx, "fn"_v8(iso)).ToLocal(&fn_value))
          return false;
        if (!fn_value->IsFunction())
          return throw_type_error(iso, "Layout.solve: measure.fn must be a function");
        auto fn = std::make_shared<Global<Function>>(iso, fn_value.As<Function>());
        out.measure = [iso, state, fn](float aw, float ah) {
          if (!state->pending_exception.IsEmpty())
            return layout::MeasureResult{};

          Isolate::Scope isolate_scope(iso);
          HandleScope handle_scope(iso);
          Local<Context> ctx = iso->GetCurrentContext();
          Context::Scope context_scope(ctx);
          TryCatch tc(iso);

          auto arg = Object::New(iso);
          if (std::isfinite(aw))
            (void)arg->Set(ctx, "width"_v8(iso), Number::New(iso, static_cast<double>(aw)));
          if (std::isfinite(ah))
            (void)arg->Set(ctx, "height"_v8(iso), Number::New(iso, static_cast<double>(ah)));

          Local<Value> argv[1] = {arg};
          Local<Function> local_fn = Local<Function>::New(iso, *fn);
          Local<Value> ret;
          if (!local_fn->Call(ctx, Undefined(iso), 1, argv).ToLocal(&ret)) {
            return set_pending_exception(state, tc.Exception()), layout::MeasureResult{};
          }
          if (!ret->IsObject() || ret->IsNull()) {
            (void)throw_type_error(
                iso, "Layout.solve: measure.fn must return { width: number, height: number }");
            return set_pending_exception(state, tc.Exception()), layout::MeasureResult{};
          }
          auto ret_obj = ret.As<Object>();
          Local<Value> width_value;
          Local<Value> height_value;
          if (!ret_obj->Get(ctx, "width"_v8(iso)).ToLocal(&width_value) ||
              !ret_obj->Get(ctx, "height"_v8(iso)).ToLocal(&height_value)) {
            return set_pending_exception(state, tc.Exception()), layout::MeasureResult{};
          }
          if (!width_value->IsNumber() || !height_value->IsNumber()) {
            (void)throw_type_error(
                iso, "Layout.solve: measure.fn must return { width: number, height: number }");
            return set_pending_exception(state, tc.Exception()), layout::MeasureResult{};
          }
          const double width = num(ctx, width_value);
          const double height = num(ctx, height_value);
          if (!std::isfinite(width) || !std::isfinite(height)) {
            (void)throw_type_error(iso, "Layout.solve: measure.fn must return finite width/height");
            return set_pending_exception(state, tc.Exception()), layout::MeasureResult{};
          }
          return layout::MeasureResult{static_cast<float>(width), static_cast<float>(height)};
        };
        return true;
      }

      return throw_type_error(iso, "Layout.solve: unsupported measure.kind '{}'",
                              utf8(iso, kind_str));
    }

    Local<Object> result_to_v8(Isolate* iso, Local<Context> ctx, const layout::Result& result) {
      EscapableHandleScope hs(iso);
      auto obj = Object::New(iso);
      (void)obj->Set(ctx, "x"_v8(iso), Number::New(iso, static_cast<double>(result.x)));
      (void)obj->Set(ctx, "y"_v8(iso), Number::New(iso, static_cast<double>(result.y)));
      (void)obj->Set(ctx, "width"_v8(iso), Number::New(iso, static_cast<double>(result.width)));
      (void)obj->Set(ctx, "height"_v8(iso), Number::New(iso, static_cast<double>(result.height)));
      (void)obj->Set(ctx, "paddingLeft"_v8(iso),
                     Number::New(iso, static_cast<double>(result.padding_left)));
      (void)obj->Set(ctx, "paddingTop"_v8(iso),
                     Number::New(iso, static_cast<double>(result.padding_top)));
      (void)obj->Set(ctx, "paddingRight"_v8(iso),
                     Number::New(iso, static_cast<double>(result.padding_right)));
      (void)obj->Set(ctx, "paddingBottom"_v8(iso),
                     Number::New(iso, static_cast<double>(result.padding_bottom)));
      auto children = Array::New(iso, static_cast<int>(result.children.size()));
      for (u32 i = 0; i < result.children.size(); ++i)
        (void)children->Set(ctx, i, result_to_v8(iso, ctx, result.children[i]));
      (void)obj->Set(ctx, "children"_v8(iso), children);
      return hs.Escape(obj);
    }
  } // namespace

  static void layout_solve(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* iso = info.GetIsolate();
    HandleScope hs(iso);
    auto ctx = iso->GetCurrentContext();

    if (info.Length() < 1) {
      (void)throw_type_error(iso, "Layout.solve(root, constraint?)");
      return;
    }

    CallState state;
    state.iso = iso;
    layout::Node root;
    if (!build_node(iso, ctx, info[0], &state, root))
      return;

    layout::Constraint constraint;
    if (info.Length() >= 2 && !parse_constraint(iso, ctx, info[1], constraint))
      return;

    layout::Result result;
    try {
      result = layout::solve(root, constraint);
    } catch (const std::exception& e) {
      (void)throw_range_error(iso, e.what());
      return;
    }

    if (!state.pending_exception.IsEmpty()) {
      iso->ThrowException(Local<Value>::New(iso, state.pending_exception));
      return;
    }

    info.GetReturnValue().Set(result_to_v8(iso, ctx, result));
  }

  void install_layout_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "solve", FunctionTemplate::New(iso, layout_solve));
    global->Set(iso, "Layout", ns);
  }
} // namespace fxe::js
