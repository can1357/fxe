#include "os/a11y.hpp"

#import <AppKit/AppKit.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace {
  using json = nlohmann::json;

  std::shared_ptr<const fxe::os::a11y::snapshot> fxe_accessibility_snapshot_for_view(NSView* view) {
    if (!view)
      return nullptr;
    if (NSWindow* window = view.window) {
      if (auto snap = fxe::os::a11y::snapshot_for_window((__bridge void*)window))
        return snap;
      if (auto snap = fxe::os::a11y::snapshot_for_window(
              reinterpret_cast<void*>(static_cast<uintptr_t>(window.windowNumber))))
        return snap;
    }
    if (auto snap = fxe::os::a11y::snapshot_for_window((__bridge void*)view))
      return snap;
    return nullptr;
  }

  NSString* fxe_nsstring(std::string_view value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding];
  }

  NSString* fxe_json_string_field(const json& obj, std::string_view key) {
    auto it = obj.find(std::string(key));
    if (it == obj.end() || !it->is_string())
      return nil;
    return fxe_nsstring(it->get_ref<const std::string&>());
  }

  double fxe_json_number_field(const json& obj, std::string_view key, double fallback = 0.0) {
    auto it = obj.find(std::string(key));
    if (it == obj.end() || !it->is_number())
      return fallback;
    return it->get<double>();
  }

  bool fxe_json_bool_field(const json& obj, std::string_view key, bool fallback = false) {
    auto it = obj.find(std::string(key));
    if (it == obj.end() || !it->is_boolean())
      return fallback;
    return it->get<bool>();
  }

  NSAccessibilityRole fxe_ns_role_for_string(std::string_view role) {
    if (role == "text" || role == "heading" || role == "status")
      return NSAccessibilityStaticTextRole;
    if (role == "button")
      return NSAccessibilityButtonRole;
    if (role == "link")
      return NSAccessibilityLinkRole;
    if (role == "image")
      return NSAccessibilityImageRole;
    if (role == "textbox")
      return NSAccessibilityTextFieldRole;
    if (role == "searchbox")
      return NSAccessibilityTextFieldRole;
    if (role == "checkbox" || role == "switch")
      return NSAccessibilityCheckBoxRole;
    if (role == "radio")
      return NSAccessibilityRadioButtonRole;
    if (role == "slider")
      return NSAccessibilitySliderRole;
    if (role == "progressbar")
      return NSAccessibilityProgressIndicatorRole;
    if (role == "list")
      return NSAccessibilityListRole;
    if (role == "scrollview")
      return NSAccessibilityScrollAreaRole;
    if (role == "menu")
      return NSAccessibilityMenuRole;
    if (role == "menuitem")
      return NSAccessibilityMenuItemRole;
    if (role == "tablist")
      return NSAccessibilityTabGroupRole;
    if (role == "tab")
      return NSAccessibilityRadioButtonRole;
    return NSAccessibilityGroupRole;
  }

  NSRect fxe_frame_for_node(NSWindow* window, const json& node) {
    auto rect_it = node.find("rect");
    if (rect_it == node.end() || !rect_it->is_object())
      return NSZeroRect;
    const auto& rect = *rect_it;
    NSRect frame = NSMakeRect(fxe_json_number_field(rect, "x"), fxe_json_number_field(rect, "y"),
                              fxe_json_number_field(rect, "width"),
                              fxe_json_number_field(rect, "height"));
    return window ? [window convertRectToScreen:frame] : frame;
  }

  std::vector<std::string> fxe_child_ids_for_node(const json& node_index, const json& child_index,
                                                  std::string_view node_id) {
    std::vector<std::string> out;
    auto child_it = child_index.find(std::string(node_id));
    if (child_it != child_index.end() && child_it->is_array()) {
      out.reserve(child_it->size());
      for (const auto& child_id : *child_it) {
        if (child_id.is_string())
          out.push_back(child_id.get<std::string>());
      }
      return out;
    }
    auto node_it = node_index.find(std::string(node_id));
    if (node_it == node_index.end() || !node_it->is_object())
      return out;
    auto nested_it = node_it->find("children");
    if (nested_it == node_it->end() || !nested_it->is_array())
      return out;
    out.reserve(nested_it->size());
    for (const auto& child : *nested_it) {
      if (!child.is_object())
        continue;
      auto id_it = child.find("id");
      if (id_it != child.end() && id_it->is_string())
        out.push_back(id_it->get<std::string>());
    }
    return out;
  }
}

@interface FxeAXElement : NSAccessibilityElement <NSAccessibility> {
 @private
  __weak id _fxeParent;
  NSArray* _fxeChildren;
  NSString* _fxeRole;
  NSString* _fxeLabel;
  NSString* _fxeIdentifier;
  NSRect _fxeFrame;
  BOOL _fxeFocused;
  BOOL _fxeEnabled;
}
- (instancetype)initWithParent:(id)parent
                          role:(NSAccessibilityRole)role
                         label:(NSString*)label
                    identifier:(NSString*)identifier
                         frame:(NSRect)frame
                       focused:(BOOL)focused
                       enabled:(BOOL)enabled;
- (void)setFxeChildren:(NSArray*)children;
@end

@implementation FxeAXElement
- (instancetype)initWithParent:(id)parent
                          role:(NSAccessibilityRole)role
                         label:(NSString*)label
                    identifier:(NSString*)identifier
                         frame:(NSRect)frame
                       focused:(BOOL)focused
                       enabled:(BOOL)enabled {
  self = [super init];
  if (!self)
    return nil;
  _fxeParent = parent;
  _fxeChildren = @[];
  _fxeRole = [role copy];
  _fxeLabel = [label copy];
  _fxeIdentifier = [identifier copy];
  _fxeFrame = frame;
  _fxeFocused = focused;
  _fxeEnabled = enabled;
  return self;
}

- (void)setFxeChildren:(NSArray*)children {
  _fxeChildren = [children copy];
}

- (BOOL)isAccessibilityElement {
  return YES;
}

- (id)accessibilityParent {
  return _fxeParent;
}

- (NSArray*)accessibilityChildren {
  return _fxeChildren ? _fxeChildren : @[];
}

- (NSAccessibilityRole)accessibilityRole {
  return _fxeRole ? _fxeRole : NSAccessibilityGroupRole;
}

- (NSString*)accessibilityLabel {
  return _fxeLabel;
}

- (NSString*)accessibilityIdentifier {
  return _fxeIdentifier;
}

- (NSRect)accessibilityFrame {
  return _fxeFrame;
}

- (BOOL)accessibilityFocused {
  return _fxeFocused;
}

- (BOOL)accessibilityEnabled {
  return _fxeEnabled;
}
@end

@interface FxeContentView : NSView
@end

@interface FxeContentView (FxeAccessibility) <NSAccessibility>
@end

@implementation FxeContentView (FxeAccessibility)
- (BOOL)isAccessibilityElement {
  return YES;
}

- (NSAccessibilityRole)accessibilityRole {
  return NSAccessibilityGroupRole;
}

- (NSArray*)accessibilityChildren {
  auto snap = fxe_accessibility_snapshot_for_view(self);
  if (!snap || snap->json.empty())
    return @[];

  json tree;
  try {
    tree = json::parse(snap->json);
  } catch (...) {
    return @[];
  }
  if (!tree.is_object())
    return @[];

  auto root_id_it = tree.find("rootId");
  auto nodes_it = tree.find("nodesById");
  auto children_it = tree.find("childrenById");
  if (root_id_it == tree.end() || !root_id_it->is_string() || nodes_it == tree.end() ||
      !nodes_it->is_object() || children_it == tree.end() || !children_it->is_object()) {
    return @[];
  }

  const auto& nodes = *nodes_it;
  const auto& child_index = *children_it;
  const std::string root_id = root_id_it->get<std::string>();
  std::string focused_id;
  if (auto focused_it = tree.find("focusedId"); focused_it != tree.end() && focused_it->is_string())
    focused_id = focused_it->get<std::string>();

  std::function<FxeAXElement*(const std::string&, id)> build_node;
  build_node = [&](const std::string& node_id, id parent) -> FxeAXElement* {
    auto node_it = nodes.find(node_id);
    if (node_it == nodes.end() || !node_it->is_object())
      return nil;
    const auto& node = *node_it;
    NSString* role_name = fxe_json_string_field(node, "role");
    NSString* label = fxe_json_string_field(node, "label");
    NSString* identifier = fxe_nsstring(node_id);
    FxeAXElement* element = [[FxeAXElement alloc]
          initWithParent:parent
                    role:fxe_ns_role_for_string(role_name ? std::string_view(role_name.UTF8String) : "group")
                   label:label
              identifier:identifier
                   frame:fxe_frame_for_node(self.window, node)
                 focused:node_id == focused_id
                 enabled:!fxe_json_bool_field(node.value("state", json::object()), "disabled", false)];
    if (!element)
      return nil;

    NSMutableArray* children = [NSMutableArray array];
    for (const auto& child_id : fxe_child_ids_for_node(nodes, child_index, node_id)) {
      if (FxeAXElement* child = build_node(child_id, element))
        [children addObject:child];
    }
    [element setFxeChildren:children];
    return element;
  };

  if (FxeAXElement* root = build_node(root_id, self))
    return @[root];
  return @[];
}
@end
